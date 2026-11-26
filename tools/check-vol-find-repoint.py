#!/usr/bin/env python3
"""Re-point the vol_find ledger after an edit shifted lines.

`tools/check-vol-find.sh` keys its ledger by `file:line`, which is the right idea
(a site must say what it does when the lookup fails) and a nuisance in practice:
adding fifteen lines to fuse_fs.c turns eight correct entries into STALE ones and
the same eight sites into UNMARKED, and the build goes red for something nobody
changed semantically. That has now happened four times in one day, so the
re-pointing is a tool rather than a hand-edit.

How it works: the ledger is re-pointed from a KNOWN-GOOD commit, not from the
current tree. Give it the commit whose ledger matched its tree (`--from c890ccb`)
and it applies the line delta of every hunk between that commit and the working
tree to each entry of the affected file. Edits do not reorder call sites within a
file, so a pure shift is exactly what a diff can tell us -- and unlike a
nearest-call-site search, it cannot silently attach an entry to the wrong site.

Usage:
  tools/check-vol-find-repoint.py --from <commit>      # rewrite, then verify
  tools/check-vol-find-repoint.py --from <commit> -n   # dry run
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                      capture_output=True, text=True).stdout.strip()
LEDGER = os.path.join(ROOT, "tools/check-vol-find.sh")
ENTRY = re.compile(r"^(src/[\w/]+\.(?:c|h)):(\d+):(ACTS|SKIP)(.*)$")


def hunks_for(path, base):
    diff = subprocess.run(["git", "diff", "-U0", f"{base}..HEAD", "--", path],
                          cwd=ROOT, capture_output=True, text=True).stdout
    out = []
    for m in re.finditer(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", diff):
        old_start = int(m.group(1))
        old_len = int(m.group(2) or 1)
        new_len = int(m.group(4) or 1)
        out.append((old_start, new_len - old_len))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="base", required=True,
                    help="commit whose ledger matched its tree")
    ap.add_argument("-n", "--dry-run", action="store_true")
    args = ap.parse_args()

    lines = open(LEDGER, encoding="utf-8", newline="").read().split("\n")
    by_file = {}
    for i, line in enumerate(lines):
        m = ENTRY.match(line)
        if m:
            by_file.setdefault(m.group(1), []).append((i, m))

    moved = 0
    for path, entries in sorted(by_file.items()):
        hunks = hunks_for(path, args.base)
        if not hunks:
            continue
        for i, m in entries:
            old = int(m.group(2))
            new = old + sum(d for a, d in hunks if old > a)
            if new != old:
                lines[i] = "%s:%d:%s%s" % (m.group(1), new, m.group(3), m.group(4))
                print("%-24s %5d -> %5d" % (path.split("/")[-1], old, new))
                moved += 1
    print("%d entr%s %s" % (moved, "y" if moved == 1 else "ies",
                            "would move" if args.dry_run else "moved"))
    if args.dry_run or not moved:
        return 0
    open(LEDGER, "w", encoding="utf-8", newline="").write("\n".join(lines))
    rc = subprocess.run(["bash", "tools/check-vol-find.sh"], cwd=ROOT).returncode
    print("check-vol-find.sh rc=%d" % rc)
    return rc


if __name__ == "__main__":
    sys.exit(main())
