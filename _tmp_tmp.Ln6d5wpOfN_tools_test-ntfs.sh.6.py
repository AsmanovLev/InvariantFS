import sys
w = sys.argv[1]
rows = []
for line in open(w + "/table"):
    line = line.rstrip("\n")
    if not line:
        continue
    idx, name, usize = line.split("\t")
    rows.append((int(idx), name, int(usize)))
assert rows, "no members"
idxs = [r[0] for r in rows]
assert len(idxs) == len(set(idxs)), "duplicate idx"
names = sorted(r[1] for r in rows)
print("  members:", len(rows), "names:", names)
# expected membership: 5 pins + frag + sparse + big-or-hard + elf + nested
assert sum(1 for n in names if n.startswith("pin_")) == 5, "want 5 pins"
assert "frag.bin" in names and "sparse.bin" in names
assert "program.elf" in names and "nested.c" in names
# the hardlink pair shares ONE record -> exactly one of the two names
assert ("big.txt" in names) != ("hard.txt" in names), "hardlink must be one member"
# resident/empty files are NOT members
assert "tiny.txt" not in names and "empty.txt" not in names
for _i, n, u in rows:
    if n == "sparse.bin": assert u == 16*1024*1024
    if n == "frag.bin":   assert u == 10*1024*1024
open(w + "/members", "w").write("\n".join("%d %s %d" % r for r in rows) + "\n")
