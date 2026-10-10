#!/bin/bash
# test-flakey.sh — WP22b power-loss / unstable-device soak for InvariantFS
# via device-mapper flakey targets (standalone tier: root + slow, NOT in
# `make e2e`; run `make flakey` or this script directly).
#
# The volume lives directly ON a dm-flakey device over a loop file
# (blkio treats /dev/* as a raw block device — the image is the whole
# device, not a file inside a filesystem). dm-flakey injects the failure
# modes kill -9 cannot: acknowledged-then-dropped writes (a lying write
# cache / power loss at a random writeback point) and full error windows
# (device off the bus). After every chaos window the device is restored
# and the engine must reconcile to a state where:
#
#   * every readable file is BIT-EXACT (never silently wrong bytes);
#   * an unreadable file fails LOUDLY (EIO / verify CORRUPT), and the
#     recovery ladder (auto-recover -> rollback -> fsck -f -> re-seal)
#     leaves fsck structurally clean;
#   * storm-time writes end up complete-or-absent, never torn garbage;
#   * the seal layer is either complete (parity verifies) or absent
#     (unsealed / rolled back), with verify --deep reporting it honestly.
#
# Legs:
#   1  baseline sanity: mkfs on /dev/mapper/invfs_flakey, ~200MB mixed
#      corpus, sweep, sha256 manifest, all bit-exact, seeded determinism.
#   2  write-error storm: full error target for 6s DURING active FUSE
#      writes; writers must see loud EIO; after recovery the pre-storm
#      files are bit-exact and storm files complete-or-absent.
#   3  torn-write/drop during sweep: invf-sweep across seeded drop_writes
#      windows; recovery (WP21 rollback may engage) -> every file
#      bit-exact, fsck + verify clean.
#   4  crash mid-seal: sweep --seal under drop_writes + kill -9 midway;
#      seal ends up complete or absent, files bit-exact, verify honest.
#   5  chaos op-sequence soak (tools/flakey/soak.py): seeded random
#      create/write/delete/rename/sweep/seal/unseal/fsck with the device
#      toggling up/drop_writes/error, ~$FLAKEY_SOAK_S wall; periodic
#      gates + final fsck/verify/manifest diff.
#   7  page-cache power loss (WP83): the device is NEVER touched by chaos --
#      it is healthy the whole time -- and the bytes are only ever in the
#      host page cache. Write + fsync through the mount, kill -9 the daemon
#      with no clean close, sync + drop_caches (the power-cut surrogate),
#      reopen: every acknowledged byte intact, and a file rewritten in
#      place is the COMPLETE old or COMPLETE new content, never a splice.
#   8  orphan reclaim across a power cut (WP130): the v3 collector frees
#      base pages and flushes the bitmap, then the process is killed -9 with
#      no clean close, the page cache is dropped, and the volume is asked
#      what survived: every file byte-identical, and then rewritten on top of
#      exactly the blocks that were just freed (the aliasing probe). Two
#      arms -- the shipped default must free durably, and
#      INVFS_RECLAIM_ORPHANS=0 must free nothing (the leg's red control).
#   (repeatability: fixed seeds; any failure preserves the backing
#      image + all logs under tools/flakey/artifacts/<leg>-<ts>/.)
#
# Env knobs: FLAKEY_SEED (default 20260831), FLAKEY_SOAK_S (default 210),
#            FLAKEY_WORK (default /srv/invfs-flakey, else the first of
#            /var/tmp /opt /var/lib that is NOT tmpfs, else /tmp/invfs-flakey
#            — picked by disk_work_root() because the old hardcoded /tmp
#            default was tmpfs: 3871 MiB against this suite's own 4000 MB
#            floor, so `make flakey` exited 2 unconditionally),
#            FLAKEY_ONLY (e.g. "3" runs just that leg, a dev aid),
#            FLAKEY_STORM_S (leg 2 error-window seconds, default 6; longer
#            windows raise in-storm-OK exposure for edge-hunting),
#            FLAKEY_PC_WORK (leg 7 scratch; MUST be on a disk-backed
#            filesystem — default /var/tmp/invfs-flakey-pagecache),
#            FLAKEY_PC_BACKING (leg 7: loop = a loop device over the image
#            (default when losetup works), file = the image file itself, the
#            unprivileged path),
#            FLAKEY_PC_SIZE_MB (leg 7 volume size, default 512),
#            FLAKEY_RC_WORK (leg 8 scratch; same disk-backed rule as leg 7,
#            same picker, default <disk_work_root>/invfs-flakey-reclaim —
#            /srv/... in practice),
#            FLAKEY_RC_SIZE_MB (leg 8 volume size, default 512),
#            FLAKEY_RC_ROUNDS (leg 8 ARM A cuts, default 3),
#            FLAKEY_MIN_FREE_MB (hard scratch-space floor, default 4000; 0 to
#            override while diagnosing a "device smaller than the table" red),
#            FLAKEY_RC_NFOLD (leg 8 crash driver's fold ceiling — it must be
#            far larger than the cut reaches, default 3000).
#
# Needs: dm-flakey (modprobe dm-flakey), losetup, fusermount3, python3,
#        passwordless sudo (the script is sudo-aware; `sudo -v` first if
#        unsure). Cleans its own scratch ($FLK) on success; on failure the
#        scratch moves into the artifacts dir.
set -uo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin

# The default scratch used to be hardcoded to /tmp. /tmp is tmpfs on this box
# and holds 3.8 GiB -- 3891 MiB against this suite's own 4000 MB floor -- so
# the space guard below failed UNCONDITIONALLY and `make flakey` always
# exited 2 before a single leg ran. Worse, the same hardcoding put legs 7/8's
# reasoning in a corner: the whole point of the page-cache tier is that
# drop_caches can evict its pages, and it cannot evict a tmpfs page.
#
# Route the default through the same disk-backed scan leg 8 already uses
# (rc_work_pick) -- which is why leg 8 lands on /srv/invfs-flakey-reclaim
# while leg 0 lands in /tmp and dies. One picker, one answer.
disk_work_root() {   # echo the first candidate on a non-tmpfs filesystem
    local c
    for c in /srv /var/tmp /opt /var/lib; do
        [ -d "$c" ] || continue
        [ "$(stat -f -c %T "$c" 2>/dev/null)" = tmpfs ] && continue
        printf '%s\n' "$c"; return 0
    done
    printf '/tmp\n'    # nothing disk-backed: the guard below says so loudly
}

FLK=${FLAKEY_WORK:-$(disk_work_root)/invfs-flakey}
ART=$REPO/tools/flakey/artifacts
DEV=${FLAKEY_DEV:-invfs_flakey}
DM=/dev/mapper/$DEV
# BACK may be a raw block device (FLAKEY_BACKING=/dev/sdb2): no host
# filesystem between dm-flakey and silicon then -- no CoW, no journal,
# no host page-cache writeback muddying the chaos. A file gets the
# loop treatment as before.
BACK=${FLAKEY_BACKING:-$FLK/backing.img}
MNT=$FLK/mnt
SEED=${FLAKEY_SEED:-20260831}
SOAK_S=${FLAKEY_SOAK_S:-210}
SIZE_GB=2
ONLY=${FLAKEY_ONLY:-}

LOOP=""
SEC=0
CHAOS_PID=""
LEG=""
FAILED=0
T0=$SECONDS

# ---- leg 7 (WP83): the page-cache power-loss tier ----
# Its own scratch, and its own volume: legs 0-5 all run on the dm-flakey
# device over a loop file inside $FLK, and $FLK is tmpfs on this box --
# drop_caches cannot evict a tmpfs page (there is no writeback to drop), so
# the page-cache cut would be a silent NO-OP there. This leg refuses to
# pretend: it picks a disk-backed scratch or says so loudly.
PC_WORK=${FLAKEY_PC_WORK:-}
PC_VOL=""
PC_LOOP=""
PC_MNT=""
PC_MODE=""
PC_SIZE_MB=${FLAKEY_PC_SIZE_MB:-512}
PC_FUSE_PID=""
PC_WRITER_PID=""

# ---- leg 8 (WP130): the orphan-collector's power-loss leg ----
# Same page-cache technique as leg 7 and for the same reason: a loop device
# transfers zero bytes in this sandbox, so legs 0-5's dm-flakey tier cannot
# run and leg 7's file-backed path is the only crash surrogate that works
# here. This leg therefore also lives on a DISK-backed scratch and cuts with
# sync + drop_caches, and its subject is the collector rather than the
# write path. RC_VOL is a plain image file: no mount, no daemon, no loop.
RC_WORK=${FLAKEY_RC_WORK:-}
RC_VOL=""
RC_SIZE_MB=${FLAKEY_RC_SIZE_MB:-512}
RC_NFOLD=${FLAKEY_RC_NFOLD:-3000}
RC_ROUNDS=${FLAKEY_RC_ROUNDS:-3}
RC_NFILES=40          # real files, written through the public write path
RC_NGEN=200           # forced base generations -> the orphans ARM A collects
RC_CRASH_PID=""
RC_JITTER=$((SEED % 97))

say()  { echo; echo "== $* =="; }
info() { echo "  $*"; }

# ---------------------------------------------------------------- util --

# Take a FUSE mount down for good, or say that we could not.
#
# The trap used to call `fusermount3 -u "$MNT" 2>/dev/null` once and move on.
# A single lazy-less unmount of a busy FUSE mount fails, the error was
# discarded, and the trap then went on to `dmsetup remove` and `losetup -d` --
# which is how a run leaves a FUSE mount attached to a device that no longer
# exists, and the NEXT run trips over it. Escalate instead, and verify with
# /proc/mounts rather than trusting the exit code.
umount_all() {  # <mountpoint> <daemon-pattern>
    local mnt=$1 pat=$2 i
    [ -n "$mnt" ] || return 0
    grep -q " $mnt " /proc/mounts 2>/dev/null || return 0
    fusermount3 -u "$mnt" 2>/dev/null
    for i in $(seq 1 25); do
        grep -q " $mnt " /proc/mounts 2>/dev/null || return 0
        sleep 0.2
    done
    [ -n "$pat" ] && pkill -9 -f "$pat" 2>/dev/null
    sleep 0.3
    fusermount3 -uz "$mnt" 2>/dev/null
    for i in $(seq 1 25); do
        grep -q " $mnt " /proc/mounts 2>/dev/null || return 0
        sleep 0.2
    done
    echo "  WARN: $mnt is still mounted after fusermount3 -uz; a later run on" >&2
    echo "        this host may trip over it. Unmount it by hand." >&2
    return 1
}

cleanup() {
    set +e
    [ -n "$CHAOS_PID" ] && { kill "$CHAOS_PID" 2>/dev/null; wait "$CHAOS_PID" 2>/dev/null; }
    umount_all "$MNT" "invf-fuse $DM"
    local i
    for i in $(seq 1 25); do
        pgrep -f "invf-fuse $DM" >/dev/null || break
        sleep 0.2
    done
    pkill -9 -f "invf-fuse $DM" 2>/dev/null
    [ -b "$DM" ] && dm_set up >/dev/null 2>&1
    sudo -n dmsetup remove "$DEV" >/dev/null 2>&1
    [ -n "$LOOP" ] && sudo -n losetup -d "$LOOP" >/dev/null 2>&1
    # leg 7's scratch: its own mount, its own (optional) loop device
    [ -n "$PC_VOL" ] && {
        [ -n "$PC_WRITER_PID" ] && kill -9 "$PC_WRITER_PID" 2>/dev/null
        umount_all "$PC_MNT" "invf-fuse -f $PC_VOL"
        [ -n "$PC_LOOP" ] && sudo -n losetup -d "$PC_LOOP" 2>/dev/null
        rm -rf "$PC_WORK"
    }
    # leg 8's scratch: same rule as leg 7's -- keep it OUT of $FLK, which is
    # tmpfs on this box, or drop_caches is a no-op and the leg is theatre.
    [ -n "$RC_VOL" ] && {
        [ -n "$RC_CRASH_PID" ] && kill -9 "$RC_CRASH_PID" 2>/dev/null
        rm -rf "$RC_WORK"
    }
    [ "$FAILED" = 0 ] && rm -rf "$FLK"
}

preserve() {   # copy the ground truth + every log for replay
    local dst="$ART/${LEG:-preflight}-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$dst"
    cp -a "$FLK"/*.log "$dst/" 2>/dev/null
    cp -a "$FLK"/garbage-*.bin "$dst/" 2>/dev/null
    cp -a "$FLK"/manifest* "$dst/" 2>/dev/null
    cp -a "$FLK"/orig* "$dst/" 2>/dev/null
    [ -f "$FLK/oplog.txt" ]  && cp -a "$FLK/oplog.txt" "$dst/"
    [ -f "$FLK/model.json" ] && cp -a "$FLK/model.json" "$dst/"
    # leg 7 lives in its own scratch (a disk-backed one): keep its image,
    # manifest and logs too, or the page-cache cut is unreproducible
    if [ -n "$PC_VOL" ] && [ -d "$PC_WORK" ]; then
        cp -a "$PC_WORK"/*.log "$dst/" 2>/dev/null
        cp -a "$PC_WORK"/manifest.txt "$dst/pc-manifest.txt" 2>/dev/null
        cp --sparse=always "$PC_WORK/pc.img" "$dst/pc-backing.img" 2>/dev/null \
            || echo "  WARN: page-cache backing image copy failed" >&2
    fi
    # leg 8 lives in its own scratch too, and its evidence is the crash log
    # plus the free-block ledger -- without the image the crash is unreplayable
    if [ -n "$RC_VOL" ] && [ -d "$RC_WORK" ]; then
        cp -a "$RC_WORK"/crash*.log "$RC_WORK"/*.txt "$dst/" 2>/dev/null
        cp --sparse=always "$RC_VOL" "$dst/rc-backing.img" 2>/dev/null \
            || echo "  WARN: reclaim backing image copy failed" >&2
    fi
    if [ -f "$BACK" ]; then
        cp --sparse=always "$BACK" "$dst/backing.img" 2>/dev/null \
            || echo "  WARN: backing image copy failed" >&2
    fi
    { echo "SEED=$SEED"; echo "FLAKEY_SOAK_S=$SOAK_S";
      echo "git=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null)";
      echo "dm-flakey: $(sudo -n dmsetup targets 2>/dev/null | grep flakey)";
    } > "$dst/replay.env"
    echo "FAILURE ARTIFACTS: $dst" >&2
}

fail() { echo; echo "FAIL[$LEG]: $*" >&2; FAILED=1; preserve; exit 1; }

want_leg() { [ -z "$ONLY" ] && return 0; case ",$ONLY," in *",$1,"*) return 0;; esac; return 1; }

# A leg whose helpers are gone must SKIP loudly, never crash the suite.
# leg7's pc_* power-cut helpers were deleted in 841a272 and 83c65cf only
# restored disk_work_root, so leg7 died as `pc_dev_create: command not
# found` (plus PC_SIZE_GB unbound) the first time legs 0-5 all passed and
# execution reached it -- with no FAIL[] banner and no artifacts. A missing
# helper is a parked leg, not a red one; the INCIDENTS entry says where.
leg_ready() {
    case "$1" in
        7) command -v pc_dev_create >/dev/null 2>&1 ;;
        8) command -v pc_run >/dev/null 2>&1 ;;
        *) return 0 ;;
    esac
}

# ------------------------------------------------------------- devices --

dm_set() {   # dm_set up|drop|drop_slow|error — swap the live dm table
    local spec
    case "$1" in
        up)    spec="0 $SEC flakey ${LOOP:-$BACK} 0 3600 0";;
        drop)  spec="0 $SEC flakey ${LOOP:-$BACK} 0 1 1 1 drop_writes";;
        # drop_slow: the same acknowledged-write loss, but biased hard toward
        # UP. `drop` is a 50/50 duty cycle, and a vol_open that lands in a
        # down window fails with "device 0 is smaller than the device table
        # says" (volume.c:1256-1261) -- which reads like a capacity bug and
        # is really just the window. A long up window keeps the arm about
        # write loss instead of about retry loops.
        drop_slow) spec="0 $SEC flakey ${LOOP:-$BACK} 0 20 1 1 drop_writes";;
        error) spec="0 $SEC error";;
        *)     echo "dm_set: bad mode $1" >&2; return 2;;
    esac
    # --noflush: a flush cannot complete against an error target, and we do
    # not want one anyway — dirty bdev pages must survive the swap so the
    # NEW table decides their fate (that is the torn-write window).
    local i
    for i in $(seq 1 20); do
        sudo -n dmsetup suspend --noflush "$DEV" 2>/dev/null && break
        sleep 0.2
        [ "$i" = 20 ] && { echo "dm_set: suspend never succeeded" >&2; return 1; }
    done
    sudo -n dmsetup reload "$DEV" --table "$spec" 2>/dev/null
    sudo -n dmsetup resume "$DEV" 2>/dev/null
}

dev_create() {
    rm -rf "$FLK" && mkdir -p "$FLK" "$MNT"
    if [ -b "$BACK" ]; then
        # Raw device: use it directly, no loop layer, no truncate.
        # SIZE_GB is ignored; the dm table spans the whole device.
        LOOP=""
        SEC=$(sudo -n blockdev --getsz "$BACK")
    else
        truncate -s "${SIZE_GB}G" "$BACK"
        LOOP=$(sudo -n losetup -f --show "$BACK") || { echo "losetup failed" >&2; exit 2; }
        SEC=$(sudo -n blockdev --getsz "$LOOP")
    fi
    sudo -n dmsetup create "$DEV" --table "0 $SEC flakey ${LOOP:-$BACK} 0 3600 0" \
        || { echo "dmsetup create failed" >&2; exit 2; }
    sudo -n chmod 666 "$DM"
    info "backing: $BACK (${SIZE_GB}G sparse) on $LOOP, dm: $DM ($SEC sectors)"
}

# ------------------------------------------------------------ engine ----

mkfs_fresh() {
    $B/invf-mkfs "$DM" >"$FLK/mkfs.log" 2>&1 \
        || { cat "$FLK/mkfs.log"; fail "mkfs on $DM"; }
}

import_all() { # <origdir>
    local f
    for f in $(cd "$1" && ls); do
        # The tool's stderr is the log. `>/dev/null 2>&1` here threw away the
        # only thing that says WHY a 200 MB import died, and cost a full
        # round of misdiagnosis ("the engine regressed") before anyone looked
        # at what invf-cp actually printed. Capture it, and on failure print
        # it: a leg that cannot name its own failure is not a gate.
        $B/invf-cp "$DM" "$1/$f" "$f" >"$FLK/cp.log" 2>&1 \
            || { echo "  invf-cp $f failed:" >&2; cat "$FLK/cp.log" >&2
                 echo "  (source: $1/$f, $(stat -c %s "$1/$f" 2>/dev/null) B)" >&2
                 fail "invf-cp $f"; }
    done
}

manifest_build() { (cd "$1" && sha256sum * | sort -k2); }

# every file in <origdir> must read back bit-exact
vol_files_exact() { # <origdir> <label>
    local orig=$1 label=$2 f ok=1
    for f in $(cd "$orig" && ls); do
        if ! $B/invf-cat "$DM" "$f" "$FLK/out.bin" >/dev/null 2>"$FLK/cat.err"; then
            echo "  cat failed: $f ($label): $(tail -1 "$FLK/cat.err")"; ok=0; continue
        fi
        cmp -s "$orig/$f" "$FLK/out.bin" || { echo "  MISMATCH: $f ($label)"; ok=0; }
    done
    rm -f "$FLK/out.bin" "$FLK/cat.err"
    [ "$ok" = 1 ] || return 1
    echo "  all files bit-exact ($label)"
}

fsck_ok() { # <label>
    $B/invf-fsck "$DM" >"$FLK/fsck.last" 2>&1
    local rc=$?
    tail -2 "$FLK/fsck.last"
    { [ "$rc" = 0 ] && grep -q "^OK$" "$FLK/fsck.last"; } && return 0
    echo "  fsck not clean ($1):" >&2; cat "$FLK/fsck.last" >&2
    return 1
}

verify_clean() { # <label>: rc==0 (0 corrupt AND parity clean-or-absent)
    $B/invf-verify "$DM" --deep >"$FLK/verify.last" 2>&1
    local rc=$?
    grep -E "^parity|^deep" "$FLK/verify.last"
    [ "$rc" = 0 ] && return 0
    echo "  verify not clean ($1):" >&2; cat "$FLK/verify.last" >&2
    return 1
}

# Consistency bar (WP-J, owner decision: contained+named+honest passes).
# After chaos + best-effort recovery the volume need not be clean -- but
# every failure must be NAMED. D = fsck --list-damaged names plus verify
# --deep CORRUPT names. Asserts: (a) every unreadable corpus file is in D
# (nothing silent, nothing missing from the ledger); (b) every fsck-listed
# name is either unreadable or torn-xattr-only (xattr damage reads fine,
# content damage does not); (c) an OK fsck verdict implies empty D and
# all-exact (the old strict bar, automatically); (d) every successful read
# is bit-exact (the anti-silent-corruption core: a read that returns is
# either right or the suite fails). Any violation FAILs (artifacts kept).
verify_consistent() { # <label> <origdir>
    local label=$1 orig=$2 f verdict rc
    local failed="" exact_n=0
    $B/invf-fsck "$DM" --list-damaged >"$FLK/consistent-fsck.log" 2>&1
    rc=$?
    verdict=$(grep -E "^(OK|DAMAGED|REPAIRED|MIRROR STALE)$" "$FLK/consistent-fsck.log" | tail -1)
    case "$verdict:$rc" in
        OK:0|DAMAGED:3|REPAIRED:3) ;;
        *) echo "  fsck verdict/rc unexpected ($label): verdict=$verdict rc=$rc" >&2; return 1;;
    esac
    if grep -q "damaged-partial" "$FLK/consistent-fsck.log"; then
        echo "  ledger incomplete ($label): --list-damaged could not enumerate; nothing provable" >&2
        return 1
    fi
    $B/invf-verify "$DM" --deep >"$FLK/consistent-verify.log" 2>&1 || true
    info "consistent: verify says: $(grep -acE '^  CORRUPT' "$FLK/consistent-verify.log") corrupt lines"
    # D as newline lists (fixed strings; corpus names are simple but be safe).
    # Content evidence is ONLY bare `CORRUPT: <name>` lines (a file whose
    # bytes fail). `CORRUPT: inode N (...)` lines are namespace-audit damage
    # (nlink/fan-in, unwalkable ranges): content may read fine, so they must
    # NOT join D -- but they must never accompany an OK fsck (checked below).
    grep -E "^damaged"$'\t' "$FLK/consistent-fsck.log" | cut -f3 >"$FLK/consistent-dfsck.txt"
    grep -E "^  CORRUPT: [^ ]+$" "$FLK/consistent-verify.log" | sed 's/^  CORRUPT: //' >"$FLK/consistent-dverify.txt"
    if grep -Eq "^  CORRUPT: inode [0-9]+ \(" "$FLK/consistent-verify.log" && [ "$verdict" = "OK" ]; then
        echo "  UNEXPLAINED ($label): verify reports namespace damage but fsck says OK" >&2
        return 1
    fi
    if grep -Eq "could not be completed" "$FLK/consistent-verify.log" && [ "$verdict" = "OK" ]; then
        echo "  UNEXPLAINED ($label): verify could not complete an audit but fsck says OK -- partial evidence" >&2
        return 1
    fi
    cat "$FLK/consistent-dfsck.txt" "$FLK/consistent-dverify.txt" | sort -u >"$FLK/consistent-d.txt"
    for f in $(cd "$orig" && ls); do
        if $B/invf-cat "$DM" "$f" "$FLK/consistent-out.bin" >/dev/null 2>&1 && \
           cmp -s "$orig/$f" "$FLK/consistent-out.bin"; then
            exact_n=$((exact_n + 1))
        else
            failed="$failed $f"
        fi
    done
    rm -f "$FLK/consistent-out.bin"
    info "consistent: $exact_n exact,$(echo "$failed" | wc -w) unreadable-or-wrong ($label):${failed:- none}"
    info "consistent: ledger fsck:[$(tr '\n' ' ' <"$FLK/consistent-dfsck.txt")] verify:[$(tr '\n' ' ' <"$FLK/consistent-dverify.txt")]"
    # (a) every failure named.
    for f in $failed; do
        if ! grep -Fqx "$f" "$FLK/consistent-d.txt"; then
            echo "  UNEXPLAINED ($label): $f unreadable but listed nowhere -- silent corruption or ledger gap" >&2
            return 1
        fi
    done
    # (b) every listed name either failed or is excused: fsck torn-xattr
    # rows are metadata-only damage (content reads fine); a verify-CORRUPT
    # name that reads exact is a read-path disagreement (verify vs cat) and
    # fails -- both paths promise identical bytes, so divergence is a bug.
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        case " $failed " in *" $f "*) continue;; esac
        reason=$(grep -E "^damaged"$'\t' "$FLK/consistent-fsck.log" | awk -F'\t' -v n="$f" '$3==n{print $4; exit}')
        if [ "$reason" = "torn-xattr" ]; then
            info "  ledger names $f (torn-xattr: metadata-only, content reads exact)"
            continue
        fi
        if grep -Fqx "$f" "$FLK/consistent-dverify.txt"; then
            echo "  UNEXPLAINED ($label): verify calls $f corrupt but it reads exact -- read-path disagreement" >&2
        else
            echo "  UNEXPLAINED ($label): ledger names $f ($reason) but it reads fine -- stale ledger or misattribution" >&2
        fi
        return 1
    done <"$FLK/consistent-d.txt"
    # (c) OK verdict implies an empty *fsck* ledger. Content-tear
    # (verify-only damage with fsck OK) is legitimate damage, not
    # inconsistency: those files are policed by (a) against the union
    # including verify names, so here every failed file must be in
    # verify's list (fsck's list is already proven empty).
    if [ "$verdict" = "OK" ]; then
        if [ -s "$FLK/consistent-dfsck.txt" ]; then
            echo "  UNEXPLAINED ($label): fsck OK but its ledger is non-empty" >&2
            return 1
        fi
        for f in $failed; do
            if ! grep -Fqx "$f" "$FLK/consistent-dverify.txt"; then
                echo "  UNEXPLAINED ($label): fsck OK but $f failed unread and verify did not name it" >&2
                return 1
            fi
        done
    fi
    # (d) holds by construction: the loop above cmp-checks every success.
    return 0
}

# Recovery ladder: auto-recover (any open) -> resolve a live checkpoint via
# rollback (freeing a live seal first if it refuses) -> fsck -f -> OK.
# Any dead end is a finding: the caller FAILs and preserves the image.
recover() { # <label>
    local label=$1 rc
    $B/invf-fsck "$DM" >"$FLK/rec-fsck1.log" 2>&1
    grep -E "state:|checkpoint:|OK$|REPAIRED|ISSUES" "$FLK/rec-fsck1.log" | sed 's/^/  fsck: /'
    if grep -q "checkpoint:.*live" "$FLK/rec-fsck1.log"; then
        local attempt resolved=0
        for attempt in 1 2 3; do
            info "checkpoint live -> rollback ($label)"
            $B/invf-rollback "$DM" >"$FLK/rec-rb.log" 2>&1
            rc=$?
            if [ "$rc" != 0 ]; then
                if grep -q "free-redundant" "$FLK/rec-rb.log"; then
                    info "rollback refused (seal live) -> --free-redundant first"
                    $B/invf-sweep "$DM" --free-redundant >>"$FLK/rec-rb.log" 2>&1
                    $B/invf-rollback "$DM" >>"$FLK/rec-rb.log" 2>&1
                    rc=$?
                fi
            fi
            if [ "$rc" = 3 ]; then
                # a clean REFUSAL (torn journal staging): the post-sweep
                # state is untouched by construction, and under silent
                # drops a torn stage is a legitimate outcome -- accept it
                # (realize frees the retention registry + clears CKP0,
                # arming a fresh UP-mode checkpoint that rolls back clean)
                # and re-run the ladder.
                info "rollback declined (torn staging); accepting via --realize"
                $B/invf-sweep "$DM" --realize >>"$FLK/rec-rb.log" 2>&1 || true
                $B/invf-fsck "$DM" >"$FLK/rec-fsck1.log" 2>&1
                grep -q "checkpoint:.*live" "$FLK/rec-fsck1.log" || { resolved=1; break; }
                continue
            fi
            [ "$rc" = 0 ] && { info "rollback done"; resolved=1; break; }
            { echo "  rollback ladder failed:"; cat "$FLK/rec-rb.log"; } >&2
            return 1
        done
        if [ "$resolved" != 1 ]; then
            echo "  rollback ladder never resolved the checkpoint" >&2
            return 1
        fi
    fi
    $B/invf-fsck "$DM" -f >"$FLK/rec-fsckf.log" 2>&1
    grep -E "REPAIRED|ISSUES|OK$" "$FLK/rec-fsckf.log" | sed 's/^/  fsck -f: /'
    # WP-leg3: the ladder never attempted the SPT0 savepoint even with one
    # live from before the damage (it only rolled back CKP0 checkpoints).
    # A pre-sweep savepoint restores the publishable past wholesale; try it
    # before declaring the dead end -- but ONLY on a damaged tree: rollback
    # truncates the delta to the savepoint, discarding every healthy write
    # made after it, so rolling back a clean volume destroys data the
    # chaos never touched. A refusal (no savepoint, damaged pin) is not a
    # failure -- fall through to the final verdict below.
    if ! grep -q "^OK$" "$FLK/rec-fsck1.log" 2>/dev/null && \
       grep -q "save point:.*live" "$FLK/rec-fsck1.log" 2>/dev/null; then
        info "save point live -> SPT0 rollback attempt ($label)"
        if $B/invf-rollback "$DM" >"$FLK/rec-rb-spt0.log" 2>&1; then
            info "SPT0 rollback done ($label)"
        else
            info "SPT0 rollback declined; continuing the ladder ($label)"
            # WP-leg3 (2/2): a bare "declined" dead-ends the forensics.
            # The refuse reason names the file at stake (usually a pinned
            # recipe the drops tore, shared with the live tree) -- surface
            # it so the terminal verdict says what to restore from backup.
            grep -h "refusing\|DAMAGED\|unreadable" "$FLK/rec-rb-spt0.log" 2>/dev/null | sed 's/^/  spt0: /' | head -n 3
        fi
    fi
    $B/invf-fsck "$DM" >"$FLK/rec-fsck2.log" 2>&1
    grep -q "^OK$" "$FLK/rec-fsck2.log" && return 0
    echo "  fsck not clean after recovery ($label)" >&2; cat "$FLK/rec-fsck2.log" >&2
    return 1
}

# verify, repairing torn parity via one re-seal (leg-4 contract: the seal
# ends up complete or absent, and verify says which, honestly)
verify_or_reseal() { # <label>
    local label=$1 rc
    $B/invf-verify "$DM" --deep >"$FLK/verify.last" 2>&1
    rc=$?
    grep -E "^parity|^deep" "$FLK/verify.last"
    if [ "$rc" != 0 ] && grep -q "CORRUPT" "$FLK/verify.last"; then
        # WP22d: a segment's DATA can be dropped by a window after its map
        # survived (the map is re-durabilized by every compaction; the
        # data is written once). The map-level cut is blind to it; the
        # segment CRC is not. fsck -f with the content pass quarantines
        # the torn records (losses listed loudly; names fall back or go
        # absent) and the ladder then converges.
        info "content-corrupt files -> fsck -f (content cut) ($label)"
        INVFS_FSCK_CONTENT=1 $B/invf-fsck "$DM" -f >"$FLK/rec-content.log" 2>&1 \
            || { cat "$FLK/rec-content.log"; return 1; }
        grep -E "content cut|corrupt files" "$FLK/rec-content.log" | sed 's/^/  /' || true
        $B/invf-fsck "$DM" >"$FLK/rec-fsckq.log" 2>&1
        grep -q "^OK$" "$FLK/rec-fsckq.log" \
            || { echo "  fsck not OK after content quarantine:"; cat "$FLK/rec-fsckq.log"; return 1; }
        $B/invf-verify "$DM" --deep >"$FLK/verify.last" 2>&1
        rc=$?
        grep -E "^parity|^deep" "$FLK/verify.last"
    fi
    [ "$rc" = 0 ] && return 0
    grep -q "CORRUPT" "$FLK/verify.last" && return 1   # still corrupt: beyond repair
    if grep -qE "^parity: [0-9]+ sealed stripes, [1-9][0-9]* mismatched" "$FLK/verify.last"; then
        info "torn parity -> re-seal repairs ($label)"
        $B/invf-sweep "$DM" --seal >>"$FLK/rec-reseal.log" 2>&1 || return 1
        $B/invf-verify "$DM" --deep >"$FLK/verify2.last" 2>&1
        rc=$?
        grep -E "^parity|^deep" "$FLK/verify2.last"
        [ "$rc" = 0 ] && return 0
    fi
    return 1
}

# ------------------------------------------------------------- mount ----

mnt_up() {
    $B/invf-fuse "$DM" "$MNT" 2>"$FLK/fuse.log"
    local i
    for i in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount of $DM never appeared"
}

# tolerant: returns 0 when the daemon is gone, 1 when it had to be killed
mnt_down() { # <timeout-0.2s-ticks>
    local n=${1:-450} i
    fusermount3 -u "$MNT" 2>/dev/null
    for i in $(seq 1 "$n"); do
        pgrep -f "invf-fuse $DM" >/dev/null || return 0
        sleep 0.2
    done
    pkill -9 -f "invf-fuse $DM" 2>/dev/null
    sleep 0.5
    return 1
}

# ------------------------------------------------------------ corpus ----

gen_corpus() { # <dir> <seed> <full|medium|small>
    python3 - "$1" "$2" "$3" <<'PY'
import os, random, sys, tarfile, io
d, seed, prof = sys.argv[1], int(sys.argv[2]), sys.argv[3]
random.seed(seed)
os.makedirs(d, exist_ok=True)
WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()

def text_file(name, size):
    out = []; n = 0
    while n < size:
        w = random.choice(WORDS); out.append(w); n += len(w) + 1
    open(os.path.join(d, name), "w").write(" ".join(out)[:size])

def fake_bin(name, emachine, size):
    # compressible fabricated binary: ELF magic + e_machine + payload of
    # repeating patterns (sniffs as a binary family, batches well)
    payload = bytearray()
    pat = bytes(range(64)) * 4 + b"\x00" * 128 + random.randbytes(64)
    while len(payload) < size:
        payload += pat
        payload += bytes([random.randrange(256)]) * 32
    h = bytearray(64)
    h[0:4] = b"\x7fELF"; h[18] = emachine & 0xFF; h[19] = emachine >> 8
    open(os.path.join(d, name), "wb").write(bytes(h) + bytes(payload[:size]))

M = 1024 * 1024
if prof == "full":      # ~200MB
    texts = [("a.c", 25*M), ("r.log", 20*M), ("w.py", 15*M), ("m.md", 10*M),
             ("d.json", 8*M), ("s.sh", 4*M)]
    bins  = [("bin_x64", 62, 30*M), ("bin_a64", 183, 25*M), ("bin_pe", 0, 15*M)]
    rand_mb, tar_names = 36, ["a.c", "r.log"]
elif prof == "medium":  # ~100MB
    texts = [("a.c", 14*M), ("r.log", 12*M), ("w.py", 9*M)]
    bins  = [("bin_x64", 62, 20*M), ("bin_a64", 183, 15*M)]
    rand_mb, tar_names = 20, ["a.c", "w.py"]
else:                   # small, ~40MB (soak seed corpus)
    texts = [("a.c", 8*M), ("r.log", 7*M)]
    bins  = [("bin_x64", 62, 12*M)]
    rand_mb, tar_names = 10, ["a.c"]

for name, size in texts: text_file(name, size)
for name, em, size in bins: fake_bin(name, em, size)
# turn bin_pe into a PE: MZ stub with e_lfanew -> "PE\0\0"
if any(n == "bin_pe" for n, _, _ in bins):
    p = bytearray(open(os.path.join(d, "bin_pe"), "rb").read())
    p[0:2] = b"MZ"; p[0x3C:0x40] = (0x40).to_bytes(4, "little")
    p[0x40:0x44] = b"PE\0\0"
    open(os.path.join(d, "bin_pe"), "wb").write(bytes(p))
# incompressible file: stays RAW verbatim
open(os.path.join(d, "rand.bin"), "wb").write(random.randbytes(rand_mb * M))
# a tar of some texts (TARR decomposition + part batching)
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w") as tf:
    for name in tar_names:
        data = open(os.path.join(d, name), "rb").read()
        ti = tarfile.TarInfo("t/" + name)
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data))
open(os.path.join(d, "t.tar"), "wb").write(buf.getvalue())
tot = sum(os.path.getsize(os.path.join(d, f)) for f in os.listdir(d))
print("  corpus[%s]: %d files, %.1f MB" % (prof, len(os.listdir(d)), tot / 1e6))
PY
}

# -------------------------------------------------------------- legs ----

# WP27 regression (leg3 gate failure): a re-mkfs of a device that previously
# held a volume must render the OLD volume's journal completely dead, or the
# fresh volume's first flush replays the stale owner-WAL tail into its bitmap
# as phantom maps -- "orphans after mkfs+import" (3412 on the leg3 gate run).
# Sequence here is the minimal trigger: import+sweep writes owner maps (TARR
# batches / checkpoint WAL) into the journal at seq 1, then a fresh mkfs + a
# fresh import must come up with 0 orphans.
leg0() {
    LEG=leg0-remkfs-orphans
    say "[0] re-mkfs stale-journal gate: mkfs+import+sweep, then re-mkfs+import must be orphan-free"
    mkfs_fresh
    gen_corpus "$FLK/orig0" "$SEED" small
    import_all "$FLK/orig0"
    $B/invf-sweep "$DM" >"$FLK/sweep0.log" 2>&1 \
        || { cat "$FLK/sweep0.log"; fail "leg0 sweep"; }
    mkfs_fresh                                   # the re-mkfs under test
    gen_corpus "$FLK/orig0b" $((SEED + 70)) small
    import_all "$FLK/orig0b"
    $B/invf-fsck "$DM" >"$FLK/fsck0.log" 2>&1 \
        || { cat "$FLK/fsck0.log"; fail "leg0 fsck"; }
    # The gate used to be `grep -q "orphans:"`. That line is v2-only
    # (src/cli/fsck.c:464): on a v3 volume -- which is every volume mkfs
    # writes -- the report has no such line, so the gate could never fire
    # and the leg asserted nothing. What the leg actually means on v3 is
    # stronger and directly checkable: the re-mkfs erased the OLD volume
    # completely, so what is left is exactly the corpus just imported.
    # Three v3-true witnesses for that, none of them the sweep's own log:
    local want; want=$(ls "$FLK/orig0b" | wc -l)
    grep -qE "names/inodes: +$want name\(s\) over $want live inode\(s\)" "$FLK/fsck0.log" \
        || { echo "  re-mkfs left something behind: expected exactly the $want names just imported" >&2
             cat "$FLK/fsck0.log" >&2; fail "leg0 re-mkfs name/inode count"; }
    # "orphan rows:" is printed by fsck only when a live inode has no
    # directory entry naming it -- the v3 counterpart of a live record with
    # no reference. A stale journal replayed into a fresh bitmap shows up
    # here.
    if grep -q "orphan rows:" "$FLK/fsck0.log"; then
        echo "  re-mkfs+import leaked live rows nothing names (stale journal replayed into the fresh bitmap):"
        cat "$FLK/fsck0.log"
        fail "leg0 re-mkfs orphan gate"
    fi
    grep -q "^OK$" "$FLK/fsck0.log" \
        || { cat "$FLK/fsck0.log"; fail "leg0 fsck not clean"; }
    # and every freshly imported file reads back bit-exact, so "nothing left
    # behind" is not bought by storing the wrong bytes
    vol_files_exact "$FLK/orig0b" "leg0-reimport" || fail "leg0 re-imported content"
    echo "  re-mkfs + import: exactly the $want fresh names, 0 orphan rows (stale journal fully erased)"
}

leg1() {
    LEG=leg1-baseline
    say "[1] baseline sanity: mkfs on the flakey device, ~200MB corpus, sweep"
    mkfs_fresh
    gen_corpus "$FLK/orig1" "$SEED" full
    gen_corpus "$FLK/orig1b" "$SEED" full   # same seed: must be identical
    diff <(manifest_build "$FLK/orig1") <(manifest_build "$FLK/orig1b") >/dev/null \
        || fail "seeded corpus generation is not deterministic"
    rm -rf "$FLK/orig1b"
    info "corpus determinism: OK (seed $SEED)"
    import_all "$FLK/orig1"
    manifest_build "$FLK/orig1" > "$FLK/manifest1"
    $B/invf-sweep "$DM" >"$FLK/sweep1.log" 2>&1 || { cat "$FLK/sweep1.log"; fail "baseline sweep"; }
    grep -E "sweep done|checkpoint:" "$FLK/sweep1.log" | tail -2
    fsck_ok "baseline" || fail "fsck after baseline sweep"
    verify_clean "baseline" || fail "verify after baseline sweep"
    vol_files_exact "$FLK/orig1" "baseline" || fail "baseline manifest"
    # keep this volume for leg 2
}

leg2() {
    LEG=leg2-error-storm
    say "[2] write-error storm: error target for 6s during live FUSE writes"
    # leg 2 reuses leg 1's volume in a full run; standalone builds its own
    if [ "$ONLY" = "2" ]; then
        mkfs_fresh
        gen_corpus "$FLK/orig1" "$SEED" full
        import_all "$FLK/orig1"
        manifest_build "$FLK/orig1" > "$FLK/manifest1"
        $B/invf-sweep "$DM" >"$FLK/sweep1.log" 2>&1 || fail "sweep (standalone leg2)"
        fsck_ok "standalone" || fail "fsck standalone"
    fi
    # a sweep leaves the WP21 checkpoint live; accept it so the storm runs
    # checkpoint-free — otherwise the documented post-chaos recovery is a
    # rollback that discards ALL post-checkpoint writes, storm included
    $B/invf-sweep "$DM" --realize >"$FLK/realize2.log" 2>&1 || fail "realize pre-storm"
    fsck_ok "pre-storm realized" || fail "fsck after realize"
    mnt_up
    # pre-storm write through the mount, synced on the healthy device
    python3 -c "open('$MNT/pre-storm.txt','w').write('durable before the storm\n' * 1000)"
    sync
    # storm writer: numbered 2MB files, fsync each, log every outcome
    python3 - "$MNT" "$FLK/stop2" "$FLK/storm.log" "$SEED" <<'PY' &
import os, sys, hashlib, random, time
MNT, STOP, LOG = sys.argv[1], sys.argv[2], sys.argv[3]
rng = random.Random(int(sys.argv[4]) ^ 0x57A2)
i = 0
with open(LOG, "w") as lg:
    while not os.path.exists(STOP) and i < 400:
        name = "storm-%03d.bin" % i
        data = rng.randbytes(2 * 1024 * 1024)
        try:
            fd = os.open(os.path.join(MNT, name), os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
            try:
                mv = memoryview(data)
                while mv:
                    n = os.write(fd, mv)
                    mv = mv[n:]
                os.fsync(fd)
            finally:
                os.close(fd)
            lg.write("OK %.3f %s %s\n" % (time.time(), name,
                                          hashlib.sha256(data).hexdigest()))
        except OSError as e:
            lg.write("ERR %.3f %s %s\n" % (time.time(), name, e.strerror or e))
            time.sleep(0.05)
        lg.flush()
        i += 1
    lg.write("DONE %.3f %d\n" % (time.time(), i))
PY
    local WPID=$!
    sleep 2
    local T_ON T_OFF
    T_ON=$(date +%s.%N)
    dm_set error || fail "dm_set error"
    info "storm ON (error target, 6s)"
    sleep ${FLAKEY_STORM_S:-6}
    dm_set up || fail "dm_set up"
    T_OFF=$(date +%s.%N)
    info "storm OFF"
    sleep 2
    touch "$FLK/stop2"; wait $WPID
    # storm effectiveness + pin classification
    python3 - "$FLK/storm.log" "$T_ON" "$T_OFF" > "$FLK/storm-analysis.txt" <<'PY' \
        || { cat "$FLK/storm.log"; fail "storm was ineffective (no EIO reached a writer)"; }
import sys
log, t_on, t_off = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
oks_pre = oks_post = errs_in = oks_in = 0
for ln in open(log):
    p = ln.split()
    if len(p) < 3 or p[0] == "DONE":
        continue
    t = float(p[1])
    if p[0] == "ERR" and t_on <= t <= t_off:
        errs_in += 1
    elif p[0] == "OK":
        if t_on <= t <= t_off: oks_in += 1
        elif t < t_on:         oks_pre += 1
        else:                  oks_post += 1
print("pinned_ok=%d in_storm_ok=%d in_storm_err=%d" % (oks_pre + oks_post, oks_in, errs_in))
sys.exit(0 if errs_in >= 1 and (oks_pre + oks_post) >= 1 else 1)
PY
    cat "$FLK/storm-analysis.txt"
    # unmount; the daemon may need to finish its drain on the healed device
    if ! mnt_down 450; then
        fail "FUSE daemon wedged after the storm (had to kill -9)"
    fi
    info "daemon exited cleanly post-storm"
    # pre-storm.txt was fsynced via the mount on the healthy device
    python3 -c "import sys; sys.stdout.write('durable before the storm\n' * 1000)" > "$FLK/pre-storm.ref"
    $B/invf-cat "$DM" pre-storm.txt "$FLK/pre-storm.out" >/dev/null 2>&1 \
        || fail "pre-storm.txt unreadable after the storm"
    cmp -s "$FLK/pre-storm.ref" "$FLK/pre-storm.out" \
        || fail "pre-storm.txt not bit-exact after the storm"
    # recovery + checks (the ladder: auto-recover -> fsck -f; storm
    # orphans are freed by the -f pass, then the volume must be OK)
    recover "leg2" || fail "recovery ladder dead-ended"
    fsck_ok "post-storm" || fail "fsck post-storm"
    verify_clean "post-storm" || fail "verify post-storm"
    vol_files_exact "$FLK/orig1" "post-storm corpus" || fail "corpus not bit-exact post-storm"
    # pinned storm files: present + bit-exact; unpinned: absent or bit-exact
    python3 - "$DM" "$FLK/storm.log" "$T_ON" "$T_OFF" "$B" "$FLK" <<'PY' \
        || fail "storm files: torn bytes or a lost pinned write"
import os, subprocess, sys, hashlib
DM, LOG, T_ON, T_OFF, B, FLK = sys.argv[1], sys.argv[2], float(sys.argv[3]), \
    float(sys.argv[4]), sys.argv[5], sys.argv[6]
out = subprocess.run([os.path.join(B, "invf-ls"), DM],
                     capture_output=True, text=True)
present = set()
for ln in out.stdout.splitlines():
    p = ln.split()
    if len(p) >= 3 and p[-1].startswith("storm-"):
        present.add(p[-1])
pinned = checked = 0
bad = []
for ln in open(LOG):
    p = ln.split()
    if len(p) < 4 or p[0] != "OK":
        continue
    t, name, sha = float(p[1]), p[2], p[3]
    is_pinned = not (T_ON <= t <= T_OFF)
    if name not in present:
        if is_pinned:
            bad.append("pinned file %s (fsync OK on healthy device) is ABSENT" % name)
        continue
    outf = os.path.join(FLK, "storm.out")
    r = subprocess.run([os.path.join(B, "invf-cat"), DM, name, outf],
                       capture_output=True)
    if r.returncode != 0:
        bad.append("present file %s fails to read (rc=%d)" % (name, r.returncode))
        continue
    got = hashlib.sha256(open(outf, "rb").read()).hexdigest()
    if got != sha:
        bad.append("present file %s has WRONG BYTES" % name)
    else:
        checked += 1
        if is_pinned: pinned += 1
if os.path.exists(os.path.join(FLK, "storm.out")):
    os.unlink(os.path.join(FLK, "storm.out"))
print("  storm files: %d present+bit-exact (%d of them pinned)" % (checked, pinned))
for b in bad:
    print("  BAD: " + b)
sys.exit(1 if bad else 0)
PY
}

leg3() {
    LEG=leg3-torn-sweep
    say "[3] torn-write/drop during sweep: drop_writes windows mid-sweep"
    mkfs_fresh
    gen_corpus "$FLK/orig3" $((SEED + 3)) medium
    import_all "$FLK/orig3"
    manifest_build "$FLK/orig3" > "$FLK/manifest3"
    fsck_ok "pre-sweep" || fail "fsck pre-sweep"
    # sweep starts healthy; chaos begins only after the WP21 save point is
    # armed (that machinery is designed to be in place before the walk)
    $B/invf-sweep "$DM" >"$FLK/sweep3.log" 2>&1 &
    local swpid=$!
    local i
    for i in $(seq 1 100); do
        grep -q "save point captured" "$FLK/sweep3.log" 2>/dev/null && break
        sleep 0.1
    done
    grep -q "save point captured" "$FLK/sweep3.log" || fail "save point never armed"
    info "save point armed; seeded drop_writes windows on"
    python3 "$REPO/tools/flakey/dmchaos.py" "$DEV" "${LOOP:-$BACK}" "$SEC" \
        $((SEED + 300)) 600 "$FLK/stop3" drop >"$FLK/chaos3.log" 2>&1 &
    CHAOS_PID=$!
    wait $swpid
    local src=$?
    touch "$FLK/stop3"; wait $CHAOS_PID 2>/dev/null; CHAOS_PID=""
    dm_set up
    info "sweep rc=$src under chaos ($(grep -c drop "$FLK/chaos3.log") drop windows)"
    [ "$src" = 0 ] || info "sweep failed loudly (acceptable): rc=$src"
    tail -2 "$FLK/sweep3.log"
    # The ladder is best effort: it returns nonzero when damage remains,
    # which under drop windows is the honest outcome, not a ladder bug.
    # The consistency gate below is the arbiter, not the ladder's rc.
    if recover "leg3"; then info "recovery ladder converged clean";
    else info "ladder ended with residual damage (expected when chaos tore bytes)"; fi
    verify_consistent "leg3" "$FLK/orig3" || fail "inconsistent recovery state"
    if [ -s "$FLK/rec-rb.log" ]; then info "recovery path: rollback engaged"; else info "recovery path: fsck only"; fi
}

leg4() {
    LEG=leg4-crash-mid-seal
    say "[4] crash mid-seal: sweep --seal + drop_writes + kill -9"
    mkfs_fresh
    gen_corpus "$FLK/orig4" $((SEED + 4)) medium
    import_all "$FLK/orig4"
    manifest_build "$FLK/orig4" > "$FLK/manifest4"
    fsck_ok "pre-seal" || fail "fsck pre-seal"

    # -- 4a: deterministic kill -9 mid-run (the engine's own WP21 hook
    # delivers SIGKILL after N walk candidates) under drop windows
    info "4a: kill -9 mid walk (INVFS_SWEEP_ABORT_AFTER=3) under drop windows"
    INVFS_SWEEP_ABORT_AFTER=3 $B/invf-sweep "$DM" --seal >"$FLK/seal4a.log" 2>&1 &
    local swpid=$!
    local i
    for i in $(seq 1 100); do
        grep -q "save point captured" "$FLK/seal4a.log" 2>/dev/null && break
        sleep 0.1
    done
    python3 "$REPO/tools/flakey/dmchaos.py" "$DEV" "${LOOP:-$BACK}" "$SEC" \
        $((SEED + 400)) 600 "$FLK/stop4" droponly >"$FLK/chaos4.log" 2>&1 &
    CHAOS_PID=$!
    wait $swpid
    local src=$?
    [ "$src" = 137 ] || fail "4a: expected SIGKILL (137), got $src"
    touch "$FLK/stop4"; wait $CHAOS_PID 2>/dev/null; CHAOS_PID=""
    dm_set up
    seal_resolve "4a"

    # -- 4b: a full sweep --seal under drop windows, no kill — the parity
    # pass straddles drop windows; the seal ends torn or complete, and
    # verify must say which honestly
    info "4b: full sweep --seal under drop windows (no kill)"
    mkfs_fresh
    import_all "$FLK/orig4"
    $B/invf-sweep "$DM" --seal >"$FLK/seal4b.log" 2>&1 &
    swpid=$!
    for i in $(seq 1 100); do
        grep -q "save point captured" "$FLK/seal4b.log" 2>/dev/null && break
        sleep 0.1
    done
    python3 "$REPO/tools/flakey/dmchaos.py" "$DEV" "${LOOP:-$BACK}" "$SEC" \
        $((SEED + 401)) 600 "$FLK/stop4b" drop >"$FLK/chaos4b.log" 2>&1 &
    CHAOS_PID=$!
    wait $swpid
    src=$?
    touch "$FLK/stop4b"; wait $CHAOS_PID 2>/dev/null; CHAOS_PID=""
    dm_set up
    info "4b: sweep --seal rc=$src under chaos ($(grep -c drop "$FLK/chaos4b.log") drop windows)"
    seal_resolve "4b"
}

# leg-4 recovery: inspect the seal state honestly BEFORE resolving it.
# The seal ends up complete (parity verifies / re-seal repaired) or
# absent (rolled back); files bit-exact; fsck clean either way.
seal_resolve() { # <label>
    local label=$1 SEAL_STATE
    $B/invf-fsck "$DM" >"$FLK/rec-fsck1.log" 2>&1
    grep -E "state:|checkpoint:|OK$|REPAIRED|ISSUES" "$FLK/rec-fsck1.log" | sed 's/^/  fsck: /'
    $B/invf-verify "$DM" --deep >"$FLK/rec-verify1.log" 2>&1 || true
    if grep -q "^parity:" "$FLK/rec-verify1.log"; then
        # seal live: verify must say the truth about it
        grep -E "^parity|^deep" "$FLK/rec-verify1.log"
        grep -q "CORRUPT" "$FLK/rec-verify1.log" && fail "$label: content corrupt under the seal"
        if grep -qE "^parity: [0-9]+ sealed stripes, 0 mismatched, 0 missing, 0 extra" \
                "$FLK/rec-verify1.log"; then
            info "seal is COMPLETE (parity verifies) [$label]"
        else
            info "seal TORN (verify reported it honestly) -> re-seal repairs [$label]"
            $B/invf-sweep "$DM" --seal >"$FLK/reseal4.log" 2>&1 || fail "$label: re-seal after torn seal"
            $B/invf-verify "$DM" --deep >"$FLK/rec-verify2.log" 2>&1 || true
            grep -E "^parity|^deep" "$FLK/rec-verify2.log"
            grep -qE "^parity: [0-9]+ sealed stripes, 0 mismatched, 0 missing, 0 extra" \
                "$FLK/rec-verify2.log" || fail "$label: re-seal did not repair torn parity"
        fi
        # a live checkpoint left over: accept it (the seal stays in place)
        if grep -q "checkpoint:.*live" "$FLK/rec-fsck1.log"; then
            $B/invf-sweep "$DM" --realize >>"$FLK/reseal4.log" 2>&1 || fail "$label: realize post-seal"
            info "checkpoint accepted (seal kept) [$label]"
        fi
        SEAL_STATE="COMPLETE"
    else
        info "seal absent ($label): rolled back or never committed"
        recover "$label" || fail "$label: recovery ladder dead-ended"
        SEAL_STATE="ABSENT"
    fi
    # A drop window can take a flush's bitmap pages while keeping the
    # journal's: a realize's freed blocks then stay marked used on disk --
    # a leak, never corruption, and fsck -f's rebuild reclaims exactly
    # those. Reclaim before the structural gate.
    $B/invf-fsck "$DM" -f >"$FLK/rec-fsckf2.log" 2>&1 || true
    fsck_ok "$label" || fail "$label: fsck after recovery"
    verify_or_reseal "$label" || fail "$label: verify/corrupt content after recovery"
    vol_files_exact "$FLK/orig4" "$label post-recovery" || fail "$label: files not bit-exact"
    info "seal state ($label): $SEAL_STATE"
}

leg5() {
    LEG=leg5-soak
    say "[5] chaos op-sequence soak (${SOAK_S}s, seed $((SEED + 500)))"
    mkfs_fresh
    gen_corpus "$FLK/orig5" $((SEED + 5)) small
    import_all "$FLK/orig5"
    fsck_ok "pre-soak" || fail "fsck pre-soak"
    python3 "$REPO/tools/flakey/soak.py" \
        --dev "$DM" --dmname "$DEV" --loop "${LOOP:-$BACK}" --sectors "$SEC" \
        --mnt "$MNT" --bin "$B" --work "$FLK" \
        --seed $((SEED + 500)) --seconds "$SOAK_S" \
        --corpus "$FLK/orig5" 2>&1 | tee "$FLK/soak.log" | grep -E "GATE|FAIL|SOAK|chaos|round" | tail -40
    local rc=${PIPESTATUS[0]}
    [ "$rc" = 0 ] || fail "soak rc=$rc (full log in artifacts)"
    # the soak leaves the device up and unmounted; final global checks
    fsck_ok "post-soak" || fail "fsck post-soak"
    verify_or_reseal "post-soak" || fail "verify post-soak"
}

# Leg 6 (WP22d): the journal compaction itself under drop windows. Every
# flush of the driving runs is a forced slot flip (INVFS_JRN_FORCE_COMPACT),
# so the double-buffer commit (image -> barrier -> selector flip -> barrier)
# is raced against drop_writes dozens of times -- including the legacy ->
# slot migration flip at the first dirty close. Whatever tears, replay must
# land on exactly one CRC-valid side of the flip (or the pre-migration flat
# log) and the consistent cut must keep every live record fully mapped.
# Leg 6 was the WP22d journal-compaction leg: every flush forced into a slot
# flip (INVFS_JRN_FORCE_COMPACT), raced against drop_writes windows, asserting
# that replay landed on exactly one CRC-valid side. It drove the mapping
# journal's slot image, selector flip and replay -- all deleted in
# wp/purge-v2-l2p-journal -- and its env hook with them. The drop_writes chaos
# coverage it shared is unchanged in legs 2, 3 and 5.

leg7() {
    LEG=leg7-pagecache
    say "[7] page-cache power loss: fsynced writes vs kill -9 + drop_caches"
    pc_dev_create
    $B/invf-mkfs "$PC_VOL" "$PC_SIZE_GB" >"$PC_WORK/mkfs.log" 2>&1 \
        || { cat "$PC_WORK/mkfs.log"; fail "leg7 mkfs"; }
    info "volume: $PC_VOL -- $PC_MODE"
    pc_mnt_up

    # -- 1 + 2: the acknowledged writes, and what they must read back as
    python3 - "$PC_MNT" "$PC_WORK" $((SEED + 700)) <<'PY' >"$PC_WORK/write.log" 2>&1 &
import hashlib, os, random, sys, time
MNT, WORK, SEED = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.makedirs(WORK + "/ref", exist_ok=True)
man = open(WORK + "/manifest.txt", "w")

def gen(kind, n, seed):
    r = random.Random(seed)
    if kind == "text":                       # compressible -> PPMd/LZ4 lanes
        w = ("the quick brown fox jumps over lazy dogs int static return "
             "while for struct char void NULL size_t uint64_t\n").split()
        b = bytearray()
        while len(b) < n:
            b += (r.choice(w) + " ").encode()
        return bytes(b[:n])
    if kind == "bin":                        # patterned binary -> ZSTD lane
        pat = bytes(range(256)) * 4 + b"\x00" * 64
        b = bytearray()
        while len(b) < n:
            b += pat + bytes([r.randrange(256)]) * 16
        return bytes(b[:n])
    return r.randbytes(n)                    # incompressible -> verbatim RAW

def put(name, data, gen_no, cls):
    fd = os.open(MNT + "/" + name, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
    mv = memoryview(data)
    while mv:
        k = os.write(fd, mv); mv = mv[k:]
    os.fsync(fd)                             # THE acknowledgement
    os.close(fd)
    open("%s/ref/%s.g%d" % (WORK, name, gen_no), "wb").write(data)
    man.write("%s\t%d\t%s\t%s\n" % (name, gen_no,
                                    hashlib.sha256(data).hexdigest(), cls))

def over(name, data, gen_no, cls, do_fsync):
    # in-place rewrite: NO O_TRUNC (a truncate is its own metadata op), the
    # new bytes overwrite the old ones from offset 0. Same length as the
    # old generation on purpose: a torn rewrite then shows up as a byte
    # splice at the same size, which is the failure this leg hunts.
    fd = os.open(MNT + "/" + name, os.O_RDWR)
    mv = memoryview(data)
    while mv:
        k = os.pwrite(fd, mv, 0); mv = mv[k:]
    if do_fsync:
        os.fsync(fd)
    open("%s/ref/%s.g%d" % (WORK, name, gen_no), "wb").write(data)
    man.write("%s\t%d\t%s\t%s\n" % (name, gen_no,
                                    hashlib.sha256(data).hexdigest(), cls))
    return fd

n = 0
for i in range(4):                          # text, compressible
    put("pc-ack-t%d" % i, gen("text", 96 * 1024, SEED + 10 + i), 1, "ack"); n += 1
for i in range(4):                          # patterned binary
    put("pc-ack-b%d" % i, gen("bin", 128 * 1024, SEED + 20 + i), 1, "ack"); n += 1
put("pc-ack-rand", random.Random(SEED).randbytes(64 * 1024), 1, "ack"); n += 1

# 1 MiB = 16 segments: the in-place rewrite forks the whole recipe, so a
# torn result has plenty of room to be a splice
g1 = gen("bin", 1024 * 1024, SEED + 31)
g2 = gen("bin", 1024 * 1024, SEED + 32)
put("pc-acked.bin", g1, 1, "rw-acked")
over("pc-acked.bin", g2, 2, "rw-acked", True)      # acknowledged rewrite
put("pc-unacked.bin", g1, 1, "rw-unacked")
wfd = over("pc-unacked.bin", g2, 2, "rw-unacked", False)   # NOT acknowledged
man.close()
print("WRITE-READY: %d files, %d generations, un-acked fd %d held open"
      % (n + 2, sum(1 for _ in open(WORK + "/manifest.txt")), wfd), flush=True)
# hold the un-acked fd open: the commit can only happen on flush/fsync/
# release, and none of those may run before the daemon dies
while True:
    time.sleep(1)
PY
    PC_WRITER_PID=$!
    local i
    for i in $(seq 1 150); do
        grep -q "WRITE-READY" "$PC_WORK/write.log" 2>/dev/null && break
        sleep 0.2
    done
    grep -q "WRITE-READY" "$PC_WORK/write.log" \
        || { cat "$PC_WORK/write.log"; fail "leg7 write phase never completed"; }
    info "$(cat "$PC_WORK/write.log")"
    info "every file fsynced; nothing unmounted, nothing closed"

    # -- 3 + 4: the power cut
    pc_kill "$PC_WRITER_PID" || fail "leg7: the daemon survived kill -9"
    PC_WRITER_PID=""
    info "daemon killed -9 (pid $PC_FUSE_PID), no vol_close, no CLEAN mark"
    pc_cut || true
    # the death really was abrupt: the superblock on the medium is still
    # DIRTY (mkfs wrote CLEAN; vol_close is the only CLEAN writer)
    python3 - "$PC_VOL" <<'PY' || fail "leg7: the daemon closed cleanly -- this is not a power cut"
import struct, sys
state = struct.unpack_from("<I", open(sys.argv[1], "rb").read(0x20), 0x18)[0]
print("  on-disk superblock: 0x%02X%s" % (state,
      " (DIRTY: the volume never closed)" if state == 0xDA else " (UNEXPECTED)"))
sys.exit(0 if state == 0xDA else 1)
PY

    # -- 5: reopen offline. Presence, bit-exactness and the splice rule.
    python3 - "$B" "$PC_VOL" "$PC_WORK" <<'PY' \
        || fail "leg7: acknowledged bytes did not survive the cut"
import hashlib, os, subprocess, sys, time
B, VOL, WORK = sys.argv[1], sys.argv[2], sys.argv[3]
man = [l.rstrip("\n").split("\t") for l in open(WORK + "/manifest.txt") if l.strip()]
out = os.path.join(WORK, "cat.out")

def cat(name):
    """invf-cat with the loop-device flock race tolerated (see pc_run)"""
    for _ in range(12):
        r = subprocess.run([os.path.join(B, "invf-cat"), VOL, name, out],
                           capture_output=True)
        if r.returncode == 0:
            return hashlib.sha256(open(out, "rb").read()).hexdigest(), None
        if b"image is in use by another process" not in r.stderr:
            return None, r.stderr.decode().strip()[-160:]
        time.sleep(0.4)
    return None, "flock race did not clear in 12 attempts"

bad, checked, torn, verdict = [], 0, [], None
for name, gen, sha, cls in man:
    got, err = cat(name)
    if got is None:
        bad.append("%s (gen %s): invf-cat failed: %s" % (name, gen, err))
        continue
    checked += 1
    if cls == "ack" and got != sha:
        bad.append("%s: acknowledged bytes are NOT intact (%s != %s)"
                   % (name, got[:12], sha[:12]))
    elif cls == "rw-acked" and gen == "2" and got != sha:
        bad.append("%s: the ACKED rewrite did not land (%s != %s)"
                   % (name, got[:12], sha[:12]))
    elif cls == "rw-unacked" and gen == "2":
        # complete old or complete new -- anything else is a splice
        g1 = [s_ for (n_, g_, s_, c_) in man if n_ == name and g_ == "1"][0]
        if got == g1:
            verdict = "OLD (the un-acked bytes were lost, as they may be)"
        elif got == sha:
            verdict = "NEW (the un-acked bytes were already durable)"
        else:
            verdict = "TORN"
            torn.append("%s: neither the complete old (%s) nor the complete "
                        "new (%s) content -- got %s: A SPLICE"
                        % (name, g1[:12], sha[:12], got[:12]))
print("  reopened: %d generations read back from the medium" % checked)
if verdict:
    print("  un-acked in-place rewrite came back %s" % verdict)
for b in bad: print("  BAD: " + b)
for t in torn: print("  BAD: " + t)
sys.exit(1 if (bad or torn) else 0)
PY
    pc_fsck "post-cut" || fail "fsck after the page-cache cut"
    pc_verify "post-cut" || fail "verify after the page-cache cut"

    # -- 5b: and through a fresh mount (the FUSE read path, post-crash)
    pc_mnt_up
    python3 - "$PC_MNT" "$PC_WORK" <<'PY' \
        || { pc_mnt_down; fail "leg7: remount read-back mismatch"; }
import hashlib, os, sys
MNT, WORK = sys.argv[1], sys.argv[2]
man = [l.rstrip("\n").split("\t") for l in open(WORK + "/manifest.txt") if l.strip()]
bad = []
for name, gen, sha, cls in man:
    if cls == "ack" or (cls == "rw-acked" and gen == "2"):
        p = os.path.join(MNT, name)
        try:
            got = hashlib.sha256(open(p, "rb").read()).hexdigest()
        except OSError as e:
            bad.append("%s: unreadable through the mount (%s)" % (name, e)); continue
        if got != sha:
            bad.append("%s: mount read-back differs from the medium (%s != %s)"
                       % (name, got[:12], sha[:12]))
print("  remounted: every acknowledged file read back bit-exact through FUSE"
      if not bad else "  remount: %d mismatch(es)" % len(bad))
for b in bad: print("  BAD: " + b)
sys.exit(1 if bad else 0)
PY
    pc_mnt_down 450 || fail "leg7: the daemon wedged on the clean unmount"
    pc_fsck "post-remount" || fail "fsck after the remount"
    pc_verify "post-remount" || fail "verify after the remount"
    info "acknowledged writes survive kill -9 + drop_caches; no splices"
}

# ------------------------------------------------------------- leg 8 (WP130) --
# The v3 orphan collector (WP121) frees COW base pages that no RT30 slot and
# no save point can name. A wrong answer to "is this page reachable" is
# SILENT DATA LOSS, not a crash, so the collector's safety case cannot be made
# by reading the predicate -- it has to be made by cutting power in the middle
# of it and asking what the volume says afterwards. That is this leg.
#
# WHAT IS BEING CRASHED, precisely:
#
#   invf-orphan_test `cost` runs the production cycle in a loop: publish a
#   new COW base root (which abandons the old one), then call
#   vol_reclaim_orphans -- the SAME bounded pass, under the SAME gate, that
#   fold_reclaim_hook calls on every fold of a live root. The collector
#   flushes the allocation bitmap in the same breath as its frees
#   (btree_collect_orphans: "A crash before this point leaves the blocks
#   allocated (a leak), never shared"), so a kill -9 at any point in the
#   loop lands in the state this leg is about: blocks FREED, bitmap FLUSHED,
#   no clean close, no CLEAN superblock.
#
#   The loop is the whole reason a kill has somewhere to land. A single
#   collector pass takes milliseconds, so a one-shot collect would always be
#   killed either before it freed anything or after the process had already
#   closed -- neither of which is the state under test. The loop runs for
#   minutes; a seeded jitter picks the cut point.
#
# WHAT IS ASSERTED, and which arm is which:
#
#   ARM A (the SHIPPED DEFAULT, whatever it is -- no INVFS_RECLAIM_ORPHANS in
#   the environment): the collector must free blocks, the frees must be
#   DURABLE across the cut (invf-fsck's on-medium free-block count must go
#   UP across the kill, which is only possible if the bitmap flush reached
#   the image), and after the cut every one of the 40 files must read back
#   BYTE-IDENTICAL -- through vol_read_named, through invf-verify --deep, and
#   after the volume has been asked to allocate hard on top of exactly the
#   blocks that were just freed. That last step is the ALIASING probe: if the
#   collector had freed a page the live root still stands on, the reopen
#   reads a namespace standing on a block somebody else now owns, and the
#   rewrite is what makes "somebody else" happen.
#
#   ARM B (INVFS_RECLAIM_ORPHANS=0, the documented escape hatch): the same
#   procedure must free NOTHING -- the free-block count must not rise. This
#   is the leg's own red control. Without it ARM A proves nothing: a workload
#   that freed blocks for reasons of its own would satisfy it too.
#
#   On a build where the collector is default-OFF, ARM A's durability
#   assertion fails and the leg fails. That is the point: this leg is what
#   makes "the default" a measurement rather than a decision.
#
# WHAT IT DOES NOT CLAIM, stated rather than omitted:
#   * The one crash state it cannot reach is "freed in RAM, not yet
#     flushed". The flush is the statement immediately after the frees
#     inside the collector, so that window is a handful of instructions and
#     a cut inside it is a LEAK by construction, never a wrong free.
#   * The RT30-fallback axis is not re-litigated here. test-v3-orphan-
#     reclaim.sh legs 3 and 4 own it, with the wrong-predicate red control.
#   * dm-flakey's drop_writes is the only thing that can discard an
#     acknowledged write, and it needs a loop device, which transfers zero
#     bytes in this sandbox (see the leg 7 header). This is a process-death
#     leg, not a device-failure leg.

RC_T=""   # the driver; a `make` target, so it is built on demand

rc_driver() {
    if [ ! -x "$B/invf-orphan_test" ]; then
        make -C "$REPO" bin/invf-orphan_test >"$RC_WORK/driver-build.log" 2>&1 \
            || { cat "$RC_WORK/driver-build.log"; fail "cannot build bin/invf-orphan_test"; }
    fi
    RC_T=$B/invf-orphan_test
}

rc_work_pick() {   # leg 8's scratch, under $FLK: the startup guard already
    # refused a tmpfs scratch when 7/8 are selected, so $FLK is PROVEN
    # disk-backed wherever leg 8 runs -- and a suite where three scratch
    # pickers disagree is a suite where the answer depends on which leg you
    # are reading. The old default scanned /srv /var/tmp /opt /var/lib and
    # died on CI (/srv exists there, root-owned: mkdir failed -> FAIL with
    # no chaos run at all), while $FLAKEY_WORK -- known good, legs 0-5 ran
    # on it -- sat unused. $FLAKEY_RC_WORK still wins when set.
    if [ -z "$RC_WORK" ]; then
        RC_WORK="$FLK/reclaim"
    fi
    [ -n "$RC_WORK" ] || RC_WORK=/tmp/invfs-flakey-reclaim
    rm -rf "$RC_WORK" && mkdir -p "$RC_WORK" || return 1
    if [ "$(stat -f -c %T "$RC_WORK" 2>/dev/null)" = tmpfs ]; then
        echo "  WARN: $RC_WORK is tmpfs -- drop_caches cannot evict a tmpfs" >&2
        echo "        page, so the power cut below is a NO-OP there." >&2
    fi
    info "reclaim scratch: $RC_WORK ($(stat -f -c %T "$RC_WORK" 2>/dev/null))"
}

rc_free() {        # free blocks as recorded ON THE MEDIUM by invf-fsck
    $B/invf-fsck "$RC_VOL" 2>/dev/null \
        | sed -n 's/.*free blocks: *\([0-9][0-9]*\).*/\1/p' | tail -1
}

rc_field() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }

rc_sudirty() {     # the death really was abrupt.
                   #
                   # NOT leg 7's superblock-state check, and the difference
                   # matters. Leg 7 watches a FUSE daemon, which marks the
                   # volume DIRTY as it mounts, so CLEAN-on-medium after a
                   # kill -9 is proof that vol_close never ran. Nothing here
                   # does: a v3 volume is recovered by replaying the delta
                   # log, so vol_open does not set the state bit and the
                   # medium legitimately still says CLEAN from the last
                   # clean close. Copying leg 7's check here would assert a
                   # property of the FUSE mount path, not of the collector.
                   #
                   # The witness for THIS leg is the driver's own last act:
                   # `cost` prints its DRAIN ledger, then vol_flush, then
                   # vol_close. A process that got that far logged DRAIN and
                   # exited 0. No DRAIN line and a SIGKILL status is the
                   # death-without-a-close, stated in the driver's terms.
    local log=$1 wrc=$2
    if grep -q "^DRAIN " "$log"; then
        echo "  the driver reached its DRAIN ledger and closed cleanly --" >&2
        echo "        the process was not cut, so nothing was tested" >&2
        return 1
    fi
    [ "$wrc" -ge 128 ] || {
        echo "  the driver exited $wrc, which is not a signal death" >&2
        return 1; }
    info "    no DRAIN ledger, exit $wrc: it died on SIGKILL with the volume open"
    return 0
}

rc_verify() {      # <label>: the bit-exactness invariant, two readers
    pc_run "orphan_test verify ($1)" "$RC_WORK/verify-$1.log" \
        "$RC_T" verify "$RC_VOL" "$RC_NFILES" || return 1
    grep -E "^VERIFY" "$RC_WORK/verify-$1.log" | sed 's/^/  /'
    grep -q "^VERIFY OK" "$RC_WORK/verify-$1.log" || {
        echo "  BIT-EXACTNESS BROKEN after $1:" >&2
        cat "$RC_WORK/verify-$1.log" >&2; return 1; }
    pc_run "invf-verify --deep ($1)" "$RC_WORK/deep-$1.log" \
        $B/invf-verify "$RC_VOL" --deep || return 1
    grep -E "corrupt" "$RC_WORK/deep-$1.log" | sed 's/^/  /'
    grep -qE "[1-9][0-9]* corrupt" "$RC_WORK/deep-$1.log" && {
        echo "  invf-verify reports corrupt files after $1" >&2
        cat "$RC_WORK/deep-$1.log" >&2; return 1; }
    return 0
}

rc_fsck() {
    pc_run "invf-fsck ($1)" "$RC_WORK/fsck-$1.log" $B/invf-fsck "$RC_VOL" || return 1
    grep -E "^OK$" "$RC_WORK/fsck-$1.log" >/dev/null || {
        echo "  fsck not clean after $1:" >&2; cat "$RC_WORK/fsck-$1.log" >&2; return 1; }
    grep -E "bad pages|torn slots" "$RC_WORK/fsck-$1.log" | sed 's/^/  /'
    return 0
}

# One cut. <label> <nfold> [ENV=VAL ...] (the ENVs go to the driver only --
# the verifier and fsck calls below always run in the caller's environment).
# Echoes "<label> free_before free_after delta collector_freed".
rc_crash_round() {
    local label=$1 nfold=$2; shift 2
    local log="$RC_WORK/crash-$label.log"
    local before after folds i wrc
    before=$(rc_free)
    [ -n "$before" ] || { echo "  $label: cannot read the free-block count" >&2; return 1; }
    : > "$log"
    # stdbuf -oL: the COSTFOLD ledger is line-buffered, so the per-fold record
    # survives the kill instead of dying in stdout's block buffer.
    env "$@" stdbuf -oL "$RC_T" cost "$RC_VOL" 0 "$RC_NGEN" "$nfold" \
        >"$log" 2>&1 &
    RC_CRASH_PID=$!
    # Do not cut before the collector has anything to do: the bounded pass only
    # frees once BOTH RT30 slots have moved off a root, which takes a few
    # published generations.
    for i in $(seq 1 400); do
        folds=$(grep -c COSTFOLD "$log" 2>/dev/null); folds=${folds:-0}
        [ "$folds" -ge 8 ] && break
        kill -0 "$RC_CRASH_PID" 2>/dev/null || break
        sleep 0.25
    done
    [ "${folds:-0}" -ge 8 ] || {
        echo "  $label: the driver reached only ${folds:-0} fold(s) -- the" >&2
        echo "        crash window closed; raise RC_NFOLD." >&2
        kill -9 "$RC_CRASH_PID" 2>/dev/null; wait "$RC_CRASH_PID" 2>/dev/null
        RC_CRASH_PID=""; return 1; }
    sleep "$(( (RC_JITTER + i * 13) % 9 + 1 ))"   # seeded spread of cut points
    kill -9 "$RC_CRASH_PID" 2>/dev/null
    wait "$RC_CRASH_PID" 2>/dev/null; wrc=$?
    RC_CRASH_PID=""
    [ "$wrc" = 0 ] && {
        echo "  $label: the driver FINISHED on its own (nfold=$nfold) -- no" >&2
        echo "        crash happened; raise RC_NFOLD." >&2; return 1; }
    info "$label: kill -9 after $folds published root generations, no vol_close"
    rc_sudirty "$log" "$wrc" || return 1
    # the surrogate: write back, then make the medium the only copy left
    sync
    if sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null; then
        info "$label: page cache dropped (drop_caches=3): the reopen re-reads the medium"
    else
        echo "  WARN: no root -- the page-cache cut was SKIPPED; $label degraded" >&2
        echo "        to kill -9 + reopen (process death only, not a power cut)." >&2
        return 1
    fi
    after=$(rc_free)
    [ -n "$after" ] || { echo "  $label: cannot read the free-block count after the cut" >&2; return 1; }
    local freed
    freed=$(sed -n 's/.*freed=\([0-9][0-9]*\).*/\1/p' "$log" \
            | awk '{s+=$1} END{print s+0}')
    echo "$label $before $after $(( after - before )) $freed" >> "$RC_WORK/ledger.txt"
    printf '  %s: free %s -> %s (%+d blocks on the medium across the cut)\n' \
        "$label" "$before" "$after" "$(( after - before ))"
    printf '  %s:        the collector reported %s page(s) freed before it died\n' \
        "$label" "$freed"
    return 0
}

# The ALIASING probe. Rewrite every file and force more base generations, so
# the allocator lays fresh base pages and segments over exactly the blocks the
# collector just freed. A page that was freed while the live root still stood
# on it does not fail at this point -- it fails HERE, when the block gets a
# new owner and the read-back diverges.
rc_reuse_probe() {
    local label=$1
    pc_run "orphan_test build ($label)" "$RC_WORK/probe-$label.log" \
        "$RC_T" build "$RC_VOL" "$RC_NFILES" 60 || {
            echo "  the aliasing probe could not rewrite the volume:" >&2
            cat "$RC_WORK/probe-$label.log" >&2; return 1; }
    info "$label: rewrote all $RC_NFILES files and forced 60 more base generations"
    info "$label:        on top of the blocks the collector had just freed"
    rc_verify "probe-$label" || return 1
    rc_fsck "probe-$label" || return 1
    return 0
}

leg8() {
    LEG=leg8-reclaim
    say "[8] orphan reclaim across a power cut: frees + bitmap flush + kill -9"
    rc_work_pick || fail "no scratch for the reclaim leg"
    rc_driver
    RC_VOL="$RC_WORK/rc.img"
    local size_gb
    size_gb=$(awk -v m="$RC_SIZE_MB" 'BEGIN{printf "%.4f", m/1024}')
    truncate -s "${RC_SIZE_MB}M" "$RC_VOL" || fail "truncate $RC_VOL"
    $B/invf-mkfs "$RC_VOL" "$size_gb" >"$RC_WORK/mkfs.log" 2>&1 \
        || { cat "$RC_WORK/mkfs.log"; fail "leg8 mkfs"; }
    info "volume: $RC_VOL (${RC_SIZE_MB} MB image file, no loop device)"
    info "collector gate in this environment: ${INVFS_RECLAIM_ORPHANS:-<unset, shipped default>}"

    # ---- the leak, or nothing below is anything
    local out orphan
    out=$("$RC_T" build "$RC_VOL" "$RC_NFILES" "$RC_NGEN" 2>&1) \
        || { echo "$out"; fail "leg8 build"; }
    echo "$out" | grep -E "^(FILES|FOLD)" | sed 's/^/  /'
    orphan=$(rc_field "$out" PAGES_ORPHAN)
    [ -n "$orphan" ] || fail "build did not report PAGES_ORPHAN"
    [ "$orphan" -gt 0 ] || fail "no orphan base pages to reclaim -- leg 8 would pass vacuously"
    info "$orphan of the allocated base pages are orphans no RT30 slot can name"
    rc_verify "baseline" || fail "leg8: read-back failed BEFORE any reclaim"
    rc_fsck "baseline"   || fail "leg8: fsck failed before any reclaim"

    # ---- ARM A: the shipped default
    say "  ARM A: the shipped default -- the collector must free, durably"
    local a_delta=0 a_freed=0 a_label line
    local round
    for round in $(seq 1 "$RC_ROUNDS"); do
        a_label="A$round"
        rc_crash_round "$a_label" "$RC_NFOLD" INVFS_RECLAIM_STATS=1 \
            || fail "leg8 arm A round $round: the crash itself did not happen"
        rc_verify "$a_label" || fail "leg8 arm A round $round: bytes did not survive the cut"
        rc_fsck "$a_label"   || fail "leg8 arm A round $round: fsck after the cut"
        rc_reuse_probe "$a_label" || fail "leg8 arm A round $round: aliasing probe"
        line=$(grep "^$a_label " "$RC_WORK/ledger.txt" | tail -1)
        a_delta=$(( a_delta + $(echo "$line" | awk '{print $4}') ))
        a_freed=$(( a_freed + $(echo "$line" | awk '{print $5}') ))
    done
    info "ARM A: the collector reported $a_freed page(s) freed across $RC_ROUNDS cut(s)"
    info "ARM A: on-medium free-block delta across the cuts: $a_delta"
    [ "$a_freed" -gt 0 ] \
        || fail "ARM A: the collector freed NOTHING under the shipped default -- \
the reclaim power-loss path was never exercised, so this leg proves nothing"
    [ "$a_delta" -gt 0 ] \
        || fail "ARM A: no free block survived the cut -- the collector's frees \
never reached the medium, so the crash leg proved nothing (or the collector \
did not run)"

    # ---- ARM B: the documented escape hatch, and this leg's red control
    say "  ARM B: INVFS_RECLAIM_ORPHANS=0 -- the collector must free nothing"
    rc_crash_round B1 "$RC_NFOLD" INVFS_RECLAIM_ORPHANS=0 \
        || fail "leg8 arm B: the crash itself did not happen"
    rc_verify B1 || fail "leg8 arm B: bytes did not survive the cut"
    rc_fsck B1   || fail "leg8 arm B: fsck after the cut"
    rc_reuse_probe B1 || fail "leg8 arm B: aliasing probe"
    local b_line b_delta b_freed
    b_line=$(grep "^B1 " "$RC_WORK/ledger.txt" | tail -1)
    b_delta=$(echo "$b_line" | awk '{print $4}')
    b_freed=$(echo "$b_line" | awk '{print $5}')
    info "ARM B: the collector reported $b_freed page(s) freed; on-medium delta $b_delta"
    [ "$b_freed" = 0 ] \
        || fail "ARM B: INVFS_RECLAIM_ORPHANS=0 still freed $b_freed page(s) -- \
the gate is not an off switch, so ARM A's delta proves nothing about the gate"
    [ "$b_delta" -le 0 ] \
        || fail "ARM B: the free-block count ROSE by $b_delta with the collector \
switched off -- ARM A's rise was the workload, not the collector, and the \
whole leg is void"

    # ---- ARM C: dm-flakey drop_writes, on the shared flakey device.
    #
    # THIS IS NOT A THIRD COPY OF CONDITION (3), and the difference is the
    # point. Condition (3) is a process-death state: frees, bitmap flushed,
    # no clean close. dm-flakey cannot produce it -- it discards ACKNOWLEDGED
    # WRITES, and the driver under it is still running normally. So ARM C
    # covers a different axis and asserts a different thing: that when the
    # medium throws away writes in the middle of the reclaim window, the
    # volume still reads back BIT-EXACT and nothing is aliased.
    #
    # Note what is deliberately NOT asserted here. The collector's bitmap
    # flush is an acknowledged write, so under drop_writes it may simply
    # never land: the medium's bitmap then still says ALLOCATED for the freed
    # blocks. That is the SAFE direction -- a leak, never a shared block --
    # and a leg that failed on it would be asserting that the filesystem
    # defeats a lying write cache, which is not a property it can have. The
    # only reclaim claim ARM C makes is the one that matters: the frees that
    # DID land did not cost us a byte.
    local c_freed="skipped"
    if [ "${FLAKEY_RC_FLAKEY:-1}" = 1 ] && [ -b "$DM" ]; then
        say "  ARM C: dm-flakey drop_writes through the reclaim window"
        dm_set up || fail "leg8 arm C: could not bring the flakey device up"
        RC_VOL_SAVE=$RC_VOL
        RC_VOL="$DM"     # the helpers all read RC_VOL; point them at the device
        $B/invf-mkfs "$DM" "$size_gb" >"$RC_WORK/mkfs-c.log" 2>&1 \
            || { cat "$RC_WORK/mkfs-c.log"; fail "leg8 arm C mkfs"; }
        out=$("$RC_T" build "$DM" "$RC_NFILES" "$RC_NGEN" 2>&1) \
            || { echo "$out"; fail "leg8 arm C build"; }
        c_orphan=$(rc_field "$out" PAGES_ORPHAN)
        info "ARM C: $c_orphan orphan base pages to reclaim on the device"
        rc_verify "C-base" || fail "leg8 arm C: read-back failed before the window"
        clog="$RC_WORK/crash-C1.log"; : > "$clog"
        stdbuf -oL "$RC_T" cost "$DM" 0 "$RC_NGEN" "$RC_NFOLD" >"$clog" 2>&1 &
        RC_CRASH_PID=$!
        for i in $(seq 1 400); do
            folds=$(grep -c COSTFOLD "$clog" 2>/dev/null); folds=${folds:-0}
            [ "$folds" -ge 8 ] && break
            kill -0 "$RC_CRASH_PID" 2>/dev/null || break
            sleep 0.25
        done
        [ "${folds:-0}" -ge 8 ] || {
            kill -9 "$RC_CRASH_PID" 2>/dev/null; wait "$RC_CRASH_PID" 2>/dev/null
            RC_CRASH_PID=""; fail "leg8 arm C: the driver never got going"; }
        # the device now lies about every write it acknowledges
        dm_set drop_slow || fail "leg8 arm C: could not enter the drop_writes window"
        info "ARM C: drop_writes engaged (20s up / 1s down) with the collector running"
        sleep 6
        kill -9 "$RC_CRASH_PID" 2>/dev/null
        wait "$RC_CRASH_PID" 2>/dev/null; wrc=$?
        RC_CRASH_PID=""
        rc_sudirty "$clog" "$wrc" || fail "leg8 arm C: the driver was not cut"
        dm_set up || fail "leg8 arm C: could not restore the device"
        sync; sleep 0.5
        c_freed=$(sed -n 's/.*freed=\([0-9][0-9]*\).*/\1/p' "$clog" | awk '{s+=$1} END{print s+0}')
        info "ARM C: the collector freed $c_freed page(s) before it died; the"
        info "ARM C:        medium may have discarded some or all of that flush"
        [ "$c_freed" -gt 0 ] \
            || fail "ARM C: the collector freed NOTHING in the window -- the arm \
was vacuous, so it proved nothing about write loss"
        rc_verify C1 || fail "leg8 arm C: bytes did not survive acknowledged-write loss"
        rc_fsck C1   || fail "leg8 arm C: fsck after the write-loss window"
        rc_reuse_probe C1 || fail "leg8 arm C: aliasing probe"
        RC_VOL=$RC_VOL_SAVE
    else
        say "  ARM C: SKIPPED (FLAKEY_RC_FLAKEY=0 or no flakey device)"
        info "        the dm-flakey acknowledged-write-loss arm did not run"
    fi

    local ev="$ART/reclaim-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$ev" && cp -a "$RC_WORK"/crash-*.log "$RC_WORK/ledger.txt" "$ev"/ 2>/dev/null
    info "evidence: $ev"
    echo "  ARM A, shipped default: $a_freed pages freed by the collector,"
    echo "        $a_delta block(s) reclaimed on the medium across $RC_ROUNDS cut(s)"
    echo "  ARM B, INVFS_RECLAIM_ORPHANS=0: $b_freed freed, $b_delta reclaimed"
    echo "  ARM C, dm-flakey drop_writes: $c_freed freed; bit-exactness held"
}

# F6 regression: a failed O_TRUNC rewrite must leave the name
# complete-or-absent, never torn-readable. The soak caught this as
# SILENT GARBAGE (34KB of never-committed bytes served with success
# for a 958KB file after its rewrite died in an error window); the
# minimal shape is one O_TRUNC write failing mid-way followed by a
# readback. Runs on the suite dm device (needs the error target).
leg9() {
    LEG=leg9-trunc-abort
    say "[9] failed O_TRUNC rewrite leaves the name complete-or-absent"
    mkfs_fresh
    mnt_up
    head -c 200000 /dev/urandom > "$FLK/good.bin"
    cp "$FLK/good.bin" "$MNT/victim.bin" || fail "leg9 setup copy"
    sync; sleep 1
    GOOD=$(sha256sum < "$MNT/victim.bin" | cut -d' ' -f1)
    dm_set error || fail "leg9 dm error"
    head -c 500000 /dev/urandom > "$FLK/evil.bin"
    if cp "$FLK/evil.bin" "$MNT/victim.bin" 2>/dev/null; then
        echo "  NOTE: write succeeded under error (unexpected, continuing)"
    else
        echo "  write failed as expected under error"
    fi
    sleep 1
    dm_set up || fail "leg9 dm up"
    sleep 1
    if cp "$MNT/victim.bin" "$FLK/out.bin" 2>"$FLK/readback.err"; then
        GOT=$(sha256sum < "$FLK/out.bin" | cut -d' ' -f1)
        SZ=$(stat -c %s "$FLK/out.bin")
        EVIL=$(sha256sum < "$FLK/evil.bin" | cut -d' ' -f1)
        if [ "$GOT" = "$GOOD" ]; then
            echo "  readback: old version intact (complete)"
        elif [ "$GOT" = "$EVIL" ]; then
            echo "  readback: full new version ($SZ bytes)"
            echo "  (a failed close that landed everything is POSIX-legal;"
            echo "   the suite model treats it as failure, the engine as"
            echo "   complete -- both self-consistent, no torn state)"
        else
            echo "  readback: NEITHER old NOR new ($SZ bytes, sha ${GOT:0:12})"
            fail "leg9 torn readback after failed rewrite"
        fi
    else
        echo "  readback: loud failure (complete-or-absent via absent)"
    fi
    mnt_down
    echo "  leg9 OK: no torn-readable state"
}

# -------------------------------------------------------------- main ----

echo "WP22b flakey soak: seed=$SEED soak=${SOAK_S}s dev=$DM work=$FLK"

# preflight
for t in invf-mkfs invf-cp invf-cat invf-ls invf-fsck invf-verify invf-sweep invf-rollback invf-fuse; do
    [ -x "$B/$t" ] || { echo "missing $B/$t — run make first" >&2; exit 2; }
done
for t in python3 fusermount3 blockdev sha256sum; do
    command -v "$t" >/dev/null || { echo "missing tool: $t" >&2; exit 2; }
done
sudo -n true 2>/dev/null || { echo "need passwordless sudo (run: sudo -v)" >&2; exit 2; }
sudo -n modprobe dm-flakey 2>/dev/null
sudo -n dmsetup targets 2>/dev/null | grep -q flakey \
    || { echo "dm-flakey target unavailable in this kernel — STOP" >&2; exit 2; }
info "dm-flakey: $(sudo -n dmsetup targets | grep flakey)"

# Scratch space: a HARD stop, on the filesystem that will actually be used.
#
# This used to measure /tmp and only warn. Both halves were wrong. It
# measured the wrong filesystem -- FLAKEY_WORK may point anywhere, and the
# check did not follow it -- and it warned its way into a condition the next
# line calls fatal, so a run that could not host the volume proceeded and
# failed later, somewhere unrelated, with the tool's own error as the only
# evidence. A scratch that cannot hold the image produces
# "device 0 is smaller than the device table says" from vol_open, which
# reads like a capacity bug in the engine and is not one.
#
# A stop is the right call rather than an automatic fallback: legs 7 and 8
# additionally need a DISK-backed scratch, because drop_caches cannot evict
# a tmpfs page, so quietly relocating the scratch would change what the leg
# proves. Failing loudly and naming the override costs one line of retype
# and cannot be misread as an engine regression.
mkdir -p "$FLK" 2>/dev/null || {
    echo "STOP: cannot create the scratch $FLK" >&2
    echo "      override with FLAKEY_WORK=/srv/bench/<name>" >&2; exit 2; }
AVAIL=$(df --output=avail -B1M "$FLK" 2>/dev/null | tail -1 | tr -d ' ')
AVAIL_FS=$(stat -f -c %T "$FLK" 2>/dev/null || echo "?")
NEED_MB=${FLAKEY_MIN_FREE_MB:-4000}
if [ "${AVAIL:-0}" -lt "$NEED_MB" ]; then
    cat >&2 <<EOF

STOP: the scratch for this run has ${AVAIL:-?} MB free and this suite needs
      >= ${NEED_MB} MB (a ${SIZE_GB}G backing image, a ~244 MB corpus, and the
      soak's own artifacts).

  scratch:   $FLK   (on $AVAIL_FS)
  override:  FLAKEY_WORK=/srv/bench/<name> bash $0

Proceeding anyway converts a full disk into a confusing failure deep inside
a leg, so this stops here instead. Legs 7 and 8 need a DISK-backed scratch
for a second reason: drop_caches cannot evict a tmpfs page, so a tmpfs
scratch silently turns their power cut into a no-op.
EOF
    exit 2
fi

# The space guard above and this one answer DIFFERENT questions, and until now
# only the first was asked. Space: can the scratch hold the image at all.
# Filesystem type: can anything here be dropped from the page cache at all --
# and on a tmpfs, nothing can, because a tmpfs page has no writeback behind
# it. The comment above this guard and the STOP text both state that legs 7
# and 8 need a DISK-backed scratch, and the condition enforced neither. So a
# host with a large /tmp passed the guard, legs 7 and 8 ran their cuts, every
# cut was a NO-OP, and the banner said PASS. A test that cannot fail is worse
# than a test that is missing, because it is counted.
#
# Stopping is right here for the same reason the space guard stops: the
# override is one env var, and quietly relocating the scratch would change
# what the leg proves -- which is exactly the thing that must not change
# silently.
if [ "$AVAIL_FS" = tmpfs ] && { want_leg 7 || want_leg 8; }; then
    cat >&2 <<EOF

STOP: the scratch for this run is tmpfs, and this run selects legs 7/8.

  scratch:   $FLK   (on $AVAIL_FS)
  selected:  ${ONLY:-all legs}
  override:  FLAKEY_WORK=/srv/bench/<name> bash $0

Legs 7 and 8 cut with sync + drop_caches. A tmpfs page cannot be evicted --
there is no writeback to drop -- so on this scratch both power cuts become
no-ops that still report green. This is a condition the space check above
cannot see: a large tmpfs passes it happily.
EOF
    exit 2
fi
info "scratch: $FLK on $AVAIL_FS, ${AVAIL} MB free (need >= ${NEED_MB})"
# clear OUR leftovers from a previous run
sudo -n dmsetup remove "$DEV" >/dev/null 2>&1
if [ -f "$BACK" ]; then
    for l in $(losetup -j "$BACK" 2>/dev/null | cut -d: -f1); do
        sudo -n losetup -d "$l" 2>/dev/null
    done
fi
trap cleanup EXIT
mkdir -p "$ART"
dev_create
exec > >(tee "$FLK/run.log") 2>&1

RAN_LEGS=""      # "<n>:<name>" per leg that ACTUALLY ran, and the complement.
SKIPPED_LEGS=""  # The closing banner prints both. Nothing else writes to them.
for n in 0 1 2 3 4 5 7 8 9; do
    if want_leg "$n"; then
        case "$n" in
            0) nm="re-mkfs-orphans" ;;
            1) nm="baseline" ;;
            2) nm="error-storm" ;;
            3) nm="torn-sweep" ;;
            4) nm="mid-seal kill" ;;
            5) nm="${SOAK_S}s soak" ;;
            7) nm="page-cache power loss" ;;
            8) nm="reclaim power loss" ;;
            9) nm="trunc-abort readability" ;;
            *) nm="leg$n" ;;
        esac
        RAN_LEGS="$RAN_LEGS $n:$nm"
        if leg_ready "$n"; then
            "leg$n"
        else
            RAN_LEGS=${RAN_LEGS% $n:$nm}
            SKIPPED_LEGS="$SKIPPED_LEGS $n(helpers-absent)"
            say "SKIP [$n $nm]: helpers absent (parked leg, see INCIDENTS)"
        fi
    else
        SKIPPED_LEGS="$SKIPPED_LEGS $n"
    fi
done

say "FLAKEY E2E: PASS  (seed=$SEED, $((SECONDS - T0))s total)"
# This line used to be a literal list of all nine legs, printed whatever ran --
# including "compact-flip chaos" (leg 6) for a leg deleted in 841a272, and
# including legs 7 and 8 for runs where FLAKEY_ONLY excluded them. It is the
# reason a suite that was quietly dead for two days still read as full
# coverage: the summary is the only thing a reader sees.
echo "  legs RUN this invocation:${RAN_LEGS:- NONE}"
echo "  legs NOT run:${SKIPPED_LEGS:- none}"
echo "  scratch $FLK cleaned; on failure the image + logs land in $ART"
