#!/usr/bin/env python3
"""How much duplicate data sits in SMALL files -- the class dedupe-probe skips.

tools/dedupe-probe.c ignores files under 64 KiB, because below that the per-
reference overhead dominates. That leaves the small-file question unmeasured, and
it is the one case where deduplication needs no new lifetime machinery: if the
bytes live inside the (already content-addressed) recipe blob, two identical small
files share one blob and there is nobody to own.

MEASURED 2026-11-25 on D: (122 776 files under 64 KiB):

    size class        files      bytes     duplicated   dup %
    0..4 KiB          91 686   89.0 MiB     29.9 MiB    33.6%
    4..16 KiB         21 005  162.2 MiB     50.4 MiB    31.1%
    16..64 KiB        10 085  310.2 MiB     70.4 MiB    22.7%
    TOTAL                     561.4 MiB    150.7 MiB    26.9%

The biggest groups are index.js / README.md / index.d.ts / LICENSE / *.go -- i.e.
package-cache and node_modules-shaped duplication, exactly what a store sees.

Reference-cost arithmetic that decides how small a unit pays: one recipe entry is
28 B (sizeof(invfs_ast_window_entry)), so at 1 KiB granularity the reference costs
2.7% of the bytes it saves, at 4 KiB 0.7%, at 64 KiB 0.04% -- and a candidate index
entry roughly doubles the small end. Clustering several members into one object
(what containerpack's !partN already does, and what the sweep's batch lane does for
small files) is the mechanism that makes the small end affordable: one reference
per cluster plus a few bytes of offset per member.

Usage: python tools/small-file-dupes.py D:\ [max_files]
"""
import hashlib
import os
import sys
import collections

ROOT = sys.argv[1] if len(sys.argv) > 1 else "D:\\"
MAX = int(sys.argv[2]) if len(sys.argv) > 2 else 200000
SKIP = {"$RECYCLE.BIN", "System Volume Information"}
CLASSES = [(0, 4096), (4096, 16384), (16384, 65536), (65536, 1 << 62)]


def main():
    sizes = collections.defaultdict(lambda: [0, 0])      # class -> [files, bytes]
    seen = {}                                            # hash -> (size, name)
    dup_bytes = collections.Counter()                    # class -> duplicate bytes
    groups = collections.Counter()                        # name -> wasted bytes
    n = 0
    stack = [ROOT]
    while stack and n < MAX:
        d = stack.pop()
        try:
            with os.scandir(d) as it:
                for e in it:
                    try:
                        if e.is_dir(follow_symlinks=False):
                            if e.name not in SKIP:
                                stack.append(e.path)
                            continue
                        if not e.is_file(follow_symlinks=False):
                            continue
                        st = e.stat(follow_symlinks=False)
                        sz = st.st_size
                        if sz == 0 or sz >= 65536:
                            continue
                        n += 1
                        cls = next((lo for lo, hi in CLASSES if lo <= sz < hi), CLASSES[-1][0])
                        sizes[cls][0] += 1
                        sizes[cls][1] += sz
                        with open(e.path, "rb") as fh:
                            data = fh.read()
                        k = hashlib.blake2b(data, digest_size=16).digest()
                        if k in seen:
                            dup_bytes[cls] += sz
                            groups[os.path.basename(seen[k][1])] += sz
                        else:
                            seen[k] = (sz, e.path)
                    except OSError:
                        continue
        except (PermissionError, OSError):
            continue

    print("scanned %d files under 64 KiB on %s" % (n, ROOT))
    print("%-16s %10s %12s %12s %8s" % ("size class", "files", "bytes", "dup bytes", "dup %"))
    tot_b = tot_d = 0
    for lo, hi in CLASSES:
        if hi > 1 << 61:
            continue
        f, b = sizes[lo]
        d_ = dup_bytes[lo]
        tot_b += b
        tot_d += d_
        print("%-16s %10d %10.1f MiB %10.1f MiB %7.1f%%"
              % ("%d..%d B" % (lo, hi), f, b / 2**20, d_ / 2**20, 100.0 * d_ / b if b else 0))
    print("%-16s %10s %10.1f MiB %10.1f MiB %7.1f%%"
          % ("TOTAL", "", tot_b / 2**20, tot_d / 2**20, 100.0 * tot_d / tot_b if tot_b else 0))
    print()
    print("biggest duplicate groups (wasted bytes, by basename):")
    for name, w in groups.most_common(8):
        print("  %-40s %8.1f KiB" % (name[:40], w / 1024))


if __name__ == "__main__":
    main()
