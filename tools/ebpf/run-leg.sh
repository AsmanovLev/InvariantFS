#!/bin/bash
# run-leg.sh — WP622 probe runner: attach instrument, run workload, save raw.
# Usage: bash tools/ebpf/run-leg.sh <leg1|leg2|leg3|leg4|overhead> <scratch> [mount-tag]
#   scratch   — prepared by setup-mounts.sh (eval its KEY=value output first)
#   mount-tag — ext4 | tmpfs | invfs (default: run all three, one at a time)
#
# Output: $scratch/<leg>-<tag>.{probe.log,work.log,summary.txt}
# Needs: bpftrace (sudo), python3, bc. The box must otherwise be idle;
# the runner records uptime + slabinfo-dentry deltas as quiet-proof.
set -e
set -o pipefail

EBPF="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
. "$EBPF/workloads.sh"

LEG=$1
S=$2
TAG=${3:-all}

command -v bpftrace >/dev/null || { echo "need bpftrace"; exit 2; }
command -v bc >/dev/null || { echo "need bc"; exit 2; }

mnt_for() {
    case $1 in
        ext4) echo "$MNT_EXT4" ;;
        tmpfs) echo "$MNT_TMPFS" ;;
        invfs) echo "$MNT_INVFS" ;;
    esac
}

probe_for() {
    case $1 in
        leg1) echo "$EBPF/leg1-rw.bt" ;;
        leg2) echo "$EBPF/leg2-meta.bt" ;;
        leg3) echo "$EBPF/leg3-sync.bt" ;;
        leg4) echo "$EBPF/leg4-pressure.bt" ;;
    esac
}

work_for() {
    case $1 in
        leg1) echo "wl_leg1_rw" ;;
        leg2) echo "wl_leg2_meta" ;;
        leg3) echo "wl_leg3_sync" ;;
        leg4) echo "wl_leg4_pressure" ;;
    esac
}

run_one() {  # <leg> <tag>
    local leg=$1 tag=$2 mnt probe work
    mnt=$(mnt_for "$tag")
    probe=$(probe_for "$leg")
    work=$(work_for "$leg")
    local plog="$S/$leg-$tag.probe.log" wlog="$S/$leg-$tag.work.log"
    echo "== $leg on $tag ($mnt) =="
    if ! grep -q " $mnt " /proc/mounts; then
        echo "FAIL: $mnt is not mounted; refusing to measure the wrong fs"
        return 1
    fi
    sudo cat /proc/slabinfo > /dev/null
    wl_seed "$S" "$mnt" >> "$wlog" 2>&1
    sudo cat /proc/slabinfo | awk '/^dentry / {print "slab-dentry-before: " $2 " " $3}' > "$S/$leg-$tag.quiet.txt"
    cat /proc/uptime >> "$S/$leg-$tag.quiet.txt"
    cat /proc/sys/vm/vfs_cache_pressure >> "$S/$leg-$tag.quiet.txt"
    sudo bpftrace "$probe" > "$plog" 2>&1 &
    local btpid=$!
    sleep 6  # let probes attach (kprobe registration is the slow part)
    if ! kill -0 $btpid 2>/dev/null; then
        echo "probe failed to start; $plog tail:"; tail -n 15 "$plog"; return 1
    fi
    # shellcheck disable=SC2086
    $work "$mnt" >> "$wlog" 2>&1
    sleep 2  # drain in-flight (FUSE roundtrips, delayed bios)
    sudo kill -INT $btpid 2>/dev/null || true
    wait $btpid 2>/dev/null || true
    sudo cat /proc/slabinfo | awk '/^dentry / {print "slab-dentry-after: " $2 " " $3}' >> "$S/$leg-$tag.quiet.txt"
    cat /proc/uptime >> "$S/$leg-$tag.quiet.txt"
    {
        echo "# $leg/$tag summary $(date -u +%FT%TZ)";
        echo "--- workload ---"; cat "$wlog";
        echo "--- quiet ---"; cat "$S/$leg-$tag.quiet.txt";
        echo "--- probe tail ---"; tail -n 60 "$plog";
    } > "$S/$leg-$tag.summary.txt"
    echo "saved $S/$leg-$tag.summary.txt"
}

run_overhead() {  # probe-on vs probe-off wall comparison on one mount
    local tag=${1:-ext4} mnt
    mnt=$(mnt_for "$tag")
    echo "== overhead: wl_leg1_rw on $tag, 3x off then 3x on =="
    for i in 1 2 3; do
        wl_leg1_rw "$mnt" 2>&1 | grep leg1-wall
    done > "$S/overhead-off.work.log" 2>&1 || true
    cat "$S/overhead-off.work.log"
    sudo bpftrace "$EBPF/leg1-rw.bt" > "$S/overhead.probe.log" 2>&1 &
    local btpid=$!
    sleep 6
    for i in 1 2 3; do
        wl_leg1_rw "$mnt" 2>&1 | grep leg1-wall
    done > "$S/overhead-on.work.log" 2>&1 || true
    sudo kill -INT $btpid 2>/dev/null || true
    wait $btpid 2>/dev/null || true
    cat "$S/overhead-on.work.log"
    echo "--- probe ran alongside (tail) ---"
    tail -n 12 "$S/overhead.probe.log"
}

case $LEG in
    leg1|leg2|leg3|leg4)
        if [ "$TAG" = "all" ]; then
            for t in ext4 tmpfs invfs; do run_one "$LEG" "$t"; done
        else
            run_one "$LEG" "$TAG"
        fi
        ;;
    overhead) run_overhead "$TAG" ;;
    *) echo "usage: run-leg.sh <leg1|leg2|leg3|leg4|overhead> <scratch> [tag]"; exit 2 ;;
esac
