import os
import random
import struct
import sys
import zlib

d = sys.argv[1]
rnd = random.Random(21)
MB = 1048576

def write_vdi(path, cblocks, blk_slot, contents, cbdisk=None, tail=b"",
              pad_junk=b"", vtype=1, uuid_link=bytes(16),
              text=b"<<< Oracle VM VirtualBox Disk Image >>>\n"):
    """blk_slot: {blk: slot}; contents: {blk: bytes(cbblock)}."""
    cbblock = MB
    if cbdisk is None:
        cbdisk = cblocks * cbblock
    offblocks = 0x200
    offdata = 0x200 + ((4 * cblocks + 511) // 512) * 512
    hdr = bytearray(offdata)
    hdr[0:len(text)] = text
    struct.pack_into('<I', hdr, 0x40, 0xBEDA107F)   # signature
    struct.pack_into('<I', hdr, 0x44, 0x00010001)   # version 1.1
    struct.pack_into('<I', hdr, 0x48, 0x190)        # header size (VBox-ish)
    struct.pack_into('<I', hdr, 0x4C, vtype)        # image type
    struct.pack_into('<I', hdr, 0x154, offblocks)
    struct.pack_into('<I', hdr, 0x158, offdata)
    struct.pack_into('<I', hdr, 0x168, 512)         # sector size
    struct.pack_into('<Q', hdr, 0x170, cbdisk)
    struct.pack_into('<I', hdr, 0x178, cbblock)
    struct.pack_into('<I', hdr, 0x180, cblocks)
    struct.pack_into('<I', hdr, 0x184, len(blk_slot))
    hdr[0x188:0x198] = rnd.randbytes(16)            # uuidCreate
    hdr[0x198:0x1A8] = rnd.randbytes(16)            # uuidModify
    hdr[0x1A8:0x1B8] = uuid_link                    # uuidLinkage
    bmap = [0xFFFFFFFF] * cblocks
    for b, s in blk_slot.items():
        bmap[b] = s
    struct.pack_into('<%dI' % cblocks, hdr, offblocks, *bmap)
    if pad_junk:                                    # bmap padding region
        hdr[offblocks + 4 * cblocks:][:len(pad_junk)] = pad_junk
    nslots = max(blk_slot.values()) + 1 if blk_slot else 0
    data = bytearray(rnd.randbytes(nslots * cbblock))  # slot holes stay junk
    for b, s in blk_slot.items():
        data[s * cbblock:(s + 1) * cbblock] = contents[b]
    out = bytes(hdr) + bytes(data) + tail
    open(path, 'wb').write(out)
    return out

def tile(blob, n):
    return (blob * (n // len(blob) + 1))[:n]

# content sources: a real busybox C source + a real x86-64 ELF
text = None
for root, dirs, files in os.walk("/home/user/InvariantFS/tools/busybox-src"):
    dirs.sort()
    for n in sorted(files):
        if n.endswith(".c"):
            p = os.path.join(root, n)
            if os.path.getsize(p) > 20000:
                text = open(p, "rb").read()
                break
    if text:
        break
assert text, "no busybox .c fixture found"
elf = None
for p in ("/usr/bin/passwd", "/usr/bin/gpg", "/bin/ls", "/usr/bin/ls",
          "/bin/bash", "/usr/bin/bash"):
    if os.path.isfile(p) and not os.path.islink(p):
        with open(p, "rb") as f:
            h = f.read(20)
        if h[:4] == b"\x7fELF" and h[18] == 62 and os.path.getsize(p) > 65536:
            elf = open(p, "rb").read()
            break
assert elf, "no x86-64 ELF fixture found"

zeros = bytes(MB)
# --- diska.vdi: 8 of 16 blocks, interleaved blocks, scrambled slots,
#     unused slots 7+8 (junk holes inside the data area), junk bmap
#     padding, trailing junk ------------------------------------------------
ablk = {0: zeros, 2: tile(elf, MB), 5: tile(text, MB), 7: rnd.randbytes(MB),
        8: zeros, 10: tile(text, MB), 13: rnd.randbytes(MB), 15: tile(elf, MB)}
aslot = {0: 0, 2: 3, 5: 1, 7: 6, 8: 2, 10: 9, 13: 4, 15: 5}
tail = rnd.randbytes(1000)
pad = rnd.randbytes(0x400 - (0x200 + 4 * 16))
img = write_vdi(os.path.join(d, "diska.vdi"), 16, aslot, ablk, tail=tail,
                pad_junk=pad)
# reference member stream: allocated blocks in TABLE order
open(os.path.join(d, "member-a.ref"), "wb").write(
    b"".join(ablk[b] for b in sorted(ablk)))
offdata = 0x200 + ((4 * 16 + 511) // 512) * 512
with open(os.path.join(d, "diska.layout"), "w") as f:
    f.write("size=%d offdata=%d hole_off=%d ext9_off=%d tail_off=%d\n"
            % (len(img), offdata, offdata + 7 * MB, offdata + 9 * MB,
               offdata + 10 * MB))

# --- diskb.vdi: the virtual disk's first 4 MiB are a coherent raw disk
#     image (GPT + one 2 MiB Linux-data partition); blocks 0-3 allocated
#     (slots scrambled), so the compacted member stream IS that disk. The
#     GPT carries NO protective-MBR 55AA on purpose: the fatfs pack sniffs
#     55AA@510 like rawdisk does, and a pack-claimed member waits on ITS
#     probe — the GPT-only signature keeps the composition channel
#     deterministically on rawdisk ("EFI PART" @ LBA1). ---------------------
gpt = bytearray(4 * MB)
NLBA = 4 * MB // 512
ent = bytearray(128 * 128)
# Linux filesystem data GUID, GPT mixed-endian on-disk form
ent[0:16] = bytes.fromhex("af3dc60f83847247798e793dd69d8477de")
ent[16:32] = bytes.fromhex("00112233445566778899aabbccddeeff")
struct.pack_into('<Q', ent, 32, 2048)           # first_lba
struct.pack_into('<Q', ent, 40, 6143)           # last_lba (2 MiB)
nm = "vdicompose".encode('utf-16-le')
ent[56:56 + len(nm)] = nm
gpt[2 * 512:2 * 512 + len(ent)] = ent
ecrc = zlib.crc32(ent) & 0xFFFFFFFF

def gpt_header(my_lba, alt_lba, ent_lba, ecrc):
    h = bytearray(512)
    h[0:8] = b"EFI PART"
    struct.pack_into('<I', h, 8, 0x00010000)    # revision 1.0
    struct.pack_into('<I', h, 12, 92)           # header size
    struct.pack_into('<Q', h, 24, my_lba)
    struct.pack_into('<Q', h, 32, alt_lba)
    struct.pack_into('<Q', h, 40, 34)           # first usable
    struct.pack_into('<Q', h, 48, 8158)         # last usable
    h[56:72] = bytes.fromhex("aabbccddeeff00112233445566778899")
    struct.pack_into('<Q', h, 72, ent_lba)
    struct.pack_into('<I', h, 80, 128)          # num entries
    struct.pack_into('<I', h, 84, 128)          # entry size
    struct.pack_into('<I', h, 88, ecrc)
    struct.pack_into('<I', h, 16, zlib.crc32(h[:92]) & 0xFFFFFFFF)
    return h

gpt[512:1024] = gpt_header(1, NLBA - 1, 2, ecrc)
gpt[8159 * 512:8159 * 512 + len(ent)] = ent     # backup entries
gpt[8191 * 512:] = gpt_header(8191, 1, 8159, ecrc)  # backup header
bblk = {i: bytes(gpt[i * MB:(i + 1) * MB]) for i in range(4)}
bslot = {0: 2, 1: 0, 2: 3, 3: 1}
img = write_vdi(os.path.join(d, "diskb.vdi"), 16, bslot, bblk)
open(os.path.join(d, "member-b.ref"), "wb").write(
    b"".join(bblk[b] for b in sorted(bblk)))

# --- decline fixtures -------------------------------------------------------
# static (type 2): 4 blocks, all allocated
sblk = {i: rnd.randbytes(MB) for i in range(4)}
write_vdi(os.path.join(d, "stat.vdi"), 4, {i: i for i in range(4)}, sblk,
          vtype=2)
# differencing: a valid dynamic image but with a parent UUID link
dblk = {0: rnd.randbytes(MB)}
write_vdi(os.path.join(d, "diff.vdi"), 2, {0: 0}, dblk,
          uuid_link=b"\x42" + bytes(15))
# corrupt: bmap slot whose extent lies beyond EOF
img = bytearray(write_vdi(os.path.join(d, "broke.vdi"), 4, {1: 0},
                          {1: rnd.randbytes(MB)}))
struct.pack_into('<I', img, 0x200 + 2 * 4, 77)    # block 2 -> slot 77
open(os.path.join(d, "broke.vdi"), "wb").write(bytes(img))

for f in sorted(os.listdir(d)):
    if f.endswith(".vdi"):
        print("  %-10s %d bytes" % (f, os.path.getsize(os.path.join(d, f))))
