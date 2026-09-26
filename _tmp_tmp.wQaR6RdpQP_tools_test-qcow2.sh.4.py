import sys
w = sys.argv[1]
raw = open(w + "/out/diskb.raw", "rb").read()
ref = open(w + "/orig/member-b.ref", "rb").read()
MB = 1048576
assert len(raw) == 16 * MB, len(raw)
assert raw[0:4 * MB] == ref, "virtual disk prefix != the GPT disk image"
assert raw[4 * MB:] == bytes(12 * MB), "unallocated tail not zeros"
assert raw[512:520] == b"EFI PART", "the member stream is a GPT disk"
print("  qemu-img convert: diskb virtual disk = GPT image + zero tail")
