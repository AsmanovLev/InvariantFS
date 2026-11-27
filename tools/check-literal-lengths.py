#!/usr/bin/env python3
"""check-literal-lengths.py -- find comparisons against a string literal that is
SHORTER than the length being compared.

The defect this exists for (fixed 2026-11-27, vol_spt0.c):

    memcmp(in->recipe_addr, "\\0\\0\\0\\0\\0\\0\\0\\0", INVFS_RECIPE_ADDR_LEN)

Eight bytes of literal, thirty-two bytes compared: the remaining 24 came from
whatever .rodata happened to follow, so the answer was a question about the LINK
LAYOUT. The code was asking "is this recipe address all zero" in a function that
computes the save point's protected block set -- a wrong answer there frees a block
a live row still needs. It survived every test until the build flags changed.

Why a checker and not just a fix: the shape is invisible in review (the literal and
the define live far apart), and its failure mode is a different answer per build.

SELF-CONTROL. A checker that reports zero must be shown able to report one, or
"no candidates" is indistinguishable from a broken parser. Every run first feeds
itself a snippet carrying the known-bad shape and REQUIRES a hit; if that fails, the
checker exits 3 -- a checker that cannot fail is worse than no checker.

Exit: 0 clean, 3 self-control broken, 1 candidates found, 2 usage.
"""
import io
import os
import re
import sys
import tempfile

CALL = re.compile(r"\b(memcmp|strncmp)\s*\(", re.S)
DEFINE = re.compile(r"^#define\s+(\w+)\s+\(?\s*(\d+)u?\s*\)?", re.M)


def literal_len(body):
    """decoded length of a C literal body (quotes stripped), terminator excluded"""
    n, i = 0, 0
    while i < len(body):
        if body[i] == "\\":
            i += 2
            if i <= len(body) and body[i - 1] in "xX":
                while i < len(body) and body[i] in "0123456789abcdefABCDEF":
                    i += 1
        else:
            i += 1
        n += 1
    return n


def collect_defines(root):
    d = {}
    for dirpath, _, files in os.walk(root):
        for f in files:
            if not f.endswith((".c", ".h")):
                continue
            try:
                t = io.open(os.path.join(dirpath, f), encoding="utf-8",
                            errors="replace", newline="").read()
            except OSError:
                continue
            for m in DEFINE.finditer(t):
                d.setdefault(m.group(1), int(m.group(2)))
    return d


def scan(root, defines):
    hits = []
    for dirpath, _, files in os.walk(root):
        for f in sorted(files):
            if not f.endswith((".c", ".h")):
                continue
            p = os.path.join(dirpath, f)
            try:
                t = io.open(p, encoding="utf-8", errors="replace", newline="").read()
            except OSError:
                continue
            for m in CALL.finditer(t):
                i = m.end() - 1
                depth, j = 0, i
                while j < len(t):
                    if t[j] == "(":
                        depth += 1
                    elif t[j] == ")":
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                args = t[i + 1:j]
                parts = [a.strip() for a in re.split(r",(?![^()]*\))", args)]
                if len(parts) != 3:
                    continue
                lm = re.match(r'^"((?:[^"\\]|\\.)*)"$', parts[1], re.S)
                if not lm:
                    continue
                nm = re.match(r"^\(?\s*(\w+|\d+)\s*\)?$", parts[2])
                if not nm:
                    continue
                tok = nm.group(1)
                n = int(tok) if tok.isdigit() else defines.get(tok)
                if n is None:
                    continue
                llen = literal_len(lm.group(1))
                if llen < n:
                    hits.append((p, t[:m.start()].count("\n") + 1, m.group(1),
                                 llen, tok, n))
    return hits


SELF = """#define INVFS_RECIPE_ADDR_LEN   32u
int f(const unsigned char *addr)
{
    return memcmp(addr, "\\0\\0\\0\\0\\0\\0\\0\\0", INVFS_RECIPE_ADDR_LEN) == 0;
}
"""


def self_control():
    d = tempfile.mkdtemp()
    os.makedirs(os.path.join(d, "core"))
    with io.open(os.path.join(d, "core", "bad.c"), "w") as f:
        f.write(SELF)
    hits = scan(d, collect_defines(d))
    return len(hits) == 1


def main(argv):
    root = argv[1] if len(argv) > 1 else "src"
    if not self_control():
        print("check-literal-lengths: SELF-CONTROL FAILED -- the checker cannot "
              "report the known-bad shape, so 'no candidates' below would mean "
              "nothing", file=sys.stderr)
        return 3
    hits = scan(root, collect_defines(root))
    for p, line, fn, llen, tok, n in hits:
        print("%s:%d: %s() compares %d literal byte(s) against %s=%d -- the rest "
              "of the compared bytes come from whatever follows the literal in "
              ".rodata, so the answer depends on the link layout"
              % (p, line, fn, llen, tok, n))
    if hits:
        print("check-literal-lengths: %d candidate(s) in %s" % (len(hits), root),
              file=sys.stderr)
        return 1
    print("check-literal-lengths: ok (self-control fired, 0 candidates in %s)" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
