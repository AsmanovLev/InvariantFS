#!/bin/sh
# tools/test-fixture-bigvol.sh — WP45 shared big-volume fixture (~30k objects,
# two devices) + canonical-counts.json that every downstream suite sources.
#
# Why: existing fixtures fit one metadata extent; the legacy walkers
# silently no-op once records span MANY dynamic extents (the stage3-sized
# volume is 66k records over 214 extents). This fixture is the minimum e2e
# pressure: a fresh 1 GiB dev0 + 2 GiB dev1 pair (INVFS_DEV1 form), ~30k
# deterministic small objects, ONE invf-import, canonical counts recorded.
#
# Use:
#   tools/test-fixture-bigvol.sh              # self-check suite (default dir)
#   tools/test-fixture-bigvol.sh <workdir>    # build into <workdir>, exit 0/1
#   BIGVOL_LIB=1 . "$REPO/tools/test-fixture-bigvol.sh"   # library use:
#        bigvol_cleanup DIR
#        bigvol_build   DIR      # writes DIR/canonical-counts.json
#
# Run through the e2e lock:
#   INVFS_E2E_AGENT=wp45 bash tools/run-e2e.sh tools/test-fixture-bigvol.sh
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin

# Metadata zone sized for churn-heavy trees (AGENTS 2.7: rootfs-like trees
# with ~30k+ records want 16-24).
INVFS_META_FRAC=16
export INVFS_META_FRAC

bigvol_default_dir() {
    echo /tmp/invfs-e2e-test-fixture-bigvol
}

# Best-effort cleanup: evict stale FUSE mounts then remove the work dir.
bigvol_cleanup() {
    _dir=$1
    if [ -d "$_dir/mnt" ]; then
        timeout 2 fusermount3 -uz "$_dir/mnt" 2>/dev/null || \
        timeout 2 fusermount -uz "$_dir/mnt" 2>/dev/null || true
    fi
    rm -rf "$_dir" 2>/dev/null || true
}

# build the source tree: deterministic pseudo-random content from awk seeds,
# so every rebuild (and every suite) produces byte-identical files.
#   60 top dirs x (495 files + sub/ with 5 files) = 30000 files
#   120 dirs, 40 symlinks
bigvol_mktree() {
    _src=$1
    _nd=${BIGVOL_NDIRS:-60}
    _pf=${BIGVOL_PERDIR:-495}
    _nl=${BIGVOL_NLINKS:-40}
    _d=0
    while [ "$_d" -lt "$_nd" ]; do
        _dp=$(printf '%s/d%02d' "$_src" "$_d")
        mkdir -p "$_dp/sub"
        _d=$((_d + 1))
    done
    # one awk process writes every file (close(path) after each: 30k
    # simultaneous open FDs would blow the limit)
    awk -v src="$_src" -v nf="${_pf}" -v nd="${_nd}" -v nl="${_nl}" 'BEGIN{
        split("the quick brown fox jumps over lazy dog static return size_t "\
              "uint64 heat tier sweep while struct void block record extent "\
              "mapper journal compact shard token buf line", wrds, " ")
        nz = 52
        total = nd * (nf + 5)
        for (i = 0; i < total; i++) {
            d = int(i / (nf + 5)); k = i % (nf + 5)
            if (k < nf) p = sprintf("%s/d%02d/f%05d", src, d, k)
            else        p = sprintf("%s/d%02d/sub/s%05d", src, d, k - nf)
            srand(i * 7919 + 12345)
            size = 200 + (i * 137) % 3800
            out = ""
            n = 0
            while (n < size) {
                w = wrds[int(rand() * nz) + 1]
                out = out w " "
                n += length(w) + 1
            }
            print out > p
            close(p)
        }
        for (i = 0; i < nl; i++)
            printf "d%02d/f%05d", i % nd, (i * 113) % nf \
                > sprintf("%s/l%03d", src, i)
    }'
    _i=0
    while [ "$_i" -lt "$_nl" ]; do
        _lnk=$(printf '%s/l%03d' "$_src" "$_i")
        _tgt=$(cat "$_lnk")
        rm -f "$_lnk"
        ln -s "$_tgt" "$_lnk"
        _i=$((_i + 1))
    done
}

# canonical counts: taken from the SOURCE tree (never from volume walkers,
# which are exactly the thing under test). Writes <workdir>/canonical-counts.json.
bigvol_canon() {
    _src=$1 _out=$2
    _files=$(find "$_src" -mindepth 1 -type f | wc -l)
    _dirs=$(find "$_src" -mindepth 1 -type d | wc -l)
    _links=$(find "$_src" -mindepth 1 -type l | wc -l)
    _bytes=$(find "$_src" -type f -printf '%s\n' | awk '{s+=$1}END{printf "%d", s}')
    cat > "$_out" <<EOF
{
  "dev0": "$_img0",
  "dev1": "$_img1",
  "dirs": $_dirs,
  "files": $_files,
  "links": $_links,
  "logical_bytes": $_bytes,
  "import_dirs": $_imp_dirs,
  "import_files": $_imp_files,
  "import_links": $_imp_links
}
EOF
    [ "$_files" -eq "$_imp_files" ] && [ "$_dirs" -eq "$_imp_dirs" ] && \
      [ "$_links" -eq "$_imp_links" ]
}

# json field fetch (integers / quoted strings): bigvol_get <json> <key>
bigvol_get() {
    _j=$1 _k=$2
    sed -n "s/^[[:space:]]*\"$_k\"[[:space:]]*:[[:space:]]*\\([^,]*\\),\\{0,1\\}/\\1/p" \
        "$_j" | head -1 | tr -d ' "'
}

# Build the whole fixture into DIR. rc 0 on success.
# Env knobs (optional): BIGVOL_NDIRS BIGVOL_PERDIR BIGVOL_NLINKS
bigvol_build() {
    _work=$1
    mkdir -p "$_work" || return 1
    _img0="$_work/dev0.img"
    _img1="$_work/dev1.img"
    _src="$_work/src"
    INVFS_DEV1="$_img1" \
    "$B/invf-mkfs" "$_img0" 1 "$_img1" 2 >"$_work/mkfs.log" 2>&1 || {
        echo "FAIL: mkfs: $(tail -2 "$_work/mkfs.log")"; return 1; }
    mkdir -p "$_src"
    bigvol_mktree "$_src"
    INVFS_DEV1="$_img1" \
    "$B/invf-import" "$_img0" "$_src" >"$_work/import.log" 2>&1 || {
        echo "FAIL: import: $(tail -2 "$_work/import.log")"; return 1; }
    _imp_dirs=$(sed -n 's/^imported: \([0-9]*\) dirs.*/\1/p' "$_work/import.log")
    _imp_files=$(sed -n 's/^imported: [0-9]* dirs, \([0-9]*\) files.*/\1/p' "$_work/import.log")
    _imp_links=$(sed -n 's/^imported: [0-9]* dirs, [0-9]* files, \([0-9]*\) symlinks.*/\1/p' "$_work/import.log")
    [ "${_imp_files:-0}" -gt 0 ] || {
        echo "FAIL: import reported no files: $(tail -2 "$_work/import.log")"; return 1; }
    bigvol_canon "$_src" "$_work/canonical-counts.json" || {
        echo "FAIL: source tree counts disagree with import"; return 1; }
    return 0
}

# ---- library mode -----------------------------------------------------
if [ "${BIGVOL_LIB:-0}" = 1 ]; then
    return 0 2>/dev/null || exit 0
fi

# ---- standalone run: build (opt: self-check) --------------------------
PASS=0; FAIL=0
say(){ echo "[bigvol] $*"; }
ok(){ PASS=$((PASS+1)); say "PASS: $*"; }
bad(){ FAIL=$((FAIL+1)); say "FAIL: $*"; }
fail(){ bad "$*"; echo "RESULT: $PASS passed, $FAIL failed"; exit 1; }

WORK=${1:-$(bigvol_default_dir)}

say "cleanup + fixture build into $WORK"
bigvol_cleanup "$WORK"
mkdir -p "$WORK"

case "$WORK" in /tmp/invfs-e2e-*|/tmp/wp*) ;; *) say "note: workdir is outside /tmp/invfs-e2e-*";; esac

bigvol_build "$WORK" || { bad "fixture build"; echo "RESULT: $PASS passed, $FAIL failed"; exit 1; }
ok "mkfs dev0 1GiB + dev1 2GiB (INVFS_DEV1) + one invf-import"
cat "$WORK/import.log"

COUNTS=$WORK/canonical-counts.json
if [ -s "$COUNTS" ] && [ "$(bigvol_get "$COUNTS" files)" = "$(bigvol_get "$COUNTS" import_files)" ]; then
    ok "canonical-counts.json written and self-consistent ($(bigvol_get "$COUNTS" files) files)"
else
    bad "canonical-counts.json missing/inconsistent"
fi

# tolerance gate: reading back through the walkers is what the component WPs
# fix, so spot-checks here are informational. After WP41-44 land they must
# read bit-exact.
SPOT_OK=0; SPOT_TRY=0; SPOT_ABSENT=0
_i=0
while [ "$_i" -lt 5 ]; do
    _name=$(printf 'd%02d/f%05d' "$_i" "$((_i * 7 + 3))")
    if "$B/invf-cat" "$WORK/dev0.img" "$_name" "$WORK/spot.out" >/dev/null 2>&1; then
        _p=$(printf '%s/d%02d/f%05d' "$WORK/src" "$_i" "$((_i * 7 + 3))")
        if cmp -s "$_p" "$WORK/spot.out"; then SPOT_OK=$((SPOT_OK+1)); fi
    else
        SPOT_ABSENT=$((SPOT_ABSENT+1))
    fi
    _i=$((_i+1))
done
if [ "$SPOT_OK" -eq 5 ]; then
    ok "invf-cat spot-checks bit-exact (5/5)"
else
    say "INFO: spot-checks read $SPOT_OK/5 (walker population zero or partial - component WPs pending)"
fi

echo "RESULT: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
