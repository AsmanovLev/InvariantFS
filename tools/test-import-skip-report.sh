#!/bin/bash
# test-import-skip-report.sh — WP138: invf-import must never drop a path
# without naming it.
#
# THE DEFECT THIS EXISTS FOR. `invf-import` counted a "skip" in one
# unsigned long and recorded nothing else. Run as an ordinary user against a
# root filesystem, `open()` returns EACCES on /etc/shadow and every other
# root-only file and `opendir()` returns EACCES on /root, so every one of
# them was dropped -- the tool exited 0, its summary carried a bare count,
# and `invf-fsck` reported OK on the result, because nothing in the volume
# references a file that is not there. That is silent ABSENCE, a different
# failure shape from a wrong byte: there is no damage for a checker to find.
#
# The fix reports every skipped path with the reason it was skipped, and
# distinguishes the class that is data loss (unreadable source) from the
# class that is a format limit. This suite asserts BOTH halves of the
# finding separately, because either alone would pass on the old code:
#
#   leg 0  the fixture is genuinely unreadable to the RUNNING user. If it
#          is not, every later leg passes vacuously -- so this asserts the
#          EACCES directly instead of trusting the chmod.
#   leg 1  a readable file still imports and comes back BIT-EXACT
#          (invf-cat + cmp). The report must not break the good path.
#   leg 2  each unreadable path is ABSENT from the volume AND NAMED in the
#          diagnostic. Absent-but-silent is the defect; present-but-warned
#          is the fix.
#   leg 3  the number of "skipped" lines equals the summary's count. The
#          report is exhaustive, and the count and the list are the same
#          number by construction rather than by coincidence.
#   leg 4  an unreadable DIRECTORY is reported as a directory, saying its
#          entries went with it. One increment hid an entire subtree, so a
#          reader must never take the count for a file count.
#   leg 5  --fail-on-skip exits 3; without it the exit is still 0 (the fix
#          must not break an existing caller that ignores stderr).
#   leg 6  a fully readable tree reports NOTHING: no skip lines, no
#          warning. A report that cries wolf on the happy path gets
#          ignored on the day it matters.
#
# WHY IT NEEDS NO ROOT. The fixture is built BY the ordinary user and then
# mode-denied to ITSELF (chmod 000), so the kernel returns the same EACCES
# that /etc/shadow returns to a non-root importer, at the same line, for
# the same reason. `make test` on a build host is frequently run as root,
# and root bypasses the denial -- every leg would pass vacuously. So when
# this suite is root it RE-EXECS ITSELF as `nobody` and the assertions run
# unprivileged; it only declines, loudly, on a host where it cannot drop.
#
# Run:  bash tools/run-e2e.sh tools/test-import-skip-report.sh
#   or:  bash tools/test-import-skip-report.sh     (from make test)
set -u
set -o pipefail

# leg -1: drop to an unprivileged uid. Done first, before anything else,
# because the whole suite is meaningless as root -- see the header.
#
# Each candidate is PROBED before it is used. `command -v` is not enough:
# under a user namespace created by `unshare -r` (which is how
# tools/run-e2e.sh runs an ISOLATED suite, and why this suite is not run
# through $(TESTISO) in the Makefile) the uid map holds exactly one entry,
# so setpriv EXISTS and still fails with EINVAL on a uid that is not
# mapped. exec-ing into a failing command would take the shell with it and
# turn an honest SKIP into a bare exit 127.
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"

can_drop_to_nobody() {   # can_drop_to_nobody <tool>
    local uid
    case "$1" in
    setpriv) uid=$(setpriv --reuid=65534 --regid=65534 --clear-groups \
                          id -u 2>/dev/null) ;;
    runuser) uid=$(runuser -u nobody -- id -u 2>/dev/null) ;;
    su)      uid=$(su -s /bin/sh nobody -c 'id -u' 2>/dev/null) ;;
    *)       return 1 ;;
    esac
    [ -n "$uid" ] && [ "$uid" != "0" ]
}

if [ "$(id -u)" = "0" ] && [ -z "${INVFS_IMPORT_SKIP_DROPPED:-}" ]; then
    for tool in setpriv runuser su; do
        command -v "$tool" >/dev/null 2>&1 || continue
        can_drop_to_nobody "$tool" || continue
        case "$tool" in
        setpriv)
            exec setpriv --reuid=65534 --regid=65534 --clear-groups \
                env INVFS_IMPORT_SKIP_DROPPED=1 bash "$SELF" "$@" ;;
        runuser)
            exec runuser -u nobody -- \
                env INVFS_IMPORT_SKIP_DROPPED=1 bash "$SELF" "$@" ;;
        su)
            exec su -s /bin/bash nobody -c \
                "INVFS_IMPORT_SKIP_DROPPED=1 bash '$SELF'" ;;
        esac
    done
    cat >&2 <<'EOM'
SKIP: tools/test-import-skip-report.sh must run unprivileged -- it needs to BE
the ordinary user whose /etc/shadow is unreadable -- and this host could not
produce a non-root uid for it. The usual cause is a user namespace with a
single-entry uid map (`unshare -r`, i.e. tools/run-e2e.sh's ISOLATED mode, and
why the Makefile runs this suite WITHOUT $(TESTISO)): setpriv exists there but
every uid outside the map is EINVAL. Run it from a real shell as a non-root
user, or install util-linux's `newuidmap` for a wider map.
Declining rather than reporting a green that would mean nothing: as uid 0 the
fixture's mode-000 files are readable, so every leg passes vacuously -- which
is precisely the failure this suite exists to catch.
EOM
    exit 0
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
B=$REPO/bin

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "  ok: $* (as uid $(id -u))"; }

for t in invf-import invf-mkfs invf-cat invf-fsck; do
    [ -x "$B/$t" ] || fail "bin/$t missing (run make)"
    [ -r "$B/$t" ] || fail "bin/$t not readable by uid $(id -u)"
done

# A private dir per run: /dev/shm, and unique, so concurrent runs (e2e
# slots) never collide and never inherit a previous run's mode-000 tree.
WORK=$(mktemp -d /dev/shm/importskip.XXXXXX) || fail "cannot create a work dir"
# The fixture ends up containing mode-000 files AND directories; a plain
# `rm -rf` cannot unlink entries inside a directory it cannot open, so the
# cleanup re-opens them first.
cleanup() { chmod -R u+rwX "$WORK" 2>/dev/null; rm -rf "$WORK" 2>/dev/null; }
trap cleanup EXIT
R=$WORK/tree

# ---------------------------------------------------------------- fixture
# shaped like the trixie rootfs that started this: readable files around
# root-only ones, plus an unreadable DIRECTORY (the /root case, which is
# one increment and a whole subtree).
mkdir -p "$R/etc/security" "$R/etc/uid" "$R/var/log" "$R/root" "$R/usr/bin"
printf 'root:$6$abc$def:19000:0:99999:7:::\n' > "$R/etc/shadow"
printf 'root:*:19000:0:99999:7:::\n'         > "$R/etc/shadow-"
printf 'root:!:19000::::::\n'                > "$R/etc/gshadow"
printf 'root:!::\n'                          > "$R/etc/gshadow-"
printf 'root:$6$x:19000::::::\n'             > "$R/etc/security/opasswd"
: > "$R/etc/.pwd.lock"
printf 'a\x00b\x00c\x00'                    > "$R/var/log/btmp"
printf 'export PATH=/bin\n'                 > "$R/root/.bashrc"
printf '# root profile\n'                    > "$R/root/.profile"
printf '9999\n'                             > "$R/etc/uid/0"      # readable sibling
printf 'debian\n'                           > "$R/etc/hostname"  # readable sibling
printf '#!/bin/sh\necho hi\n'               > "$R/usr/bin/hi"
chmod 644 "$R/etc/hostname" "$R/etc/uid/0" "$R/usr/bin/hi"
# The real rootfs files are root-owned mode 0640/0660; the running user is
# denied read on them the same way, and reaches the same EACCES at the same
# syscall, whether the denial is by mode or by ownership.
chmod 000 "$R"/etc/shadow "$R"/etc/shadow- "$R"/etc/gshadow \
            "$R"/etc/gshadow- "$R"/etc/security/opasswd \
            "$R"/etc/.pwd.lock "$R"/var/log/btmp
chmod 000 "$R/root"

mkvol() {  # mkvol <img>
    rm -f "$1"
    INVFS_META_FRAC=16 "$B/invf-mkfs" "$1" 64 >"$1.mkfs" 2>&1 \
        || { cat "$1.mkfs"; fail "mkfs failed for $1"; }
}

# --------------------------------------------------------------- leg 0
echo "== leg 0: the fixture is genuinely unreadable to uid $(id -u) =="
if cat "$R/etc/shadow" >/dev/null 2>&1; then
    fail "leg 0: $R/etc/shadow is READABLE -- every leg below would pass vacuously"
fi
if ls "$R/root" >/dev/null 2>&1; then
    fail "leg 0: $R/root is LISTABLE -- the directory leg would pass vacuously"
fi
cat "$R/etc/hostname" >/dev/null 2>&1 \
    || fail "leg 0: the readable control file is not readable; the fixture is wrong"
ok "7 files and 1 directory are unreadable; the sibling files are readable"

# --------------------------------------------------------------- leg 1/2/3/4
echo "== legs 1-4: import as the ordinary user =="
mkvol "$WORK/v.img"
"$B/invf-import" "$WORK/v.img" "$R" >"$WORK/out.txt" 2>"$WORK/err.txt"
RC=$?
cat "$WORK/out.txt"
SUMMARY_SKIPPED=$(sed -n 's/.*, \([0-9][0-9]*\) skipped in .*/\1/p' "$WORK/out.txt" | head -1)
[ -n "$SUMMARY_SKIPPED" ] || fail "leg 1: no 'N skipped' in the summary line"

echo "--- leg 1: the readable files are imported and bit-exact ---"
"$B/invf-cat" "$WORK/v.img" etc/hostname > "$WORK/hostname.out" 2>/dev/null \
    || fail "leg 1: etc/hostname did not import"
cmp -s "$WORK/hostname.out" "$R/etc/hostname" \
    || fail "leg 1: etc/hostname came back DIFFERENT from the source"
"$B/invf-cat" "$WORK/v.img" etc/uid/0 > "$WORK/uid0.out" 2>/dev/null \
    || fail "leg 1: etc/uid/0 did not import"
cmp -s "$WORK/uid0.out" "$R/etc/uid/0" \
    || fail "leg 1: etc/uid/0 came back DIFFERENT from the source"
ok "readable siblings imported and byte-identical (invf-cat + cmp)"

echo "--- leg 2: every unreadable path is ABSENT and NAMED ---"
# The two halves are asserted separately on purpose: "absent" alone passes
# on main, and "named" alone would pass if the path were also imported.
for rel in etc/shadow etc/shadow- etc/gshadow etc/gshadow- \
           etc/security/opasswd etc/.pwd.lock var/log/btmp; do
    if "$B/invf-cat" "$WORK/v.img" "$rel" >/dev/null 2>&1; then
        fail "leg 2: $rel is IN the volume but the source denies reading it"
    fi
    grep -q "invf-import: skipped $R/$rel: " "$WORK/err.txt" \
        || fail "leg 2: $rel is absent from the volume and NOT named on stderr -- this is the defect"
done
ok "7 unreadable files: absent from the volume AND named in the diagnostic"

echo "--- leg 3: the report is exhaustive (count == number of lines) ---"
LINES=$(grep -c '^invf-import: skipped ' "$WORK/err.txt")
[ "$LINES" -eq "$SUMMARY_SKIPPED" ] \
    || fail "leg 3: summary says $SUMMARY_SKIPPED skipped but stderr names $LINES -- the report is not exhaustive"
grep -q "^WARNING: $SUMMARY_SKIPPED path(s) were NOT imported" "$WORK/out.txt" \
    || fail "leg 3: stdout does not restate the count, so a reader who drops stderr still cannot see it"
ok "summary count $SUMMARY_SKIPPED == $LINES skip lines, restated on stdout"

echo "--- leg 4: the unreadable DIRECTORY says its entries went with it ---"
grep -q "^invf-import: skipped $R/root: " "$WORK/err.txt" \
    || fail "leg 4: the unreadable directory is not named"
grep "^invf-import: skipped $R/root: " "$WORK/err.txt" | grep -q "every entry under it was skipped" \
    || fail "leg 4: the directory skip does not say its whole subtree is gone -- one increment hid two files"
for rel in root/.bashrc root/.profile; do
    "$B/invf-cat" "$WORK/v.img" "$rel" >/dev/null 2>&1 \
        && fail "leg 4: $rel is IN the volume"
done
# 8 skips, 9 files absent: the count is not a file count, and the report has
# to say so rather than let a reader divide.
[ "$SUMMARY_SKIPPED" -eq 8 ] \
    || fail "leg 4: expected 8 skips (7 files + 1 directory), got $SUMMARY_SKIPPED"
ok "directory skip names itself, says the subtree went, and hides 2 files for 1 increment"

echo "--- fsck still cannot see it (this is why the report is the only defence) ---"
"$B/invf-fsck" "$WORK/v.img" >"$WORK/fsck.txt" 2>&1
grep -q '^OK$' "$WORK/fsck.txt" \
    || fail "leg 4: fsck is no longer OK on a volume with 9 files missing -- the premise of this suite changed"
ok "invf-fsck reports OK: nothing in the volume references what is missing"

# --------------------------------------------------------------- leg 5
echo "== leg 5: --fail-on-skip =="
mkvol "$WORK/v2.img"
"$B/invf-import" --fail-on-skip "$WORK/v2.img" "$R" >/dev/null 2>&1
[ $? -eq 3 ] || fail "leg 5: --fail-on-skip exit was $?, want 3"
mkvol "$WORK/v3.img"
"$B/invf-import" "$WORK/v3.img" "$R" >/dev/null 2>&1
[ $? -eq 0 ] || fail "leg 5: plain import exit was $?, want 0 (the fix must not break existing callers)"
ok "--fail-on-skip exits 3; plain import still exits 0"

# --------------------------------------------------------------- leg 6
echo "== leg 6: a readable tree reports nothing =="
mkdir -p "$WORK/clean"
printf 'clean\n' > "$WORK/clean/a"
printf 'also clean\n' > "$WORK/clean/b"
mkvol "$WORK/v4.img"
"$B/invf-import" --fail-on-skip "$WORK/v4.img" "$WORK/clean" \
    >"$WORK/out2.txt" 2>"$WORK/err2.txt"
RC=$?
cat "$WORK/out2.txt"
[ $RC -eq 0 ] || fail "leg 6: a clean tree exited $RC"
grep -q ' 0 skipped in ' "$WORK/out2.txt" \
    || fail "leg 6: a clean tree reported a non-zero skip count"
grep -q '^invf-import: skipped ' "$WORK/err2.txt" \
    && fail "leg 6: a clean tree printed a skip line"
grep -q '^WARNING:' "$WORK/out2.txt" \
    && fail "leg 6: a clean tree printed a WARNING"
ok "no skip lines, no warning, exit 0 -- a report that cries wolf gets ignored"

echo "PASS: test-import-skip-report.sh"