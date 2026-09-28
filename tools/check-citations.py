#!/usr/bin/env python3
"""AGENTS.md 1.7: a subsystem doc cites file:line, and a claim that cannot be
pointed at in the tree is deleted, not softened.

This checks the half of that rule nobody enforced: that a cited line number
is still inside the file. Citations rot silently -- a file grows, a WP
shifts everything down, and the doc keeps citing a line that now says
something else. The existing path-existence check in
tools/check-repo-hygiene.sh cannot see that.

Two things this deliberately does NOT do:

  * It does not treat a missing file as a hard failure. Prose legitimately
    names a file that was never written -- Benchmark.md says outright that
    `bench-invfs.sh` does not exist and that the run is not reproducible
    from the repo. That is an honest documented absence, not a broken
    citation, and flagging it as one would push a correct doc toward a
    vaguer sentence. Missing files are reported as WARN.
  * It does not try to verify that a cited line says what the doc claims.
    That is a judgement for a reader, and automating it would produce
    confident nonsense.

Usage: tools/check-citations.py [paths...]   (default: AGENTS.md + docs/)
Exit 0 if every cited line number is in range.
"""

import os
import re
import sys

# path[:line[-line]]  -- deliberately narrow: prose, not diffs or URLs.
CITATION = re.compile(
    r"`([A-Za-z0-9_][A-Za-z0-9_./+-]*\.(?:c|h|sh|md|8|py))"
    r"(?::(\d+)(?:-(\d+))?)?`"
)

# Where a bare filename is allowed to resolve from. A citation in a doc may
# name a source file without its directory.
SEARCH_PREFIXES = (
    ".",
    "src/core",
    "src/cli",
    "src",
    "tools",
    "docs",
    "docs/architecture",
    "packaging/man",
    "impl_docs",
)


def resolve(name, docdir="."):
    """Find the cited file, or None. Directories in the name are honoured.

    A doc's own directory is searched first: a file in docs/benchmarks/ that
    cites a sibling by bare name means that sibling, and searching only a
    fixed prefix list reports it as missing.
    """
    if os.path.exists(name):
        return name
    if os.path.exists(os.path.join(docdir, name)):
        return os.path.join(docdir, name)
    for prefix in SEARCH_PREFIXES:
        candidate = os.path.join(prefix, name)
        if os.path.exists(candidate):
            return candidate
    return None


def line_count(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return sum(1 for _ in handle)


def check(path):
    """Return (out_of_range, missing_file) findings for one doc."""
    out_of_range, missing = [], []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for lineno, text in enumerate(handle, 1):
            for match in CITATION.finditer(text):
                name, first, last = match.groups()
                target = resolve(name, os.path.dirname(path) or ".")
                if target is None:
                    # Honest documented absence, or a genuinely wrong path.
                    # WARN, not FAIL -- see the module docstring.
                    missing.append((lineno, name))
                    continue
                if first is None:
                    continue  # path-only citation, already covered by hygiene
                total = line_count(target)
                low = int(first)
                high = int(last) if last else low
                if low < 1 or high > total:
                    span = f"{name}:{first}-{last}" if last else f"{name}:{first}"
                    out_of_range.append((lineno, span, total))
    return out_of_range, missing


def main(argv):
    targets = argv[1:]
    if not targets:
        targets = ["AGENTS.md"]
        for root, _, files in os.walk("docs"):
            targets += [os.path.join(root, f) for f in sorted(files) if f.endswith(".md")]

    bad = 0
    for path in targets:
        if not os.path.exists(path):
            print(f"  cite: skip (not found) {path}")
            continue
        out_of_range, missing = check(path)
        for lineno, span, total in out_of_range:
            bad += 1
            print(
                f"  cite: {path}:{lineno} cites {span} but the file has {total} lines",
                file=sys.stderr,
            )
        for lineno, name in missing:
            print(f"  cite: WARN {path}:{lineno} names {name}, which is not in the tree")

    if bad:
        print(
            f"citation check: FAIL -- {bad} cited line number(s) no longer exist. "
            "AGENTS.md 1.7: a claim that cannot be pointed at is deleted, not softened.",
            file=sys.stderr,
        )
        return 1
    print(f"citation check: OK (line numbers in range across {len(targets)} doc(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
