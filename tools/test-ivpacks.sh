#!/bin/bash
# test-ivpacks.sh — ADR-007 plugin packaging end-to-end: every C containerpack
# as BOTH a CLI helper and a lib<name>.so plugin, plus the .ivpack bundles.
#
#   what it proves, per pack:
#     [desc]   lib<name>.so loads through dlmopen(LM_ID_NEWLM) (dlopen
#              fallback), exports the three ABI symbols, and its descriptor
#              agrees with the pack's own manifest (name / pack_class /
#              api_version >= IVPACK_API_VERSION_MIN).
#     [decl]   plugin estimate parity on a DECLINED input: the .so must return
#              the very status the CLI exits with (3), not 0 and not a crash.
#              This is the leg the fork guard in ivpack_impl.h exists for --
#              every containerpack declines with a bare exit(3), which inside a
#              long-lived worker would otherwise kill the worker.
#     [junk]   64 KiB of random bytes: decline, and CLI/so agree.
#     [rt]     for the packs whose fixture tooling is installed: enumerate ->
#              extract EVERY member -> strip -> rebuild -> cmp against the
#              original image, bit-exact, all through the plugin ABI; then the
#              same five calls through the CLI, and the two must agree byte for
#              byte (table / recipe / map / rebuilt image). This is the leg that
#              pins the operand mapping (which of in/idx/out/recipe/dir each
#              command consumes) -- a swapped argument shows up as a failed
#              rebuild, not as a vague error.
#     [ivpack] tools/pack-ivpack.sh output is an uncompressed ZIP-0 holding
#              manifest + sha256 + lib/<name>.so + bin/<name>, every listed
#              checksum verifies, and the .so unpacked from the archive still
#              loads and answers.
#
#   fixtures: qcow2 (qemu-img/qemu-io), vdi (qemu-img convert), ext4fs
#     (mkfs.ext4 + debugfs), rawdisk (inline python GPT builder). A pack whose
#     fixture tooling is missing (fatfs: mkfs.vfat/mtools, ntfs: mkfs.ntfs +
#     sudo loop mount, xfs: mkfs.xfs + sudo loop mount, p7z: 7zz) degrades to
#     the desc/decl/junk legs and is reported as SKIP, never FAIL.
#
# Run from the repo root after `make`:  bash tools/test-ivpacks.sh
# Scratch lives in $WORK (default /dev/shm/ivpacks, override for a small tmpfs).
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
B=$REPO/bin
WORK="${WORK:-/dev/shm/ivpacks}"
PACKROOT=$REPO/tools/codecpacks
CPACKS="qcow2 ext4fs fatfs ntfs rawdisk vdi xfs p7z"

trap 'rm -rf "$WORK"' EXIT
rm -rf "$WORK"
mkdir -p "$WORK/orig" "$WORK/out" "$WORK/bin" "$WORK/packs" "$WORK/unz"
cd "$WORK"

fails=0
skips=0
note() { printf '  %s\n' "$*"; }
ok()   { printf '  ok    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*"; fails=$((fails + 1)); }
skip() { printf '  SKIP  %s\n' "$*"; skips=$((skips + 1)); }

echo "== tools =="
for t in cc python3 sha256sum unzip zip; do
    command -v "$t" >/dev/null || { echo "FAIL: $t not installed"; exit 1; }
done
HAVE_QEMU=0; command -v qemu-img  >/dev/null && HAVE_QEMU=1
HAVE_E2FS=0; command -v mkfs.ext4 >/dev/null && command -v debugfs >/dev/null && HAVE_E2FS=1
note "qemu-img: $([ $HAVE_QEMU = 1 ] && echo yes || echo 'no (qcow2+vdi fixtures skipped)')"
note "mkfs.ext4+debugfs: $([ $HAVE_E2FS = 1 ] && echo yes || echo 'no (ext4fs fixture skipped)')"

echo "== build: CLI helpers + plugin .so (the -Wall -Wextra -Werror gate) =="
make -C "$REPO" -s plugin-so >/dev/null
# qcow2 is the one pack whose CLI needs more than libc + system zlib: since
# Q2R3 its cmd_strip/cmd_rebuild call invfs_deflate_repro_*, so the CLI must
# link the repro core, both deflate backends and the bundled stock zlib they
# were compiled against. Ask the Makefile for that list (PLUGIN_EXTRA_qcow2)
# rather than hand-maintaining it -- a new CORE member or a new src/zlib/*.c
# used to be able to leave this harness silently under-linked.
DEFLATE_OBJ=$(make -s -C "$REPO" print-obj-qcow2 || true)
# make emits REPO-relative objects and this script has cd'd to $WORK, so anchor
# them (the same `sed "s|^|$REPO/|"` every other tools/test-*.sh uses on
# build/core_objs.txt). An empty list must stay empty here -- prefixing it
# would turn "nothing to link" into one bogus path.
[ -z "${DEFLATE_OBJ//[[:space:]]/}" ] || \
    DEFLATE_OBJ=$(printf '%s\n' $DEFLATE_OBJ | sed "s|^|$REPO/|")
check_deflate_objs() {
    # 1. the Makefile must have handed us a non-empty list
    set -- $DEFLATE_OBJ
    [ $# -gt 0 ] || { echo "    (make print-obj-qcow2 returned nothing)"; return 1; }
    # 2. every object it named must exist (run `make` first)
    for o in "$@"; do
        [ -f "$o" ] || { echo "    (missing $o -- run make first)"; return 1; }
    done
    # 3. the stock-zlib half must cover EVERY src/zlib/*.c. This is the check
    #    that turns "the list silently lost its stock objects" into a loud
    #    failure: a wildcard that expanded to nothing used to produce a binary
    #    whose stock backend resolved against the system zlib instead.
    #    (`|| true` on both: the script runs under `set -o pipefail`, so a
    #    non-matching glob would otherwise abort the suite before it could
    #    report WHICH pack lost its objects.)
    ns=$(ls -1 "$REPO"/src/zlib/*.c 2>/dev/null | wc -l || true)
    [ "$ns" -gt 0 ] || { echo "    (no src/zlib/*.c: stock backend cannot link)"; return 1; }
    no=$(printf '%s\n' "$@" | grep -c '/zlib_stock_' || true)
    [ "$no" = "$ns" ] || {
        echo "    (stock zlib objects incomplete: $no linked, $ns sources)"; return 1; }
    return 0
}
for p in $CPACKS; do
    d=$PACKROOT/$p.codecpack
    mkdir -p "$d/bin"
    EXTRA_SRC=""; EXTRA_OBJ=""
    if [ "$p" = qcow2 ]; then
        EXTRA_SRC="-I$REPO/src/codecs -I$REPO/src/zlib -DZ_PREFIX"
        check_deflate_objs || { bad "$p: deflate core link set unusable"; continue; }
        EXTRA_OBJ="$DEFLATE_OBJ"
    fi
    # EXTRA_SRC/EXTRA_OBJ are deliberately unquoted: a flag list / an object
    # list, both whitespace-separated. shellcheck disable=SC2086
    cc -std=c11 -O2 -Wall -Wextra -Werror $EXTRA_SRC -o "$d/bin/$p" \
       "$d/$p.c" $EXTRA_OBJ -lz \
        || { bad "$p: CLI build failed"; continue; }
    [ -s "$d/lib$p.so" ] || bad "$p: make plugin-so produced no lib$p.so"
done
cc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$REPO/src" -o "$WORK/bin/ivpack-probe" \
    "$REPO/tools/ivpack_probe.c" -ldl
ok "8 CLI helpers + 8 plugin .so built clean"

# ---------------------------------------------------------------- fixtures --
echo "== fixtures =="
python3 - "$WORK/orig" <<'PY'
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
PY
if [ $HAVE_QEMU = 1 ]; then
    qemu-img create -f qcow2 "$WORK/orig/disk.qcow2" 4M >/dev/null
    qemu-io -c 'write -P 0x5a 0 64k' -c 'write -P 0x3c 1M 128k' \
        "$WORK/orig/disk.qcow2" >/dev/null 2>&1
    note "qcow2: disk.qcow2 $(stat -c%s "$WORK/orig/disk.qcow2") bytes"
    qemu-img create -f raw "$WORK/orig/vdi-src.raw" 4M >/dev/null
    python3 -c "
import random,sys
b=bytearray(bytes(random.Random(3).randrange(256) for _ in range(1<<20)))
open(sys.argv[1],'r+b').write(b)" "$WORK/orig/vdi-src.raw"
    qemu-img convert -f raw -O vdi "$WORK/orig/vdi-src.raw" "$WORK/orig/disk.vdi" >/dev/null
    note "vdi: disk.vdi $(stat -c%s "$WORK/orig/disk.vdi") bytes"
fi
if [ $HAVE_E2FS = 1 ]; then
    F="$WORK/orig/disk.ext4"
    mkfs.ext4 -q -F -b 4096 -O '^has_journal,^metadata_csum,^64bit,^inline_data' \
        "$F" 16M >/dev/null 2>&1 || mkfs.ext4 -q -F -b 4096 "$F" 16M >/dev/null
    mkdir -p "$WORK/stage"
    python3 -c "
import random
open('$WORK/stage/big.bin','wb').write(bytes(random.Random(5).randrange(256) for _ in range(256<<10)))"
    head -c 96000 "$REPO/tools/codecpacks/ext4fs.codecpack/ext4fs.c" > "$WORK/stage/src.c"
    debugfs -w -R "write $WORK/stage/big.bin /big.bin" "$F" >/dev/null 2>&1
    debugfs -w -R "write $WORK/stage/src.c /src.c" "$F" >/dev/null 2>&1
    note "ext4fs: disk.ext4 $(stat -c%s "$F") bytes, 2 files written"
fi

# ------------------------------------------------------- desc / decl / junk --
echo "== per-pack ABI: descriptor + decline parity =="
for p in $CPACKS; do
    d=$PACKROOT/$p.codecpack
    so=$d/lib$p.so
    cli=$d/bin/$p

    # descriptor agrees with the manifest
    desc=$("$WORK/bin/ivpack-probe" desc "$so") || { bad "$p: desc probe failed"; continue; }
    mname=$(sed -n 's/^name = //p' "$d/manifest" | head -1)
    mtype=$(sed -n 's/^type = //p' "$d/manifest" | head -1)
    case "$desc" in *"name=$mname "*) ;; *) bad "$p: desc name != manifest ($desc)"; continue ;; esac
    case "$desc" in *"pack_class=${mtype}pack "*) ;; *) bad "$p: desc pack_class != manifest type '$mtype' ($desc)"; continue ;; esac
    case "$desc" in *"cmd=yes estimate=yes"*) ;; *) bad "$p: missing cmd/estimate export ($desc)"; continue ;; esac
    ok "$p: $desc"

    # decline parity: the CLI's exit status must equal the plugin's return code
    for img in junk.bin; do
        # `set -e` needs the non-zero exit captured in a condition, not after
        # a `;` -- a decline (3) is the expected answer here.
        crc=0; "$cli" estimate "$WORK/orig/$img" >/dev/null 2>&1 || crc=$?
        prc=0; "$WORK/bin/ivpack-probe" est "$so" "$WORK/orig/$img" >/dev/null 2>&1 || prc=$?
        if [ "$crc" != "$prc" ]; then
            bad "$p: decline parity on $img (cli=$crc plugin=$prc)"
        elif [ "$prc" -ge 100 ]; then
            bad "$p: plugin died by signal on $img (rc=$prc)"
        else
            ok "$p: $img declined identically (cli=$crc plugin=$prc)"
        fi
    done
done

# ------------------------------------------------------------ round trips --
# rt <pack> <image>: enumerate/extract-all/strip/rebuild/map through the PLUGIN
# ABI, bit-exact against the original, then the same through the CLI and the
# two must agree byte for byte.
rt() {
    p=$1; img=$2; tag=$3
    d=$PACKROOT/$p.codecpack
    so=$d/lib$p.so; cli=$d/bin/$p
    o=$WORK/out/$tag; mkdir -p "$o/mbr" "$o/cli/mbr"

    # --- plugin leg ---
    "$WORK/bin/ivpack-probe" cmd "$so" 1 "$img" - "$o/table" - - >/dev/null \
        || { bad "$tag: plugin enumerate"; return; }
    "$WORK/bin/ivpack-probe" cmd "$so" 3 "$img" - "$o/recipe" - - >/dev/null \
        || { bad "$tag: plugin strip"; return; }
    [ -s "$o/table" ] || { bad "$tag: plugin enumerate wrote no table"; return; }
    nmem=$(wc -l < "$o/table")
    while read -r idx sname usize; do
        [ -n "$idx" ] || continue
        "$WORK/bin/ivpack-probe" cmd "$so" 2 "$img" "$idx" "$o/mbr/$idx" - - >/dev/null \
            || { bad "$tag: plugin extract idx=$idx"; return; }
        got=$(stat -c%s "$o/mbr/$idx")
        [ "$got" = "$usize" ] || { bad "$tag: idx=$idx size $got != announced $usize"; return; }
    done < "$o/table"
    "$WORK/bin/ivpack-probe" cmd "$so" 4 - - "$o/rebuilt" "$o/recipe" "$o/mbr" >/dev/null \
        || { bad "$tag: plugin rebuild"; return; }
    cmp -s "$img" "$o/rebuilt" || { bad "$tag: plugin rebuild is NOT bit-exact"; return; }
    # map consumes the IMAGE, not the recipe: it recomputes the recipe layout
    # strip would write (that is what invfs_codec_pack_cmd passes as `in`).
    "$WORK/bin/ivpack-probe" cmd "$so" 5 "$img" - "$o/map" - - >/dev/null \
        || { bad "$tag: plugin map"; return; }

    # --- CLI leg, same operands ---
    "$cli" enumerate "$img" "$o/cli/table" >/dev/null || { bad "$tag: cli enumerate"; return; }
    "$cli" strip "$img" "$o/cli/recipe" >/dev/null   || { bad "$tag: cli strip"; return; }
    while read -r idx sname usize; do
        [ -n "$idx" ] || continue
        "$cli" extract "$img" "$idx" "$o/cli/mbr/$idx" >/dev/null \
            || { bad "$tag: cli extract idx=$idx"; return; }
    done < "$o/cli/table"
    "$cli" rebuild "$o/cli/recipe" "$o/cli/mbr" "$o/cli/rebuilt" >/dev/null \
        || { bad "$tag: cli rebuild"; return; }
    "$cli" map "$img" "$o/cli/map" >/dev/null || { bad "$tag: cli map"; return; }

    # --- agreement ---
    cmp -s "$o/table"  "$o/cli/table"  || { bad "$tag: table differs (plugin vs cli)"; return; }
    cmp -s "$o/recipe" "$o/cli/recipe" || { bad "$tag: recipe differs (plugin vs cli)"; return; }
    cmp -s "$o/map"    "$o/cli/map"    || { bad "$tag: map differs (plugin vs cli)"; return; }
    for m in "$o"/mbr/*; do
        cmp -s "$m" "$o/cli/mbr/$(basename "$m")" \
            || { bad "$tag: member $(basename "$m") differs (plugin vs cli)"; return; }
    done
    # estimate parity: the number the plugin reports is the number the CLI prints
    cli_est=$("$cli" estimate "$img" | head -1)
    so_est=$("$WORK/bin/ivpack-probe" est "$so" "$img" | sed -n 's/.*mbr_size=\([0-9]*\).*/\1/p')
    [ "$cli_est" = "$so_est" ] || { bad "$tag: estimate differs (cli=$cli_est plugin=$so_est)"; return; }
    ok "$tag: $nmem members, plugin round-trip bit-exact, plugin == cli byte for byte, estimate=$cli_est"
}

echo "== round trips through the plugin ABI =="
# `|| true`: rt() reports its own failures through bad(); a non-zero return
# must not trip `set -e` and abort the rest of the suite.
rt rawdisk "$WORK/orig/disk-gpt.img" rawdisk || true
if [ $HAVE_QEMU = 1 ]; then
    rt qcow2 "$WORK/orig/disk.qcow2" qcow2 || true
    rt vdi "$WORK/orig/disk.vdi" vdi || true
else
    skip "qcow2 + vdi round trips (qemu-img not installed)"
fi
if [ $HAVE_E2FS = 1 ]; then
    rt ext4fs "$WORK/orig/disk.ext4" ext4fs || true
else
    skip "ext4fs round trip (mkfs.ext4/debugfs not installed)"
fi
for p in fatfs ntfs xfs p7z; do
    skip "$p round trip (fixture tooling not installed here: $(
        case $p in
            fatfs) echo 'mkfs.vfat + mtools' ;;
            ntfs)  echo 'mkfs.ntfs + sudo loop mount' ;;
            xfs)   echo 'mkfs.xfs + sudo loop mount' ;;
            p7z)   echo '7zz' ;;
        esac))"
done

# ---------------------------------------------------------------- .ivpack --
echo "== .ivpack bundles (ADR-007 §3: zip-0 + manifest + sha256 + lib/ + bin/) =="
mkdir -p "$WORK/dist"
for p in $CPACKS; do
    d=$PACKROOT/$p.codecpack
    out=$WORK/dist/$p.ivpack
    ( cd "$REPO" && bash tools/pack-ivpack.sh "$d" "$out" >/dev/null ) \
        || { bad "$p: pack-ivpack.sh failed"; continue; }
    # uncompressed ZIP-0: every entry's method must be 0 (Stored)
    meth=$(unzip -v "$out" | awk 'NF>=8 && $1 ~ /^[0-9]+$/ { print $2 }' | sort -u | tr '\n' ',')
    # `unzip -v` prints the method name ("Stored"), `unzip -Z1` the raw code;
    # zip-0 means every entry is stored, never deflated.
    case "$meth" in
        "Stored,"|"0,") ;;
        *) bad "$p: .ivpack is not zip-0 (compression methods seen: $meth)" ;;
    esac
    for want in manifest sha256 "lib/lib$p.so" "bin/$p"; do
        unzip -l "$out" "$want" >/dev/null 2>&1 || bad "$p: .ivpack lacks $want"
    done
    # checksums verify
    rm -rf "$WORK/unz/$p"; mkdir -p "$WORK/unz/$p"
    unzip -q -o "$out" -d "$WORK/unz/$p"
    ( cd "$WORK/unz/$p" && sha256sum -c sha256 >/dev/null 2>&1 ) \
        || bad "$p: sha256 verification failed"
    # the .so from inside the archive still loads and answers
    "$WORK/bin/ivpack-probe" desc "$WORK/unz/$p/lib/lib$p.so" >/dev/null \
        || bad "$p: lib$p.so from the .ivpack does not load"
    ok "$p: $p.ivpack $(stat -c%s "$out") bytes, zip-0, sha256 verified, .so loads"
done

echo
printf '%d failure(s), %d skip(s)\n' "$fails" "$skips"
[ "$fails" = 0 ] && echo PASS || echo FAIL
exit $((fails > 0))
