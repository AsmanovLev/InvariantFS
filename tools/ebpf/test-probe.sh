#!/bin/bash
# test-probe.sh — WP622 gate: the instruments build, attach, and emit.
#
#   bash tools/run-e2e.sh tools/ebpf/test-probe.sh
#
# Runs LOCKED (it greps /tmp/ and calls sudo): it needs CAP_BPF, loop
# mounts and a FUSE mount, so it may not run inside a user namespace.
#
# What it proves, in order:
#   1. toolchain: BTF, bpftrace, clang, libbpf, python3/bc workloads
#   2. `make -C tools/ebpf check`: vmlinux.h + BPF object + CO-RE binary
#   3. every .bt leg attaches (`bpftrace --dry-run` attaches all probes
#      then exits; a bad helper/probe name fails here, not mid-bench)
#   4. the three mounts come up and each leg's probe survives its workload
#   5. the CO-RE leg1 emits per-magic histograms (numbers, not silence)
#   6. teardown leaves nothing mounted and no loop device behind
#
# Disk: 48M ext4 + 0.1G sparse invfs + 128M tmpfs under $S, all removed.
set -e
set -o pipefail

EBPF="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$EBPF/../.." && pwd)"
S=${WP622_GATE_SCRATCH:-/tmp/wp622-gate-$$}
LEG=${WP622_GATE_LEG:-leg1}     # which leg to run live (default: leg1)
TAGS=${WP622_GATE_TAGS:-"ext4 tmpfs invfs"}
fails=0
ok()   { echo "  OK   $*"; }
bad()  { echo "  FAIL $*"; fails=$((fails + 1)); }

cleanup() {
    bash "$EBPF/setup-mounts.sh" --down "$S" >/dev/null 2>&1 || true
    rm -rf "$S" "$S".*.log
}
trap cleanup EXIT

echo "== 1. toolchain =="
[ -e /sys/kernel/btf/vmlinux ] && ok "BTF present" || bad "no /sys/kernel/btf/vmlinux"
for t in bpftrace clang cc bc python3; do
    command -v "$t" >/dev/null && ok "$t" || bad "$t missing (see tools/ebpf/README.md)"
done
BPFTOOL=$(command -v bpftool || echo /sbin/bpftool)
[ -x "$BPFTOOL" ] && ok "bpftool ($BPFTOOL)" || bad "bpftool missing"

echo "== 2. build =="
if make -C "$EBPF" check >"$S.build.log" 2>&1 || make -C "$EBPF" B="$S/build" check >"$S.build.log" 2>&1; then
    ok "make -C tools/ebpf check"
else
    bad "make -C tools/ebpf check (log: $S.build.log)"; tail -n 20 "$S.build.log"
fi
BIN=$( { ls "$EBPF"/.build/leg1-rwpdf "$S/build/leg1-rwpdf" 2>/dev/null || true; } | head -1)
[ -n "$BIN" ] || BIN=$(find "$EBPF"/.build "$S/build" -maxdepth 1 -name leg1-rwpdf -type f 2>/dev/null | head -1 || true)

echo "== 3. attach (--dry-run attaches every probe, then exits) =="
for bt in leg1-rw leg2-meta leg3-sync leg4-pressure; do
    if sudo bpftrace --dry-run "$EBPF/$bt.bt" >"$S.$bt.log" 2>&1; then
        ok "$bt attached ($(grep -c '^Attaching' "$S.$bt.log" >/dev/null && echo probes ok))"
    else
        bad "$bt attach refused:"; tail -n 10 "$S.$bt.log"
    fi
done

echo "== 4. mounts =="
if ! bash "$EBPF/setup-mounts.sh" "$S" >"$S.setup.log" 2>&1; then
    bad "setup-mounts.sh failed:"; tail -n 15 "$S.setup.log"
fi
# MNT_[A-Z0-9]+, not MNT_: the group is followed by '=', so 'MNT_'
# alone matches nothing and every mount var comes back empty.
eval "$(grep -E '^(MNT_[A-Z0-9]+|LOOPDEV|SCRATCH)=' "$S.setup.log" 2>/dev/null || true)"
# run-leg.sh reads the mounts from its environment (it is a child process),
# so the KEY=value lines have to be exported, not just set here.
export MNT_EXT4 MNT_TMPFS MNT_INVFS LOOPDEV SCRATCH
for m in "$MNT_EXT4" "$MNT_TMPFS" "$MNT_INVFS"; do
    grep -q " $m " /proc/mounts && ok "mounted $m" || bad "not mounted: $m"
done

echo "== 5. live leg: $LEG =="
for tag in $TAGS; do
    if bash "$EBPF/run-leg.sh" "$LEG" "$S" "$tag" >"$S.$LEG-$tag.run.log" 2>&1; then
        s="$S/$LEG-$tag.summary.txt"
        # leg1/leg2 print histograms ("[lo, hi)  n=N"), leg3 prints raw
        # maps ("@map[key]: value"), leg4 prints reclaim counts. Any of the
        # three shapes means the probe saw the workload; silence means the
        # attach point missed, which is the failure this gate exists for.
        if grep -qE ' n=[0-9]+' "$s" 2>/dev/null \
           || grep -qE '^@[A-Za-z_]+\[[^]]*\]: *-?[0-9]+' "$s" 2>/dev/null \
           || grep -qE '^\[[^]]+\]: *-?[0-9]+' "$s" 2>/dev/null; then
            ok "$LEG/$tag emitted probe data"
        else
            bad "$LEG/$tag ran but the probe printed no buckets"
        fi
    else
        bad "$LEG/$tag run failed:"; tail -n 15 "$S.$LEG-$tag.run.log"
    fi
done

echo "== 6. CO-RE binary =="
if [ -n "$BIN" ]; then
    if sudo "$BIN" -t 3 >"$S.core.log" 2>&1 && grep -qE ' n=[0-9]+' "$S.core.log"; then
        ok "leg1-rwpdf -t 3 emitted $(grep -cE ' n=[0-9]+' "$S.core.log") counted lines"
    else
        bad "leg1-rwpdf emitted nothing:"; tail -n 15 "$S.core.log"
    fi
else
    bad "leg1-rwpdf binary not found after build"
fi

echo "== 7. teardown =="
bash "$EBPF/setup-mounts.sh" --down "$S" >/dev/null 2>&1 || true
left=0
for m in "$MNT_EXT4" "$MNT_TMPFS" "$MNT_INVFS"; do
    [ -n "$m" ] && grep -q " $m " /proc/mounts && { bad "still mounted: $m"; left=1; }
done
[ "$left" = 0 ] && ok "nothing left mounted"
[ -n "${LOOPDEV:-}" ] && { sudo losetup -d "$LOOPDEV" 2>/dev/null || true; }

if [ "$fails" -gt 0 ]; then
    echo "PROBE GATE: FAIL ($fails)"
    exit 1
fi
echo "PROBE GATE: PASS (build + 4 attaches + $LEG on [$TAGS] + CO-RE emit)"
