#!/bin/bash
# test-sandbox.sh — WP12d completion: the codecpack-child Landlock sandbox
# (persistent regression).
#
#   (a) CANARY pack (a splt_test-style fixture, materialized in $WORK):
#       its helper pokes at /etc/passwd (read canary) and tries to write a
#       marker outside the scratch dir (write canary). With the sandbox ON
#       (the default) the helper gets EACCES on the read, refuses, and the
#       pack guard falls back to generic (GENERIC_GUARD{47,1}); the marker
#       never appears. With INVFS_PACK_SANDBOX=0 the read succeeds, the
#       file transcodes (CODEC{47,1}) and the marker lands — proving the
#       canary helper is sensitive to the sandbox and nothing else.
#       NOTE: /etc/passwd, not /etc/shadow — shadow is mode-000 root-only,
#       so DAC would deny the read even with the sandbox off (uid 1000
#       here) and the OFF leg could not prove the canary works.
#   (b) REGRESSION: the three pack-heavy suites — test-containerpack.sh,
#       test-rawimg.sh, test-jxl.sh — run end-to-end with the sandbox ON
#       (default env).
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-sandbox.sh
# The regression legs invoke the sibling suites with plain `bash` on
# purpose: the outer run-e2e already holds the global e2e flock, a nested
# run-e2e would self-deadlock waiting for it.
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO=/home/user/InvariantFS
B=$REPO/bin
WT=$(cd "$(dirname "$0")/.." && pwd)   # the checkout this script lives in
WORK=/dev/shm/wp12dsandbox
trap 'rm -rf "$WORK" /dev/shm/wp12dsbx*.img' EXIT
IMGON=wp12dsbx-on.img
IMGOFF=wp12dsbx-off.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" \
    "$WORK/packs/canary.codecpack"
cd /dev/shm
rm -f "$IMGON" "$IMGOFF"

echo "== tools =="
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }

echo "== canary pack fixture =="
cat > "$WORK/packs/canary.codecpack/canary.py" <<'PY'
#!/usr/bin/env python3
"""canary.py — WP12d sandbox canary helper (InvariantFS).

Every command first pokes at the world outside the whitelist:
  - write canary: drop CANARY_MARKER (a path outside the scratch dir);
    failure is ignored — absence/presence of the marker is the signal;
  - read canary:  read a byte from TARGET; failure REFUSES the command
    (exit 1), which is how the FS-side guard sees the sandbox bite.
The transcode itself is zlib: bit-exact by construction, smaller on the
repetitive fixture, so a successful (unsandboxed) attempt passes the
sweep's decode-back memcmp and size guards and earns the CODEC stamp.
"""
import os
import sys
import zlib

TARGET = "/etc/passwd"
MARKER = os.environ.get("CANARY_MARKER", "")


def poke():
    if MARKER:
        try:
            with open(MARKER, "w") as f:
                f.write("escaped\n")
        except OSError:
            pass
    with open(TARGET, "rb") as f:
        f.read(1)


def main(argv):
    if len(argv) < 3:
        return 2
    cmd = argv[1]
    try:
        if cmd == "estimate" and len(argv) == 3:
            poke()
            print(os.path.getsize(argv[2]))
        elif cmd == "encode" and len(argv) == 4:
            poke()
            with open(argv[2], "rb") as f:
                d = f.read()
            with open(argv[3], "wb") as f:
                f.write(zlib.compress(d, 9))
        elif cmd == "decode" and len(argv) == 4:
            with open(argv[2], "rb") as f:
                d = f.read()
            with open(argv[3], "wb") as f:
                f.write(zlib.decompress(d))
        else:
            return 2
    except (OSError, ValueError, zlib.error):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
PY
cat > "$WORK/packs/canary.codecpack/manifest" <<'EOF'
# WP12d canary pack: a codec whose helper insists on reading a file far
# outside its whitelist before doing its (trivial zlib) job.
name = canary
algo = 47
pack_version = 1
generation = 1
caps = wholefile|external
dec_mem = 0
sniff.magic = 43414E41
encode = python3 {pack}/canary.py encode {in} {out}
decode = python3 {pack}/canary.py decode {in} {out}
estimate = python3 {pack}/canary.py estimate {in}
EOF
# binary-ish, highly compressible, NOT text-family (NUL bytes): a refused
# pack falls all the way to generic ZSTD, a successful one stays CODEC
python3 -c "open('$WORK/orig/canary.bin','wb').write(b'CANA' + bytes(range(256)) * 400)"
echo "  canary.bin: $(stat -c%s "$WORK/orig/canary.bin") bytes"

# class-stamp reader (the containerpack pattern: built from repo objects)
cat > "$WORK/classof.c" <<'C'
/* classof.c — print the WP10 storage-class stamp of one file.
 * usage: classof <image> <name>  ->  "cls=<n> algo=<n> gen=<n>" or "none" */
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
CORE_O="$REPO/build/obj/volume.o $REPO/build/obj/vol_cpack.o $REPO/build/obj/vol_png.o $REPO/build/obj/vol_seal.o $REPO/build/obj/vol_repair.o $REPO/build/obj/vol_rollback.o $REPO/build/obj/vol_resize.o $REPO/build/obj/vol_fsck.o $REPO/build/obj/vol_crash.o $REPO/build/obj/vol_exer.o $REPO/build/obj/vol_dedupe.o $REPO/build/obj/vol_textzone.o $REPO/build/obj/vol_heat.o $REPO/build/obj/vol_sweep.o $REPO/build/obj/vol_read.o $REPO/build/obj/vol_write.o $REPO/build/obj/vol_records.o $REPO/build/obj/vol_ast.o $REPO/build/obj/vol_dirs.o $REPO/build/obj/arc.o $REPO/build/obj/crc32c.o $REPO/build/obj/lz4.o $REPO/build/obj/blkio.o $REPO/build/obj/flacx.o $REPO/build/obj/tarx.o $REPO/build/obj/pngx.o $REPO/build/obj/miniz.o $REPO/build/obj/ppmd8.o $REPO/build/obj/ppmd8enc.o $REPO/build/obj/ppmd8dec.o $REPO/build/obj/ppmd_codec.o $REPO/build/obj/codec.o $REPO/build/obj/bcj_x86.o $REPO/build/obj/blake3.o $REPO/build/obj/blake3_dispatch.o $REPO/build/obj/blake3_portable.o $REPO/build/obj/rs.o $REPO/build/obj/vol_tier.o"
gcc -std=gnu11 -O2 -I$REPO/src-extracted/VFS/src -o "$WORK/classof" "$WORK/classof.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== leg A1: sandbox ON (default) — read canary EACCES, guard falls to generic =="
$B/invf-mkfs "$IMGON" 0.2 >/dev/null
$B/invf-cp "$IMGON" "$WORK/orig/canary.bin" canary.bin >/dev/null
rm -f "$WORK/escape-marker"
INVFS_DEBUG=1 CANARY_MARKER="$WORK/escape-marker" \
    INVFS_CODECPACKS="$WORK/packs" \
    $B/invf-sweep "$IMGON" > "$WORK/sweep-on.log" 2>&1 \
    || { cat "$WORK/sweep-on.log"; exit 1; }
# the probe must report Landlock active (else this leg proves nothing)
grep -q "pack sandbox: Landlock ABI" "$WORK/sweep-on.log" \
    || { echo "FAIL: Landlock probe did not engage"; cat "$WORK/sweep-on.log"; exit 1; }
if grep -q "canary.bin: canary (codecpack)" "$WORK/sweep-on.log"; then
    echo "FAIL: the canary pack transcoded UNDER the sandbox"; exit 1
fi
C=$("$WORK/classof" "$IMGON" canary.bin)
echo "  canary.bin (sandbox ON): $C"
[ "$C" = "cls=6 algo=47 gen=1" ] || { echo "FAIL: want GENERIC_GUARD{47,1} (cls=6)"; exit 1; }
[ ! -e "$WORK/escape-marker" ] \
    || { echo "FAIL: write canary escaped the sandbox"; exit 1; }
$B/invf-cat "$IMGON" canary.bin "$WORK/out/on.bin" >/dev/null
cmp -s "$WORK/orig/canary.bin" "$WORK/out/on.bin" \
    || { echo "FAIL: sandbox-ON leg not bit-exact (generic store)"; exit 1; }
grep -q " 0 corrupt," <($B/invf-verify "$IMGON" --deep) \
    || { echo "FAIL: verify --deep not clean (sandbox ON)"; exit 1; }
echo "read canary denied (GENERIC_GUARD{47,1}), write canary denied, bit-exact"

echo "== leg A2: INVFS_PACK_SANDBOX=0 — the canary reads fine =="
$B/invf-mkfs "$IMGOFF" 0.2 >/dev/null
$B/invf-cp "$IMGOFF" "$WORK/orig/canary.bin" canary.bin >/dev/null
rm -f "$WORK/escape-marker"
INVFS_PACK_SANDBOX=0 CANARY_MARKER="$WORK/escape-marker" \
    INVFS_CODECPACKS="$WORK/packs" \
    $B/invf-sweep "$IMGOFF" > "$WORK/sweep-off.log" 2>&1 \
    || { cat "$WORK/sweep-off.log"; exit 1; }
grep -q "canary.bin: canary (codecpack)" "$WORK/sweep-off.log" \
    || { echo "FAIL: canary pack did not transcode with the sandbox off"; \
         cat "$WORK/sweep-off.log"; exit 1; }
C=$(INVFS_CODECPACKS="$WORK/packs" "$WORK/classof" "$IMGOFF" canary.bin)
echo "  canary.bin (sandbox OFF): $C"
[ "$C" = "cls=2 algo=47 gen=1" ] || { echo "FAIL: want CODEC{47,1} (cls=2)"; exit 1; }
[ "$(cat "$WORK/escape-marker" 2>/dev/null)" = "escaped" ] \
    || { echo "FAIL: write canary did not land with the sandbox off"; exit 1; }
# the read path decodes through the pack — under the DEFAULT (sandboxed) env
INVFS_CODECPACKS="$WORK/packs" $B/invf-cat "$IMGOFF" canary.bin \
    "$WORK/out/off.bin" >/dev/null
cmp -s "$WORK/orig/canary.bin" "$WORK/out/off.bin" \
    || { echo "FAIL: sandbox-OFF leg not bit-exact (sandboxed decode)"; exit 1; }
grep -q " 0 corrupt," <(INVFS_CODECPACKS="$WORK/packs" $B/invf-verify "$IMGOFF" --deep) \
    || { echo "FAIL: verify --deep not clean (sandbox OFF)"; exit 1; }
echo "read canary succeeded (CODEC{47,1}), marker landed, sandboxed decode bit-exact"

echo "== leg B: pack-suite regressions with the sandbox ON (default env) =="
unset INVFS_PACK_SANDBOX   # belt + braces: the default must be ON
for t in test-containerpack.sh test-rawimg.sh test-jxl.sh; do
    echo "-- $t"
    # plain bash: the outer run-e2e holds the global e2e flock already
    bash "$WT/tools/$t" > "$WORK/reg-$t.log" 2>&1 \
        || { echo "FAIL: $t"; tail -40 "$WORK/reg-$t.log"; exit 1; }
    tail -1 "$WORK/reg-$t.log"
done

echo "SANDBOX E2E: PASS"
