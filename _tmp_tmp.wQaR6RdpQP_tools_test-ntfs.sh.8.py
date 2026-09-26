import os, sys
w = sys.argv[1]
orig = os.path.getsize(w + "/fs.ntfs")
rec = os.path.getsize(w + "/recipe")
tab = os.path.getsize(w + "/table")
mapb = os.path.getsize(w + "/map")
musz = 0
for line in open(w + "/table"):
    _i, _n, u = line.rstrip("\n").split("\t")
    musz += int(u)
print("  fs.ntfs: %d B (%d MiB) decomposes into:" % (orig, orig >> 20))
print("    recipe blob  %12d B  (every non-member byte verbatim: boot" % rec)
print("                 %12s    sectors, the whole $MFT incl. fixup" % "")
print("                 %12s    trailers, $Bitmap/$LogFile/$Boot/$Secure/" % "")
print("                 %12s    $Upcase/$Extend, directory indexes," % "")
print("                 %12s    resident file data, member slack tails," % "")
print("                 %12s    and all unallocated clusters)" % "")
print("    %2d members   %12d B  (the file contents, now first-class files" %
      (sum(1 for _ in open(w + "/table")), musz))
print("                 %12s    flowing through PPMd/ZSTD batching +" % "")
print("                 %12s    dedupe)" % "")
print("    table + map  %12d B" % (tab + mapb))
tot = rec + musz + tab + mapb
print("    => the pack stores %d B (%.3fx) before the pipeline; the win is"
      % (tot, tot / orig))
print("       member-level compression/dedupe and seekable pack-free reads,")
print("       not the recipe (a filesystem's free space is metadata here)")
