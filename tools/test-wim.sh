#!/bin/bash
# test-wim.sh — WP637 wim containerpack (tools/codecpacks/wim.codecpack,
# algo 28) end-to-end.
#
# WHAT THE LANE IS. `sources/install.wim` is the bulk of every Windows
# install ISO and is entropy-coded (LZX/XPRESS per resource, LZMS in a solid
# .esd). Microsoft's encoder is closed and wimlib's stream differs, so a
# bit-exact rebuild by RE-ENCODING is unachievable; the pack therefore never
# decodes anything. It stores ONE VERBATIM MEMBER PER RESOURCE (compressed
# bytes as they lie in the file) and puts every byte outside those extents --
# the 208-byte header, the offset table, gaps with their junk, trailing junk
# -- in the recipe. Rebuild is copy-back in file order: bit-exact by
# construction, with no encoder in the loop.
#
# WHAT THIS SUITE PROVES, in order:
#   1. PACK-LEVEL round-trip (the pack is its own first arbiter) on every
#      accept fixture: enumerate -> extract/extract_batch -> strip -> rebuild
#      gives the file back byte-for-byte (cmp), the MRMP map is a strict
#      partition of [0, size) whose RECIPE src_offs land inside the recipe
#      blob, and estimate is over the file size.
#   2. the REFUSE LIST, each fixture at exit 3 exactly: bad magic, short
#      file, WRITE_IN_PROGRESS, a split-WIM part (part n of N>1), an unknown
#      version, a table stride that is not 50/62 (nor those + 8 trailing
#      bytes), a table past EOF, and a table naming no bytes.
#   3. the SYNTHETIC FS legs: mkfs -> cp -> sweep -> CONTAINER{28,1} stamp ->
#      members as "!mbrNNNN-<sname>" siblings + !mbrt + !mbrmap -> verify
#      --deep -> invf-cat bit-exact (container AND direct member reads) ->
#      pack-ABSENT reads (INVFS_CODECPACKS=/nonexistent) still bit-exact
#      through the map -> delete cascade drops every "!"-sibling -> fsck
#      clean, survivor bit-exact.
#   4. the DEDUPE leg (the reason this lane exists): two WIMs sharing one
#      identical resource at DIFFERENT file offsets. Three volumes give the
#      measurement: used(a), used(b), used(a+b); shared = used(a)+used(b) -
#      used(a+b) must be ~the resource, i.e. the resource is stored once
#      even though its offset moved by a non-segment multiple.
#   5. the DE-ALIASED solid leg: two table entries aliasing ONE extent (what
#      a solid .esd's resources look like) become ONE verbatim member, and
#      the round-trip stays bit-exact.
#   6. the SIZE-GUARD leg, which is a MEASURED finding, not an assumption:
#      a WIM whose resources are already-incompressible (the real LZX shape)
#      is priced by the engine's containerpack size guard at fixed +
#      Sigma_usize + 1280/member > original for every member count > 0, so
#      the guard declines it. This suite asserts the refusal is EXPLAINED
#      (the accounting line is on the record) and that the file is still
#      bit-exact generically -- never that it decomposes.
#   7. the BOUNDED-STUB mechanism, and then the CORPUS leg if a readable
#      install ISO is reachable: the real install.wim header and offset table
#      with every entry whose extent lies past the copied prefix zeroed (the
#      shape a split-WIM part has) prove the parser on real MS geometry --
#      real resource count, real table, real offsets, real compressed
#      payloads -- without 4 GB of staging. The mechanism is graded on a
#      fixture first, so it is exercised even on a host with no ISO; the
#      corpus half skips, loudly, when no readable ISO or no 7z is present.
#   8. the ABI leg: libwim.so is dlopen()ed and ASKED (desc vs the manifest,
#      estimate parity with the CLI, decline on noise) rather than graded by
#      `cc` alone. The other containerpacks get this from test-ivpacks.sh's
#      CPACKS loop; wim is not in that list, so this suite owes it.
#
# Run from the repo root after `make`:  bash tools/test-wim.sh
# No sudo, no loop mount, no /dev/shm requirement: every fixture is generated
# by the checked-in python writer below (the lane never re-encodes, so no MS
# encoder and no wimlib is involved anywhere in this file).
# The scratch tree is deleted on exit, like the sibling suites; set
# INVFS_WIM_KEEP=1 to keep it (the path is printed either way).
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
# WP205: one scratch-root answer for the whole suite set.
. "$REPO/tools/lib-scratch.sh"
B=$REPO/bin
PACK=$REPO/tools/codecpacks/wim.codecpack
WORK=${INVFS_WIM_WORK:-$(invfs_scratch_root)/wp637wim-$$}
IMG=wp637wim.img
IMGB=wp637wim-b.img
IMGA=wp637wim-a.img
IMGAB=wp637wim-ab.img
IMGS=wp637wim-s.img
INPROC=wp637wim-guard.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
# INVFS_TOOL_SCRATCH is deliberately NOT pinned here. The lane stages every
# member of the container in a tool scratch dir and the engine's own policy
# (src/core/tool_scratch.c, AGENTS.md 2.8b) sizes each candidate root for the
# demand + margin and migrates when the demand grows. Pinning it to $WORK
# makes the suite inherit whatever free space this host's $WORK root has, and
# on a box whose /tmp is nearly full that is a REFUSED scratch and a suite
# that fails for a reason unrelated to the lane (measured: /tmp at 41 MB free
# -> "tool_tmpdir: REFUSED ... no configured root can hold that").
# WP637: the scratch is NOT left behind by default. Sibling suites trap it
# (test-rawimg.sh:28, test-qcow2.sh:56, test-ext4fs.sh:52) and this one used
# to print "scratch left in" and keep every image, member and rebuilt copy:
# measured 105-348 MB per run (six runs on this box = 903 MB) on a host whose
# only scratch root was already full, which is how a suite in the plain `e2e`
# list starts failing other people's suites for reasons of its own. Pass
# INVFS_WIM_KEEP=1 to keep it for a post-mortem; the path is printed either
# way, so a kept run can always be found.
WIM_KEEP=${INVFS_WIM_KEEP:-0}
cleanup() {
    [ "$WIM_KEEP" = 1 ] && return 0
    [ -n "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT
rm -rf "$WORK" && mkdir -p "$WORK"/{orig,out,src,mbr,nopacks,py}
cd "$WORK"
rm -f "$IMG" "$IMGB" "$IMGA" "$IMGAB" "$IMGS" "$INPROC"

echo "== tools =="
command -v cc      >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
for t in invf-mkfs invf-cp invf-cat invf-ls invf-sweep invf-fsck invf-verify; do
    [ -x "$B/$t" ] || { echo "FAIL: $B/$t missing (run make first)"; exit 1; }
done
# build the helper BOTH ways: the CLI the engine execs and the ADR-007 .so
# (the .so must never drift from the CLI -- they are one source file)
mkdir -p "$PACK/bin"   # bin/ is gitignored: the dir is absent on a fresh tree
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/wim" "$PACK/wim.c" \
    || { echo "FAIL: the wim helper does not build as a CLI"; exit 1; }
cc -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared -DIVPACK_SHARED_LIB \
    -I"$REPO/src/include" -o "$PACK/libwim.so" "$PACK/wim.c" \
    || { echo "FAIL: the wim helper does not build as an ivpack .so"; exit 1; }
echo "  cc: CLI + libwim.so OK (one source, both builds)"
P=$PACK/bin/wim

# ---------------------------------------------------------------------------
# The checked-in WIM writer. There is no MS encoder in it and none is needed:
# the lane never re-encodes, so a fixture only has to be a well-formed
# (header, offset table, resources, gaps, junk) file with KNOWN byte streams.
# Layout produced (little-endian, MS-WIM / wimlib shape):
#   0x00 208-byte WIMHEADER, resource table right after it, then each
#   resource's extent with optional junk gaps between them, optional aliases
#   (two entries pointing at ONE extent = the solid-.esd shape), trailing junk.
# ---------------------------------------------------------------------------
cat > "$WORK/py/mkwim.py" <<'PY'
import hashlib, os, random, struct, sys

HDR = 208
ENT = 50            # SHA-1 entry: 8+8+8+20+2+4

def reshdr(cb, off, orig=0):
    """One 24-byte header slot: size:56 | flags:8, offset:56 | pad, then the
    (unused) original size. The 56/8 shape is the FORMAT, not a choice: a real
    WIM puts the flags byte in the top byte of the size word, so this has to be
    what both the fixture writer and the pack read or the two agree with each
    other while disagreeing with every WIM Microsoft ships."""
    return (cb.to_bytes(7, "little") + b"\0" +
            off.to_bytes(7, "little") + b"\0" +
            (orig or 0).to_bytes(7, "little") + b"\0")

def header(tbl_off, tbl_cb, xml_off, xml_cb, version=0x00010D00, flags=0,
           compsz=32768, part=1, parts=1, images=1, boot=(0, 0, 0), boot_idx=0,
           integ=(0, 0, 0)):
    h = bytearray(HDR)
    h[0:8] = b"MSWIM\0\0\0"
    struct.pack_into("<I", h, 0x08, HDR)
    struct.pack_into("<I", h, 0x0c, version)
    struct.pack_into("<I", h, 0x10, flags)
    struct.pack_into("<I", h, 0x14, compsz)
    h[0x18:0x28] = bytes(range(16))          # deterministic GUID
    struct.pack_into("<H", h, 0x28, part)
    struct.pack_into("<H", h, 0x2a, parts)
    struct.pack_into("<I", h, 0x2c, images)
    h[0x30:0x48] = reshdr(tbl_cb, tbl_off, tbl_cb)
    h[0x48:0x60] = reshdr(xml_cb, xml_off, xml_cb)
    h[0x60:0x78] = reshdr(*boot)
    struct.pack_into("<I", h, 0x78, boot_idx)
    h[0x7c:0x94] = reshdr(*integ)
    return bytes(h)

def junk(n, seed):
    return random.Random(seed).randbytes(n) if n else b""

def emit(path, items, xml_k=None, trailing=0, facts=None, **hdrkw):
    """items: [{'data':bytes, 'compressed':bool, 'gap':int, 'orig':int|None,
                'alias_of':int|None}]. Writes the WIM; facts gets one line per
    member extent (idx/off/len) plus the file size."""
    n = len(items)
    tbl_off, tbl_cb = HDR, n * ENT
    cur = tbl_off + tbl_cb
    exts = []                      # (off, len) per item, aliases share one
    for k, it in enumerate(items):
        g = it.get("gap", 0)
        cur += g
        if it.get("alias_of") is not None:
            o, l = exts[it["alias_of"]]
            exts.append((o, l))
            continue
        d = it["data"]
        exts.append((cur, len(d)))
        cur += len(d)
    size = cur + trailing
    xml = exts[xml_k] if xml_k is not None else (0, 0)
    out = bytearray(size)
    out[0:HDR] = header(tbl_off, tbl_cb, xml[0], xml[1], **hdrkw)
    cur = tbl_off + tbl_cb
    for k, it in enumerate(items):
        e = k * ENT + tbl_off
        o, l = exts[k]
        d = it["data"] if it.get("alias_of") is None else b""
        g = it.get("gap", 0)
        if g:
            out[cur:cur + g] = junk(g, 0x637 + k)
        cur += g
        if it.get("alias_of") is None:
            out[o:o + l] = d
            cur = o + l
        out[e:e + 7] = l.to_bytes(7, "little")            # size_in_wim:56
        out[e + 7] = 0x04 if it.get("compressed") else 0  # flags:8
        out[e + 8:e + 0x0f] = o.to_bytes(7, "little")     # offset_in_wim:56
        out[e + 0x0f:e + 0x0f + 20] = \
            hashlib.sha1(d or items[it["alias_of"]]["data"]).digest()
        struct.pack_into("<HI", out, e + 0x23, 1, 1)      # part, refcnt
    if trailing:
        out[size - trailing:size] = junk(trailing, 0x999)
    with open(path, "wb") as f:
        f.write(bytes(out))
    if facts:
        # member order is FILE ORDER (the pack's idx contract), and aliases
        # union, so recompute what the pack must announce: sort extents,
        # merge overlaps.
        ex = sorted(exts)
        mem, pos = [], 0
        for o, l in ex:
            if l == 0:
                continue
            if mem and o < mem[-1][0] + mem[-1][1]:
                mo, ml = mem[-1]
                mem[-1] = (mo, max(ml, o + l - mo))
            else:
                mem.append((o, l))
        with open(facts, "w") as f:
            f.write("size=%d\n" % size)
            f.write("entries=%d\n" % n)
            f.write("members=%d\n" % len(mem))
            for i, (o, l) in enumerate(mem):
                f.write("m%d_off=%d\nm%d_len=%d\n" % (i + 1, o, i + 1, l))
    return size

def text_blob(n):
    unit = b"the quick brown fox jumps over the lazy dog; InvariantFS WIM lane\n"
    return (unit * (n // len(unit) + 1))[:n]

def pattern_blob(n):
    """compressible, binary, and not a recognised binary family (no ELF/PE
    magic at offset 0), so the sweep leaves it to the generic floor -- which
    is where per-segment dedupe across two files can be measured."""
    unit = bytes(range(256))
    return (unit * (n // 256 + 1))[:n]

def rand_blob(n, seed):
    return random.Random(seed).randbytes(n)

def xml_blob(n):
    s = ('<?xml version="1.0"?><WIM><IMAGE INDEX="1"><NAME>fixture</NAME>'
         '</IMAGE></WIM>')
    return (s.encode("utf-16-le") * (n // len(s.encode("utf-16-le")) + 1))[:n]
PY
python3 -c "import sys; sys.path.insert(0,'$WORK/py'); import mkwim; print('  mkwim.py loads')" 2>/dev/null \
  || { python3 -c "import sys; sys.path.insert(0,'$PWD/py'); import mkwim; print('  mkwim.py loads')"; }

# ---------------------------------------------------------------------------
# The fixtures. Two accept WIMs share ONE 1 MiB resource byte-for-byte but at
# offsets whose position differs by a non-segment multiple (the dedupe leg);
# a solid one aliases a single extent from three table entries; one is
# incompressible (the real LZX shape) for the size-guard leg; the rest exist
# only to be refused.
# ---------------------------------------------------------------------------
echo "== fixtures =="
python3 - "$WORK" <<'PY'
import sys
W = sys.argv[1]
sys.path.insert(0, W + "/py")
from mkwim import (emit, text_blob, pattern_blob, rand_blob, xml_blob,
                   header, HDR, ENT)

O = W + "/orig"

def lowent(n, seed, alpha=16):
    """Compressible but NOT to nothing: uniform over 16 byte values, i.e. 4
    bits of entropy per byte (~0.55 zstd ratio). The guard prices members by
    their ZSTD-19 projection, so a near-zero fixture would be free and the
    dedupe measurement would be measuring nothing."""
    raw = rand_blob(n, seed)
    tab = bytes((i % alpha) * (256 // alpha) for i in range(256))
    return raw.translate(tab)

SHARED = lowent(1 << 20, 6371)          # identical in a.wim and b.wim

# a.wim: header+table, [text 1.5 MiB], 512 junk, [SHARED 1 MiB], [lowent 256K],
# [xml 8 KiB], 777 trailing junk. The compressed flag rides on two entries.
emit(O + "/a.wim", [
    {"data": text_blob(1536 * 1024), "compressed": True},
    {"data": SHARED, "gap": 512},
    {"data": lowent(256 * 1024, 6372)},
    {"data": xml_blob(8192), "compressed": True},
], xml_k=3, trailing=777, facts=O + "/a.facts")

# b.wim: different geometry, 128-byte gap, and SHARED sits at a DIFFERENT
# offset -- the whole point of the lane.
emit(O + "/b.wim", [
    {"data": lowent(700 * 1024, 6373)},
    {"data": SHARED, "gap": 128},
    {"data": text_blob(64 * 1024)},
    {"data": xml_blob(4096)},
], xml_k=3, trailing=101, facts=O + "/b.facts")

# solid.wim (dwVersion 0x00000E00): entries 2..4 alias ONE 2 MiB extent, the
# shape a solid .esd presents. The pack must union them into a single member.
emit(O + "/solid.wim", [
    {"data": text_blob(256 * 1024)},
    {"data": lowent(2 << 20, 6374)},
    {"alias_of": 1, "data": b""},
    {"alias_of": 1, "data": b""},
], xml_k=None, trailing=64, version=0x00000E00, facts=O + "/solid.facts")

# incomp.wim: already-incompressible resources (what MS LZX output looks like)
emit(O + "/incomp.wim", [
    {"data": rand_blob(1 << 20, 6375)},
    {"data": rand_blob(1 << 20, 6376), "gap": 256},
    {"data": rand_blob(1 << 20, 6377)},
    {"data": rand_blob(512 * 1024, 6378)},
    {"data": xml_blob(2048)},
], xml_k=4, trailing=0, facts=O + "/incomp.facts")

# ---- the refuse fixtures ----
def patch(path, off, data):
    with open(path, "r+b") as f:
        f.seek(off)
        f.write(data)

with open(O + "/badmagic.wim", "wb") as f:      # magic missing entirely
    f.write(b"PK\x03\x04" + rand_blob(4092, 11))
with open(O + "/a.wim", "rb") as f:             # shorter than the header
    pre = f.read(100)
with open(O + "/short.wim", "wb") as f:
    f.write(pre)
emit(O + "/wip.wim", [{"data": text_blob(64 * 1024)}], flags=0x40)
emit(O + "/split.wim", [{"data": text_blob(64 * 1024)}], part=1, parts=2)
emit(O + "/badver.wim", [{"data": text_blob(64 * 1024)}], version=0x00010900)
emit(O + "/badcb.wim", [{"data": text_blob(64 * 1024)}], compsz=40000)
emit(O + "/zerotbl.wim", [{"data": text_blob(64 * 1024)}])
patch(O + "/zerotbl.wim", HDR, b"\0" * ENT)     # a table naming no bytes
# a stride that is neither 50/62 nor those + 8 trailing bytes (13 stray)
emit(O + "/stride.wim", [{"data": text_blob(64 * 1024)},
                         {"data": text_blob(32 * 1024)}])
patch(O + "/stride.wim", 0x30, (2 * ENT + 13).to_bytes(8, "little"))
# ... and its CONTROL: the same table with the 8-byte compression-type tail,
# which the tolerant stride must accept.
emit(O + "/stride8.wim", [{"data": text_blob(64 * 1024)},
                          {"data": text_blob(32 * 1024)}], facts=O + "/stride8.facts")
patch(O + "/stride8.wim", 0x30, (2 * ENT + 8).to_bytes(8, "little"))
# an entry whose extent runs past EOF
emit(O + "/pastend.wim", [{"data": text_blob(64 * 1024)}], facts=O + "/pastend.facts")
sz = __import__("os").path.getsize(O + "/pastend.wim")
patch(O + "/pastend.wim", HDR + 8, (sz - 4096).to_bytes(8, "little"))
print("  wrote 4 accept + 8 refuse fixtures")
PY
for f in "$WORK"/orig/*.facts; do echo "  $(basename "$f"): $(tr '\n' ' ' < "$f" | cut -c1-96)..."; done

# ---------------------------------------------------------------------------
# The map checker. rebuild() proves the recipe; this proves the MAP, which is
# the blob the read path actually splices from -- if the two agreed by
# accident the FS would read the wrong bytes and no pack-level cmp would say
# so. It also re-derives the original from (recipe + members) through the map
# alone, so a map that partitions the file the wrong way cannot pass.
# ---------------------------------------------------------------------------
cat > "$WORK/py/checkmap.py" <<'PY'
import sys

MAP, WSIZE, RECIPE, MBRDIR, ORIG = sys.argv[1:6]
blob = open(MAP, "rb").read()
assert blob[:4] == b"MRMP", "map magic"
n = int.from_bytes(blob[4:8], "little")
assert len(blob) == 8 + n * 29, "map length %d for %d entries" % (len(blob), n)
recipe = open(RECIPE, "rb").read()
orig = open(ORIG, "rb").read()
wsize = int(WSIZE)
assert len(orig) == wsize, "original is not wim_size"
out = bytearray(wsize)
pos = 0
for i in range(n):
    e = blob[8 + i * 29: 8 + (i + 1) * 29]
    off = int.from_bytes(e[0:8], "little")
    ln = int.from_bytes(e[8:16], "little")
    kind = e[16]
    idx = int.from_bytes(e[17:21], "little")
    src = int.from_bytes(e[21:29], "little")
    assert ln > 0, "entry %d has zero length" % i
    assert off == pos, "entry %d starts at %d, want %d (not a partition)" % (i, off, pos)
    pos += ln
    if kind == 0:                       # RECIPE: bytes inside the recipe blob
        assert idx == 0, "RECIPE entry %d names member %d" % (i, idx)
        assert src + ln <= len(recipe), "entry %d src_off past recipe" % i
        out[off:off + ln] = recipe[src:src + ln]
    elif kind == 1:                     # MEMBER: the !mbrNNNN sibling
        assert src == 0, "MEMBER entry %d src_off %d" % (i, src)
        mem = open("%s/%d" % (MBRDIR, idx), "rb").read()
        assert len(mem) == ln, "member %d is %d B, map says %d" % (idx, len(mem), ln)
        out[off:off + ln] = mem
    else:
        raise AssertionError("entry %d kind %d" % (i, kind))
assert pos == wsize, "map covers %d of %d bytes" % (pos, wsize)
assert bytes(out) == orig, "map-derived rebuild differs from the original"
print("  map: %d entries, strict partition of %d B, recipe src_offs inside the blob,"
      " rebuild-through-the-map == original" % (n, wsize))
PY

echo "== pack-level: accept fixtures (enumerate/extract/strip/rebuild/map) =="
rt() {  # rt <file> <expected members>
    f="$WORK/orig/$1"; want=$2
    rm -rf "$WORK/mbr/$1" "$WORK/mbrb/$1" "$WORK/out/$1."*
    mkdir -p "$WORK/mbr/$1" "$WORK/mbrb/$1"
    "$P" enumerate "$f" "$WORK/out/$1.tbl" \
        || { echo "FAIL: enumerate $1"; exit 1; }
    got=$(wc -l < "$WORK/out/$1.tbl")
    [ "$got" = "$want" ] || { echo "FAIL: $1 -> $got members, want $want"
        cat "$WORK/out/$1.tbl"; exit 1; }
    while IFS=$'\t' read -r idx sname usize; do
        "$P" extract "$f" "$idx" "$WORK/mbr/$1/$idx" \
            || { echo "FAIL: extract $1 idx $idx"; exit 1; }
        sz=$(stat -c%s "$WORK/mbr/$1/$idx")
        [ "$sz" = "$usize" ] || { echo "FAIL: $1 idx $idx: $sz B, table says $usize"; exit 1; }
        [ "$sz" -gt 0 ] || { echo "FAIL: $1 idx $idx is empty"; exit 1; }
    done < "$WORK/out/$1.tbl"
    "$P" extract_batch "$f" "$WORK/out/$1.tbl" "$WORK/mbrb/$1" \
        || { echo "FAIL: extract_batch $1"; exit 1; }
    for m in "$WORK/mbr/$1"/*; do
        cmp -s "$m" "$WORK/mbrb/$1/$(basename "$m")" \
            || { echo "FAIL: extract_batch $1 disagrees on $(basename "$m")"; exit 1; }
    done
    "$P" strip "$f" "$WORK/out/$1.recipe" || { echo "FAIL: strip $1"; exit 1; }
    "$P" rebuild "$WORK/out/$1.recipe" "$WORK/mbr/$1" "$WORK/out/$1.rebuilt" \
        || { echo "FAIL: rebuild $1"; exit 1; }
    cmp -s "$f" "$WORK/out/$1.rebuilt" \
        || { echo "FAIL: $1 rebuild is not bit-exact"; exit 1; }
    "$P" map "$f" "$WORK/out/$1.map" || { echo "FAIL: map $1"; exit 1; }
    ws=$(stat -c%s "$f")
    python3 "$WORK/py/checkmap.py" "$WORK/out/$1.map" "$ws" \
        "$WORK/out/$1.recipe" "$WORK/mbr/$1" "$f" \
        || { echo "FAIL: $1 map check"; exit 1; }
    est=$("$P" estimate "$f") || { echo "FAIL: estimate $1"; exit 1; }
    [ "$est" -gt "$ws" ] || { echo "FAIL: $1 estimate $est <= size $ws"; exit 1; }
    echo "  $1: $got members, bit-exact, map ok, estimate $est"
}
rt a.wim 4
rt b.wim 4
rt solid.wim 2      # three table entries alias ONE extent -> one member
rt incomp.wim 5

# ---------------------------------------------------------------------------
# ABI leg: the .so does not merely BUILD, it ANSWERS -- and it answers what
# the CLI answers. Every other containerpack gets this from test-ivpacks.sh's
# CPACKS loop (desc vs manifest, CLI/plugin parity); wim is not in that list,
# so without this leg nothing on this host ever dlopen()s libwim.so and the
# ADR-007 artifact would be graded by `cc` alone. The glue fork-guards the
# call (ivpack_impl.h), so the numbers below come back through the child's
# captured stdout, which is exactly the path the engine's
# invfs_codec_pack_estimate() would take.
cat > "$WORK/py/soabi.py" <<'PY'
import ctypes, sys
so, path, want_name, want_class = sys.argv[1:5]

class Desc(ctypes.Structure):
    _fields_ = [("api_version", ctypes.c_uint32), ("name", ctypes.c_char_p),
                ("version", ctypes.c_char_p), ("pack_class", ctypes.c_char_p),
                ("flags", ctypes.c_uint32)]

class Est(ctypes.Structure):
    _fields_ = [("orig_size", ctypes.c_uint64), ("mbr_size", ctypes.c_uint64),
                ("recipe_size", ctypes.c_uint64), ("eligible", ctypes.c_int)]

lib = ctypes.CDLL(so)
lib.ivpack_get_desc.restype = ctypes.POINTER(Desc)
lib.ivpack_get_desc.argtypes = []
lib.ivpack_container_estimate.restype = ctypes.c_int
lib.ivpack_container_estimate.argtypes = [ctypes.c_char_p, ctypes.POINTER(Est)]

d = lib.ivpack_get_desc().contents
name, cls = d.name.decode(), d.pack_class.decode()
assert int(d.api_version), "desc api_version is 0"
assert name == want_name, "desc name %r != manifest %r" % (name, want_name)
assert cls == want_class, "desc class %r != manifest %r" % (cls, want_class)
est = Est()
rc = lib.ivpack_container_estimate(path.encode(), ctypes.byref(est))
print("  so: desc name=%s version=%s class=%s; estimate rc=%d orig=%d "
      "mbr=%d eligible=%d" % (name, d.version.decode(), cls, rc,
                               est.orig_size, est.mbr_size, est.eligible))
print("RC %d" % rc)
print("ORIG %d" % est.orig_size)
print("MBR %d" % est.mbr_size)
print("ELIG %d" % est.eligible)
PY

soleg() {  # soleg <file> <want rc> <want elig>
    out=$(python3 "$WORK/py/soabi.py" "$PACK/libwim.so" "$WORK/orig/$1" \
              wim containerpack) || { echo "FAIL: $1 .so leg"; exit 1; }
    echo "$out" | sed -n '1p'
    rc=$(echo "$out" | sed -n 's/^RC //p')
    elig=$(echo "$out" | sed -n 's/^ELIG //p')
    orig=$(echo "$out" | sed -n 's/^ORIG //p')
    ws=$(stat -c%s "$WORK/orig/$1")
    [ "$rc" = "$2" ] || { echo "FAIL: $1 .so rc $rc, want $2"; exit 1; }
    [ "$elig" = "$3" ] || { echo "FAIL: $1 .so eligible $elig, want $3"; exit 1; }
    if [ "$3" = 1 ]; then
        [ "$orig" = "$ws" ] || { echo "FAIL: $1 .so orig_size $orig != $ws"; exit 1; }
        mbr=$(echo "$out" | sed -n 's/^MBR //p')
        cli=$("$P" estimate "$WORK/orig/$1")
        [ "$mbr" = "$cli" ] \
            || { echo "FAIL: $1 estimate: .so $mbr != CLI $cli"; exit 1; }
    fi
}
echo "== ABI leg: libwim.so loads, answers, and agrees with the CLI =="
soleg a.wim 0 1                 # accept: the numbers must match the CLI byte for byte
soleg incomp.wim 0 1            # accept (the engine refuses it later, the pack does not)
head -c 65536 /dev/urandom > "$WORK/orig/junk.bin"
soleg junk.bin 3 0              # 64 KiB of noise is not a container: decline, not error

echo "== refuse list (exit 3 = not ours; the file stays RAW and waits) =="
refuse() {  # <file> <why>
    for cmd in enumerate strip; do
        set +e
        if [ "$cmd" = enumerate ]; then
            "$P" enumerate "$WORK/orig/$1" "$WORK/out/r.$1.tbl" 2>/dev/null
        else
            "$P" strip "$WORK/orig/$1" "$WORK/out/r.$1.recipe" 2>/dev/null
        fi
        rc=$?
        set -e
        [ "$rc" = 3 ] || { echo "FAIL: $1 $cmd -> exit $rc, want 3 ($2)"; exit 1; }
    done
    # a refusal must leave nothing behind that a later sweep could pick up
    [ ! -s "$WORK/out/r.$1.tbl" ] || { echo "FAIL: $1 refusal wrote a table"; exit 1; }
    [ ! -s "$WORK/out/r.$1.recipe" ] || { echo "FAIL: $1 refusal wrote a recipe"; exit 1; }
    echo "  $1: declined ($2)"
}
refuse badmagic.wim "no MSWIM magic"
refuse short.wim   "shorter than the 148-byte minimum header"
refuse wip.wim     "dwFlags & 0x40 (WRITE_IN_PROGRESS: a torn write)"
refuse split.wim   "part 1 of 2 (a split WIM carries only part of the set)"
refuse badver.wim  "unknown dwVersion"
refuse badcb.wim   "dwCompressionSize not a power of two"
refuse stride.wim  "table stride neither 50/62 nor those + 8 trailing bytes"
refuse pastend.wim "a resource extent runs past EOF"
refuse zerotbl.wim "the table names no bytes"
# ... and the tolerant-stride CONTROL: +8 trailing bytes must still parse.
"$P" enumerate "$WORK/orig/stride8.wim" "$WORK/out/stride8.tbl" \
    || { echo "FAIL: the 8-byte compression-type tail was not tolerated"; exit 1; }
[ "$(wc -l < "$WORK/out/stride8.tbl")" = 2 ] \
    || { echo "FAIL: stride8 member count"; cat "$WORK/out/stride8.tbl"; exit 1; }
echo "  stride8.wim: ACCEPTED (2 members; the +8 tail is tolerated)"
# extract on an unannounced idx is a hard error, never a decline
set +e
"$P" extract "$WORK/orig/a.wim" 99 "$WORK/out/nope" 2>/dev/null
rc=$?
set -e
[ "$rc" = 1 ] || { echo "FAIL: extract idx 99 -> exit $rc, want 1"; exit 1; }
echo "  extract on an unannounced idx: exit 1 (hard error)"

# ---------------------------------------------------------------------------
# The FS leg. Everything above grades the pack; from here the ENGINE has to
# accept the decomposition, and the read path has to give the bytes back.
# ---------------------------------------------------------------------------
export INVFS_CODECPACKS_SYS=0      # hermetic: repo packs only, no host packs
echo "== FS leg: mkfs + import + sweep =="
$B/invf-mkfs "$IMG" 0.5 >/dev/null
for f in a.wim b.wim; do $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null; done
$B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 || { cat "$WORK/sweep1.log"; exit 1; }
for f in a.wim b.wim; do
    grep -q "^  $f: wim (codecpack)" "$WORK/sweep1.log" \
        || { echo "FAIL: $f not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
done
grep "^  [ab].wim" "$WORK/sweep1.log"

echo "== sibling sets =="
$B/invf-ls "$IMG" > "$WORK/out/ls1"
for f in a.wim b.wim; do
    for s in mbrt mbrmap; do
        grep -q "$f!$s" "$WORK/out/ls1" || { echo "FAIL: $f!$s missing"; exit 1; }
    done
    n=$(grep -c "$f!mbr[0-9]\{4\}-" "$WORK/out/ls1")
    [ "$n" = 4 ] || { echo "FAIL: $f has $n data members, want 4"; grep "$f!" "$WORK/out/ls1"; exit 1; }
    # one DATA member per resource, one TABLE, one MAP
    tot=$(grep -c "$f!mbr" "$WORK/out/ls1")
    [ "$tot" = 6 ] || { echo "FAIL: $f has $tot !mbr siblings, want 6"; exit 1; }
done
grep "a.wim!mbr" "$WORK/out/ls1"
echo "each container: 4 members + !mbrt + !mbrmap"
# classof + unlink helpers (repo objects, the test-fatfs.sh pattern)
cat > "$WORK/classof.c" <<'C'
/* classof.c — print the WP10 storage-class stamp of one file. */
#include <stdio.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id;
    uint8_t cls, algo;
    uint16_t gen;

    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { vol_close(v); return 1; }
    if (vol_get_class(v, id, &cls, &algo, &gen) != 0)
        printf("none\n");
    else
        printf("cls=%u algo=%u gen=%u\n", cls, algo, gen);
    vol_close(v);
    return 0;
}
C
cat > "$WORK/cbrm.c" <<'C'
/* cbrm.c — unlink names through the volume API (no FUSE in the suite). */
#include <stdio.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0, rc = 0;
    invfs_volume *v;
    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    for (int i = 2; i < argc; i++)
        if (vol_unlink(v, argv[i]) != 0) { rc = 1; fprintf(stderr, "rm %s failed\n", argv[i]); }
    vol_flush(v);
    vol_close(v);
    return rc;
}
C
CORE_O="$(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt")"
for t in classof cbrm; do
    gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs \
        -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/$t" "$WORK/$t.c" \
        $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread \
        || { echo "FAIL: $t does not build"; exit 1; }
done

echo "== class stamps =="
for f in a.wim b.wim; do
    C=$("$WORK/classof" "$IMG" "$f")
    echo "  $f: $C"
    [ "$C" = "cls=3 algo=28 gen=1" ] \
        || { echo "FAIL: $f: want CONTAINER{28,1}"; exit 1; }
done
# the member siblings are the lane's, not the container's: a member rebuilt
# from a compressed resource must not carry the CONTAINER stamp.
MBR=$(awk -F'\t' 'NR==1 {printf "a.wim!mbr%04d-%s", $1, $2}' "$WORK/out/a.wim.tbl")
C=$("$WORK/classof" "$IMG" "$MBR")
echo "  $MBR: $C"
case "$C" in algo=28) echo "FAIL: a member carries the container's algo"; exit 1;; esac

echo "== verify --deep (reads every container through the map) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== bit-exact: whole container, and direct member reads =="
sha() { sha256sum "$1" | cut -d' ' -f1; }
for f in a.wim b.wim; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/cat.$f" >/dev/null
    A=$(sha "$WORK/orig/$f"); Bv=$(sha "$WORK/out/cat.$f")
    echo "  $f: $A"
    [ "$A" = "$Bv" ] || { echo "FAIL: $f read back differs"; exit 1; }
done
# the stored member table is the pack's own enumerate output, verbatim
$B/invf-cat "$IMG" "a.wim!mbrt" "$WORK/out/a.mbrt" >/dev/null
cmp -s "$WORK/out/a.wim.tbl" "$WORK/out/a.mbrt" \
    || { echo "FAIL: !mbrt is not the enumerate table"; exit 1; }
echo "  a.wim!mbrt == the pack's enumerate table"
# every member, read through its own inode, equals its slice of the original
. "$WORK/orig/a.facts"
i=1
while [ "$i" -le "$members" ]; do
    eval "off=\$m${i}_off; len=\$m${i}_len"
    sname=$(awk -F'\t' -v k="$i" '$1==k {print $2}' "$WORK/out/a.wim.tbl")
    mbr=$(printf "a.wim!mbr%04d-%s" "$i" "$sname")
    $B/invf-cat "$IMG" "$mbr" "$WORK/out/m$i" >/dev/null \
        || { echo "FAIL: read $mbr"; exit 1; }
    dd if="$WORK/orig/a.wim" of="$WORK/out/ref$i" bs=64k \
        iflag=skip_bytes,count_bytes skip="$off" count="$len" 2>/dev/null
    cmp -s "$WORK/out/ref$i" "$WORK/out/m$i" \
        || { echo "FAIL: member $i ($mbr) is not bytes [$off,+$len) of a.wim"; exit 1; }
    i=$((i + 1))
done
echo "  all $members members bit-exact against their extents"

echo "== pack-ABSENT reads (the map is self-describing) =="
# no pack can be exec'd here: the path does not exist. If the read path needed
# the pack, this leg fails -- which is the WP16b claim the map exists for.
NOP="$WORK/nopacks"
INVFS_CODECPACKS="$NOP" INVFS_CODECPACKS_SYS=0 \
    $B/invf-cat "$IMG" a.wim "$WORK/out/cat.nopack" >/dev/null \
    || { echo "FAIL: a.wim unreadable with the pack absent"; exit 1; }
cmp -s "$WORK/orig/a.wim" "$WORK/out/cat.nopack" \
    || { echo "FAIL: pack-absent read is not bit-exact"; exit 1; }
INVFS_CODECPACKS="$NOP" INVFS_CODECPACKS_SYS=0 \
    $B/invf-cat "$IMG" "a.wim!mbr0002-res0002" "$WORK/out/mbr.nopack" >/dev/null \
    || { echo "FAIL: member unreadable with the pack absent"; exit 1; }
cmp -s "$WORK/out/m2" "$WORK/out/mbr.nopack" \
    || { echo "FAIL: pack-absent member read differs"; exit 1; }
echo "  container + member reads bit-exact with no codecpack loadable"

echo '== delete cascade (unlink drops every "!" sibling) ==' 
"$WORK/cbrm" "$IMG" b.wim || { echo "FAIL: unlink b.wim"; exit 1; }
$B/invf-ls "$IMG" > "$WORK/out/ls2"
if grep -q "b\.wim!" "$WORK/out/ls2"; then
    echo "FAIL: b.wim! siblings survived the delete"; grep "b\.wim" "$WORK/out/ls2"; exit 1
fi
grep -q "a\.wim!mbrmap" "$WORK/out/ls2" || { echo "FAIL: a.wim lost its map"; exit 1; }
echo "  b.wim + its 6 siblings gone; a.wim untouched"

echo "== fsck + survivor =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }
$B/invf-cat "$IMG" a.wim "$WORK/out/surv" >/dev/null
cmp -s "$WORK/orig/a.wim" "$WORK/out/surv" \
    || { echo "FAIL: a.wim drifted after the delete"; exit 1; }
echo "a.wim bit-exact after the cascade; fsck OK"

# ---------------------------------------------------------------------------
# The dedupe leg -- the reason this lane exists. Two WIMs hold ONE identical
# 1 MiB resource at offsets that differ by a non-segment multiple (a.wim:
# 1573784, b.wim: 717336 -> 920 and 536 modulo 4096), so nothing
# offset-aligned can merge them; only the decomposition into one member per
# resource makes them the same bytes at the same address. Four volumes, each
# converged (see converge() below) before it is measured:
#   A  a.wim only          B  b.wim only        AB  both
#   S  the resource alone as a plain file (the yardstick: what the resource
#      costs when nothing is shared)
# shared = used(A) + used(B) - used(AB), in blocks, and it must be the cost
# of that one resource -- `used(S)`.
# ---------------------------------------------------------------------------
free_blocks() { $B/invf-fsck "$1" 2>/dev/null | awk '/^  free blocks:/ {print $3}'; }
converge() {  # converge <img> <tag> -> the converged free-block count on stdout
    local prev cur d stable=0 i
    prev=$(free_blocks "$1")
    [ -n "$prev" ] || { echo "FAIL: no free-block count for $1" >&2; exit 1; }
    stable=0
    for i in 1 2 3 4 5 6 7 8; do
        $B/invf-sweep "$1" > "$WORK/dedupe-$2-s$i.log" 2>&1 \
            || { cat "$WORK/dedupe-$2-s$i.log" >&2; exit 1; }
        cur=$(free_blocks "$1")
        d=$((cur > prev ? cur - prev : prev - cur))
        prev=$cur
        if [ "$d" -le 8 ]; then
            stable=$((stable + 1))
            [ "$stable" -ge 2 ] && break
        else
            stable=0
        fi
    done
    [ "$stable" -ge 2 ] \
        || { echo "FAIL: $2 free blocks did not converge in 8 sweeps" >&2; exit 1; }
    echo "$cur"
}

echo "== dedupe leg: one resource, three volumes + a yardstick =="
. "$WORK/orig/a.facts"
SHARED_OFF=$m2_off
SHARED_LEN=$m2_len
dd if="$WORK/orig/a.wim" of="$WORK/orig/shared.bin" bs=64k \
    iflag=skip_bytes,count_bytes skip="$SHARED_OFF" count="$SHARED_LEN" 2>/dev/null
[ "$(stat -c%s "$WORK/orig/shared.bin")" = "$SHARED_LEN" ] \
    || { echo "FAIL: shared.bin slice"; exit 1; }
. "$WORK/orig/b.facts"
SHARED_OFF_B=$m2_off
echo "  shared resource: $SHARED_LEN B at a.wim/$SHARED_OFF, b.wim/$SHARED_OFF_B"
echo "  (mod 4096: $((SHARED_OFF % 4096)) vs $((SHARED_OFF_B % 4096)) -- both" \
     "misaligned, and by different amounts)"
[ "$((SHARED_OFF % 4096))" != "$((SHARED_OFF_B % 4096))" ] \
    || { echo "FAIL: the two extents are segment-aligned after all"; exit 1; }

for spec in "$IMGA:a.wim:a" "$IMGB:b.wim:b" "$IMGAB:a.wim+b.wim:ab" "$IMGS:shared.bin:s"; do
    img=${spec%%:*}; rest=${spec#*:}; files=${rest%%:*}; tag=${rest##*:}
    rm -f "$img"
    $B/invf-mkfs "$img" 0.5 >/dev/null
    # the empty-volume free count IS the baseline the four "used" numbers are
    # measured from; the four volumes must agree on it (same geometry) or the
    # arithmetic below is comparing different disks.
    E=$(free_blocks "$img")
    [ -n "$E" ] || { echo "FAIL: no free count on an empty $img"; exit 1; }
    if [ -z "${F0:-}" ]; then F0=$E; fi
    [ "$E" = "$F0" ] || { echo "FAIL: $img geometry differs ($E vs $F0)"; exit 1; }
    for f in $(echo "$files" | tr '+' ' '); do
        $B/invf-cp "$img" "$WORK/orig/$f" "$f" >/dev/null
    done
    F=$(converge "$img" "$tag")
    eval "FREE_$tag=$F"
    echo "  $tag ($files): free $F after convergence"
done
for tag in a b ab s; do eval "f=\$FREE_$tag"; [ -n "$f" ] || { echo "FAIL: free_$tag empty"; exit 1; }; done
echo "  empty-volume baseline: $F0 free blocks"
USED_A=$((F0 - FREE_a))
USED_B=$((F0 - FREE_b))
USED_AB=$((F0 - FREE_ab))
USED_S=$((F0 - FREE_s))
SHARED=$((USED_A + USED_B - USED_AB))
echo "  used: A $USED_A, B $USED_B, A+B $USED_AB, resource alone $USED_S (blocks)"
echo "  shared by the two WIMs: $SHARED blocks = $((SHARED * 4096)) B" \
     "of the $SHARED_LEN B resource"
# The dedupe stage must be the thing that merged them, cross-file: read it off
# the AB convergence logs rather than inferring it from the block count.
CROSS=0
MERGED=0
for l in "$WORK"/dedupe-ab-s*.log; do
    # the stage line reads "dedupe ... cross=17 intra=0 merged=17 freed=..";
    # the older summary shape is "dedupe: merged 17 segments ... (cross 17,
    # intra 0)" -- normalise both rather than pinning one build's wording.
    line=$(grep -E "cross=[0-9]+" "$l" 2>/dev/null | tail -1)
    if [ -z "$line" ]; then
        line=$(grep "^dedupe: merged" "$l" 2>/dev/null | tail -1)
        M=$(sed -n 's/^dedupe: merged \([0-9]*\) segments.*/\1/p' <<<"$line")
        X=$(sed -n 's/.*cross \([0-9]*\).*/\1/p' <<<"$line")
    else
        M=$(sed -n 's/.*merged=\([0-9]*\).*/\1/p' <<<"$line")
        X=$(sed -n 's/.*cross=\([0-9]*\).*/\1/p' <<<"$line")
    fi
    [ -n "$M" ] && [ -n "$X" ] || continue
    MERGED=$((MERGED + M))
    CROSS=$((CROSS + X))
done
echo "  AB sweeps: merged=$MERGED cross-file=$CROSS segments"
[ "$CROSS" -ge 1 ] || { echo "FAIL: no cross-file dedupe happened on A+B"; exit 1; }
[ "$USED_S" -gt 0 ] && [ "$SHARED" -gt 0 ] \
    || { echo "FAIL: shared=$SHARED usedS=$USED_S (nothing to compare)"; exit 1; }
[ "$((SHARED * 100))" -ge "$((USED_S * 85))" ] \
    || { echo "FAIL: shared $SHARED blocks < 85% of the resource's own cost $USED_S"; exit 1; }
[ "$((SHARED * 100))" -le "$((USED_S * 120))" ] \
    || { echo "FAIL: shared $SHARED blocks > 120% of the resource's own cost $USED_S"; exit 1; }
echo "the resource is stored ONCE across the two WIMs ($SHARED blocks vs $USED_S alone)"

# ---------------------------------------------------------------------------
# The size-guard leg. This is a MEASURED finding, not an assumption: the
# engine prices a containerpack decomposition at fixed + Sigma(member
# ZSTD-19 projection) + per-member metadata, and refuses when that is not a
# gain. A WIM's resources are ALREADY entropy-coded (LZX/XPRESS/LZMS), so
# their projections are their own sizes and the price is the file plus
# overhead -- the guard refuses every incompressible WIM, however many
# resources it has. What this leg asserts is that the refusal is on the
# record with its numbers, and that the file is still bit-exact through the
# generic floor.
# ---------------------------------------------------------------------------
echo "== size-guard leg: a WIM of already-incompressible resources =="
$B/invf-mkfs "$INPROC" 0.5 >/dev/null
$B/invf-cp "$INPROC" "$WORK/orig/incomp.wim" "incomp.wim" >/dev/null
$B/invf-sweep "$INPROC" > "$WORK/guard.log" 2>&1 \
    || { cat "$WORK/guard.log"; exit 1; }
if grep -q "incomp.wim: wim (codecpack)" "$WORK/guard.log"; then
    echo "FAIL: an incompressible WIM was decomposed (the guard did not fire)"
    cat "$WORK/guard.log"; exit 1
fi
grep "incomp.wim.*size guard refused" "$WORK/guard.log" \
    || { echo "FAIL: the refusal is not on the record"; cat "$WORK/guard.log"; exit 1; }
$B/invf-ls "$INPROC" > "$WORK/out/lsguard"
if grep -q "incomp\.wim!" "$WORK/out/lsguard"; then
    echo "FAIL: a refused WIM gained siblings"; cat "$WORK/out/lsguard"; exit 1
fi
C=$("$WORK/classof" "$INPROC" incomp.wim)
echo "  incomp.wim: $C (refused, so no CONTAINER{28} stamp)"
case "$C" in *algo=28*) echo "FAIL: refused WIM carries the pack stamp"; exit 1;; esac
$B/invf-cat "$INPROC" incomp.wim "$WORK/out/incomp.cat" >/dev/null
cmp -s "$WORK/orig/incomp.wim" "$WORK/out/incomp.cat" \
    || { echo "FAIL: refused WIM is not bit-exact generically"; exit 1; }
echo "  bit-exact through the generic floor; refusal explained on the record"

# ---------------------------------------------------------------------------
# The bounded-stub mechanism, then real geometry.
#
# A real `sources/install.wim` is 4-6 GB of LZX-compressed resources, so no
# suite stages it. What IS portable is its GEOMETRY: a WIM header plus the
# offset table, with the entries whose extents lie past the copied prefix
# zeroed. That is a legal WIM shape -- `size_in_wim == 0` means "this resource
# is not in this file", which is exactly what a split-WIM part looks like, and
# the pack skips those entries by design (wim.c plan_build). The XML, boot and
# integrity reshdrs point past the truncation, so they are cleared (a WIM with
# no XML data is legal; a WIM whose XML runs past EOF is a refuse).
#
# The mechanism is exercised FIRST on a fixture (a.wim cut mid-extent), so the
# stub builder is graded even on a host with no ISO; the real-Microsoft leg
# then runs the same builder over the real table when one is readable.
# ---------------------------------------------------------------------------
cat > "$WORK/py/mkstub.py" <<'PY'
import struct, sys
prefix, stub, expect = sys.argv[1:4]
b = bytearray(open(prefix, "rb").read())
assert b[:8] == b"MSWIM\0\0\0", "not a WIM"
hdrlen = struct.unpack_from("<I", b, 0x08)[0]
ver = struct.unpack_from("<I", b, 0x0c)[0]
tbl_cb = struct.unpack_from("<Q", b, 0x30)[0] & 0x00FFFFFFFFFFFFFF  # size:56
tbl_off = struct.unpack_from("<Q", b, 0x38)[0] & 0x00FFFFFFFFFFFFFF  # offset:56
ent = 50 if tbl_cb % 50 == 0 else 62
n = tbl_cb // ent
assert tbl_off + tbl_cb <= len(b), "the whole offset table is not inside the prefix"
for off in (0x48, 0x60, 0x7c):        # xml / boot / integrity: past the cut
    struct.pack_into("<QQQ", b, off, 0, 0, 0)
kept, end = 0, tbl_off + tbl_cb
for i in range(n):
    e = tbl_off + i * ent
    ln = struct.unpack_from("<Q", b, e)[0] & 0x00FFFFFFFFFFFFFF
    off = struct.unpack_from("<Q", b, e + 8)[0] & 0x00FFFFFFFFFFFFFF
    if ln and off >= hdrlen and off + ln <= len(b):
        kept += 1
        end = max(end, off + ln)
    else:
        b[e:e + ent] = b"\0" * ent
size = (end + 4095) & ~4095
del b[size:]
b += b"\0" * (size - len(b))
open(stub, "wb").write(bytes(b))
open(expect, "w").write("%d %d %d %d\n" % (n, kept, size, ver))
PY
stub_leg() {  # stub_leg <prefix> <tag> <min-kept>
    local pre=$1 tag=$2 min=$3 ex stub ntot nkept nsize nver nmbr rs
    ex="$WORK/corpus/$tag.expect"; stub="$WORK/corpus/$tag.wim"
    python3 "$WORK/py/mkstub.py" "$pre" "$stub" "$ex" \
        || { echo "FAIL: $tag: the stub builder"; exit 1; }
    read -r ntot nkept nsize nver < "$ex"
    echo "  $tag: table $ntot entries, dwVersion 0x$(printf %08x "$nver")," \
         "kept $nkept inside $nsize B"
    [ "$nkept" -ge "$min" ] \
        || { echo "FAIL: $tag: only $nkept resources inside the prefix (want $min)"; exit 1; }
    "$P" enumerate "$stub" "$WORK/corpus/$tag.tbl" \
        || { echo "FAIL: $tag: enumerate"; exit 1; }
    nmbr=$(wc -l < "$WORK/corpus/$tag.tbl")
    echo "  $tag: $nmbr members (one per resource extent; aliases would union)"
    [ "$nmbr" = "$nkept" ] \
        || { echo "FAIL: $tag: $nmbr members for $nkept kept resources"; exit 1; }
    rm -rf "$WORK/corpus/$tag.mbr"; mkdir -p "$WORK/corpus/$tag.mbr"
    "$P" extract_batch "$stub" "$WORK/corpus/$tag.tbl" "$WORK/corpus/$tag.mbr" \
        || { echo "FAIL: $tag: extract_batch"; exit 1; }
    "$P" strip "$stub" "$WORK/corpus/$tag.recipe" \
        || { echo "FAIL: $tag: strip"; exit 1; }
    "$P" rebuild "$WORK/corpus/$tag.recipe" "$WORK/corpus/$tag.mbr" \
        "$WORK/corpus/$tag.rebuilt" || { echo "FAIL: $tag: rebuild"; exit 1; }
    cmp -s "$stub" "$WORK/corpus/$tag.rebuilt" \
        || { echo "FAIL: $tag: rebuild is not bit-exact"; exit 1; }
    "$P" map "$stub" "$WORK/corpus/$tag.map" || { echo "FAIL: $tag: map"; exit 1; }
    python3 "$WORK/py/checkmap.py" "$WORK/corpus/$tag.map" "$nsize" \
        "$WORK/corpus/$tag.recipe" "$WORK/corpus/$tag.mbr" "$stub" \
        || { echo "FAIL: $tag: map check"; exit 1; }
    rs=$(stat -c%s "$WORK/corpus/$tag.recipe")
    echo "  $tag: bit-exact; recipe $rs B; the map partitions $nsize B"
}

echo "== bounded-stub mechanism (a fixture cut mid-extent) =="
mkdir -p "$WORK/corpus"
head -c 2097152 "$WORK/orig/a.wim" > "$WORK/corpus/a.prefix"
stub_leg "$WORK/corpus/a.prefix" fixture 1
# the cut really did drop the tail: the second resource ends past 2 MiB
[ "$(stat -c%s "$WORK/corpus/fixture.wim")" -le 2097152 ] \
    || { echo "FAIL: the fixture stub is bigger than its prefix"; exit 1; }
echo "  (entry 1 fits the prefix and stays; entry 2's extent ends past it and"
echo "   is zeroed, which is what a split-WIM part looks like)"

echo "== corpus leg: a real Microsoft WIM, graded whole =="
# A bounded PREFIX of a real WIM is not gradeable, and that is a property of
# the format, not of the fixture: the offset table sits AFTER every resource
# extent, so the first 32 MiB of a 500 MB boot.wim contains a header and
# compressed resources and no table at all -- mkstub.py refuses it by name
# ("the whole offset table is not inside the prefix"). The truncated-stub
# shape is still graded, on a fixture, just above. What this leg owes is the
# other half: Microsoft's own header, its own table stride and its own
# compressed resources, complete. So it takes a WHOLE real .wim, smallest
# candidate first, bounded by a size cap so a 5.8 GB install.wim never lands
# on a test host's disk (demand is ~4.3x the WIM: source, stub, rebuilt, and
# every member extracted), and it skips with the numbers if the disk cannot
# hold it rather than dying as an opaque ENOSPC (AGENTS.md 2.8b).
CAP_MB=${INVFS_WIM_CORPUS_MAX_MB:-320}
CAP=$(( CAP_MB * 1048576 ))
ISO=""; WMEMBER=""; WSIZE=0
if [ -n "${INVFS_WIM_ISO:-}" ] && [ -f "${INVFS_WIM_ISO}" ]; then
    ISO=$INVFS_WIM_ISO
    WMEMBER=${INVFS_WIM_MEMBER:-sources/install.wim}
    set +o pipefail
    WSIZE=$(7z l -slt "$ISO" "$WMEMBER" 2>/dev/null \
            | sed -n 's/^Size = //p' | head -1) || WSIZE=""
    set -o pipefail
    case "$WSIZE" in ''|*[!0-9]*) WSIZE=0 ;; esac
else
    # Every .wim on every ISO, SORTED BY SIZE, then the first whose payload
    # actually reads as MSWIM. Two facts learned on this box: a partially
    # downloaded ISO lists its directory perfectly and yields zeroes for the
    # payload (so a listing is not evidence), and 7z exits 2 on an unreadable
    # archive and 141 when head closes the pipe (so every probe here runs with
    # pipefail OFF and cannot take the suite down through `set -e`).
    CANDS=$(
        for c in /mnt/invfs-scratch/iso/*/*.iso; do
            [ -f "$c" ] || continue
            set +o pipefail
            7z l -slt "$c" 2>/dev/null | awk -v f="$c" '
                /^Path = /{ p = substr($0, 8) }
                /^Size = /{ if (p ~ /\.wim$/ && $3 + 0 >= 4194304)
                                printf "%d\t%s\t%s\n", $3 + 0, f, p }'
            set -o pipefail
        done | sort -n | head -6 ) || CANDS=""
    while IFS=$'\t' read -r sz f mem; do
        [ -n "$mem" ] || continue
        [ "$sz" -le "$CAP" ] || continue
        set +o pipefail
        mg=$(7z x -so "$f" "$mem" 2>/dev/null \
             | head -c 8 | od -An -c | tr -d ' \n') || mg=""
        set -o pipefail
        [ "$mg" = 'MSWIM\0\0\0' ] || continue
        ISO=$f; WMEMBER=$mem; WSIZE=$sz
        break
    done <<< "$CANDS"
fi
mkdir -p "$WORK/corpus"
if [ -z "$ISO" ]; then
    echo "  SKIP: no real .wim at or under ${CAP_MB} MB on this host's ISOs"
    echo "  (set INVFS_WIM_ISO + INVFS_WIM_MEMBER, or raise"
    echo "  INVFS_WIM_CORPUS_MAX_MB). Every ISO was listed, smallest .wim"
    echo "  first, and a candidate also had to READ as MSWIM, not merely"
    echo "  list. The stub MECHANISM above ran on a fixture; what is missing"
    echo "  here is only Microsoft's own table, not coverage of it."
else
    echo "  ISO: $(basename "$ISO" | cut -c1-52)"
    echo "  member: $WMEMBER ($WSIZE B)"
    AVAIL=$(df -B1 --output=avail "$WORK" | tail -1)
    NEED=$(( WSIZE * 43 / 10 + 67108864 ))
    if [ "$AVAIL" -lt "$NEED" ]; then
        echo "  SKIP: it does not fit. This leg needs ~$NEED B ($((WSIZE/1048576)) MB"
        echo "  WIM x 4.3 + 64 MB margin), $WORK's filesystem has $AVAIL B."
        echo "  Remedy: set INVFS_WIM_WORK to a roomier root, or lower"
        echo "  INVFS_WIM_CORPUS_MAX_MB. Nothing was extracted."
    else
        set +e; set +o pipefail
        7z x -so "$ISO" "$WMEMBER" > "$WORK/corpus/real.src" 2>/dev/null
        set -e; set -o pipefail
        PL=$(stat -c%s "$WORK/corpus/real.src")
        MG=$(head -c 8 "$WORK/corpus/real.src" | od -An -c | tr -d ' \n')
        if [ "$MG" != 'MSWIM\0\0\0' ] || [ "$PL" -lt 4194304 ]; then
            echo "  SKIP: the payload is not readable ($PL B, magic '$MG')"
            echo "  -- a partial download; 7z lists the ISO's directory and"
            echo "  fails on the payload. Nothing was graded."
        else
            stub_leg "$WORK/corpus/real.src" real 1
            read -r ntot nkept rest < "$WORK/corpus/real.expect"
            if [ "$nkept" = "$ntot" ]; then
                echo "  real: all $ntot resources are inside the file -- the"
                echo "  whole WIM, not a prefix, so the table and every extent"
                echo "  Microsoft wrote were graded"
            else
                echo "  NOTE: $((ntot - nkept)) of $ntot resources lie past the"
                echo "  bytes 7z produced (short read); the stub kept the rest"
            fi
            RSR=$(stat -c%s "$WORK/corpus/real.recipe")
            printf "  real       original %9d  recipe %9d (%5.2f%%)\n" \
                "$PL" "$RSR" "$(python3 -c "print(100.0*$RSR/$PL)")"
            rm -rf "$WORK/corpus/real.mbr" "$WORK/corpus/real.rebuilt" \
                   "$WORK/corpus/real.src" "$WORK/corpus/real.wim"
            echo "  (the extracted members and the rebuilt copy are deleted"
            echo "   right here: they are 3x the WIM and the next leg is small)"
        fi
    fi
fi

# ---------------------------------------------------------------------------
# What the lane costs and what it buys, on the fixtures at hand.
# ---------------------------------------------------------------------------
echo "== ratio =="
for f in a.wim b.wim solid.wim; do
    osz=$(stat -c%s "$WORK/orig/$f")
    rsz=$(stat -c%s "$WORK/out/$f.recipe")
    msz=$(awk -F'\t' '{s += $3} END {print s}' "$WORK/out/$f.tbl")
    printf "  %-10s original %9d  recipe %9d (%5.1f%%)  member bytes %9d\n" \
        "$f" "$osz" "$rsz" \
        "$(python3 -c "print(100.0*$rsz/$osz)")" "$msz"
done
echo "  (the recipe is only the bytes OUTSIDE the resources: header, table,"
echo "   gaps, trailing junk. It grows with the RESOURCE COUNT, not the file"
echo "   size, which is why a 4 GB install.wim pays ~5 MB of it.)"

if [ "$WIM_KEEP" = 1 ]; then
    echo "  scratch kept (INVFS_WIM_KEEP=1): $WORK"
else
    echo "  scratch removed ($WORK); INVFS_WIM_KEEP=1 keeps it"
fi
echo "WIM E2E: PASS"
