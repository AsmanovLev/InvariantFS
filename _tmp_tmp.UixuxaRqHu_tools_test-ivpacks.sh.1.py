import os, sys, zlib, struct, random
d = sys.argv[1]
# a decline target that is definitely nobody's image
open(os.path.join(d, "junk.bin"), "wb").write(bytes(random.Random(7).randrange(256)
                                                   for _ in range(65536)))
# 4 MiB GPT disk: 3 named partitions (text / binary / zeros) + gap markers,
# protective MBR, primary + backup header/entries (test-rawdisk.sh's layout,
# scaled down so it fits a small tmpfs).
SEC = 512
nsec = 8192
img = bytearray(b"\xa5" * (nsec * SEC))
def text(n):
    s = b""
    i = 0
    while len(s) < n:
        s += ("line %06d: the quick brown fox jumps over the lazy dog\n" % i).encode()
        i += 1
    return s[:n]
m1 = text(64 << 10)
m2 = bytes(random.Random(11).randrange(256) for _ in range(128 << 10))
m3 = bytes(32 << 10)
parts = [("boot", 2048, 128, m1), ("rootfs", 2304, 256, m2), ("data", 2816, 64, m3)]
def guid(s):
    import uuid
    return uuid.UUID(s).bytes_le
LINUX_FS = "0fc63daf-8483-4772-8e79-3d69d8477de4"
entries = bytearray(128 * 128)
for i, (nm, start, cnt, payload) in enumerate(parts):
    e = entries[i * 128:(i + 1) * 128]
    e[0:16] = guid(LINUX_FS)
    e[16:32] = guid("11111111-2222-3333-4444-%012d" % (i + 1))
    struct.pack_into("<QQ", e, 32, start, start + cnt - 1)
    nm16 = nm.encode("utf-16-le")
    e[56:56 + len(nm16)] = nm16
    entries[i * 128:(i + 1) * 128] = e
    img[start * SEC:(start + cnt) * SEC] = payload
first_usable, last_usable = 2048, nsec - 34
ent_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF
def header(current, backup, entries_lba):
    h = bytearray(SEC)
    h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000)
    struct.pack_into("<I", h, 12, 92)
    struct.pack_into("<Q", h, 24, current)
    struct.pack_into("<Q", h, 32, backup)
    struct.pack_into("<Q", h, 40, first_usable)
    struct.pack_into("<Q", h, 48, last_usable)
    h[56:72] = guid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
    struct.pack_into("<Q", h, 72, entries_lba)
    struct.pack_into("<I", h, 80, 128)
    struct.pack_into("<I", h, 84, 128)
    struct.pack_into("<I", h, 88, ent_crc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF)
    return h
pmbr = bytearray(SEC)
pmbr[446:462] = bytes([0x00, 0x00, 0x02, 0x00, 0xEE, 0xFE, 0xFF, 0xFF]) + \
                struct.pack("<II", 1, nsec - 1)
pmbr[510:512] = b"\x55\xAA"
img[0:SEC] = pmbr
img[SEC:2 * SEC] = header(1, nsec - 1, 2)
img[2 * SEC:34 * SEC] = entries
img[(nsec - 33) * SEC:(nsec - 1) * SEC] = entries
img[(nsec - 1) * SEC:nsec * SEC] = header(nsec - 1, 1, nsec - 33)
open(os.path.join(d, "disk-gpt.img"), "wb").write(bytes(img))
for i, (nm, _s, _c, payload) in enumerate(parts):
    open(os.path.join(d, "gpt-m%d.bin" % (i + 1)), "wb").write(payload)
print("  rawdisk: disk-gpt.img %d bytes, 3 members" % (nsec * SEC))
