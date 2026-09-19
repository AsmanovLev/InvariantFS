#!/bin/bash
# tools/test-mkinitcpio-hook.sh — non-boot regression for the mkinitcpio
# integration (WP67).
#
# No root, no mkinitcpio install, no VM: a mocked mkinitcpio environment
# exercises packaging/mkinitcpio/invfs_install and .../invfs_hook.
#
#   1. sh -n (and shellcheck -S error, when available) on both hooks
#   2. install hook: mock add_module/add_binary/add_file/add_runscript and
#      run build(): the fuse module, invf-fuse, chroot and every codecpack
#      file (bin/* via add_binary, data via add_file) are staged, and the
#      runtime script is registered
#   3. runtime hook: rootfstype=invfs and root=invfs:<dev> select
#      invfs_mount_handler; non-invfs roots are not claimed; invfs.init=
#      is a passthrough
#   4. handler: invf-fuse gets rootflags via -o, INVFS_DEV1 is exported from
#      invfs.dev1, and the hand-off `exec chroot` targets invfs.init. The
#      chroot stub must be a real executable in PATH: bash's exec ignores
#      shell functions, and exec here replaces the scenario shell.
#   5. uuid probe: invfs.dev1_uuid selects a device via invf-fuse
#      --probe-uuid (skipped when the host exposes no block device)
#
# Run: bash tools/test-mkinitcpio-hook.sh
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HOOK="$ROOT/packaging/mkinitcpio/invfs_hook"
INSTALL="$ROOT/packaging/mkinitcpio/invfs_install"
TMP=$(mktemp -d /tmp/invfs-test-mkinitcpio.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
pass=0 fail=0

ok()  { printf 'ok   - %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf 'FAIL - %s\n' "$1"; fail=$((fail + 1)); }
check() { # <desc> <needle> <file>
    if grep -qF -- "$2" "$3" 2>/dev/null; then ok "$1"; else
        bad "$1 (missing: $2)"
        sed 's/^/       /' "$3" 2>/dev/null | tail -20
    fi
}

echo "== 1. syntax =="
for f in "$HOOK" "$INSTALL"; do
    if sh -n "$f" 2>"$TMP/err"; then ok "sh -n ${f##*/}"; else bad "sh -n ${f##*/}"; cat "$TMP/err"; fi
done
if command -v shellcheck >/dev/null 2>&1; then
    if shellcheck -s sh -S error "$HOOK" >"$TMP/sc" 2>&1; then ok "shellcheck invfs_hook"; else bad "shellcheck invfs_hook"; cat "$TMP/sc"; fi
    if shellcheck -s bash -S error "$INSTALL" >"$TMP/sc" 2>&1; then ok "shellcheck invfs_install"; else bad "shellcheck invfs_install"; cat "$TMP/sc"; fi
else
    echo "skip - shellcheck not installed"
fi

# --------------------------------------------------------------------------
# 2. install hook: mocked add_* functions, a fixture codecpack tree
# --------------------------------------------------------------------------
echo "== 2. install hook stages the runtime =="
PACKS="$TMP/packs"
IPK="$PACKS/demo.codecpack"
mkdir -p "$IPK/bin" "$IPK/sub"
printf 'name = demo\n' > "$IPK/manifest"
printf '#!/bin/sh\necho demo\n' > "$IPK/bin/demo-helper"
printf 'notes\n' > "$IPK/sub/notes.txt"
printf 'hidden\n' > "$IPK/.hidden"
: > "$TMP/install.calls"

(
    add_module()    { printf 'module %s\n' "$1"    >> "$TMP/install.calls"; }
    add_binary()    { printf 'binary %s\n' "$1"    >> "$TMP/install.calls"; }
    add_file()      { printf 'file %s\n'   "$1"    >> "$TMP/install.calls"; }
    add_runscript() { printf 'runscript\n'          >> "$TMP/install.calls"; }
    export INVFS_CODECPACK_DIR="$PACKS"
    # shellcheck disable=SC1090
    . "$INSTALL"
    build
)
check "fuse module staged"                 "module fuse"                    "$TMP/install.calls"
check "invf-fuse staged"                   "binary invf-fuse"               "$TMP/install.calls"
check "offline tools staged"               "binary invf-verify"             "$TMP/install.calls"
check "chroot staged for the hand-off"     "binary chroot"                  "$TMP/install.calls"
check "codecpack helper via add_binary"    "binary $IPK/bin/demo-helper"    "$TMP/install.calls"
check "codecpack manifest via add_file"    "file $IPK/manifest"             "$TMP/install.calls"
check "codecpack nested data via add_file" "file $IPK/sub/notes.txt"        "$TMP/install.calls"
check "codecpack dotfile staged"           "file $IPK/.hidden"              "$TMP/install.calls"
check "runtime script registered"          "runscript"                      "$TMP/install.calls"

# --------------------------------------------------------------------------
# mocked mkinitcpio runtime environment (sourced by each scenario)
# --------------------------------------------------------------------------
RT="$TMP/rt"
mkdir -p "$RT/bin"
cat > "$RT/bin/chroot" <<'CHROOTEOF'
#!/bin/bash
printf 'chroot argv=[%s] INVFS_DEV1=%s\n' "$*" "${INVFS_DEV1-}" >> "${CHROOT_LOG:?}"
exit 0
CHROOTEOF
chmod 755 "$RT/bin/chroot"

cat > "$RT/mocks.sh" <<'MOCKEOF'
# mocked mkinitcpio runtime environment
: "${LOG:=/dev/null}"
: "${FUSE_LOG:=/dev/null}"
: "${CHROOT_LOG:=/dev/null}"
CMDLINE="${CMDLINE:-}"

getarg() {
    local key="$1" default="${2:-}" w
    for w in $CMDLINE; do
        case "$w" in
            "$key"=*) printf '%s' "${w#*=}"; return 0;;
        esac
    done
    printf '%s' "$default"
}
msg()  { printf 'msg: %s\n' "$*"   >> "$LOG"; }
err()  { printf 'err: %s\n' "$*"   >> "$LOG"; }
panic(){ printf 'panic: %s\n' "$*" >> "$LOG"; exit 1; }
launch_interactive_shell() { printf 'shell: %s\n' "$*" >> "$LOG"; exit 1; }
modprobe() { printf 'modprobe: %s\n' "$*" >> "$LOG"; return 0; }
mkdir() { printf 'mkdir: %s\n' "$*" >> "$LOG"; return 0; }
mount() { printf 'mount: %s\n' "$*" >> "$LOG"; return 0; }
invf-fuse() {
    printf 'INVFS_DEV1=%s argv=%s\n' "${INVFS_DEV1-}" "$*" >> "$FUSE_LOG"
    return "${STUB_FUSE_RC:-0}"
}
resolve_device() {
    case "$1" in
        /dev/sda1|/dev/sdb1|/dev/vda1) printf '%s' "$1"; return 0;;
        *) return 1;;
    esac
}
MOCKEOF

# --------------------------------------------------------------------------
# 3. run_hook selects the handler from the kernel command line
# --------------------------------------------------------------------------
echo "== 3. cmdline selects the handler =="
run_hook_case() { # <fstype> <root> <cmdline> -> "<handler> <init>"
    local fstype="$1" root="$2" cmd="$3"
    LOG="$TMP/rt.log" FUSE_LOG="$TMP/rt.fuse" CHROOT_LOG="$TMP/rt.chroot" CMDLINE="$cmd" \
    bash -c '
        rootfstype="$1"; root="$2"
        . "$3"
        . "$4"
        mount_handler=default_mount_handler
        run_hook
        printf "%s %s\n" "$mount_handler" "${init:-}"
    ' bash "$fstype" "$root" "$RT/mocks.sh" "$HOOK"
}

got=$(run_hook_case invfs /dev/sda1 "")
[ "$got" = "invfs_mount_handler " ] \
    && ok "rootfstype=invfs selects invfs_mount_handler" \
    || bad "rootfstype=invfs selection (got: $got)"

got=$(run_hook_case "" invfs:/dev/sda1 "")
[ "$got" = "invfs_mount_handler " ] \
    && ok "root=invfs:<dev> selects invfs_mount_handler" \
    || bad "root=invfs:<dev> selection (got: $got)"

got=$(run_hook_case ext4 /dev/sda1 "")
[ "$got" = "default_mount_handler " ] \
    && ok "non-invfs root is not claimed" \
    || bad "non-invfs root claimed (got: $got)"

got=$(run_hook_case invfs /dev/sda1 "invfs.init=/bin/invfs-init")
[ "$got" = "invfs_mount_handler /bin/invfs-init" ] \
    && ok "invfs.init= passthrough" \
    || bad "invfs.init= passthrough (got: $got)"

# --------------------------------------------------------------------------
# 4. handler: rootflags, INVFS_DEV1 export, chroot hand-off
# --------------------------------------------------------------------------
echo "== 4. handler: rootflags + INVFS_DEV1 + chroot hand-off =="
: > "$TMP/h.log"; : > "$TMP/h.fuse"; : > "$TMP/h.chroot"
mkdir -p "$TMP/newroot/sbin"
printf '#!/bin/sh\n' > "$TMP/newroot/sbin/init"; chmod 755 "$TMP/newroot/sbin/init"

PATH="$RT/bin:$PATH" CMDLINE="invfs.dev1=/dev/sdb1 invfs.init=/sbin/init" \
  LOG="$TMP/h.log" FUSE_LOG="$TMP/h.fuse" CHROOT_LOG="$TMP/h.chroot" \
  bash -c '
    root=invfs:/dev/sda1
    rootfstype=invfs
    rootflags="ro,arc_limit=64"
    . "$1"
    . "$2"
    mount_handler=default_mount_handler
    run_hook
    invfs_mount_handler "$3"
  ' bash "$RT/mocks.sh" "$HOOK" "$TMP/newroot" || true

check "invf-fuse got rootflags as -o"    "argv=-o ro,arc_limit=64 /dev/sda1 $TMP/newroot" "$TMP/h.fuse"
check "INVFS_DEV1 passed to invf-fuse"   "INVFS_DEV1=/dev/sdb1"          "$TMP/h.fuse"
check "exec chroot <mp> <init>"          "chroot argv=[$TMP/newroot /sbin/init]" "$TMP/h.chroot"
check "INVFS_DEV1 survives into PID1"    "INVFS_DEV1=/dev/sdb1"          "$TMP/h.chroot"
check "proc rbind into new root"         "mount: --rbind /proc $TMP/newroot/proc" "$TMP/h.log"
check "sys rbind into new root"          "mount: --rbind /sys $TMP/newroot/sys"   "$TMP/h.log"
check "dev rbind into new root"          "mount: --rbind /dev $TMP/newroot/dev"   "$TMP/h.log"

# a failed mount must not fall through to the hand-off
: > "$TMP/f.log"; : > "$TMP/f.fuse"; : > "$TMP/f.chroot"
PATH="$RT/bin:$PATH" STUB_FUSE_RC=1 CMDLINE="" \
  LOG="$TMP/f.log" FUSE_LOG="$TMP/f.fuse" CHROOT_LOG="$TMP/f.chroot" \
  bash -c '
    root=/dev/sda1; rootfstype=invfs
    . "$1"
    . "$2"
    run_hook
    invfs_mount_handler "$3"
  ' bash "$RT/mocks.sh" "$HOOK" "$TMP/newroot" || true
check "mount failure calls panic"        "panic: invfs: failed to mount '/dev/sda1'" "$TMP/f.log"
if [ -s "$TMP/f.chroot" ]; then bad "hand-off ran after mount failure"; else ok "no hand-off after mount failure"; fi

# --------------------------------------------------------------------------
# 5. uuid probe (needs one real block device on the host)
# --------------------------------------------------------------------------
echo "== 5. uuid probe =="
DEV=""
for d in /dev/sda1 /dev/sdb1 /dev/vda1 /dev/vda /dev/sda; do
    [ -b "$d" ] && { DEV="$d"; break; }
done
if [ -n "$DEV" ]; then
    got=$(LOG="$TMP/p.log" FUSE_LOG="$TMP/p.fuse" CHROOT_LOG="$TMP/p.chroot" \
          INVFS_BLOCK_GLOBS="$DEV" \
          bash -c '
            . "$1"
            . "$2"
            invf-fuse() {
                [ "$1" = "--probe-uuid" ] && { printf "%s\n" "cafef00d"; return 0; }
                return 1
            }
            invfs_probe_uuid cafef00d
          ' bash "$RT/mocks.sh" "$HOOK")
    [ "$got" = "$DEV" ] \
        && ok "invfs_probe_uuid resolves the device ($DEV)" \
        || bad "invfs_probe_uuid resolution (got: $got)"
else
    echo "skip - invfs_probe_uuid (no host block device)"
fi

echo
echo "mkinitcpio-hook test: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
