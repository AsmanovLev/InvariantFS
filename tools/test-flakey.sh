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
#   6  journal compaction under drops (WP22d): every flush forced into a
#      slot flip (INVFS_JRN_FORCE_COMPACT) across seeded drop_writes
#      windows -- incl. the legacy->slot migration flip -- then recovery,
#      fsck/verify clean, files bit-exact.
#   7  page-cache power loss (WP83): the device is NEVER touched by chaos --
#      it is healthy the whole time -- and the bytes are only ever in the
#      host page cache. Write + fsync through the mount, kill -9 the daemon
#      with no clean close, sync + drop_caches (the power-cut surrogate),
#      reopen: every acknowledged byte intact, and a file rewritten in
#      place is the COMPLETE old or COMPLETE new content, never a splice.
#   (repeatability: fixed seeds; any failure preserves the backing
#      image + all logs under tools/flakey/artifacts/<leg>-<ts>/.)
#
# Env knobs: FLAKEY_SEED (default 20260831), FLAKEY_SOAK_S (default 210),
#            FLAKEY_WORK (default /tmp/invfs-flakey — tmpfs, needs ~4G of
#            quota headroom; /dev/shm is too full on this box),
#            FLAKEY_ONLY (e.g. "3" runs just that leg, a dev aid),
#            FLAKEY_PC_WORK (leg 7 scratch; MUST be on a disk-backed
#            filesystem — default /var/tmp/invfs-flakey-pagecache),
#            FLAKEY_PC_BACKING (leg 7: loop = a loop device over the image
#            (default when losetup works), file = the image file itself, the
#            unprivileged path),
#            FLAKEY_PC_SIZE_MB (leg 7 volume size, default 512).
#
# Needs: dm-flakey (modprobe dm-flakey), losetup, fusermount3, python3,
#        passwordless sudo (the script is sudo-aware; `sudo -v` first if
#        unsure). Cleans its own scratch ($FLK) on success; on failure the
#        scratch moves into the artifacts dir.
set -uo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
B=$REPO/bin
FLK=${FLAKEY_WORK:-/tmp/invfs-flakey}
ART=$REPO/tools/flakey/artifacts
DEV=${FLAKEY_DEV:-invfs_flakey}
DM=/dev/mapper/$DEV
BACK=$FLK/backing.img
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
# Its own scratch, and its own volume: legs 0-6 all run on the dm-flakey
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

say()  { echo; echo "== $* =="; }
info() { echo "  $*"; }

# ---------------------------------------------------------------- util --

cleanup() {
    set +e
    [ -n "$CHAOS_PID" ] && { kill "$CHAOS_PID" 2>/dev/null; wait "$CHAOS_PID" 2>/dev/null; }
    fusermount3 -u "$MNT" 2>/dev/null
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
        [ -n "$PC_MNT" ] && fusermount3 -uz "$PC_MNT" 2>/dev/null
        pkill -9 -f "invf-fuse -f $PC_VOL" 2>/dev/null
        [ -n "$PC_LOOP" ] && sudo -n losetup -d "$PC_LOOP" 2>/dev/null
        rm -rf "$PC_WORK"
    }
    [ "$FAILED" = 0 ] && rm -rf "$FLK"
}

preserve() {   # copy the ground truth + every log for replay
    local dst="$ART/${LEG:-preflight}-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$dst"
    cp -a "$FLK"/*.log "$dst/" 2>/dev/null
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

# ------------------------------------------------------------- devices --

dm_set() {   # dm_set up|drop|error — swap the live dm table
    local spec
    case "$1" in
        up)    spec="0 $SEC flakey $LOOP 0 3600 0";;
        drop)  spec="0 $SEC flakey $LOOP 0 1 1 1 drop_writes";;
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
    truncate -s "${SIZE_GB}G" "$BACK"
    LOOP=$(sudo -n losetup -f --show "$BACK") || { echo "losetup failed" >&2; exit 2; }
    SEC=$(sudo -n blockdev --getsz "$LOOP")
    sudo -n dmsetup create "$DEV" --table "0 $SEC flakey $LOOP 0 3600 0" \
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
        $B/invf-cp "$DM" "$1/$f" "$f" >/dev/null 2>&1 || fail "invf-cp $f"
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
    if grep -q "orphans:" "$FLK/fsck0.log" &&
       ! grep -q "orphans:      0" "$FLK/fsck0.log"; then
        echo "  re-mkfs+import leaked orphans (stale journal replayed into the fresh bitmap):"
        cat "$FLK/fsck0.log"
        fail "leg0 re-mkfs orphan gate"
    fi
    grep -q "^OK$" "$FLK/fsck0.log" \
        || { cat "$FLK/fsck0.log"; fail "leg0 fsck not clean"; }
    echo "  re-mkfs + import: 0 orphans (stale journal fully erased)"
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
    sleep 6
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
    # sweep starts healthy; chaos begins only after the WP21 checkpoint is
    # armed (that machinery is designed to be in place before the walk)
    $B/invf-sweep "$DM" >"$FLK/sweep3.log" 2>&1 &
    local swpid=$!
    local i
    for i in $(seq 1 100); do
        grep -q "checkpoint: #" "$FLK/sweep3.log" 2>/dev/null && break
        sleep 0.1
    done
    grep -q "checkpoint: #" "$FLK/sweep3.log" || fail "checkpoint never armed"
    info "checkpoint armed; seeded drop_writes windows on"
    python3 "$REPO/tools/flakey/dmchaos.py" "$DEV" "$LOOP" "$SEC" \
        $((SEED + 300)) 600 "$FLK/stop3" drop >"$FLK/chaos3.log" 2>&1 &
    CHAOS_PID=$!
    wait $swpid
    local src=$?
    touch "$FLK/stop3"; wait $CHAOS_PID 2>/dev/null; CHAOS_PID=""
    dm_set up
    info "sweep rc=$src under chaos ($(grep -c drop "$FLK/chaos3.log") drop windows)"
    [ "$src" = 0 ] || info "sweep failed loudly (acceptable): rc=$src"
    tail -2 "$FLK/sweep3.log"
    recover "leg3" || fail "recovery ladder dead-ended"
    fsck_ok "leg3" || fail "fsck after recovery"
    verify_clean "leg3" || fail "verify after recovery"
    # sweep never changes file bytes: every file must equal its original
    vol_files_exact "$FLK/orig3" "post-recovery" || fail "third state content"
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
        grep -q "checkpoint: #" "$FLK/seal4a.log" 2>/dev/null && break
        sleep 0.1
    done
    python3 "$REPO/tools/flakey/dmchaos.py" "$DEV" "$LOOP" "$SEC" \
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
        grep -q "checkpoint: #" "$FLK/seal4b.log" 2>/dev/null && break
        sleep 0.1
    done
    python3 "$REPO/tools/flakey/dmchaos.py" "$DEV" "$LOOP" "$SEC" \
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
        --dev "$DM" --dmname "$DEV" --loop "$LOOP" --sectors "$SEC" \
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
leg6() {
    LEG=leg6-compact-flip
    say "[6] journal compaction (slot flip) under drop_writes windows"
    mkfs_fresh
    # re-mkfs on a dirty device must leave NO trace of the previous volume
    # (the inode-area full-erase regression guard)
    $B/invf-fsck "$DM" >"$FLK/fsck6-postmkfs.log" 2>&1 || true
    grep -qE 'live files:\s+0$' "$FLK/fsck6-postmkfs.log" \
        || { cat "$FLK/fsck6-postmkfs.log"; fail "mkfs left stale records (pre-compact)"; }
    gen_corpus "$FLK/orig6" $((SEED + 6)) small
    import_all "$FLK/orig6"
    manifest_build "$FLK/orig6" > "$FLK/manifest6"
    fsck_ok "pre-compact" || fail "fsck pre-compact"
    python3 "$REPO/tools/flakey/dmchaos.py" "$DEV" "$LOOP" "$SEC" \
        $((SEED + 600)) 900 "$FLK/stop6" drop >"$FLK/chaos6.log" 2>&1 &
    CHAOS_PID=$!
    local round src
    for round in 1 2 3; do
        INVFS_JRN_FORCE_COMPACT=1 $B/invf-sweep "$DM" >"$FLK/sweep6-$round.log" 2>&1
        src=$?
        [ "$src" = 0 ] || info "sweep round $round failed loudly under chaos (acceptable): rc=$src"
        grep -E "sweep done|failed" "$FLK/sweep6-$round.log" | tail -1 | sed 's/^/  /'
    done
    touch "$FLK/stop6"; wait $CHAOS_PID 2>/dev/null; CHAOS_PID=""
    dm_set up
    info "compaction chaos over ($(grep -c drop "$FLK/chaos6.log") drop windows)"
    recover "leg6" || fail "recovery ladder dead-ended"
    fsck_ok "leg6" || fail "fsck after recovery"
    verify_clean "leg6" || fail "verify after recovery"
    vol_files_exact "$FLK/orig6" "post-recovery" || fail "content after compaction chaos"
}

# ---------------------------------------------- leg 7 (WP83): page cache --
# The tier legs 0-6 structurally cannot reach: there the DEVICE is the
# thing that lies (dm-flakey drop_writes / error). Here the device is
# healthy for the whole leg and the bytes never leave the host page cache
# -- which is exactly how `cp` "loses" a file that `ls` showed a second
# earlier on a real block device.
#
#   1. write 9 files through the FUSE mount, fsync() each one, and rewrite
#      two of them in place: one rewrite fsynced, one NOT (its fd is held
#      open by a live writer so the commit can never happen). No unmount,
#      no vol_close -- the fsync barriers are the only thing pinning the
#      acknowledged bytes.
#   2. record the expected sha256 of every generation
#   3. kill -9 the daemon (started with -f, so the PID is the daemon)
#   4. sync + `echo 3 > /proc/sys/vm/drop_caches`: the power-cut surrogate.
#      Every byte that is not on the medium is gone, and the reopen has to
#      re-read structure AND data from the medium.
#   5. reopen: every file present and bit-exact -- through the offline
#      tools AND through a fresh FUSE mount -- verify --deep 0 corrupt,
#      fsck OK.
#   6. THE assertion: a file rewritten in place must be the COMPLETE old
#      content or the COMPLETE new content, never a splice of the two. The
#      fsynced rewrite must be exactly the new content (it was
#      acknowledged); the un-acked one may be either.
#
# What this tier can and cannot prove, stated honestly:
# drop_caches evicts CLEAN pages only -- the kernel will not discard dirty
# ones, so a write that reached the page cache can still be written back
# after the "cut" (v3 barriers every delta append, so the un-acked file
# usually comes back NEW; the leg accepts either generation). Discarding
# un-acked bytes for real is dm-flakey drop_writes = legs 3/4/6. What this
# tier uniquely proves is that the acknowledged bytes, their delta records
# and the bitmap are on the MEDIUM and that the on-disk image is
# self-consistent when re-read from scratch -- i.e. the FUSE fsync ack
# (commit_wctx + vol_sync) is real on v3, which is what the pre-WP80
# `vol_sync` early return made a silent no-op.

pc_work_pick() {   # a DISK-backed scratch: drop_caches is a no-op on tmpfs
    local c
    if [ -z "$PC_WORK" ]; then
        for c in /var/tmp /opt /var/lib; do
            [ -d "$c" ] || continue
            [ "$(stat -f -c %T "$c" 2>/dev/null)" = tmpfs ] && continue
            PC_WORK="$c/invfs-flakey-pagecache"
            break
        done
    fi
    [ -n "$PC_WORK" ] || PC_WORK=/tmp/invfs-flakey-pagecache
    PC_MNT="$PC_WORK/mnt"
    rm -rf "$PC_WORK" && mkdir -p "$PC_WORK" "$PC_MNT" || return 1
    if [ "$(stat -f -c %T "$PC_WORK" 2>/dev/null)" = tmpfs ]; then
        echo "  WARN: $PC_WORK is tmpfs -- drop_caches cannot evict a tmpfs" >&2
        echo "        page, so the page-cache cut below is a NO-OP there." >&2
    fi
    info "page-cache scratch: $PC_WORK ($(stat -f -c %T "$PC_WORK" 2>/dev/null))"
}

pc_dev_create() {  # a loop device when privileged (the raw-device path,
                   # like production), the image file itself otherwise
    pc_work_pick || fail "no scratch for the page-cache leg"
    PC_VOL="$PC_WORK/pc.img"
    PC_SIZE_GB=$(awk -v m="$PC_SIZE_MB" 'BEGIN{printf "%.4f", m/1024}')
    truncate -s "${PC_SIZE_MB}M" "$PC_VOL" || fail "truncate $PC_VOL"
    PC_LOOP=""
    if [ "${FLAKEY_PC_BACKING:-loop}" != file ] &&
       sudo -n true 2>/dev/null &&
       PC_LOOP=$(sudo -n losetup -f --show "$PC_VOL" 2>/dev/null) &&
       [ -n "$PC_LOOP" ]; then
        sudo -n chmod 666 "$PC_LOOP" || fail "chmod $PC_LOOP"
        PC_VOL="$PC_LOOP"
        PC_MODE="loop device $PC_LOOP over pc.img"
    else
        PC_LOOP=""
        PC_VOL="$PC_WORK/pc.img"
        PC_MODE="image file (no loop device: FLAKEY_PC_BACKING=file or no losetup)"
    fi
}

# A volume open on a freshly created LOOP DEVICE can lose a race with the
# host's block-device prober: on this box a ROOT udev worker takes a LOCK_SH
# flock on the new /dev/loopN for a moment (seen in /proc/locks, pid from
# `fuser`), and vol_open's LOCK_EX then fails with "image is in use by
# another process". A LOCK_SH holder is a reader, so retrying cannot endanger
# the volume -- but a persistent conflict IS a real problem (two writers on
# one image) and must still fail. Bounded, and it says so out loud.
pc_run() {         # <label> <logfile> <cmd...>
    local label=$1 log=$2 i rc=1
    shift 2
    for i in $(seq 1 12); do
        "$@" >"$log" 2>&1
        rc=$?
        [ "$rc" = 0 ] && return 0
        grep -q "image is in use by another process" "$log" || return "$rc"
        if [ "$i" = 1 ]; then
            echo "  NOTE: $label lost a flock race with a device prober" >&2
            echo "        (root holds LOCK_SH on the new loop device);" >&2
            echo "        retrying -- a reader cannot endanger the volume." >&2
        fi
        sleep 0.4
    done
    echo "  $label: still locked after 12 attempts" >&2
    cat "$log" >&2
    return "$rc"
}

pc_mnt_up() {     # -f: the daemon stays in the foreground, so $! IS the
                  # daemon and kill -9 $! is an abrupt death (the default
                  # daemonizes, and then $! is a process that already
                  # exited -- the leg would silently become a clean close)
    local i
    for i in $(seq 1 12); do
        $B/invf-fuse -f "$PC_VOL" "$PC_MNT" 2>"$PC_WORK/fuse.log" &
        PC_FUSE_PID=$!
        local j
        for j in $(seq 1 50); do
            grep -q " $PC_MNT " /proc/mounts && return 0
            kill -0 "$PC_FUSE_PID" 2>/dev/null || break   # it gave up
            sleep 0.1
        done
        grep -q "image is in use by another process" "$PC_WORK/fuse.log" || {
            cat "$PC_WORK/fuse.log"; fail "mount of $PC_VOL never appeared"; }
        [ "$i" = 1 ] && echo "  NOTE: mount lost a flock race; retrying" >&2
        sleep 0.4
    done
    cat "$PC_WORK/fuse.log"
    fail "mount of $PC_VOL never appeared (persistent flock conflict)"
}

pc_mnt_down() {   # clean unmount (the leg's LAST mount; the crash mount is
                  # torn down by pc_kill)
    local n=${1:-450} i
    fusermount3 -u "$PC_MNT" 2>/dev/null
    for i in $(seq 1 "$n"); do
        kill -0 "$PC_FUSE_PID" 2>/dev/null || return 0
        sleep 0.2
    done
    kill -9 "$PC_FUSE_PID" 2>/dev/null
    sleep 0.5
    return 1
}

# the power cut: the writer dies first (its fd would otherwise hold the
# stale mount busy), then the daemon dies abruptly -- no vol_close, so no
# final flush and no CLEAN superblock -- then the page cache goes away
pc_kill() { # <writer-pid>
    # wait on both: it reaps them (no stray "Killed" job notice in the log)
    # and proves the death before the leg claims it was abrupt
    [ -n "$1" ] && { kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
    kill -9 "$PC_FUSE_PID" 2>/dev/null
    wait "$PC_FUSE_PID" 2>/dev/null
    kill -0 "$PC_FUSE_PID" 2>/dev/null && { echo "  daemon survived kill -9" >&2; return 1; }
    fusermount3 -uz "$PC_MNT" 2>/dev/null
    return 0
}

pc_cut() {        # the surrogate: write back, then drop the page cache
    sync
    if sudo -n true 2>/dev/null &&
       sudo -n sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null; then
        info "page cache dropped (drop_caches=3): the reopen re-reads the medium"
        return 0
    fi
    echo "  WARN: no root -- the page-cache cut was SKIPPED; this leg" >&2
    echo "        degraded to kill -9 + reopen (process-death only)." >&2
    return 1
}

pc_fsck() {       # <label>: structural gate, log in the page-cache scratch
    pc_run "invf-fsck ($1)" "$PC_WORK/fsck.last" \
        $B/invf-fsck "$PC_VOL" || return 1
    tail -2 "$PC_WORK/fsck.last"
    grep -q "^OK$" "$PC_WORK/fsck.last" && return 0
    echo "  fsck not clean ($1):" >&2; cat "$PC_WORK/fsck.last" >&2
    return 1
}

pc_verify() {     # <label>: rc==0 (0 corrupt AND parity clean-or-absent)
    pc_run "invf-verify ($1)" "$PC_WORK/verify.last" \
        $B/invf-verify "$PC_VOL" --deep || return 1
    grep -E "^parity|^deep" "$PC_WORK/verify.last"
    return 0
}

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
AVAIL=$(df --output=avail -B1M /tmp 2>/dev/null | tail -1 | tr -d ' ')
[ "${AVAIL:-0}" -ge 4000 ] || echo "  WARN: /tmp has ${AVAIL}MB free (want >=4000)"
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

want_leg 0 && leg0
want_leg 1 && leg1
want_leg 2 && leg2
want_leg 3 && leg3
want_leg 4 && leg4
want_leg 5 && leg5
want_leg 6 && leg6
want_leg 7 && leg7

say "FLAKEY E2E: PASS  (seed=$SEED, $((SECONDS - T0))s total)"
echo "  legs: re-mkfs-orphans / baseline / error-storm / torn-sweep / mid-seal kill / ${SOAK_S}s soak / compact-flip chaos / page-cache power loss"
echo "  scratch $FLK cleaned; on failure the image + logs land in $ART"
