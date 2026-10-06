#!/bin/bash
# test-meta-fsck.sh — WP-M4: metadata-v3 fsck (root + base-tree validation).
#
# The v3 write path is not wired yet (WP-M2/M3 own the engine), so this leg
# builds a small base B+-tree by hand in a real mkfs'd v3 image and checks the
# WP-M4 checker:
#   * an intact base tree + valid RT30 double slot  -> fsck OK, exit 0
#   * a corrupted base page                         -> fsck DAMAGED, exit != 0
#   * a corrupted RT30 descriptor                   -> fsck DAMAGED, exit != 0
#   * an empty v3 volume (mkfs only)                -> fsck OK, exit 0
#
# Legs 0-3 above ask invf-fsck about invf-fsck: its exit code, its verdict
# line, and its own counters. That is a real weakness, not a style note --
# WP86's `-f` excised a key RANGE, every page CRC still verified, every fan-in
# still balanced, both files' content was gone, and the tool printed OK. A
# repair verified with the repairer's own success criteria cannot detect a
# repair that meets the criteria and fails the filesystem.
#
# So the second half of this suite does two things legs 0-3 could not:
#
#   legs 4-7  drive `-f` (nothing here ran it before) and measure the RESULT
#             with `tree_probe` -- a second, independent implementation of the
#             page format in Python, which decodes RT30, walks the tree and
#             recomputes every page CRC itself. It reports the actual
#             key/VALUE pairs, not a count, so a page holding the right number
#             of the wrong bytes fails.
#             leg 6 is a differential red control: the same assertion against
#             the volume whose OTHER leaf is torn must give the opposite answer.
#
#   leg 8     builds, by hand, the volume those assertions cannot see: three
#             valid keys in a valid single-leaf root, every CRC correct, one
#             value the wrong bytes of the same length. invf-fsck calls it OK
#             with exit 0 and "base keys: 3"; invf-verify --deep calls it
#             "0 files ok, 0 corrupt". The leg asserts BOTH halves -- that the
#             trap is real, and that the second reader sees it -- so the trap
#             stays documented instead of being an argument in a commit message.
#
# This file is NEW for WP-M4; it does not edit tools/test-meta.sh (owned by
# WP-M1). Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-meta-fsck.sh
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

    if mode == 'wrongvalue':
        # The WP86-SHAPED VOLUME, by hand. A perfectly valid single-leaf root
        # holding the SAME THREE KEYS as the intact tree, every CRC correct,
        # but one VALUE is the wrong bytes of the same length ('threX' for
        # 'three'). This is the failure mode the whole assertion style of
        # legs 0-3 is blind to, and leg 8 proves it on this image rather than
        # leaving it as an argument.
        f.seek(root_pba * P)
        f.write(leaf(7, [(b'alpha', b'one'),
                        (b'beta',  b'two'),
                        (b'gamma', b'threX')]))
        rt = bytearray(48)
        rt[0:4] = b'RT30'
        struct.pack_into('<I', rt, 4, 1)
        struct.pack_into('<I', rt, 8, 4096)
        struct.pack_into('<Q', rt, 0xC, root_pba)
        struct.pack_into('<Q', rt, 0x24, 9)
        struct.pack_into('<I', rt, 0x2C, crc32c(bytes(rt[:0x2C])))
        f.seek(0x9D0); f.write(rt)
        print("crafted wrongvalue: 3 keys, valid CRCs, gamma holds the wrong "
              "5 bytes of the same length")
        raise SystemExit(0)

    if mode == 'badpage':
        f.seek(l0 * P + 300)
        b = f.read(1)
        f.seek(l0 * P + 300)
        f.write(bytes([b[0] ^ 0x01]))
        print("corrupted leaf page pba %d (holds alpha)" % l0)
    elif mode == 'badpage2':
        # The SAME tear on the OTHER leaf. Leaf 1 holds beta+gamma, so the
        # differential red control in leg 7 can demand the opposite answer
        # from the same assertion code.
        f.seek(l1 * P + 300)
        b = f.read(1)
        f.seek(l1 * P + 300)
        f.write(bytes([b[0] ^ 0x01]))
        print("corrupted leaf page pba %d (holds beta,gamma)" % l1)

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

# --- tree_probe: a SECOND reader of the tree, independent of invf-fsck ----
# Every assertion in legs 0-3 below is a question put to the tool under test
# and answered by the tool under test: its exit code, its verdict line, and
# its own counters ("base keys: 3", "bad pages: 1"). That is the shape that
# let `btree_repair_test`'s collapse leg stay green for years while `-f`
# emptied every file: a repair verified with the repairer's own success
# criteria cannot detect a repair that meets the criteria and loses the data.
#
# This is a second implementation of the page format -- Python, not the C
# checker -- so it can answer "what is physically in this volume" without
# asking invf-fsck anything. It decodes RT30 itself, walks the tree, and
# recomputes CRC32C over every page it lands on. Output:
#
#   TREE KEYS=<k>=<v> ...          the actual key/value pairs, sorted, verbatim
#   TREE BADPAGES=<n> CRCFAIL=<n> PAGES=<n>
#
# KEYS carries the VALUE BYTES, not a key and a length, so a page that came
# back holding the right number of the wrong bytes fails here and not just in
# a checksum the tool checked itself.
tree_probe() { # <img>
    python3 - "$1" <<'PROBE'
import struct, sys
P = 4096
def crc32c(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 & -(crc & 1))
    return crc ^ 0xFFFFFFFF

img = sys.argv[1]
f = open(img, 'rb')
f.seek(0x9D0)
rt = f.read(48)
if rt[0:4] != b'RT30':
    sys.stderr.write("tree_probe: no RT30 descriptor at 0x9D0\n")
    sys.exit(1)
slot0, slot1, _delta, _seq = struct.unpack_from('<QQQQ', rt, 0xC)
root = slot1 or slot0
if not root:
    sys.stderr.write("tree_probe: RT30 names no root slot\n")
    sys.exit(1)

pairs, bad, crcfail, pages = [], [], [], []

def walk(pba, depth=0):
    if pba > (1 << 40):
        bad.append((pba, 'pba out of range'))
        return
    f.seek(pba * P)
    page = bytearray(f.read(P))
    if len(page) != P:
        bad.append((pba, 'short read past end of image'))
        return
    pages.append(pba)
    if bytes(page[0:4]) != b'BPG3':
        bad.append((pba, 'not a BPG3 page'))
        return
    level, nent, crc = (struct.unpack_from('<H', page, 4 + 8)[0],
                        struct.unpack_from('<H', page, 4 + 8 + 2)[0],
                        struct.unpack_from('<I', page, 4 + 8 + 4)[0])
    calc = crc32c(bytes(page[:16]) + bytes(page[20:]))
    if calc != crc:
        crcfail.append(pba)
    body, off = page[20:], 0
    for _ in range(nent):
        if off + 2 > len(body):
            bad.append((pba, 'entry header past end of page'))
            return
        kl = struct.unpack_from('<H', body, off)[0]; off += 2
        if off + kl > len(body):
            bad.append((pba, 'key past end of page'))
            return
        k = bytes(body[off:off + kl]); off += kl
        if level == 0:
            if off + 2 > len(body):
                bad.append((pba, 'value header past end of page'))
                return
            vl = struct.unpack_from('<H', body, off)[0]; off += 2
            pairs.append((k, bytes(body[off:off + vl]))); off += vl
        else:
            if off + 24 > len(body):
                bad.append((pba, 'child pointer past end of page'))
                return
            cpba = struct.unpack_from('<Q', body, off)[0]; off += 24
            walk(cpba, depth + 1)

walk(root)
desc = ' '.join('%s=%s' % (k.decode('latin1'), v.decode('latin1'))
                for k, v in sorted(pairs))
print('TREE KEYS=%s' % desc)
print('TREE BADPAGES=%d CRCFAIL=%d PAGES=%d'
      % (len(bad), len(crcfail), len(pages)))
for pba, why in bad:
    print('TREE BADPAGE %d %s' % (pba, why))
for pba in crcfail:
    print('TREE CRCFAIL %d' % pba)
PROBE
}

# The key/value pairs this suite crafts, spelled out ONCE. Legs 5-8 assert
# against these, not against whatever the tool under test reports.
WANT_ALL="TREE KEYS=alpha=one beta=two gamma=three"
WANT_NO_ALPHA="TREE KEYS=beta=two gamma=three"
WANT_NO_BG="TREE KEYS=alpha=one"

echo "== WP-M4: v3 fsck (RT30 + base-tree walk) =="

# --- leg 0: empty v3 volume is clean ------------------------------------
$B/invf-mkfs "$IMG" 0.2 >"$WORK/mkfs.log" 2>&1 \
    || { cat "$WORK/mkfs.log"; fail "mkfs failed"; }
OUT=$(fsck_out "$IMG") || { echo "$OUT"; fail "empty v3 volume: fsck exited nonzero"; }
echo "$OUT" | grep -q "^OK$" || { echo "$OUT"; fail "empty v3 volume: not OK"; }
echo "$OUT" | grep -q "format:       v0" \
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
$B/invf-mkfs "$IMG" 0.2 >/dev/null 2>&1
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

# ==========================================================================
# Legs 4-7: THE REPAIR, measured by the SECOND reader.
#
# Everything above asks invf-fsck about invf-fsck -- its exit code, its verdict
# line, and its own counters ("base keys: 3", "bad pages: 1"). Nothing above
# runs `-f` at all, so "fsck repairs a v3 base tree correctly" was asserted
# NOWHERE in this file, and that is the question whose wrong answer cost this
# project years: WP86's `-f` excised a key RANGE, every page CRC still
# verified, every fan-in still balanced, both files' content was gone, and the
# tool printed OK. A repair verified with the repairer's own success criteria
# cannot detect a repair that meets the criteria and loses the data.
#
# The four legs below are built so the answer cannot come from the tool:
#
#   leg 4  NON-VACUITY. The independent reader finds the three crafted
#           key/value pairs in an intact tree and every CRC it recomputed
#           agrees. If this does not hold, "found nothing else" in legs 5-7 is
#           free, and the suite says so instead of passing quietly.
#   leg 5  `-f` on a torn leaf must leave EXACTLY the pairs the suite placed
#           off that leaf, with their value bytes -- asserted as WHAT WAS LOST
#           (alpha, the one pair on the torn page) rather than as a count of
#           survivors. Plus an explicit no-op guard: a repair that dropped
#           nothing would satisfy a survivor count, so the post-repair set is
#           required to differ from the pre-repair one.
#   leg 6  RED CONTROL, DIFFERENTIAL. The same assertion code against the
#           volume whose OTHER leaf is torn, demanding the OPPOSITE answer. A
#           probe that had degenerated into echoing invf-fsck could not return
#           {beta,gamma} in leg 5 and {alpha} here from the same code. This is
#           what makes legs 5 and 6 permanent rather than one-shot.
#   leg 7  THE WP86 LEG, permanent. A pass that dropped keys must not report
#           the volume clean: `LOST:` has to be on the report and the verdict
#           must not be `OK`, whatever the exit code says. This is the exact
#           assertion whose absence let `-f` print OK over an emptied volume.
#
# There is no refusal leg here and there is deliberately not one to invent: on
# this hand-built tree nothing live requires a key in the quarantined range,
# so the gate is right to proceed. The refusal contract needs a LIVE inode
# naming a key inside the range, and that is `btree_repair_test` phase J1 --
# which does it, with `invf-cat` on the far side. Two suites, one property
# each, rather than a plausible copy here.
# ==========================================================================

# --- leg 4: the independent reader agrees, on an intact tree -------------
# If this does not hold, every assertion below is vacuous: the reader would be
# finding nothing at all, and "found nothing else" would cost nothing.
PROBE=$(tree_probe "$IMG") || { echo "$PROBE"; fail "leg 4: tree_probe failed on the intact tree"; }
BEFORE=$PROBE
echo "$PROBE" | sed 's/^/  | /'
echo "$PROBE" | grep -q "^$WANT_ALL$" \
    || { echo "$PROBE"; fail "leg 4: the independent reader did not find the three crafted key/value pairs -- legs 5-7 would prove nothing"; }
echo "$PROBE" | grep -qE "^TREE BADPAGES=0 CRCFAIL=0 PAGES=3$" \
    || { echo "$PROBE"; fail "leg 4: the independent reader's own page walk and CRCs disagree with the crafted tree"; }
echo "leg 4: the second reader finds alpha=one beta=two gamma=three, CRCs recomputed and agreeing"

# --- leg 5: -f drops exactly the quarantined range, measured independently --
RIMG=metav3fsck-repair.img
rm -f "$RIMG"
$B/invf-mkfs "$RIMG" 0.2 >/dev/null 2>&1 || fail "leg 5: mkfs failed"
craft_base "$RIMG" badpage >"$WORK/craft5.log" 2>&1 \
    || { cat "$WORK/craft5.log"; fail "leg 5: craft the bad page failed"; }
set +e
OUT=$($B/invf-fsck "$RIMG" -f 2>&1); RC=$?
set -e
echo "$OUT" | sed 's/^/  | /'
echo "leg 5: invf-fsck -f exit $RC"
[ "$RC" -ne 0 ] || fail "leg 5: invf-fsck -f exited 0 on a volume with an unreadable page -- there is no repair leg here at all"
PROBE=$(tree_probe "$RIMG") || { echo "$PROBE"; fail "leg 5: tree_probe failed"; }
echo "$PROBE" | sed 's/^/  | /'
# WHAT WAS LOST, named from the suite's own craft table rather than from
# anything the tool reported: alpha was on the torn page, so it goes; beta and
# gamma were on a READABLE page outside the quarantined range, so they must
# survive with their exact value bytes. An excision that took the whole tree
# satisfies every assertion phrased as a survivor count -- and taking the whole
# tree is what WP86 did.
echo "$PROBE" | grep -q "^$WANT_NO_ALPHA$" \
    || { echo "$PROBE"; fail "leg 5: the excision dropped more than the quarantined range -- expected exactly 'beta=two gamma=three' to survive"; }
# The no-op guard. `st.nkeys + torn_keys == keys_before` was the arithmetic form
# of the same mistake: it counted what was LEFT and never what the range HELD.
# A repair that dropped nothing would sail through the line above, so require
# the key set to have actually changed.
[ "$PROBE" != "$BEFORE" ] \
    || fail "leg 5: -f changed nothing at all -- the survivor assertion above passed vacuously"
echo "$PROBE" | grep -qE "^TREE BADPAGES=0 CRCFAIL=0" \
    || { echo "$PROBE"; fail "leg 5: the repaired tree still holds an unreadable page"; }
echo "leg 5: exactly the pair on the torn page is gone; the readable page's two pairs are byte-intact"

# --- leg 6: RED CONTROL, differential -- the same assertion, other leaf ---
DIMG=metav3fsck-diff.img
rm -f "$DIMG"
$B/invf-mkfs "$DIMG" 0.2 >/dev/null 2>&1 || fail "leg 6: mkfs failed"
craft_base "$DIMG" badpage2 >"$WORK/craft6.log" 2>&1 \
    || { cat "$WORK/craft6.log"; fail "leg 6: craft the second bad page failed"; }
set +e
OUT=$($B/invf-fsck "$DIMG" -f 2>&1); RC=$?
set -e
echo "$OUT" | sed 's/^/  | /'
PROBE=$(tree_probe "$DIMG") || { echo "$PROBE"; fail "leg 6: tree_probe failed"; }
echo "$PROBE" | sed 's/^/  | /'
# SAME assertion, OPPOSITE expectation. The tear is on the leaf holding beta
# and gamma this time, so those two must go and alpha must survive. If the
# probe were only agreeing with invf-fsck it could not produce this answer
# after producing leg 5's; that is what keeps leg 5 honest.
echo "$PROBE" | grep -q "^$WANT_NO_BG$" \
    || { echo "$PROBE"; fail "leg 6 RED CONTROL: tearing the OTHER leaf did not yield the opposite answer -- the probe is echoing the tool under test, so leg 5 proves nothing"; }
echo "leg 6: red control fired -- the other tear yields the other answer, so the probe is measuring, not agreeing"

# --- leg 7: THE WP86 LEG, permanent -------------------------------------
# A pass that dropped keys must never report the volume CLEAN. The WP86
# collapse repair excised a key range, every structural check the pass ran
# still passed, and it printed OK over a volume whose files were empty. This
# is the assertion that was missing then, kept permanently now.
FIMG=metav3fsck-f.log
$B/invf-mkfs "$RIMG" 0.2 >/dev/null 2>&1 || fail "leg 7: mkfs failed"
craft_base "$RIMG" badpage >"$WORK/craft7.log" 2>&1 \
    || { cat "$WORK/craft7.log"; fail "leg 7: craft the bad page failed"; }
set +e
OUT=$($B/invf-fsck "$RIMG" -f 2>&1); RC=$?
set -e
printf '%s\n' "$OUT" >"$FIMG"
echo "$OUT" | sed 's/^/  | /'
echo "$OUT" | grep -q "^  LOST:" \
    || { cat "$FIMG"; fail "leg 7: -f dropped keys and never said so -- an operator has no way to learn the repair cost data"; }
if echo "$OUT" | grep -q "^OK$"; then
    cat "$FIMG"
    fail "leg 7: -f reported the volume CLEAN on a pass it just lost keys on -- this is the WP86 failure, verbatim"
fi
echo "leg 7: -f named the loss and did not call the volume clean (exit $RC, verdict $(printf '%s\n' "$OUT" | tail -1))"

# --- leg 8: RED CONTROL -- the volume the old assertions call clean -------
# The strongest form of "this assertion could not fail": build the thing that
# defeats it and assert that it DOES defeat it, so the trap stays documented
# and stays true.
#
# This image holds the same three keys as the intact tree, in a valid
# single-leaf root, with every page CRC correct and gamma's value the wrong
# five bytes of the same length. On it:
#
#     invf-fsck          -> OK, exit 0, "base keys: 3", "bad pages: 0"
#     invf-verify --deep -> "0 files ok, 0 corrupt"
#
# Every assertion class used above -- exit code, verdict line, key COUNT, bad
# page count, and the deep verifier -- reports a healthy volume. That is the
# WP86 shape exactly: looks coherent, lost data. And it is not a corner case:
# `invfs_ast_block_entry` (src/core/invarifs.h:970-980) carries no content
# hash, only a `pba`, so a recipe that resolves to a valid-but-wrong segment
# returns the right number of wrong bytes and no checksum on this project can
# see it. Only comparing the bytes against a known source can.
#
# So leg 8 asserts BOTH halves: the tool under test really does call this
# volume clean (which is why the old legs could not detect the loss), and the
# second reader really does name the wrong bytes (which is why it can).
WIMG=metav3fsck-wrong.img
rm -f "$WIMG"
$B/invf-mkfs "$WIMG" 0.2 >/dev/null 2>&1 || fail "leg 8: mkfs failed"
craft_base "$WIMG" wrongvalue >"$WORK/craft8.log" 2>&1 \
    || { cat "$WORK/craft8.log"; fail "leg 8: craft the wrong-value tree failed"; }
cat "$WORK/craft8.log"
set +e
OUT=$($B/invf-fsck "$WIMG" 2>&1); RC=$?
set -e
echo "$OUT" | sed 's/^/  | /'
[ "$RC" -eq 0 ] \
    || fail "leg 8: invf-fsck no longer reports the wrong-value tree clean -- if that is a real fix, delete this leg and say why in the commit"
echo "$OUT" | grep -q "^OK$" \
    || fail "leg 8: the wrong-value volume is no longer reported OK; the trap this leg documents has closed -- re-derive what replaced it before deleting the leg"
echo "$OUT" | grep -q "base keys:    3" \
    || fail "leg 8: the wrong-value tree no longer reports three keys; the 'count what remains' trap has changed shape"
echo "  ... so the exit code, the verdict line, and the KEY COUNT all call this volume clean"
PROBE=$(tree_probe "$WIMG") || { echo "$PROBE"; fail "leg 8: tree_probe failed on the wrong-value tree"; }
echo "$PROBE" | sed 's/^/  | /'
echo "$PROBE" | grep -qE "^TREE KEYS=alpha=one beta=two gamma=threX$" \
    || { echo "$PROBE"; fail "leg 8: the second reader did NOT name the wrong bytes -- it has no teeth against the failure it was added for"; }
echo "$PROBE" | grep -qE "^TREE BADPAGES=0 CRCFAIL=0" \
    || { echo "$PROBE"; fail "leg 8: the wrong-value tree was supposed to be structurally perfect; if it is not, the leg above proves nothing"; }
# And the assertion legs 5 and 6 rely on must REJECT it: the value bytes are
# the difference between 'three' and 'threX', and WANT_ALL says 'three'.
if echo "$PROBE" | grep -q "^$WANT_ALL$"; then
    echo "$PROBE"
    fail "leg 8: the second reader accepted gamma=threX as gamma=three -- every leg above is vacuous"
fi
echo "leg 8: red control fired -- invf-fsck calls this volume clean, the second reader does not"

rm -f "$RIMG" "$DIMG" "$FIMG" "$WIMG"
echo "ALL WP-M4 FSCK LEGS PASS"
