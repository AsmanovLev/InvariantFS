#!/usr/bin/env python3
"""check-retired-surface.py — fail if a deleted identifier comes back in CODE.

THE POINT. The mapping journal, the name index and the record stream were
deleted (0c82a7a removed the branches; wp/purge-v2-l2p-journal removed the
journal itself and its last reader). The author's standing complaint about
that deletion is that it was INVISIBLE: the tree still compiled, still
linked, still ran, and nothing failed.

This is not the proof that they are gone. That is
`src/cli/no_v2_surface_test.c`, which drives the shipped binaries through a
write/overwrite/sweep/fsck cycle on a real v3 volume and then scans the
32 MiB reserved gap for a single non-zero byte. Every entry point of the
deleted layer wrote there and nowhere else, so that test fails on ANY
re-grown caller, whatever it is named -- a grep for one name cannot do
that, and a link check cannot do it at all (everything already links).

What this checks is the narrower thing a disk scan cannot see: a retired
NAME coming back into a header, a struct field, a declaration or a call,
before or without any behaviour change. That is how a format gets
re-grown in practice -- a change that "just adds a field", or one that
resurrects a helper because two call sites would have been tidier with it
-- and by the time the behaviour follows, the deletion is invisible again.

COMMENTS ARE EXEMPT, on purpose. A comment that describes a format that no
longer exists is a bug in the comment (AGENTS.md 1.7), but a commit that
SAYS "this went with the journal" is the thing you want in the tree, and a
lint that fails on it teaches people to delete the explanation instead.
So the scan strips /* */ and // comments first. A hit inside a comment is
still worth a reviewer's eye; a hit in code is a build failure.

HISTORY IS EXEMPT, also on purpose: impl_docs/ and INCIDENTS.md are where a
retired name is SUPPOSED to appear. That is what makes them worth reading.

Deliberately a NAME list, not a pattern. Renaming one to get past this
check is itself the signal a reviewer should catch.

Exit 0 = clean. Exit 1 = a retired identifier is back in code.
"""
import os
import re
import subprocess
import sys

# The identifiers THIS deletion removed. A tripwire has to be green the
# moment it is installed, so this is exactly what went with the mapping
# journal -- not "everything retired-sounding".
#
# Deliberately NOT in this list yet, because they are still in the tree and
# are a separate piece of work, not a tripwire that should fail today:
#   invfs_ckp0 / ckp0_crc / vol_ckp_*   the sweep-checkpoint descriptor
#                                        (volume.c reads it at open; the FUSE
#                                        sweep arm and invf-rollback use it)
#   invfs_jrn_hdr / INVFS_JRN_MAGIC     parsed by invf-resize to decide how to
#                                        move the reserved gap
#   INVFS_JOURNAL_BLOCKS                the gap's skip distance itself; it is
#                                        load-bearing geometry, see invarifs.h
RETIRED = [
    "vol_map", "vol_lookup", "vol_lookup_entry", "vol_l2p", "vol_l2p_remove",
    "l2p_replay", "l2p_apply", "l2p_remove", "l2p_remove_mem",
    "l2p_idx_", "l2p_seed_heat", "vol_records_walk",
    "jrn_push_op", "jrn_push_meta_op", "jrn_slot_base", "jrn_flush",
    "jrn_compact", "jrn_read_hdr", "jrn_seed", "jrn_chain",
    "jrn_write_image", "jrn_append_pending", "jrn_append_legacy",
    "jrn_abort_at", "jops", "mjops", "open_cuts",
    "INVFS_JRN_FORCE_COMPACT", "INVFS_COMPACT_ABORT_AT",
]

# Where the scan looks. Not impl_docs/, not INCIDENTS.md, not packaging/
# translations: history and translations are not where a re-grow lands.
SCAN_DIRS = ("src", "tools")

BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
C_LINE_COMMENT = re.compile(r"//[^\n]*")
HASH_COMMENT = re.compile(r"(?<![\"'])[#].*?(?=\n|$)", re.S)


def _blank(m):
    """Replace with spaces, keeping every newline, so line numbers hold."""
    return re.sub(r"[^\n]", " ", m.group(0))


def strip_comments(text: str, ext: str) -> str:
    out = BLOCK_COMMENT.sub(_blank, text)
    out = C_LINE_COMMENT.sub(_blank, out)
    if ext in (".py", ".sh"):
        out = HASH_COMMENT.sub(_blank, out)
    return out


def main() -> int:
    root = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True)
    if root.returncode != 0:
        print("check-retired-surface: not inside a git worktree", file=sys.stderr)
        return 1
    root = root.stdout.strip()
    os.chdir(root)

    files = subprocess.run(["git", "ls-files", *SCAN_DIRS],
                           capture_output=True, text=True).stdout.split()
    # This file names every retired identifier in order to look for them, so
    # it necessarily matches itself. Excluded by name, not by pattern.
    me = os.path.relpath(os.path.abspath(__file__), os.getcwd())
    files = [f for f in files if os.path.normpath(f) != os.path.normpath(me)]
    exts = (".c", ".h", ".sh", ".py")
    pats = [re.compile(r"(?<![A-Za-z0-9_])" + re.escape(n) + r"(?![A-Za-z0-9_])")
            for n in RETIRED]

    hits = []
    for f in files:
        if not f.endswith(exts):
            continue
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                src = fh.read()
        except OSError:
            continue
        for i, line in enumerate(strip_comments(src, os.path.splitext(f)[1]).split("\n"), 1):
            for p in pats:
                if p.search(line):
                    hits.append("%s:%d: %s" % (f, i, line.strip()[:100]))
                    break
    for h in hits:
        print(h)
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())