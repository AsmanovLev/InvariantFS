#!/bin/bash
# test-backup-pilot.sh — WP644: the chunked 1 TB / ~1M-file backup pilot.
#
# WHY THIS SCRIPT EXISTS. The orchestrator's target is a 1 TB / ~1M-file
# laptop backup onto a dual-device volume (128 GB SSD dev0 + 2 TB HDD
# dev1), and three numbers decide whether that run is scheduled and how
# long it takes: per-FILE import rate, per-GB sweep rate, and fsck wall
# time at 100k+ inodes. Guessing them is how a 35-hour import becomes a
# surprise. This harness produces them, then runs the chunked ingest the
# full backup will use, with the fsck gate the operator doc depends on.
#
# THE SHAPE OF THE RUN (AGENTS.md 2.4-2.5 is the reason for every part):
# RAW is a window, not a tank. Continuous ingest outruns the drain (~100
# MiB/s ingest vs a measured ~40 MiB/s sweep), so the backup is CHUNKED:
# import a chunk -> sweep it -> fsck MUST report CLEAN -> record the chunk
# in the manifest -> next chunk. The manifest is the resume state: DONE
# chunks are never re-imported, FAILED ones are retried.
#
# PHASES (each idempotent, so a re-run is a resume):
#
#   plan    source preflight (counts, largest files, RAM/tier walls,
#           filesystem free space, the volume's RAW window) and the chunk
#           plan, written into the manifest as PENDING rows.
#   run     per chunk, in plan order: invf-import --fail-on-skip -> offline
#           invf-sweep -> invf-fsck CLEAN gate -> manifest row. Stops at the
#           first FAILED chunk: the next chunk must not start on a volume
#           that has not been verified.
#   verify  deterministic bit-exact sample: a fixed stride over the sorted
#           source path list, compared byte-for-byte through invf-cat (and
#           readlink for symlinks). "Deterministic sample, not vibes."
#   reclaim unlink one chunk through a FUSE mount, then settle (sweep until
#           the free-block count stops moving) and assert the space comes
#           back. This is where AGENTS.md 2.4's three deferrals show up as
#           MEASUREMENTS instead of as a leak report: the savepoint pin is
#           discharged by the NEXT capture, a TEXT-class entry names a
#           SHARED batch segment the targeted free deliberately skips, and
#           the retire window ends at the next fence.
#   report  the three rates, the per-stage sweep breakdown, the per-chunk
#           space accounting, and the calendar arithmetic for 1 TB / 1M
#           files -- with the extrapolation REFUSED (printed n/a) when the
#           sample is too small to carry it, because a rate measured on
#           700 KiB of corpus is not a rate for 1 TB.
#
# HERMETIC LEGS (default; no INVFS_PILOT_SRC). The default corpus is
# generated from a SHA-256 counter stream, so it is byte-identical on every
# host -- that determinism is load-bearing (the sample list and the reclaim
# arithmetic are only honest if the corpus is fixed). Around the phases the
# hermetic run adds the legs that need a corpus whose contents are known:
#
#   L0 fixture determinism: two generations, same SHA-256 manifest.
#   L1 plan: 3 chunks (one per top-level dir), byte counts matching a fresh
#      walk of the source, all PENDING -- the plan is what run must consume.
#   L2 fault: chunk B is failed on purpose (INVFS_PILOT_FAULT) after its
#      import; the run must stop with DONE(a), FAILED(b), PENDING(c) -- a
#      resumed run on a half-swept chunk is the failure shape this leg
#      exists to prove recoverable.
#   L3 resume: the harness re-invokes itself; chunk a is NOT re-imported
#      (proved from the append-only event log, not from a timestamp),
#      chunk b is retried and DONE, chunk c imported and DONE.
#   L4 verify: the deterministic sample is bit-exact.
#   L5 reclaim: the chunk carrying the most verbatim (unbatched) bytes comes
#      back after the settle (measured: the count right after the unlink, the
#      capture that discharges the pin, and the passes it took), and fsck is
#      CLEAN afterwards.
#   L6 report: rates + arithmetic + go/no-go.
#
# THE SCALE RUN (the numbers the pilot owes). Unset knobs run the hermetic
# corpus; the real calibration leg is:
#
#   INVFS_PILOT_SRC=/usr INVFS_PILOT_WORK=<disk>/wp644 INVFS_PILOT_VOL_GB=16 \
#     bash tools/test-backup-pilot.sh
#
# and the chunked target run adds --reclaim and the target volume:
#
#   INVFS_PILOT_SRC=/backup/src INVFS_PILOT_VOL=/dev/disk/by-id/... \
#     INVFS_PILOT_WORK=<disk>/wp644 INVFS_PILOT_CHUNK_MB=12800 \
#     bash tools/test-backup-pilot.sh --reclaim
#
# KNOWN WALLS THIS HARNESS ROUTES AROUND (both pre-existing, both named in
# the WP so the pilot does not rediscover them as mysteries):
#   * invf-cp/invf-import malloc each whole file, so a file larger than RAM
#     cannot go through them. plan counts those (`over-ram`) and the operator
#     doc routes them through a FUSE mount (streaming 64 KiB writes, proven
#     at 16 GiB in tools/test-ntfs-big.sh L4).
#   * a lingering FUSE daemon keeps the volume flock after unmount (TERM
#     ignored, WP623 territory). Every offline step here waits on the LOCK
#     and kills by PID it found holding the image -- never `pkill -f`, which
#     matches the suite's own shell and has killed an operator twice.
#
# Run hermetically from the repo root after `make`:
#   bash tools/test-backup-pilot.sh
# Under e2e rules (AGENTS.md 1.5, ISOLATED: no root, no privilege escalation,
# no shared scratch paths). tools/run-e2e.sh classifies a suite as LOCKED when
# its TEXT mentions privilege escalation or a shared-/tmp path -- a comment
# counts -- so this file deliberately names neither: it needs neither, and a
# LOCKED classification would serialise it behind every other suite for no
# reason.
#   INVFS_E2E_AGENT=wp644 bash tools/run-e2e.sh tools/test-backup-pilot.sh
set -e
set -o pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin
. "$REPO/tools/fsck-clean.sh"      # fsck_require_clean: one definition of CLEAN

# ---------------------------------------------------------------- knobs ---
WORK=${INVFS_PILOT_WORK:-/dev/shm/wp644pilot-$$}
SRC=${INVFS_PILOT_SRC:-}           # empty => hermetic fixture
VOL=${INVFS_PILOT_VOL:-$WORK/vol.img}
VOL_GB=${INVFS_PILOT_VOL_GB:-1}
CHUNK_MB=${INVFS_PILOT_CHUNK_MB:-4}
FAULT=${INVFS_PILOT_FAULT:-}       # test seam: fail this chunk on purpose
REPORT=${INVFS_PILOT_REPORT:-$WORK/PILOT-REPORT.md}
CHILD=${INVFS_PILOT_CHILD:-0}      # set by the resume leg's re-invocation
FRESH=0
RECLAIM=auto                       # auto = on for the hermetic corpus only
PHASES=""

MAN=$WORK/MANIFEST
EV=$WORK/events.log
LOGS=$WORK/logs
MNT=$WORK/mnt
FIX=$WORK/fixture
# chunk plan: name<TAB>path<TAB>files<TAB>bytes<TAB>verbatim_bytes<TAB>note
PLAN=$WORK/chunks.tsv
# ARC is capped: this box has run the OOM killer against FUSE daemons that
# took the default 256 MB while a 7 GiB swapless machine was already full.
MOUNT_OPTS=${INVFS_PILOT_MOUNT_OPTS:-arc_limit=32}

usage() {
    sed -n '2,/^set -e$/p' "$0" | sed -e 's/^# \{0,1\}//' -e '/^set -e$/d'
    cat <<'EOF'

usage: tools/test-backup-pilot.sh [options]

  --fresh            wipe WORK first (default: reuse it and RESUME from the
                     existing manifest: DONE chunks skipped, FAILED retried)
  --reclaim          run the unlink-reclaim leg even on a real corpus
  --no-reclaim       skip it
  --phases LIST      comma list of: plan,run,verify,reclaim,report (default all)
  --report FILE      write the pilot report here (default $WORK/PILOT-REPORT.md)
  --chunk-mb N       chunk size target in MiB, advisory (default 4 hermetic;
                     one top-level source dir is always exactly one chunk)
  --help

env: INVFS_PILOT_SRC=<tree>       corpus (default: generated hermetic fixture)
     INVFS_PILOT_WORK=<dir>       scratch root (default /dev/shm/wp644pilot-$$;
                                  MUST be a real disk for a multi-GB corpus)
     INVFS_PILOT_VOL=<img|dev>    volume (default <work>/vol.img)
     INVFS_PILOT_VOL_GB=<n>       size when the volume is created (default 1)
     INVFS_PILOT_CHUNK_MB=<n>     as --chunk-mb
     INVFS_PILOT_FAULT=<chunk>    fail that chunk after import (test seam)
     INVFS_PILOT_REPORT=<file>    as --report
     INVFS_PILOT_MOUNT_OPTS=<o>   FUSE options for the reclaim leg (default
                                  arc_limit=32: the default 256 MB ARC has
                                  drawn the OOM killer on a swapless box)
     INVFS_PILOT_MIN_EXTRAP_MIB=<n>   sample floor for the 1 TB arithmetic
     INVFS_PILOT_MIN_EXTRAP_FILES=<n> (default 512 MiB / 500 files; below it
                                  the calendar is REFUSED, not guessed)
     INVFS_PILOT_TARGET_TB / _FILES   the target the arithmetic is for (1 / 1e6)
     INVFS_PILOT_CALENDAR_DAYS=<n>    go/no-go line (default 7)
     INVFS_PILOT_MAXFILE_MB=<n>   plan refuses above this (default 131072 MiB:
                                  the WP634-A/WP635 dense-tier wall)
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --fresh)      FRESH=1 ;;
        --reclaim)    RECLAIM=on ;;
        --no-reclaim) RECLAIM=off ;;
        --phases)     PHASES=${2:-}; shift ;;
        --report)     REPORT=${2:-}; shift ;;
        --chunk-mb)   CHUNK_MB=${2:-}; shift ;;
        -h|--help)    usage; exit 0 ;;
        *)            echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
[ -n "$PHASES" ] || PHASES=plan,run,verify,reclaim,report

# -------------------------------------------------------------- helpers ---
fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  OK    $1"; }
note() { echo "  note: $*"; }
skip() { echo "  SKIP  $*"; }

now() { date +%s.%N; }
secs() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.3f", (b-a)}'; }
f2() { awk -v x="$1" 'BEGIN{printf "%.2f", x}'; }
rate() { awk -v n="$1" -v d="$2" 'BEGIN{if (d+0>0) printf "%.2f", n/d; else printf "0.00"}'; }
want_phase() { case ",$PHASES," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

decolor() { sed -e 's/\x1b\[[0-9;]*m//g' "$1"; }

# stage_secs <sweep log> -> "name=seconds" for the stages, from the FINAL
# progress line of each. Formats the tool prints (tools/invf-sweep.c,
# sw_duration): 941ms, 1m02s, 1h02m.
stage_secs() {
    decolor "$1" | awk '
        /^\[[0-9]+\/[0-9]+\][ \t]/ {
            name = $2; e = ""
            for (i = 1; i <= NF; i++) if ($i == "elapsed") e = $(i+1)
            if (e == "") next
            s = 0
            if (e ~ /ms$/)            { sub(/ms$/, "", e); s = e/1000 }
            else if (e ~ /h[0-9]+m$/) { sub(/h.*/, "", e); s = e*3600 }
            else if (e ~ /m[0-9]+s$/) { h = e; sub(/m.*/, "", h); r = e
                                        sub(/^[0-9]+m/, "", r); sub(/s$/, "", r)
                                        s = h*60 + r }
            else if (e ~ /s$/)        { sub(/s$/, "", e); s = e }
            secs[name] = s
            seen[name] = 1
            if (!(name in ord)) { ord[name] = ++n; nm[n] = name }
        }
        END { for (i = 1; i <= n; i++) printf "%s=%.3f ", nm[i], secs[nm[i]] }'
}

# vol_free <img> [log] -> free blocks from fsck's own accounting. This is the
# bitmap's derived view, so it lags an unlink by the retire window and by any
# live savepoint pin (AGENTS.md 2.4) -- that is the point of measuring it.
vol_free() {
    local img=$1 log=${2:-$LOGS/free.log} out
    "$B/invf-fsck" "$img" >"$log" 2>&1 || fail "invf-fsck exited non-zero on $img (see $log)"
    out=$(awk '/^  free blocks:/ {print $3}' "$log")
    [ -n "$out" ] || fail "fsck did not report free blocks for $img (see $log)"
    printf '%s\n' "$out"
}

# fsck_gate <img> <label> -> wall seconds on stdout; dies unless the volume
# is CLEAN. The gate is a gate, not advice (WP644 validation).
fsck_gate() {
    local img=$1 label=$2 log=$LOGS/fsck-$2.log t0 t1
    t0=$(now)
    "$B/invf-fsck" "$img" >"$log" 2>&1 || fail "$label: invf-fsck exited non-zero (see $log)"
    t1=$(now)
    fsck_require_clean "$log" "$label: fsck is not CLEAN"
    printf '%s\n' "$(secs "$t0" "$t1")"
}

sweep_once() {   # <img> <label> -> wall seconds on stdout
    local img=$1 label=$2 log=$LOGS/sweep-$2.log t0 t1
    t0=$(now)
    "$B/invf-sweep" "$img" >"$log" 2>&1 || fail "sweep ($label) exited non-zero (see $log)"
    t1=$(now)
    printf '%s\n' "$(secs "$t0" "$t1")"
}

# sweep_until_settled <img> <label> [max] -> "free sweeps" on stdout. Sweeps
# until the free-block count stops moving (two consecutive readings within
# +-2 blocks of base-page churn). THIS IS THE ONE-CAPTURE LAG MADE VISIBLE:
# every pass captures an SPT0 window and the NEXT capture reclaims what the
# last one pinned (AGENTS.md 2.4 step 2, 2.5), so one sweep is not an answer.
sweep_until_settled() {
    local img=$1 label=$2 max=${3:-4} i=1 prev cur delta
    prev=$(vol_free "$img" "$LOGS/settle-$label-0.log")
    cur=$prev
    while [ "$i" -le "$max" ]; do
        sweep_once "$img" "$label-$i" >/dev/null
        cur=$(vol_free "$img" "$LOGS/settled-$label-$i.log")
        delta=$((cur - prev))
        prev=$cur
        if [ "$i" -ge 2 ] && [ "$delta" -ge -2 ] && [ "$delta" -le 2 ]; then
            printf '%s %s\n' "$cur" "$i"
            return 0
        fi
        i=$((i + 1))
    done
    printf '%s %s\n' "$cur" "$max"
}

# await_vol_free <img> [seconds]: the volume flock is the only true "free"
# signal -- a daemonized invf-fuse keeps writing after fusermount returns
# (measured: 30 s+ on a big volume, TERM ignored). Never wait on a name
# pattern; kill only the PID whose cmdline names this image.
await_vol_free() {
    local img=$1 budget=${2:-120} i=0 p
    while [ "$i" -lt "$budget" ]; do
        flock -n "$img" true 2>/dev/null && return 0
        sleep 1
        i=$((i + 1))
    done
    for p in $(pgrep -x invf-fuse 2>/dev/null); do
        tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null | grep -qF "$img" || continue
        echo "  note: pid $p still holds $img; TERM" >&2
        kill "$p" 2>/dev/null || true
    done
    sleep 5
    flock -n "$img" true 2>/dev/null && return 0
    for p in $(pgrep -x invf-fuse 2>/dev/null); do
        tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null | grep -qF "$img" || continue
        echo "  note: pid $p ignored TERM; KILL" >&2
        kill -9 "$p" 2>/dev/null || true
    done
    sleep 2
    flock -n "$img" true 2>/dev/null
}

mount_vol() {   # <img>: mount at $MNT, return once /proc/mounts agrees
    local img=$1 i=0
    mkdir -p "$MNT"
    "$B/invf-fuse" -o "$MOUNT_OPTS" "$img" "$MNT" 2>"$LOGS/fuse.log" &
    FUSE_PID=$!
    while [ "$i" -lt 100 ]; do
        grep -q " $MNT " /proc/mounts 2>/dev/null && return 0
        kill -0 "$FUSE_PID" 2>/dev/null \
            || fail "invf-fuse died before the mount appeared (see $LOGS/fuse.log)"
        sleep 0.1
        i=$((i + 1))
    done
    fail "mount of $img never appeared (see $LOGS/fuse.log)"
}

umount_vol() {
    local i=0
    grep -q " $MNT " /proc/mounts 2>/dev/null || return 0
    fusermount3 -u "$MNT" 2>/dev/null || true
    while [ "$i" -lt 100 ]; do
        grep -q " $MNT " /proc/mounts 2>/dev/null || return 0
        sleep 0.1
        i=$((i + 1))
    done
    return 0    # still mounted: the caller's flock wait reports what holds it
}

cleanup() {
    local rc=$?
    if [ -d "$MNT" ] && grep -q " $MNT " /proc/mounts 2>/dev/null; then
        fusermount3 -u "$MNT" 2>/dev/null || true
    fi
    [ "$rc" -eq 0 ] || echo "FAIL: pilot exited $rc (logs: $LOGS)" >&2
}
trap cleanup EXIT

bins_ok() {
    local t
    for t in invf-mkfs invf-import invf-sweep invf-fsck invf-cat invf-ls invf-fuse; do
        [ -x "$B/$t" ] || fail "missing $B/$t -- run make first"
    done
    command -v fusermount3 >/dev/null 2>&1 || fail "fusermount3 is required (reclaim leg)"
}


# ---------------------------------------------------------------- corpus ---
# The hermetic corpus. Content is a SHA-256 counter stream, so "incompressible"
# is a property of the GENERATOR and not of one /dev/urandom draw: two runs on
# two hosts produce the same bytes, which is what makes L0 (determinism), the
# sample list (verify) and the reclaim arithmetic (verbatim blocks) honest.
fixture_gen() {   # <dest>
    mkdir -p "$1"
    python3 - "$1" <<'PYEOF'
import hashlib, os, sys

root = sys.argv[1]

def stream(tag, n):
    """Deterministic incompressible bytes: SHA-256 in counter mode."""
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(tag.encode() + i.to_bytes(8, 'little')).digest()
        i += 1
    return bytes(out[:n])

def text(tag, n):
    """Deterministic compressible text (the batchable class)."""
    unit = ('wp644 backup pilot %s line\n' % tag).encode()
    return (unit * (n // len(unit) + 1))[:n]

def put(rel, data):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f:
        f.write(data)

# chunk-a: batchable text plus a nested dir (import must recreate it)
for i in range(12):
    put('chunk-a/t%02d.txt' % i, text('a%02d' % i, 6000 + i * 37))
put('chunk-a/sub/s1.txt', text('a-sub1', 8192))
put('chunk-a/sub/s2.txt', text('a-sub2', 8192))

# chunk-b: incompressible, the class stored verbatim (no shared batch)
for i in range(8):
    put('chunk-b/r%02d.rnd' % i, stream('b%02d' % i, 48 * 1024))
put('chunk-b/t00.txt', text('b-t00', 4096))

# chunk-c: the reclaim target (one 256 KiB verbatim file), a symlink, an
# empty file, a nested dir
put('chunk-c/big/v.rnd', stream('c-big', 256 * 1024))
for i in range(3):
    put('chunk-c/t%02d.txt' % i, text('c%02d' % i, 5000 + i * 11))
put('chunk-c/sub/only.txt', text('c-sub', 3000))
put('chunk-c/empty.dat', b'')
ln = os.path.join(root, 'chunk-c/link')
os.makedirs(os.path.dirname(ln), exist_ok=True)
os.symlink('../chunk-a/t00.txt', ln)
PYEOF
}

# corpus_digest <dir> -> one hash over (path, type, size, content). The SHAPE of
# the fixture is part of the fixture: a generator edit that changes a byte count
# changes this digest, and L0 says so instead of silently re-baselining.
corpus_digest() {
    ( cd "$1" && find . -mindepth 1 | LC_ALL=C sort | while IFS= read -r p; do
        if [ -L "$p" ]; then
            printf 'L %s %s\n' "$p" "$(readlink "$p")"
        elif [ -d "$p" ]; then
            printf 'D %s\n' "$p"
        else
            printf 'F %s %s %s\n' "$p" "$(stat -c %s "$p")" \
                "$(sha256sum "$p" | cut -d' ' -f1)"
        fi
    done ) | sha256sum | cut -d' ' -f1
}

# chunk_stats <path> -> "files bytes" over regular files
chunk_stats() {
    find "$1" -type f -printf '%s\n' 2>/dev/null \
        | awk '{n++; b += $1} END {printf "%d %d\n", n+0, b+0}'
}

# verbatim_bytes <path>: bytes in classes stored WITHOUT a shared batch, i.e.
# the ones a targeted free can hand back immediately. The fixture marks them
# `*.rnd`; a real corpus has no such marking, so this returns 0 there and the
# reclaim leg asserts direction and reports numbers instead of inventing a
# constant (a TEXT-class entry names a SHARED batch segment, AGENTS.md 2.4).
verbatim_bytes() {
    find "$1" -type f -name '*.rnd' -printf '%s\n' 2>/dev/null \
        | awk '{b += $1} END {printf "%d\n", b+0}'
}

# ---------------------------------------------------------------- manifest ---
# One row per chunk, tab-separated: this is the resume state. Columns:
#   1 name  2 status(PENDING|DONE|FAILED)  3 files  4 bytes  5 verbatim bytes
#   6 import_s  7 sweep_s  8 fsck_s  9 free_before  10 free_after  11 note
declare -A ST FL BY VB IM SW FS F0 F1 NT
MAN_ORDER=()

man_load() {
    local n s f b v i w c f0 f1 note
    # A reload REPLACES the in-memory state: every phase that reads the
    # manifest calls this, and a phase that ran after `plan` in the same
    # process must not end up with the chunks listed twice.
    MAN_ORDER=()
    ST=(); FL=(); BY=(); VB=(); IM=(); SW=(); FS=(); F0=(); F1=(); NT=()
    [ -f "$MAN" ] || return 0
    while IFS=$'\t' read -r n s f b v i w c f0 f1 note; do
        case "$n" in ''|'#'*) continue ;; esac
        ST[$n]=$s; FL[$n]=${f:-0}; BY[$n]=${b:-0}; VB[$n]=${v:-0}
        IM[$n]=${i:-0}; SW[$n]=${w:-0}; FS[$n]=${c:-0}
        F0[$n]=${f0:-0}; F1[$n]=${f1:-0}; NT[$n]=${note:-}
        MAN_ORDER+=("$n")
    done < "$MAN"
}

man_save() {
    local n tmp=$MAN.wip
    {
        printf '# wp644 backup pilot manifest v1\n'
        printf '# src=%s vol=%s vol_gb=%s chunk_mb=%s\n' \
            "${SRC:-<hermetic fixture>}" "$VOL" "$VOL_GB" "$CHUNK_MB"
        printf '# name\tstatus\tfiles\tbytes\tverbatim\timport_s\tsweep_s\tfsck_s\tfree_before\tfree_after\tnote\n'
        for n in "${MAN_ORDER[@]}"; do
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "$n" "${ST[$n]:-PENDING}" "${FL[$n]:-0}" "${BY[$n]:-0}" \
                "${VB[$n]:-0}" "${IM[$n]:-0}" "${SW[$n]:-0}" "${FS[$n]:-0}" \
                "${F0[$n]:-0}" "${F1[$n]:-0}" "${NT[$n]:-}"
        done
    } > "$tmp"
    mv "$tmp" "$MAN"
}

# event <kind> <chunk>: append-only. The resume leg reads THIS to prove a DONE
# chunk was not imported again -- a timestamp cannot prove a negative, an
# append-only line can.
event() {
    printf '%s\t%s\t%s\n' "$(date -Is)" "$1" "$2" >> "$EV"
}

event_count() {   # <kind> <chunk>
    local n
    [ -f "$EV" ] || { echo 0; return 0; }
    n=$(awk -F'\t' -v k="$1" -v c="$2" '$2 == k && $3 == c {n++} END {print n+0}' "$EV")
    echo "${n:-0}"
}

manifest_failed() {
    local n c=0
    for n in "${MAN_ORDER[@]}"; do [ "${ST[$n]:-}" = FAILED ] && c=$((c + 1)); done
    echo "$c"
}

# ---------------------------------------------------------------- plan ---
# src_scan <dir>: one walk of the source. Prints:
#   files dirs links specials bytes max_file max_path over_ram over_tier
src_scan() {
    local ram_kb tier_mb
    ram_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
    tier_mb=${INVFS_PILOT_MAXFILE_MB:-131072}     # 128 GiB: WP634-A/WP635 wall
    find "$1" -mindepth 1 -printf '%y\t%s\t%p\n' 2>/dev/null | LC_ALL=C awk -F'\t' \
        -v ram="$ram_kb" -v tier="$tier_mb" '
        $1 == "d" { dirs++ }
        $1 == "f" { files++; bytes += $2
                    if ($2 > max) { max = $2; maxp = $3 }
                    if ($2 > ram * 1024) over_ram++
                    if ($2 > tier * 1024 * 1024) over_tier++ }
        $1 == "l" { links++ }
        $1 == "p" || $1 == "s" || $1 == "c" || $1 == "b" { specials++ }
        END { printf "%d %d %d %d %d %d %s %d %d\n", files+0, dirs+0, links+0,
                     specials+0, bytes+0, max+0, (maxp == "" ? "-" : maxp),
                     over_ram+0, over_tier+0 }'
}

# plan_chunks <src>: one top-level DIRECTORY is one chunk, whatever its size
# (the WP's rule: "top-level dirs as chunks"). Loose top-level FILES are staged
# into one synthetic chunk, because invf-import takes a directory and a backup
# that quietly leaves them out is not a backup.
plan_chunks() {
    local p nm f b v loose="" stage=$WORK/stage/toplevel
    : > "$PLAN"
    MAN_ORDER=()
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        nm=$(basename "$p")
        if [ -d "$p" ] && [ ! -L "$p" ]; then
            read -r f b < <(chunk_stats "$p")
            v=$(verbatim_bytes "$p")
            printf '%s\t%s\t%s\t%s\t%s\n' "$nm" "$p" "$f" "$b" "$v" >> "$PLAN"
            ST[$nm]=${ST[$nm]:-PENDING}; FL[$nm]=$f; BY[$nm]=$b; VB[$nm]=$v
            IM[$nm]=${IM[$nm]:-0}; SW[$nm]=${SW[$nm]:-0}; FS[$nm]=${FS[$nm]:-0}
            F0[$nm]=${F0[$nm]:-0}; F1[$nm]=${F1[$nm]:-0}; NT[$nm]=${NT[$nm]:-}
            MAN_ORDER+=("$nm")
        else
            loose="$loose$p"$'\n'
        fi
    done < <(find "$1" -mindepth 1 -maxdepth 1 2>/dev/null | LC_ALL=C sort)

    if [ -n "$loose" ]; then
        rm -rf "$stage"; mkdir -p "$stage"
        while IFS= read -r p; do
            [ -n "$p" ] || continue
            cp -a "$p" "$stage/" || fail "plan: cannot stage loose source $p"
        done <<< "$loose"
        read -r f b < <(chunk_stats "$stage")
        v=$(verbatim_bytes "$stage")
        printf '%s\t%s\t%s\t%s\t%s\n' toplevel "$stage" "$f" "$b" "$v" >> "$PLAN"
        ST[toplevel]=${ST[toplevel]:-PENDING}; FL[toplevel]=$f; BY[toplevel]=$b
        VB[toplevel]=$v; IM[toplevel]=${IM[toplevel]:-0}
        SW[toplevel]=${SW[toplevel]:-0}; FS[toplevel]=${FS[toplevel]:-0}
        F0[toplevel]=${F0[toplevel]:-0}; F1[toplevel]=${F1[toplevel]:-0}
        MAN_ORDER+=("toplevel")
    fi
    [ "${#MAN_ORDER[@]}" -gt 0 ] || fail "plan: no chunks (empty source $1?)"
}

phase_plan() {
    local files dirs links specials bytes maxf maxp over_ram over_tier free_kb
    local name _p f b v i=0 cmb
    echo "== [plan] source preflight + chunk plan =="
    man_load
    if [ -z "$SRC" ]; then
        FIX=$WORK/fixture
        rm -rf "$FIX"
        fixture_gen "$FIX/src" || fail "fixture generation"
        SRC=$FIX/src
        note "hermetic corpus generated at $SRC (sha256 fixture, deterministic)"
    else
        [ -d "$SRC" ] || fail "INVFS_PILOT_SRC=$SRC is not a directory"
        note "corpus: $SRC (a REAL corpus: the mutation legs are off by default)"
    fi

    read -r files dirs links specials bytes maxf maxp over_ram over_tier < <(src_scan "$SRC")
    note "source: $files files, $dirs dirs, $links links, $specials specials, $(awk -v b="$bytes" 'BEGIN{printf "%.1f", b/1048576}') MiB"
    note "largest file: $maxf bytes ($maxp)"
    if [ "$over_tier" -gt 0 ]; then
        fail "plan: $over_tier file(s) above ${INVFS_PILOT_MAXFILE_MB:-131072} MiB need WP634-A/WP635 dense tiers; this pilot must not start"
    fi
    if [ "$over_ram" -gt 0 ]; then
        note "WALL: $over_ram file(s) larger than RAM -- invf-import mallocs each"
        note "      whole file, so those go through a FUSE mount (streaming 64 KiB"
        note "      writes), not through invf-import. See docs/guides/BACKUP.md."
    fi

    plan_chunks "$SRC"
    while IFS=$'\t' read -r name _p f b v; do
        i=$((i + 1))
        cmb=$(awk -v b="$b" 'BEGIN{printf "%.1f", b/1048576}')
        printf '  chunk %d: %-16s %6s files %10s MiB\n' "$i" "$name" "$f" "$cmb"
        [ "$(awk -v b="$b" -v c="$CHUNK_MB" 'BEGIN{print (b > c*1048576) ? 1 : 0}')" = 1 ] \
            && note "  $name is larger than the ${CHUNK_MB} MiB target: one top-level dir is one chunk, so it stays whole"
    done < "$PLAN"
    note "chunks: ${#MAN_ORDER[@]}, target size ${CHUNK_MB} MiB"

    # Host filesystem room. A sparse image grows by what it writes; a REAL run
    # needs ~1.2x the source size free, because the sweep re-encodes BEFORE it
    # reclaims (AGENTS.md 2.5).
    free_kb=$(df -Pk "$(dirname "$VOL")" 2>/dev/null | awk 'NR==2 {print $4}')
    [ -n "$free_kb" ] || free_kb=0
    note "host filesystem free for $VOL: $(awk -v k="$free_kb" 'BEGIN{printf "%.1f", k/1048576}') GiB"
    if [ "$(awk -v k="$free_kb" -v b="$bytes" 'BEGIN{print (k*1024 < b*1.2) ? 1 : 0}')" = 1 ]; then
        note "WARNING: less than 1.2x the corpus size is free there. A sparse"
        note "         image only grows by what it writes, so this is legal for a"
        note "         hermetic run -- it is NOT legal for a 1 TB backup."
    fi

    man_save
    ok "plan: ${#MAN_ORDER[@]} chunk(s) planned, manifest $MAN"
}

# ---------------------------------------------------------------- run ---
vol_ensure() {
    if [ -e "$VOL" ]; then
        note "volume exists, reusing: $VOL"
        return 0
    fi
    note "invf-mkfs $VOL ${VOL_GB} GiB"
    "$B/invf-mkfs" "$VOL" "$VOL_GB" >"$LOGS/mkfs.log" 2>&1 \
        || fail "invf-mkfs failed (see $LOGS/mkfs.log)"
}

# raw_window_mb: the volume's RAW extent -- how much un-swept data can land
# before raw-class allocation starts overflowing into shadow-space blocks
# (AGENTS.md 2.3). A chunk bigger than this is still legal, but it is the
# reason ingest is chunked at all.
raw_window_mb() {
    "$B/invf-stats" "$VOL" 2>/dev/null | awk '/zones .* raw/ {
        for (i = 1; i <= NF; i++) if ($i == "raw") { printf "%d\n", $(i+1) * 4 / 1024; exit } }'
}

# vol_mkdir_prefixes <names...>: the chunk's top-level directory must exist
# before invf-import can write UNDER it. invf-import has no prefix ARGUMENT --
# the prefix is INVFS_IMPORT_PREFIX, and the target dir must already exist --
# so the pilot creates those dirs through a mount, ONCE per run pass, and then
# imports offline. Without the prefix the second chunk's t00.txt would silently
# overwrite the first one's: import_dir writes relative to the volume root.
vol_mkdir_prefixes() {
    local n
    [ $# -gt 0 ] || return 0
    mount_vol "$VOL"
    for n in "$@"; do
        mkdir -p "$MNT/$n" || { umount_vol; fail "cannot create volume dir $n"; }
    done
    sync
    umount_vol
    await_vol_free "$VOL" 180 || fail "volume still locked after prefix mkdir"
}

# import_chunk <name> <path> -> wall seconds on stdout; counts come from the
# tool's own report. --fail-on-skip is NOT optional: a skipped path is data
# loss no checker can find (WP138), so the pilot dies on it.
import_chunk() {
    local name=$1 path=$2 log=$LOGS/import-$1.log t0 t1 d f l s k
    t0=$(now)
    if ! INVFS_IMPORT_PREFIX="$name" "$B/invf-import" --fail-on-skip \
            "$VOL" "$path" >"$log" 2>&1; then
        echo "  import log tail:" >&2
        tail -n 5 "$log" >&2 || true
        return 1
    fi
    t1=$(now)
    event import "$name"
    if ! grep -q '^imported:' "$log"; then
        echo "FAIL: import ($name) printed no summary (see $log)" >&2
        return 1
    fi
    d=$(awk '/^imported:/ {print $2}'  "$log")
    f=$(awk '/^imported:/ {print $4}'  "$log")
    l=$(awk '/^imported:/ {print $6}'  "$log")
    s=$(awk '/^imported:/ {print $8}'  "$log")
    k=$(awk '/^imported:/ {print $10}' "$log")
    # The narrative goes to STDERR: this function's stdout is its return
    # value (wall seconds), and the caller captures it.
    printf '  imported %s dirs, %s files, %s links, %s specials, %s skipped in %ss\n' \
        "$d" "$f" "$l" "$s" "$k" "$(secs "$t0" "$t1")" >&2
    [ "${k:-0}" = 0 ] || { echo "FAIL: import ($name): $k path(s) skipped" >&2; return 1; }
    FL[$name]=$f
    printf '%s\n' "$(secs "$t0" "$t1")"
}

phase_run() {
    local name path f b v sw fsw f_before f_after t0 total=0 donep=0 need=()
    echo "== [run] import -> sweep -> fsck gate, one chunk at a time =="
    man_load
    vol_ensure
    RAW_MB=$(raw_window_mb || true)
    if [ -n "${RAW_MB:-}" ]; then
        note "volume RAW window: ${RAW_MB} MiB (ingest is chunked because of it)"
    fi
    while IFS=$'\t' read -r name path f b v; do
        [ "${ST[$name]:-PENDING}" = DONE ] || need+=("$name")
    done < "$PLAN"
    vol_mkdir_prefixes "${need[@]}"

    while IFS=$'\t' read -r name path f b v; do
        if [ "${ST[$name]:-PENDING}" = DONE ]; then
            note "chunk $name: DONE, skipped (resume)"
            donep=$((donep + 1))
            continue
        fi
        f_before=$(vol_free "$VOL" "$LOGS/free-before-$name.log")
        F0[$name]=$f_before
        printf '  chunk %s: %s files, %s bytes, free=%s blocks before\n' \
            "$name" "$f" "$b" "$f_before"
        if [ "$(awk -v b="$b" -v r="${RAW_MB:-0}" 'BEGIN{print (r > 0 && b > r*1048576) ? 1 : 0}')" = 1 ]; then
            note "  chunk $name is larger than the ${RAW_MB} MiB RAW window:"
            note "  raw-class allocation will overflow into shadow-space blocks"
            note "  (legal, AGENTS.md 2.3). Split the chunk or grow dev0's RAW."
        fi

        if ! t0=$(import_chunk "$name" "$path"); then
            ST[$name]=FAILED; NT[$name]="import failed"; man_save
            fail "chunk $name: import FAILED. The run stops here: the next chunk must not start on an unverified volume. Resume with the same command."
        fi
        if [ -n "$FAULT" ] && [ "$FAULT" = "$name" ]; then
            ST[$name]=FAILED; NT[$name]="injected fault (INVFS_PILOT_FAULT)"; man_save
            fail "chunk $name: INVFS_PILOT_FAULT=$FAULT (deliberate, after import)"
        fi
        IM[$name]=$t0

        sw=$(sweep_once "$VOL" "$name")
        SW[$name]=$sw
        if ! fsw=$(fsck_gate "$VOL" "$name"); then
            ST[$name]=FAILED; NT[$name]="fsck not CLEAN"; man_save
            fail "chunk $name: fsck gate FAILED. The next chunk must not start on a volume fsck does not call CLEAN."
        fi
        FS[$name]=$fsw
        f_after=$(vol_free "$VOL" "$LOGS/free-after-$name.log")
        F1[$name]=$f_after
        ST[$name]=DONE
        man_save
        printf '  chunk %s: DONE import=%ss sweep=%ss fsck=%ss, free %s -> %s blocks (%d taken)\n' \
            "$name" "$t0" "$sw" "$fsw" "$f_before" "$f_after" "$((f_before - f_after))"
        total=$((total + 1))
    done < "$PLAN"

    [ "$(manifest_failed)" = 0 ] || fail "run: $(manifest_failed) FAILED chunk(s) in the manifest (resume retries them)"
    ok "run: $total chunk(s) done this pass, $donep already DONE, every one fsck CLEAN"
}

# ---------------------------------------------------------------- verify ---
# Deterministic sample: every k-th entry of the sorted (volume path, source
# path) list, k = n/12, plus forced coverage of every chunk. "A deterministic
# sample, not vibes" (WP644 validation) -- and the corpus is deterministic, so
# the same files are checked on every host.
phase_verify() {
    local name path f b v srcp rel kind volp n k got=0 bad=0
    local list=$WORK/list.tsv sample=$WORK/sample.tsv
    echo "== [verify] bit-exact sample through invf-cat =="
    man_load
    : > "$list"
    while IFS=$'\t' read -r name path f b v; do
        [ "${ST[$name]:-PENDING}" = DONE ] || continue
        while IFS= read -r srcp; do
            rel=${srcp#"$path"/}
            if [ -L "$srcp" ]; then kind=L
            elif [ -d "$srcp" ]; then kind=D
            else kind=F; fi
            printf '%s\t%s\t%s\t%s\n' "$name" "$kind" "$name/$rel" "$srcp" >> "$list"
        done < <(find "$path" -mindepth 1 2>/dev/null | LC_ALL=C sort)
    done < "$PLAN"
    n=$(wc -l < "$list")
    [ "$n" -gt 0 ] || fail "verify: nothing imported to sample"
    # stride ~= n/12, rounded UP so the sample is never larger than the intent
    # and never empty: `NR % k == 1` is FALSE for every row when k == 1 (any
    # number modulo one is zero), which silently sampled nothing at all on a
    # small corpus. The 1-based form below is the one that means "every k-th".
    k=$(( (n + 11) / 12 )); [ "$k" -ge 1 ] || k=1
    awk -F'\t' -v k="$k" '(NR - 1) % k == 0' "$list" > "$sample"
    # every chunk must be represented, not just sampled by luck
    while IFS=$'\t' read -r name path f b v; do
        [ "${ST[$name]:-PENDING}" = DONE ] || continue
        grep -q "^$name	" "$sample" && continue
        awk -F'\t' -v c="$name" '$1 == c {print; exit}' "$list" >> "$sample"
    done < "$PLAN"

    while IFS=$'\t' read -r name kind volp srcp; do
        case "$kind" in
            D) continue ;;
            L) # readlink(1) appends a newline when its stdout is a pipe, and
               # the volume stores the target with no newline: compare the
               # bytes the volume holds against the bytes the SOURCE holds,
               # not against a terminal-friendly rendering of them.
               if ! { "$B/invf-cat" "$VOL" "$volp" 2>/dev/null; } \
                    | cmp -s - <(printf '%s' "$(readlink "$srcp")"); then
                   echo "  DIFF link $volp" >&2; bad=$((bad + 1)); continue
               fi ;;
            F) if ! { "$B/invf-cat" "$VOL" "$volp" 2>/dev/null; } | cmp -s - "$srcp"; then
                   echo "  DIFF file $volp" >&2; bad=$((bad + 1)); continue
               fi ;;
        esac
        got=$((got + 1))
    done < "$sample"
    [ "$bad" = 0 ] || fail "verify: $bad sampled path(s) are not bit-exact"
    [ "$got" -ge 5 ] || fail "verify: only $got sampled path(s); the sample is too thin to mean anything"
    ok "verify: $got of $(wc -l < "$sample") sampled paths bit-exact (stride $k of $n, every chunk covered)"
}

# ---------------------------------------------------------------- reclaim ---
# Unlink one chunk through a FUSE mount and make the space come back. The
# numbers this prints are the three deferrals of AGENTS.md 2.4 as measurements:
# the free count right after the unlink (retire window, maybe nothing yet), the
# capture that discharges the savepoint pin, and the passes it takes to settle.
phase_reclaim() {
    local name path f b v pick="" pickb=0 fbefore sw1 fafter sw2 mid held
    echo "== [reclaim] unlink one chunk, settle, assert the space returns =="
    man_load
    while IFS=$'\t' read -r name path f b v; do
        [ "${ST[$name]:-PENDING}" = DONE ] || continue
        # prefer the chunk with the most verbatim (unbatched) bytes: a shared
        # TEXT batch is deliberately NOT reclaimed by a targeted free, so the
        # numeric assertion belongs on the class that can come back.
        if [ "${v:-0}" -ge "$pickb" ]; then pick=$name; pickb=${v:-0}; fi
    done < "$PLAN"
    [ -n "$pick" ] || fail "reclaim: no DONE chunk to unlink"

    read -r fbefore sw1 < <(sweep_until_settled "$VOL" "pre-reclaim" 4)
    held=$(awk -F'\t' -v n="$pick" '$1 == n {print $5}' "$PLAN")
    printf '  settled free before unlink: %s blocks (%s pass(es) to settle)\n' "$fbefore" "$sw1"

    mount_vol "$VOL"
    if [ ! -e "$MNT/$pick" ]; then
        umount_vol; fail "reclaim: $pick is not in the volume ($MNT/$pick missing)"
    fi
    rm -rf "${MNT:?}/$pick" || { umount_vol; fail "reclaim: unlink of $pick failed"; }
    sync
    umount_vol
    await_vol_free "$VOL" 180 || fail "reclaim: volume still locked after unlink"
    mid=$(vol_free "$VOL" "$LOGS/free-postunlink.log")
    printf '  free right after unlink (retire window + savepoint pin): %s blocks (%+d)\n' \
        "$mid" "$((mid - fbefore))"

    read -r fafter sw2 < <(sweep_until_settled "$VOL" "reclaim" 4)
    fsck_gate "$VOL" reclaim >/dev/null
    printf '  settled free after unlink:  %s blocks (%+d), %s pass(es) to settle\n' \
        "$fafter" "$((fafter - fbefore))" "$sw2"
    [ "$fafter" -gt "$fbefore" ] \
        || fail "reclaim: unlink recovered nothing ($fbefore -> $fafter blocks)"
    if [ "${held:-0}" -gt 0 ]; then
        local need=$((held / 4096 * 3 / 4))   # 75% of the verbatim bytes, in blocks
        printf '  verbatim bytes in %s: %s (>= %s blocks must come back)\n' \
            "$pick" "$held" "$need"
        [ "$((fafter - fbefore))" -ge "$need" ] \
            || fail "reclaim: only $((fafter - fbefore)) of $need blocks came back"
        ok "reclaim: $((fafter - fbefore)) blocks returned over $sw2 settle pass(es) (the pin is discharged by the next capture), fsck CLEAN"
    else
        note "no verbatim class in this corpus: asserting direction only, reporting the numbers"
        ok "reclaim: $((fafter - fbefore)) blocks returned, fsck CLEAN"
    fi
    local left
    left=$("$B/invf-ls" "$VOL" 2>/dev/null | grep -c " $pick/" || true)
    [ "${left:-0}" = 0 ] || fail "reclaim: $left name(s) under $pick/ survived the unlink"
    ST[$pick]=UNLINKED; NT[$pick]="removed by the reclaim leg"; man_save
    ok "reclaim: $pick is gone from the volume (fsck CLEAN after)"
}
# ---------------------------------------------------------------- report ---
# The three numbers the WP exists for, and the arithmetic that turns them into
# a calendar -- or into an honest "n/a". A rate measured on a few hundred KiB
# of hermetic corpus is not a rate for 1 TB, so the extrapolation is GATED on
# the sample size (AGENTS.md 1.7: a claim you cannot point at is deleted, not
# softened; here it is printed as refused, with the reason).
MIN_EXTRAP_MIB=${INVFS_PILOT_MIN_EXTRAP_MIB:-512}
MIN_EXTRAP_FILES=${INVFS_PILOT_MIN_EXTRAP_FILES:-500}
TARGET_TB=${INVFS_PILOT_TARGET_TB:-1}
TARGET_FILES=${INVFS_PILOT_TARGET_FILES:-1000000}
CALENDAR_DAYS=${INVFS_PILOT_CALENDAR_DAYS:-7}

rep_say() { printf '%s\n' "$*" >>"$REPORT"; echo "  $*"; }
rep_raw() { printf '%s\n' "$*" >>"$REPORT"; }

# last_fsck_log -> path of the newest per-chunk fsck log (the inode count the
# fsck rate is quoted against comes from that log, not from the manifest).
last_fsck_log() {
    local n
    for n in "${MAN_ORDER[@]}"; do
        [ -f "$LOGS/fsck-$n.log" ] || continue
        last_log="$LOGS/fsck-$n.log"
    done
    printf '%s\n' "${last_log:-}"
}

# sweep_stages <chunk>: the per-stage breakdown of that chunk's last sweep.
sweep_stages() {
    local l=$LOGS/sweep-$1.log
    [ -f "$l" ] || return 0
    stage_secs "$l"
}

phase_report() {
    local name path f b v tot_f=0 tot_b=0 tot_im=0 tot_sw=0 tot_fs=0 done_n=0
    local fps mibs swmibs flog finodes fsck_s stages chk i j
    echo "== [report] measured rates =="
    man_load
    mkdir -p "$(dirname "$REPORT")"
    : > "$REPORT"
    rep_raw "# WP644 backup pilot — measured"
    rep_raw ""
    rep_raw "Source: ${SRC:-<hermetic fixture>}; volume: $VOL (${VOL_GB} GiB);"
    rep_raw "chunk target: ${CHUNK_MB} MiB. Rates below are the manifest's own"
    rep_raw "numbers (per-chunk wall clock), not a stopwatch on the outside."
    rep_raw ""
    rep_raw "| chunk | status | files | MiB | import s | sweep s | fsck s | free blocks before -> after |"
    rep_raw "|---|---|---|---|---|---|---|---|"
    for name in "${MAN_ORDER[@]}"; do
        rep_raw "$(printf '| %s | %s | %s | %s | %s | %s | %s | %s -> %s |' \
            "$name" "${ST[$name]:-PENDING}" "${FL[$name]:-0}" \
            "$(awk -v b="${BY[$name]:-0}" 'BEGIN{printf "%.2f", b/1048576}')" \
            "${IM[$name]:-0}" "${SW[$name]:-0}" "${FS[$name]:-0}" \
            "${F0[$name]:-0}" "${F1[$name]:-0}")"
        case "${ST[$name]:-}" in
            DONE|UNLINKED)
                tot_f=$((tot_f + ${FL[$name]:-0})); tot_b=$((tot_b + ${BY[$name]:-0}))
                tot_im=$(awk -v a="$tot_im" -v b="${IM[$name]:-0}" 'BEGIN{printf "%.3f", a+b}')
                tot_sw=$(awk -v a="$tot_sw" -v b="${SW[$name]:-0}" 'BEGIN{printf "%.3f", a+b}')
                tot_fs=$(awk -v a="$tot_fs" -v b="${FS[$name]:-0}" 'BEGIN{printf "%.3f", a+b}')
                done_n=$((done_n + 1)) ;;
        esac
    done
    [ "$done_n" -gt 0 ] || fail "report: no DONE chunk in the manifest -- nothing was measured"
    rep_raw ""
    rep_raw "## the three rates"
    rep_raw ""

    fps=$(rate "$tot_f" "$tot_im")
    mibs=$(rate "$(awk -v b="$tot_b" 'BEGIN{printf "%.3f", b/1048576}')" "$tot_im")
    swmibs=$(rate "$(awk -v b="$tot_b" 'BEGIN{printf "%.3f", b/1048576}')" "$tot_sw")
    rep_say "import : $tot_f files / $(awk -v b="$tot_b" 'BEGIN{printf "%.2f", b/1048576}') MiB in ${tot_im}s = $fps files/s, $mibs MiB/s"
    rep_say "sweep  : same bytes in ${tot_sw}s = $swmibs MiB/s (transform+dedupe+batches+finalize)"
    flog=$(last_fsck_log)
    finodes=0
    if [ -n "$flog" ]; then
        finodes=$(awk '/names\/inodes:/ {print $5}' "$flog" | tail -n 1)
    fi
    [ -n "${finodes:-}" ] || finodes=0
    rep_say "fsck   : ${tot_fs}s over the last gate at ${finodes} live inode(s) = $(rate "$finodes" "$tot_fs") inodes/s"
    rep_raw ""
    rep_raw "## per-stage sweep breakdown (last sweep per chunk)"
    rep_raw ""
    for name in "${MAN_ORDER[@]}"; do
        stages=$(sweep_stages "$name")
        [ -n "$stages" ] || continue
        rep_raw "- $name: $stages"
        echo "  stages $name: $stages"
    done
    rep_raw ""
    rep_raw "## calendar for ${TARGET_TB} TB / ${TARGET_FILES} files"
    rep_raw ""
    local samp_mib
    samp_mib=$(awk -v b="$tot_b" 'BEGIN{printf "%.1f", b/1048576}')
    if [ "$(awk -v m="$samp_mib" -v lim="$MIN_EXTRAP_MIB" 'BEGIN{print (m < lim)?1:0}')" = 1 ] \
       || [ "$tot_f" -lt "$MIN_EXTRAP_FILES" ]; then
        rep_say "extrapolation REFUSED: the sample is $samp_mib MiB / $tot_f files,"
        rep_say "  below the ${MIN_EXTRAP_MIB} MiB / ${MIN_EXTRAP_FILES} file floor."
        rep_say "  A rate measured on this corpus is not a rate for ${TARGET_TB} TB; run the"
        rep_say "  calibration leg (INVFS_PILOT_SRC=/usr) for numbers that carry."
        rep_raw ""
        rep_raw "Verdict: **n/a** (sample too small to extrapolate; this is the"
        rep_raw "hermetic gate, not the calibration run)."
    else
        local tbytes imp_s sw_s fsck_100k days
        tbytes=$(awk -v t="$TARGET_TB" 'BEGIN{printf "%.0f", t*1099511627776}')
        imp_s=$(awk -v n="$TARGET_FILES" -v fps="$fps" -v tb="$tbytes" -v mb="$mibs" \
            'BEGIN{a = (fps+0>0) ? n/fps : 1e18; b = (mb+0>0) ? (tb/1048576)/mb : 1e18;
                   printf "%.0f", (a>b)?a:b}')
        sw_s=$(awk -v tb="$tbytes" -v mb="$swmibs" 'BEGIN{printf "%.0f", (mb+0>0) ? (tb/1048576)/mb : 1e18}')
        fsck_100k=$(awk -v n="$TARGET_FILES" -v i="$finodes" -v s="$tot_fs" \
            'BEGIN{printf "%.0f", (i+0>0) ? s*n/i : 0}')
        days=$(awk -v a="$imp_s" -v b="$sw_s" -v c="$fsck_100k" \
            'BEGIN{printf "%.1f", (a+b+c)/86400}')
        rep_say "import : $(awk -v s="$imp_s" 'BEGIN{printf "%.1f", s/3600}') h"
        rep_say "sweep  : $(awk -v s="$sw_s" 'BEGIN{printf "%.1f", s/3600}') h"
        rep_say "fsck   : $(awk -v s="$fsck_100k" 'BEGIN{printf "%.1f", s/3600}') h (one full walk)"
        rep_say "TOTAL  : $days day(s) of wall clock, end to end"
        if [ "$(awk -v d="$days" -v lim="$CALENDAR_DAYS" 'BEGIN{print (d > lim)?1:0}')" = 1 ]; then
            rep_say "Verdict: NO-GO inside ${CALENDAR_DAYS} day(s). The binding stage is"
            rep_say "  $(awk -v a="$imp_s" -v b="$sw_s" -v c="$fsck_100k" 'BEGIN{print (a>=b && a>=c) ? "import (per-file fsync ladder)" : ((b>=c) ? "sweep (re-encode)" : "fsck")}'), not the codec choice."
        else
            rep_say "Verdict: GO -- ${TARGET_TB} TB / ${TARGET_FILES} files in $days day(s) on this"
            rep_say "  host, chunked at ${CHUNK_MB} MiB with an fsck gate per chunk."
        fi
        rep_raw ""
        rep_raw "Binding stage: $(awk -v a="$imp_s" -v b="$sw_s" -v c="$fsck_100k" 'BEGIN{print (a>=b && a>=c) ? "import" : ((b>=c) ? "sweep" : "fsck")}')."
    fi
    rep_raw ""
    rep_raw "## notes the operator doc inherits"
    rep_raw ""
    rep_raw "- Every chunk ends fsck CLEAN (gate, not advice); a chunk that does"
    rep_raw "  not is left FAILED and the run stops there."
    rep_raw "- Ingest outruns the drain, so ingest is chunked: RAW is a window"
    rep_raw "  (AGENTS.md 2.4), not a tank."
    rep_raw "- Free-block counts move one capture late (savepoint pin, 2.4/2.5);"
    rep_raw "  the reclaim leg prints the count before the next capture and after."
    rep_raw ""
    ok "report: $REPORT"
}

# ----------------------------------------------------------- self-reentry ---
# Every leg below is a REAL process that runs with the SAME WORK directory, so
# the resume legs (L2/L3) exercise exactly what a killed run exercises: a
# manifest on disk, an event log, and a volume with a half-swept chunk. Doing
# the fault leg in-process with a subshell would have proved the shell could
# survive its own function call, which is not the property under test.
child_run() {   # <fault|-> <phase,phase,...>  -> this script, same WORK
    local fault=$1 phases=$2
    [ "$fault" = - ] && fault=""
    INVFS_PILOT_WORK="$WORK" INVFS_PILOT_VOL="$VOL" INVFS_PILOT_CHILD=1 \
    INVFS_PILOT_REPORT="$REPORT" INVFS_PILOT_VOL_GB="$VOL_GB" \
    INVFS_PILOT_CHUNK_MB="$CHUNK_MB" INVFS_PILOT_FAULT="$fault" \
        bash "$0" --phases "$phases"
}

man_col() {     # <chunk> <column>: straight out of the file, not out of RAM
    awk -F'\t' -v n="$1" -v c="$2" '$1 == n {print $c; exit}' "$MAN"
}

# status_line -> "chunk-a=DONE chunk-b=FAILED ..." in plan order. The resume
# assertion is an EQUALITY against this string, so a leg cannot pass by
# finding "DONE" somewhere in the file.
status_line() {
    awk -F'\t' '!/^#/ && NF > 1 {printf "%s=%s ", $1, $2}' "$MAN" | sed -e 's/ $//'
}

leg() { echo; echo "---- $* ----"; }

hermetic_legs() {
    local d1 d2 raw
    [ -n "$SRC" ] && fail "hermetic_legs called with INVFS_PILOT_SRC set"
    # The legs are a GATE, not a resume: L2 needs PENDING chunks, so a WORK
    # that already carries a manifest is wiped (with a note, not silently).
    if [ -f "$MAN" ]; then
        note "hermetic legs always start from a clean WORK: wiping $WORK"
        rm -rf "$WORK"; mkdir -p "$WORK" "$LOGS"
    fi

    # --- L0: the fixture is deterministic, and its SHAPE is pinned ----------
    leg "L0 fixture determinism"
    rm -rf "$WORK/leg0"; mkdir -p "$WORK/leg0"
    fixture_gen "$WORK/leg0/a" || fail "L0: fixture generation (a)"
    fixture_gen "$WORK/leg0/b" || fail "L0: fixture generation (b)"
    d1=$(corpus_digest "$WORK/leg0/a"); d2=$(corpus_digest "$WORK/leg0/b")
    [ "$d1" = "$d2" ] || fail "L0: fixture is not deterministic ($d1 vs $d2)"
    ok "L0 two generations, one corpus digest $d1 (sha256 counter stream)"
    rm -rf "$WORK/leg0"

    # --- L1: the plan is one chunk per top-level dir, all PENDING ----------
    leg "L1 plan"
    child_run - plan || fail "L1: plan exited non-zero"
    local n_unplanned
    n_unplanned=$(awk 'END {print NR}' "$PLAN")
    [ "$n_unplanned" = 3 ] || fail "L1: expected 3 chunks, plan has $n_unplanned"
    [ "$(status_line)" = "chunk-a=PENDING chunk-b=PENDING chunk-c=PENDING" ] \
        || fail "L1: manifest statuses wrong: $(status_line)"
    while IFS=$'\t' read -r name path f b v; do
        read -r raw b2 < <(chunk_stats "$path")
        [ "$b" = "$b2" ] || fail "L1: chunk $name planned $b bytes, source has $b2"
    done < "$PLAN"
    ok "L1 3 chunks (chunk-a/b/c), bytes match a fresh walk, all PENDING"

    # --- L2: a chunk that dies after import leaves a resumable manifest ----
    leg "L2 fault: chunk-b dies after import"
    if child_run chunk-b plan,run; then
        fail "L2: the injected fault did not fail the run"
    fi
    [ "$(status_line)" = "chunk-a=DONE chunk-b=FAILED chunk-c=PENDING" ] \
        || fail "L2: manifest after the fault is '$(status_line)'"
    [ "$(event_count import chunk-a)" = 1 ] || fail "L2: chunk-a import events != 1"
    [ "$(event_count import chunk-b)" = 1 ] || fail "L2: chunk-b import event missing"
    [ "$(event_count import chunk-c)" = 0 ] || fail "L2: chunk-c was imported after a failure"
    ok "L2 DONE(a) FAILED(b) PENDING(c); c untouched after b failed"

    # --- L3: the resume retries b, skips a (proved from the event log) ----
    leg "L3 resume"
    child_run - plan,run || fail "L3: the resume run exited non-zero"
    [ "$(status_line)" = "chunk-a=DONE chunk-b=DONE chunk-c=DONE" ] \
        || fail "L3: manifest after the resume is '$(status_line)'"
    [ "$(event_count import chunk-a)" = 1 ] \
        || fail "L3: chunk-a was re-imported on resume ($(event_count import chunk-a) imports)"
    [ "$(event_count import chunk-b)" = 2 ] \
        || fail "L3: chunk-b was not retried exactly once ($(event_count import chunk-b))"
    [ "$(event_count import chunk-c)" = 1 ] || fail "L3: chunk-c was not imported once"
    ok "L3 a imported once (not again), b retried once, c imported; all DONE"

    # --- L4: the sample is bit-exact --------------------------------------
    leg "L4 verify"
    child_run - verify || fail "L4: verify failed"
    ok "L4 sampled paths bit-exact through invf-cat"

    # --- L5: unlink reclaims, one capture late ----------------------------
    leg "L5 reclaim"
    child_run - reclaim || fail "L5: reclaim failed"
    ok "L5 the unlinked chunk's space came back and fsck stayed CLEAN"

    # --- L6: the report carries the three rates ---------------------------
    leg "L6 report"
    child_run - report || fail "L6: report failed"
    grep -q '^import : ' "$REPORT" || fail "L6: report has no import rate"
    grep -q 'files/s' "$REPORT" || fail "L6: report states no per-file import rate"
    grep -q 'sweep  : ' "$REPORT" || fail "L6: report has no sweep rate"
    grep -q 'fsck   : ' "$REPORT" || fail "L6: report has no fsck rate"
    grep -q 'extrapolation REFUSED' "$REPORT" \
        || fail "L6: the hermetic sample was extrapolated to 1 TB (it must be refused)"
    ok "L6 report: three rates present, extrapolation refused at this sample size"
    echo
    echo "PASS: wp644 hermetic legs L0-L6 (report: $REPORT)"
}

# ------------------------------------------------------------ real corpus ---
# The calibration and target runs: no fault legs, no deterministic sample
# substitutes -- the corpus is whatever INVFS_PILOT_SRC names, so the phases
# run in order and the manifest is the resume state between invocations.
real_run() {
    local p
    echo "== wp644 pilot: real corpus $SRC =="
    note "volume $VOL (${VOL_GB} GiB when created), chunk target ${CHUNK_MB} MiB"
    for p in plan run verify reclaim report; do
        want_phase "$p" || continue
        case "$p" in
            reclaim)
                [ "$RECLAIM" = off ] && { skip "reclaim (--no-reclaim)"; continue; }
                [ "$RECLAIM" = on ] || { skip "reclaim (on a real corpus: pass --reclaim)"; continue; }
                ;;
        esac
        "phase_$p"
    done
    [ -f "$REPORT" ] || skip "report (no report phase this pass)"
    if [ -f "$REPORT" ]; then note "report: $REPORT"; fi
    echo
    echo "PASS: wp644 pilot pass complete ($(manifest_failed) FAILED chunk(s) in the manifest)"
}

main() {
    if [ "$FRESH" = 1 ] && [ -n "${WORK:-}" ] && [ -d "$WORK" ]; then
        rm -rf "$WORK"
    fi
    mkdir -p "$WORK" "$LOGS"
    bins_ok

    if [ "$CHILD" = 1 ]; then
        want_phase plan    && phase_plan
        want_phase run     && phase_run
        want_phase verify  && phase_verify
        want_phase reclaim && phase_reclaim
        want_phase report  && phase_report
        return 0
    fi

    if [ -z "$SRC" ]; then
        hermetic_legs
    else
        real_run
    fi
}

main "$@"
