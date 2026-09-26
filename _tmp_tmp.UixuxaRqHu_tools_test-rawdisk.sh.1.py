import os
import struct
import sys
import zlib

SEC = 512
REPO = "/home/user/InvariantFS"

def xorshift_stream(seed, n):
    """Deterministic 'arbitrary content' for gaps (incompressible-ish)."""
    out = bytearray()
    x = seed & 0xFFFFFFFFFFFFFFFF
    while len(out) < n:
        x ^= (x << 13) & 0xFFFFFFFFFFFFFFFF
        x ^= x >> 7
        x ^= (x << 17) & 0xFFFFFFFFFFFFFFFF
        out += struct.pack("<Q", x)
    return bytes(out[:n])

def gap_fill(n, marker):
    b = bytearray(xorshift_stream(0x5D00D15C ^ n, n))
    m = marker.encode()
    b[:len(m)] = m
    return bytes(b)

def mark(img, off, m):
    """Exact-width marker write (never resizes the bytearray)."""
    img[off:off + len(m)] = m

def text_bytes(n):
    """A real busybox .c file, repeated/truncated to exactly n bytes."""
    src = None
    for root, dirs, files in os.walk(os.path.join(REPO, "tools/busybox-src")):
        dirs.sort()
        for fn in sorted(files):
            if fn.endswith(".c"):
                p = os.path.join(root, fn)
                if os.path.getsize(p) > 100000:
                    src = p
                    break
        if src:
            break
    assert src, "no big busybox .c found"
    data = open(src, "rb").read()
    reps = (n + len(data) - 1) // len(data)
    return (data * reps)[:n]

def elf_bytes(n):
    """Real x86-64 ELF chunks (busybox-static head) with zeros between."""
    elf = open(os.path.join(REPO, "bin/busybox-static"), "rb").read(1 << 20)
    assert elf[:4] == b"\x7fELF" and elf[18] == 62, "busybox-static not x86-64 ELF"
    b = bytearray(n)
    b[:len(elf)] = elf
    mid = n // 2
    b[mid:mid + len(elf)] = elf
    return bytes(b)

def mbr_entry(etype, start, count):
    return bytes(4) + bytes([etype]) + bytes(3) + struct.pack("<II", start, count)

def build_mbr(d):
    nsec = 81920                                # 40 MiB
    img = bytearray(gap_fill(nsec * SEC, "RAWDISK-FIXTURE-MBR boot-area"))
    m1 = text_bytes(10 << 20)
    m2 = elf_bytes(8 << 20)
    m11 = bytes(5 << 20)
    p1_start, p1_cnt = 2048, 20480              # 10 MiB  -> [2048,22528)
    p2_start, p2_cnt = 22528, 16384             # 8 MiB   -> [22528,38912)
    ext_start, ext_cnt = 47104, 16384           # 8 MiB   -> [47104,63488)
    # the 4 MiB marker gap [38912,47104) keeps its PRNG+marker content
    mark(img, 38912 * SEC, b"GAPMARKER-4MIB between p2 and extended")
    mark(img, 47104 * SEC + SEC, b"GAPMARKER-extended-slack")
    mark(img, 63488 * SEC, b"GAPMARKER-trailing-space")
    mbr = bytearray(SEC)
    mbr[0:16] = b"RAWDISK-FIXTURE!"
    mbr[446:462] = mbr_entry(0x83, p1_start, p1_cnt)
    mbr[462:478] = mbr_entry(0x07, p2_start, p2_cnt)
    mbr[478:494] = mbr_entry(0x05, ext_start, ext_cnt)
    mbr[510:512] = b"\x55\xAA"
    img[0:SEC] = mbr
    ebr = bytearray(SEC)
    ebr[446:462] = mbr_entry(0x83, 2048, 10240)  # rel to EBR -> [49152,59392)
    ebr[510:512] = b"\x55\xAA"
    img[ext_start * SEC:(ext_start + 1) * SEC] = ebr
    img[p1_start * SEC:(p1_start + p1_cnt) * SEC] = m1
    img[p2_start * SEC:(p2_start + p2_cnt) * SEC] = m2
    img[49152 * SEC:(49152 + 10240) * SEC] = m11
    open(os.path.join(d, "disk-mbr.img"), "wb").write(img)
    open(os.path.join(d, "mbr-m1.bin"), "wb").write(m1)
    open(os.path.join(d, "mbr-m2.bin"), "wb").write(m2)
    open(os.path.join(d, "mbr-m11.bin"), "wb").write(m11)
    return {"size": nsec * SEC,
            "p1_off": p1_start * SEC, "p1_len": p1_cnt * SEC,
            "p2_off": p2_start * SEC, "p2_len": p2_cnt * SEC,
            "p11_off": 49152 * SEC, "p11_len": 10240 * SEC}

def build_gpt(d):
    nsec = 98304                                # 48 MiB
    img = bytearray(gap_fill(nsec * SEC, "RAWDISK-FIXTURE-GPT boot-area"))
    m1 = text_bytes(12 << 20)
    m2 = elf_bytes(10 << 20)
    m3 = bytes(6 << 20)
    parts = [("boot", 2048, 24576, m1),         # -> [2048,26624)
             ("rootfs", 28672, 20480, m2),      # -> [28672,49152)
             ("data", 53248, 12288, m3)]        # -> [53248,65536)
    mark(img, 26624 * SEC, b"GAPMARKER-gpt-gap-1")
    mark(img, 49152 * SEC, b"GAPMARKER-gpt-gap-2")
    mark(img, 65536 * SEC, b"GAPMARKER-gpt-trailing")

    def guid_bytes(s):
        import uuid
        return uuid.UUID(s).bytes_le          # GPT wire format

    LINUX_FS = "0fc63daf-8483-4772-8e79-3d69d8477de4"
    entries = bytearray(128 * 128)
    for i, (nm, start, cnt, payload) in enumerate(parts):
        e = entries[i * 128:(i + 1) * 128]
        e[0:16] = guid_bytes(LINUX_FS)
        e[16:32] = guid_bytes("11111111-2222-3333-4444-%012d" % (i + 1))
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
        h[56:72] = guid_bytes("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
        struct.pack_into("<Q", h, 72, entries_lba)
        struct.pack_into("<I", h, 80, 128)
        struct.pack_into("<I", h, 84, 128)
        struct.pack_into("<I", h, 88, ent_crc)
        crc = zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF
        struct.pack_into("<I", h, 16, crc)
        return h

    pmbr = bytearray(SEC)
    pmbr[446:462] = mbr_entry(0xEE, 1, nsec - 1)
    pmbr[510:512] = b"\x55\xAA"
    img[0:SEC] = pmbr
    img[SEC:2 * SEC] = header(1, nsec - 1, 2)
    img[2 * SEC:34 * SEC] = entries
    img[(nsec - 33) * SEC:(nsec - 1) * SEC] = entries
    img[(nsec - 1) * SEC:nsec * SEC] = header(nsec - 1, 1, nsec - 33)
    open(os.path.join(d, "disk-gpt.img"), "wb").write(img)
    open(os.path.join(d, "gpt-m1.bin"), "wb").write(m1)
    open(os.path.join(d, "gpt-m2.bin"), "wb").write(m2)
    open(os.path.join(d, "gpt-m3.bin"), "wb").write(m3)
    return {"size": nsec * SEC,
            "p1_off": 2048 * SEC, "p1_len": 24576 * SEC,
            "p2_off": 28672 * SEC, "p2_len": 20480 * SEC,
            "p3_off": 53248 * SEC, "p3_len": 12288 * SEC}

d = sys.argv[1]
la = build_mbr(d)
lb = build_gpt(d)
notes = (b"These are plain notes about the disk images, not a disk.\n" * 200)
open(os.path.join(d, "notes.img"), "wb").write(notes)
with open(os.path.join(d, "layout.txt"), "w") as f:
    for k, v in [("a_size", la["size"]),
                 ("a_p1_off", la["p1_off"]), ("a_p1_len", la["p1_len"]),
                 ("a_p2_off", la["p2_off"]), ("a_p2_len", la["p2_len"]),
                 ("a_p11_off", la["p11_off"]), ("a_p11_len", la["p11_len"]),
                 ("b_size", lb["size"]),
                 ("b_p1_off", lb["p1_off"]), ("b_p1_len", lb["p1_len"]),
                 ("b_p2_off", lb["p2_off"]), ("b_p2_len", lb["p2_len"]),
                 ("b_p3_off", lb["p3_off"]), ("b_p3_len", lb["p3_len"])]:
        f.write("%s=%d\n" % (k, v))
print("  disk-mbr.img %d (members: 10M text, 8M elf+zeros, 5M zeros)" % la["size"])
print("  disk-gpt.img %d (members: 12M text, 10M elf+zeros, 6M zeros)" % lb["size"])
print("  notes.img    %d (plain text, must be refused)" % len(notes))
