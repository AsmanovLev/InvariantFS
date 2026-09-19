#!/bin/bash
# test-meta-v3-fsck.sh — WP-M4: metadata-v3 fsck (root + base-tree validation).
#
# The v3 write path is not wired yet (WP-M2/M3 own the engine), so this leg
# builds a small base B+-tree by hand in a real mkfs'd v3 image and checks the
# WP-M4 checker:
#   * an intact base tree + valid RT30 double slot  -> fsck OK, exit 0
#   * a corrupted base page                         -> fsck DAMAGED, exit != 0
#   * a corrupted RT30 descriptor                   -> fsck DAMAGED, exit != 0
#   * an empty v3 volume (mkfs only)                -> fsck OK, exit 0
#
# This file is NEW for WP-M4; it does not edit tools/test-meta-v3.sh (owned by
# WP-M1). Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-v3-fsck.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
WORK=/dev/shm/metav3fsck
IMG=metav3fsck.img
rm -rf "$WORK" && mkdir -p "$WORK"
cd /dev/shm
rm -f "$IMG"

fail() { echo "FAIL: $*" >&2; exit 1; }

# craft_base <img> <mode>
#   mode = intact   : valid 2-level tree, RT30 slot0 -> root, seq 3
#   mode = badpage  : same tree, then flip one byte in a leaf page body
#   mode = badrt30  : same tree, then flip a byte in the RT30 descriptor
# The page/entry encoding matches WP-M2/M3:
#   header {magic "BPG3", u64 gen, u16 level, u16 nentries, u32 crc}
#   leaf entry   [u16 klen][key][u16 vlen][val]
#   internal entry [u16 klen][key][u64 pba][u32 crc][u64 gen][u32 flags]
# The page CRC is CRC32C over page[0:16] ++ page[20:4096] (the checksum field
# is skipped); the RT30 CRC is CRC32C over its first 0x2C bytes.
craft_base() {
    python3 - "$1" "$2" <<'PY'
import struct, sys
img, mode = sys.argv[1], sys.argv[2]
P = 4096

def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1))
    return crc ^ 0xFFFFFFFF

def page_crc(page):
    return crc32c(bytes(page[:16]) + bytes(page[20:]))

def seal(page):
    struct.pack_into('<I', page, 16, page_crc(page))
    return bytes(page)

def leaf(gen, entries):
    body = bytearray()
    for k, v in entries:
        body += struct.pack('<H', len(k)) + k
        body += struct.pack('<H', len(v)) + v
    p = bytearray(P)
    p[0:4] = b'BPG3'
    struct.pack_into('<Q', p, 4, gen)
    struct.pack_into('<H', p, 12, 0)              # INVFS_PAGE_LEVEL_LEAF
    struct.pack_into('<H', p, 14, len(entries))
    p[20:20 + len(body)] = body
    return bytearray(seal(p))

def internal(gen, children):
    body = bytearray()
    for k, pba, crc, cgen, flags in children:
        body += struct.pack('<H', len(k)) + k
        body += struct.pack('<Q', pba) + struct.pack('<I', crc)
        body += struct.pack('<Q', cgen) + struct.pack('<I', flags)
    p = bytearray(P)
    p[0:4] = b'BPG3'
    struct.pack_into('<Q', p, 4, gen)
    struct.pack_into('<H', p, 12, 1)              # internal
    struct.pack_into('<H', p, 14, len(children))
    p[20:20 + len(body)] = body
    return bytearray(seal(p))

with open(img, 'r+b') as f:
    blk = f.read(P)
    total = struct.unpack_from('<Q', blk, 0x20)[0]
    mapper_pba = struct.unpack_from('<Q', blk, 0x98)[0]
    mapper_blocks = struct.unpack_from('<I', blk, 0xA0)[0]
    root_pba = mapper_pba + mapper_blocks       # first reserved root-area page
    l0, l1 = root_pba + 1, root_pba + 2
    assert l1 < total, "root area past end of volume"

    la = leaf(1, [(b'alpha', b'one')])
    ca = struct.unpack_from('<I', la, 16)[0]
    lb = leaf(1, [(b'beta', b'two'), (b'gamma', b'three')])
    cb = struct.unpack_from('<I', lb, 16)[0]
    root = internal(2, [(b'alpha', l0, ca, 1, 1),
                        (b'beta',  l1, cb, 1, 1)])

    f.seek(l0 * P); f.write(la)
    f.seek(l1 * P); f.write(lb)
    f.seek(root_pba * P); f.write(root)

    if mode == 'badpage':
        f.seek(l0 * P + 300)
        b = f.read(1)
        f.seek(l0 * P + 300)
        f.write(bytes([b[0] ^ 0x01]))
        print("corrupted leaf page pba %d" % l0)

    rt = bytearray(48)
    rt[0:4] = b'RT30'
    struct.pack_into('<I', rt, 4, 1)             # version
    struct.pack_into('<I', rt, 8, 4096)          # page_size
    struct.pack_into('<Q', rt, 0xC, root_pba)    # root_slot[0]
    struct.pack_into('<Q', rt, 0x24, 3)          # seq
    struct.pack_into('<I', rt, 0x2C, crc32c(bytes(rt[:0x2C])))
    if mode == 'badrt30':
        rt[0x24] ^= 0x01                         # seq byte: CRC now stale
        print("corrupted RT30 descriptor")
    f.seek(0x9D0); f.write(rt)
print("crafted %s: root_pba=%d leaves=%d,%d" % (mode, root_pba, l0, l1))
PY
}

fsck_out() { $B/invf-fsck "$1" 2>&1; }

echo "== WP-M4: v3 fsck (RT30 + base-tree walk) =="

# --- leg 0: empty v3 volume is clean ------------------------------------
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.2 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "INVFS_V3=1 mkfs failed"; }
OUT=$(fsck_out "$IMG") || { echo "$OUT"; fail "empty v3 volume: fsck exited nonzero"; }
echo "$OUT" | grep -q "^OK$" || { echo "$OUT"; fail "empty v3 volume: not OK"; }
echo "$OUT" | grep -q "format:       v3" \
    || { echo "$OUT"; fail "empty v3 volume: not reported as v3"; }
echo "empty v3 volume: OK (exit 0)"

# --- leg 1: intact hand-built base tree is CLEAN and walked -------------
craft_base "$IMG" intact >"$WORK/craft1.log" 2>&1 \
    || { cat "$WORK/craft1.log"; fail "craft intact base tree failed"; }
cat "$WORK/craft1.log"
OUT=$(fsck_out "$IMG") || { echo "$OUT"; fail "intact v3 tree: fsck exited nonzero"; }
echo "$OUT" | grep -q "^OK$" || { echo "$OUT"; fail "intact v3 tree: not OK"; }
echo "$OUT" | grep -qE "pages walked: [1-9]" \
    || { echo "$OUT"; fail "intact v3 tree: no pages walked"; }
echo "$OUT" | grep -q "base keys:    3" \
    || { echo "$OUT"; fail "intact v3 tree: wrong key count"; }
echo "intact v3 base tree: CLEAN, 3 pages walked (exit 0)"

# --- leg 2: corrupted base page is DAMAGED, nonzero exit ----------------
craft_base "$IMG" badpage >"$WORK/craft2.log" 2>&1 \
    || { cat "$WORK/craft2.log"; fail "craft bad page failed"; }
cat "$WORK/craft2.log"
set +e
OUT=$(fsck_out "$IMG"); RC=$?
set -e
echo "$OUT"
[ "$RC" -ne 0 ] || fail "corrupted base page: fsck exited 0 (false clean)"
echo "$OUT" | grep -q "^DAMAGED$" \
    || fail "corrupted base page: not reported DAMAGED"
echo "$OUT" | grep -qE "bad pages:    [1-9]" \
    || fail "corrupted base page: bad-page count not reported"
echo "corrupted base page: DAMAGED, exit $RC"

# --- leg 3: corrupted RT30 descriptor is DAMAGED, nonzero exit ----------
INVFS_V3=1 $B/invf-mkfs "$IMG" 0.2 >/dev/null 2>&1
craft_base "$IMG" badrt30 >"$WORK/craft3.log" 2>&1 \
    || { cat "$WORK/craft3.log"; fail "craft bad RT30 failed"; }
cat "$WORK/craft3.log"
set +e
OUT=$(fsck_out "$IMG"); RC=$?
set -e
echo "$OUT"
[ "$RC" -ne 0 ] || fail "corrupted RT30: fsck exited 0 (false clean)"
echo "$OUT" | grep -q "^DAMAGED$" \
    || fail "corrupted RT30: not reported DAMAGED"
echo "corrupted RT30 descriptor: DAMAGED, exit $RC"

echo "ALL WP-M4 FSCK LEGS PASS"
