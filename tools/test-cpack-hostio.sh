#!/bin/bash
# test-cpack-hostio.sh — WP-CPACK-HOSTIO-V3 / ADR-015.
#
# The claim under test, in one sentence: a containerpack can decompose a
# container it never gets a path to, so the sweep's PIN -- the whole container
# written into scratch before the pack can look at it -- is gone.
#
# Why it needs its own suite: every other pack test runs with a scratch area
# bigger than its corpus, so the pin is invisible. Here the scratch is a 1 MiB
# tmpfs and the container is several MiB, which makes the difference between
# "reads ranges" and "materializes a file" a pass/fail rather than a
# stopwatch. The control leg (`INVFS_CPACK_HOSTIO=0`) is the same sweep on the
# same volume with the pin: it MUST fail, or the test proves nothing.
#
#   J1  host_io: 1 MiB scratch, 8 MiB ext4 container -> decomposed, guard
#       passed, bit-exact round trip, scratch root left EMPTY (du = 0)
#   J2  control: same run with INVFS_CPACK_HOSTIO=0 -> refused (scratch too
#       small for the pin), no CONTAINER stamp, and the volume still reads the
#       container bit-exactly (a refusal changes nothing)
#   J3  version gate: the SAME pack source built as descriptor v2 (nothing
#       else differs) -> the io fields are not filled, the pin comes back, and
#       the 1 MiB scratch refuses again. That is IVPACK_API_VERSION_MIN == 1
#       doing its job: an old pack keeps working exactly as before.
#   J4  solid: a pack that reports IVPACK_RC_DECLINE_SOLID is stamped
#       INVFS_CLASS_GENERIC_SOLID, counted as "refused (solid)", NOT
#       decomposed and NOT retried; INVFS_CPACK_SOLID=1 turns the verdict back
#       into a plain decline.
#   J5  streaming member: a member far larger than the sweep's peak RSS is
#       committed (it used to be one malloc, and one failure aborted the whole
#       container), bit-exact, with VmHWM measured below the member's size.
#
# Run from the repo root after `make` and `make plugin-so`:
#   bash tools/test-cpack-hostio.sh
# Uses /dev/shm only (never /tmp for images), like the other soak scripts.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
# INVFS_BIN lets a sanitizer build (bin-tsan) be exercised without
# replacing the binaries the normal runs use.
B=${INVFS_BIN:-$REPO/bin}
PACKDIR=$REPO/tools/codecpacks/ext4fs.codecpack
WORK=/dev/shm/wpcphostio
SCRATCHROOT=/dev/shm/wpcphostio-scratch
IMGBIG=wpcphostio-big.img
cleanup() {
    umount "$SCRATCHROOT" 2>/dev/null || true
    rm -rf "$WORK" /dev/shm/wpcphostio*.img "$SCRATCHROOT"
}
trap cleanup EXIT

rm -rf "$WORK" "$SCRATCHROOT"
mkdir -p "$WORK/incoming" "$WORK/out"
cd /dev/shm
rm -f wpcphostio*.img

say() { echo "  $*"; }
fail() { echo "FAIL: $*"; exit 1; }

echo "== tools =="
for t in mkfs.ext4 debugfs e2fsck python3 sha256sum; do
    command -v "$t" >/dev/null || fail "$t is not installed"
done
[ -x "$B/invf-sweep" ] || fail "build first (make): $B/invf-sweep missing"
[ -f "$PACKDIR/libext4fs.so" ] || fail "build the plugin half first: make plugin-so"

# The pack's CLI half. Built here exactly the way tools/test-ext4fs.sh builds
# it (same flags, -Werror), because the core still execs it: host I/O replaces
# the INPUT of a v3 pack, never the command set, and every leg below that does
# not take the io path runs this binary.
echo "== build the pack CLI (cc -O2 -Wall -Wextra -Werror) =="
mkdir -p "$PACKDIR/bin"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACKDIR/bin/ext4fs" "$PACKDIR/ext4fs.c"     || fail "the pack CLI does not build clean"

# ---- FS-side helpers (classof = the storage-class stamp of one file) ------
echo "== build classof =="
cat > "$WORK/classof.c" <<'C'
/* classof.c — print the storage-class stamp of one file. */
#include <stdio.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0;
    uint8_t cls = 0, algo = 0;
    uint16_t gen = 0;
    invfs_volume *v;
    uint64_t id;
    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    id = vol_find(v, argv[2]);
    if (!id) { printf("none\n"); vol_close(v); return 0; }
    if (vol_get_class(v, id, &cls, &algo, &gen) != 0) printf("none\n");
    else printf("cls=%d algo=%d gen=%d\n", cls, algo, gen);
    vol_close(v);
    return 0;
}
C
CORE_O="$(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt")"
"${CC:-gcc}" ${INVFS_TEST_CFLAGS:-} -std=gnu11 -O2 -I"$REPO/src" -I"$REPO/src/core" -I"$REPO/src/codecs" \
    -I"$REPO/src/recipes" -I"$REPO/src/vendor7z" -o "$WORK/classof" \
    "$WORK/classof.c" $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread \
    || fail "classof build"

# ---- fixture: an 8 MiB ext4 with two small files --------------------------
# The point of the sizes: the container (8 MiB) cannot fit the 1 MiB scratch,
# its members (192 KiB) comfortably can.
echo "== fixture: 8 MiB ext4, two 96 KiB files =="
head -c 98304 /dev/urandom > "$WORK/alpha.bin"
head -c 98304 /dev/urandom > "$WORK/beta.bin"
truncate -s 8M "$WORK/disk.ext4"
mkfs.ext4 -q -b 4096 -F "$WORK/disk.ext4" >/dev/null 2>&1
cat > "$WORK/dbg.cmd" <<EOF
mkdir /a
cd /a
write $WORK/alpha.bin alpha.bin
write $WORK/beta.bin beta.bin
EOF
debugfs -w -f "$WORK/dbg.cmd" "$WORK/disk.ext4" >/dev/null 2>&1 \
    || fail "debugfs could not populate the image"
e2fsck -fn "$WORK/disk.ext4" >/dev/null 2>&1 || fail "fixture is not a clean ext4"
cp "$WORK/disk.ext4" "$WORK/incoming/disk.ext4"
SRC_SHA=$(sha256sum "$WORK/disk.ext4" | awk '{print $1}')
say "container $(stat -c%s "$WORK/disk.ext4") bytes, sha256 ${SRC_SHA:0:16}…"

# The scratch policy reserves a 128 MiB cushion on top of the job's own demand
# (src/core/tool_scratch.c:scratch_margin) so a scratch root is never filled to
# the brim. That cushion is a policy, not part of this claim, and a 1 MiB root
# could never satisfy it -- so it is set to 0 for BOTH the host_io leg and its
# control, which keeps the comparison honest: what the root has to hold is the
# PNG-free truth, 8 MiB of pin versus 192 KiB of members.
export INVFS_SCRATCH_MARGIN_MB=0

# ---- the 1 MiB scratch root ----------------------------------------------
mkdir -p "$SCRATCHROOT"
# The acceptance is this mount: 1 MiB of scratch versus an 8 MiB container.
# It needs an unprivileged mount namespace, which Ubuntu 24.04 restricts by
# default (kernel.apparmor_restrict_unprivileged_userns=1) -- that is why
# engine-ci's first e2e-full run reported this suite red with the message below
# rather than testing anything. The job now lowers the restriction before the
# tier; the message names it so a host that forbids it says so instead of
# looking like a suite bug.
mount -t tmpfs -o size=1m tmpfs "$SCRATCHROOT" 2>/dev/null \
    || fail "cannot mount a tmpfs: this leg needs an unprivileged mount ns (run it via tools/run-e2e.sh; on Ubuntu 24.04 also 'sysctl -w kernel.apparmor_restrict_unprivileged_userns=0')"
echo "== scratch root: $(df -h --output=size "$SCRATCHROOT" | tail -1 | tr -d ' ') tmpfs at $SCRATCHROOT =="

mkvol() {   # mkvol <image>
    $B/invf-mkfs "$1" 0.6 >/dev/null
    $B/invf-import "$1" "$WORK/incoming" >/dev/null
}

# ---------------------------------------------------------------------------
echo "== J1: host_io -- no pin, 1 MiB scratch =="
IMG=wpcphostio-j1.img
mkvol "$IMG"
INVFS_CODECPACKS=$REPO/tools/codecpacks \
INVFS_TOOL_SCRATCH=$SCRATCHROOT \
INVFS_DEBUG_PACKS=1 \
    $B/invf-sweep "$IMG" > "$WORK/j1.log" 2>&1 || { cat "$WORK/j1.log"; fail "J1 sweep rc"; }
grep -q "host_io admission" "$WORK/j1.log" \
    || { grep -E "pin|host_io|scratch" "$WORK/j1.log"; fail "J1 did not take the host_io admission"; }
grep -q "disk.ext4: ext4fs (codecpack)" "$WORK/j1.log" \
    || { cat "$WORK/j1.log"; fail "J1: disk.ext4 not decomposed"; }
say "decomposed without a pin:"
grep -E "host_io admission|disk.ext4: ext4fs" "$WORK/j1.log" | sed 's/^/    /'
CLS=$("$WORK/classof" "$IMG" disk.ext4)
[ "$CLS" = "cls=3 algo=17 gen=1" ] || fail "J1 stamp: got '$CLS', want cls=3 algo=17 gen=1"
say "stamp: $CLS (CONTAINER, ext4fs)"
$B/invf-cat "$IMG" disk.ext4 "$WORK/j1.out" >/dev/null || fail "J1: invf-cat"
GOT_SHA=$(sha256sum "$WORK/j1.out" | awk '{print $1}')
[ "$GOT_SHA" = "$SRC_SHA" ] || fail "J1 round trip not bit-exact"
say "round trip bit-exact: sha256 ${GOT_SHA:0:16}…"
LEFT=$(du -sk "$SCRATCHROOT" | awk '{print $1}')
[ "$LEFT" -eq 0 ] || { du -a "$SCRATCHROOT" | head; fail "J1 left $LEFT KiB in the scratch root"; }
say "scratch root after the run: du -sk = ${LEFT} KiB (0 = the container was never materialized)"
$B/invf-fsck "$IMG" > "$WORK/j1.fsck" 2>&1 && say "fsck clean" || { cat "$WORK/j1.fsck"; fail "J1 fsck"; }

# ---------------------------------------------------------------------------
echo "== J2: control -- INVFS_CPACK_HOSTIO=0, same 1 MiB scratch =="
IMG=wpcphostio-j2.img
mkvol "$IMG"
INVFS_CODECPACKS=$REPO/tools/codecpacks \
INVFS_TOOL_SCRATCH=$SCRATCHROOT \
INVFS_CPACK_HOSTIO=0 \
    $B/invf-sweep "$IMG" > "$WORK/j2.log" 2>&1 || { cat "$WORK/j2.log"; fail "J2 sweep rc"; }
if grep -q "disk.ext4: ext4fs (codecpack)" "$WORK/j2.log"; then
    fail "J2: the pin path decomposed through a 1 MiB scratch -- the leg proves nothing"
fi
CLS=$("$WORK/classof" "$IMG" disk.ext4)
case "$CLS" in
    cls=3*) fail "J2: the pack refused but the file is CONTAINER-stamped ('$CLS')" ;;
esac
say "declined by the pin, fell through to the generic floor: stamp '$CLS'"
$B/invf-cat "$IMG" disk.ext4 "$WORK/j2.out" >/dev/null || fail "J2: invf-cat"
[ "$(sha256sum "$WORK/j2.out" | awk '{print $1}')" = "$SRC_SHA" ] \
    || fail "J2: a refusal changed the file"
say "container still bit-exact after the refusal"

# ---------------------------------------------------------------------------
echo "== J3: version gate -- the same pack source compiled as descriptor v2 =="
# Nothing differs but the version the descriptor reports, which is exactly what
# the gate reads: desc->api_version >= 3 AND IVPACK_F_HOST_IO. A v2 pack must
# get a path (and therefore the pin) again, so the 1 MiB scratch refuses.
#
# The v2 descriptor is a stand-in .so, not the ext4fs source compiled with
# -DIVPACK_API_VERSION=2: the header defines that macro itself, so a -D
# loses to it (and a pack that reported v2 by accident is the exact shape of
# bug this leg exists to catch). The stand-in reports api_version 2 WITH
# IVPACK_F_HOST_IO SET, which is the interesting case: the flag alone must not
# unlock the callbacks, the version has to be there too. The command set is
# still served by the pack's CLI helper, which is what "a v2 pack works
# unchanged" means.
mkdir -p "$WORK/packsv2"
cp -r "$PACKDIR" "$WORK/packsv2/ext4fs.codecpack"
rm -f "$WORK/packsv2/ext4fs.codecpack/libext4fs.so"
cat > "$WORK/v2stub.c" <<'C'
/* a containerpack descriptor at ABI v2, with the host-io flag set: the core
 * must refuse it the callbacks (api_version < 3) and hand it a path. */
#include <string.h>
#include "ivpack_api.h"
static const ivpack_desc d = { 2u, "ext4fs", "1.0.0", "containerpack",
                               IVPACK_F_HOST_IO };
const ivpack_desc *ivpack_get_desc(void) { return &d; }
int ivpack_container_cmd(const ivpack_container_args *a)
{ (void)a; return 3; }
int ivpack_container_estimate(const char *in, ivpack_estimate_res *r)
{ (void)in; if (r) memset(r, 0, sizeof *r); return 3; }
C
cc -std=gnu11 -O2 -fPIC -shared -I"$REPO/src/include"    -o "$WORK/packsv2/ext4fs.codecpack/libext4fs.so" "$WORK/v2stub.c"     || fail "v2 stand-in plugin build"
IMG=wpcphostio-j3.img
mkvol "$IMG"
INVFS_CODECPACKS=$WORK/packsv2 \
INVFS_TOOL_SCRATCH=$SCRATCHROOT \
INVFS_DEBUG_PACKS=1 \
    $B/invf-sweep "$IMG" > "$WORK/j3.log" 2>&1 || { cat "$WORK/j3.log"; fail "J3 sweep rc"; }
if grep -q "host_io admission" "$WORK/j3.log"; then
    fail "J3: a v2 descriptor was given host I/O (the version gate is not gating)"
fi
if grep -q "disk.ext4: ext4fs (codecpack)" "$WORK/j3.log"; then
    fail "J3: a v2 pack decomposed through a 1 MiB scratch"
fi
case "$("$WORK/classof" "$IMG" disk.ext4)" in
    cls=3*) fail "J3: a v2-descriptor pack decomposed and stamped the file" ;;
esac
say "v2 pack: no io fields, pin flow, refused by the 1 MiB scratch -- unchanged behaviour"

# ---------------------------------------------------------------------------
echo "== J4: solid verdict -- REFUSED, stamped, counted, not retried =="
IMG=wpcphostio-j4.img
mkvol "$IMG"
# INVFS_HELPER_KEEPENV is the pack sandbox's documented way to let ONE
# variable through the environment scrub (helper_exec.c:helper_scrub_env
# clears everything else, in-process child included) -- so the hook reaches the
# pack exactly the way a real pack's own knob would.
EXT4FS_SOLID=1 INVFS_HELPER_KEEPENV=EXT4FS_SOLID \
INVFS_CODECPACKS=$REPO/tools/codecpacks \
    $B/invf-sweep "$IMG" > "$WORK/j4.log" 2>&1 || { cat "$WORK/j4.log"; fail "J4 sweep rc"; }
grep -q "solid archive" "$WORK/j4.log" || { cat "$WORK/j4.log"; fail "J4: no solid verdict reported"; }
grep -q "refused (solid): 1 file, 8.0 MiB" "$WORK/j4.log" \
    || { grep -E "refused|solid" "$WORK/j4.log"; fail "J4: the sweep report did not count the solid refusal"; }
CLS=$("$WORK/classof" "$IMG" disk.ext4)
[ "$CLS" = "cls=11 algo=17 gen=1" ] || fail "J4 stamp: got '$CLS', want cls=11 (GENERIC_SOLID)"
if $B/invf-ls "$IMG" | grep -q "disk.ext4!"; then fail "J4 left siblings"; fi
say "stamped GENERIC_SOLID, no siblings, counted in the report"
# the opt-in turns the verdict back into a plain decline: not stamped, retried
IMG=wpcphostio-j4b.img
mkvol "$IMG"
EXT4FS_SOLID=1 INVFS_HELPER_KEEPENV=EXT4FS_SOLID \
INVFS_CPACK_SOLID=1 INVFS_CODECPACKS=$REPO/tools/codecpacks \
    $B/invf-sweep "$IMG" > "$WORK/j4b.log" 2>&1 || { cat "$WORK/j4b.log"; fail "J4b sweep rc"; }
[ "$("$WORK/classof" "$IMG" disk.ext4)" != "cls=11 algo=17 gen=1" ] \
    || fail "J4b: INVFS_CPACK_SOLID=1 still stamped the solid verdict"
if grep -q "refused (solid)" "$WORK/j4b.log"; then
    fail "J4b counted a refusal the operator opted out of"
fi
say "INVFS_CPACK_SOLID=1: plain decline, no stamp, no count"

# ---------------------------------------------------------------------------
echo "== J5: a member far larger than the sweep's peak RSS =="
# 384 MiB container, one 192 MiB member. The commit used to slurp the member
# whole (one malloc, one write) and abort the container when that failed; the
# streamed commit copies it in 4 MiB windows, so the sweep's high-water mark
# stays far below the member's size. VmHWM is sampled while the sweep runs.
head -c 201326592 /dev/urandom > "$WORK/big.bin"
truncate -s 400M "$WORK/bigdisk.ext4"
mkfs.ext4 -q -b 4096 -F "$WORK/bigdisk.ext4" >/dev/null 2>&1
printf 'mkdir /a\ncd /a\nwrite %s big.bin\n' "$WORK/big.bin" > "$WORK/dbgbig.cmd"
debugfs -w -f "$WORK/dbgbig.cmd" "$WORK/bigdisk.ext4" >/dev/null 2>&1 \
    || fail "debugfs could not populate the big image"
e2fsck -fn "$WORK/bigdisk.ext4" >/dev/null 2>&1 || fail "big fixture is not clean"
rm -rf "$WORK/incoming"
mkdir -p "$WORK/incoming"
cp "$WORK/bigdisk.ext4" "$WORK/incoming/bigdisk.ext4"
BIG_SHA=$(sha256sum "$WORK/bigdisk.ext4" | awk '{print $1}')
mkvol "$IMGBIG"
# No INVFS_ARC_BYTES here on purpose: the sweep's whole-file admission is
# capped by min(256 MiB, arc_budget), so a small arc budget would keep the 400
# MiB container out of the container lane entirely and this leg would measure
# nothing. Above the cap the lane takes the STREAMING admission
# (vol_containerpack_sweep_stream) -- which is the one that used to pin.
INVFS_CODECPACKS=$REPO/tools/codecpacks \
    $B/invf-sweep "$IMGBIG" > "$WORK/j5.log" 2>&1 &
SWEEP=$!
HWM=0
while kill -0 $SWEEP 2>/dev/null; do
    V=$(awk '/^VmHWM:/{print $2}' /proc/$SWEEP/status 2>/dev/null || echo 0)
    [ -n "$V" ] && [ "$V" -gt "$HWM" ] && HWM=$V
    sleep 0.2
done
wait $SWEEP || { cat "$WORK/j5.log"; fail "J5 sweep rc"; }
grep -q "bigdisk.ext4: ext4fs (codecpack)" "$WORK/j5.log" \
    || { cat "$WORK/j5.log"; fail "J5: big container not decomposed"; }
[ "$("$WORK/classof" "$IMGBIG" bigdisk.ext4)" = "cls=3 algo=17 gen=1" ] \
    || fail "J5: no CONTAINER stamp"
$B/invf-cat "$IMGBIG" bigdisk.ext4 "$WORK/j5.out" >/dev/null || fail "J5: invf-cat"
[ "$(sha256sum "$WORK/j5.out" | awk '{print $1}')" = "$BIG_SHA" ] || fail "J5 round trip not bit-exact"
MEMBER_MIB=192
HWM_MIB=$((HWM / 1024))                  # VmHWM is in KiB
say "member ${MEMBER_MIB} MiB committed; sweep VmHWM ${HWM_MIB} MiB (peak RSS, sampled)"
[ "$HWM_MIB" -lt "$MEMBER_MIB" ] \
    || fail "J5: VmHWM ${HWM_MIB} MiB >= the ${MEMBER_MIB} MiB member -- it was still held whole"
say "round trip bit-exact; peak RSS stays below the member's size"

echo
echo "CPACK HOST I/O (ADR-015): PASS"
