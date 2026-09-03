#!/bin/bash
# test-acl.sh — WP-A: POSIX ACLs + full permission enforcement on the
# FUSE path (persistent regression suite).
#
#   Leg A (storage):   setfacl/getfacl round-trip through the mount; the
#                      stored system.posix_acl_access value is compared
#                      BYTE-EXACT against a hand-built standard
#                      posix_acl_xattr blob (python), proving the INO2 TLV
#                      layer keeps the kernel layout verbatim. Size query,
#                      listxattr, absent-ACL ENODATA, junk-blob refusal.
#   Leg B (enforce):   access decisions as uid nobody (sudo -u) and with
#                      supplementary groups (setpriv): named user, named
#                      group, primary group, mask bounding, directory
#                      traversal (X) vs listing (R), create/unlink in
#                      foreign dirs, sticky-dir (+t) rule, chmod/chown
#                      ownership, exec permission, root bypass.
#   Leg C (inherit):   default-ACL inheritance on create/mkdir: child
#                      access ACL = parent's default masked by the create
#                      mode; subdirs also inherit the default verbatim;
#                      a plain dir (no default) yields ACL-less children.
#   Leg D (chmod):     chmod folds the mode into the ACL mask entry
#                      (POSIX.1e): mask follows the group bits live.
#   Leg E (persist):   unmount/remount keeps ACLs and enforcement;
#                      invf-fsck + invf-verify --deep clean.
#
# Run from the repo root after `make`:  bash tools/test-acl.sh
# Same /dev/shm conventions as test-writepath.sh: cd /dev/shm, RELATIVE
# image path (blkio treats /dev/* as raw devices), absolute mountpoint.
set -e
set -o pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
B=$REPO/bin
WORK=/dev/shm/acltest
IMG=acltest.img
MNT=$WORK/mnt
rm -rf "$WORK" && mkdir -p "$MNT"
cd /dev/shm
rm -f "$IMG"

for t in setfacl getfacl python3 sudo setpriv fusermount3; do
    command -v $t >/dev/null || { echo "$t required"; exit 1; }
done
id nobody >/dev/null 2>&1 || { echo "user nobody required"; exit 1; }
sudo -n -u nobody id >/dev/null 2>&1 || { echo "passwordless sudo required"; exit 1; }
grep -q '^user_allow_other' /etc/fuse.conf || { echo "/etc/fuse.conf needs user_allow_other"; exit 1; }
[ -x "$B/invf-fuse" ] || { echo "run make first"; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }

mnt_up() {   # daemonized mount, wait for /proc/mounts; attr_t=0 = no attr cache
    $B/invf-fuse -o allow_other,attr_t=0 "$IMG" "$MNT" 2>"$WORK/fuse.log"
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts && return 0
        sleep 0.1
    done
    fail "mount never appeared"
}

mnt_down() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    for _ in $(seq 1 50); do
        grep -q " $MNT " /proc/mounts || break
        sleep 0.1
    done
    for _ in $(seq 1 300); do
        pgrep -f "invf-fuse $IMG" >/dev/null || return 0
        sleep 0.2
    done
    fail "invf-fuse daemon did not exit after unmount"
}

fsck_ok() { $B/invf-fsck "$IMG" | tee "$WORK/fsck.last" | grep -q "^OK$" \
    || { cat "$WORK/fsck.last"; fail "fsck not clean"; }; }

deep_ok() { $B/invf-verify "$IMG" --deep | tail -1 | grep -q " 0 corrupt," \
    || fail "verify --deep not clean"; }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    pkill -f "invf-fuse $IMG" 2>/dev/null || true
}
trap cleanup EXIT

# --- helpers --------------------------------------------------------------
# "as nobody" runners: rc propagation, no shell quoting surprises
nb()  { sudo -n -u nobody -- "$@"; }
nbsh(){ sudo -n -u nobody -- /bin/sh -c "$1"; }
# allow <desc> <cmd...> / deny <desc> <cmd...>
allow() { local d=$1; shift; "$@" >/dev/null 2>&1 || fail "expected OK: $d"; }
deny()  { local d=$1; shift; "$@" >/dev/null 2>&1 && fail "expected DENIED: $d"; return 0; }
gf()    { getfacl -p --omit-header -- "$1" 2>/dev/null; }

NOBODY=65534   # nobody uid/gid
USERGID=1000   # the mounting user's gid

echo "== [A] ACL xattr storage: setfacl/getfacl round-trip, verbatim blob =="
$B/invf-mkfs "$IMG" 0.1 >/dev/null
mnt_up
echo secret > "$MNT/s.txt"
chmod 600 "$MNT/s.txt"
setfacl -m u:nobody:r "$MNT/s.txt" || fail "setfacl"
gf "$MNT/s.txt" | tee "$WORK/gf.s"
grep -qx 'user::rw-' "$WORK/gf.s"          || fail "user::rw-"
grep -qx 'user:nobody:r--' "$WORK/gf.s"    || fail "user:nobody:r--"
grep -qx 'group::---' "$WORK/gf.s"         || fail "group::---"
grep -qx 'mask::r--' "$WORK/gf.s"          || fail "mask::r--"
grep -qx 'other::---' "$WORK/gf.s"         || fail "other::---"
# the on-disk value must be the standard posix_acl_xattr layout, byte-exact:
# header {u32 version=2}, then {u16 tag, u16 perm, u32 id} entries:
# USER_OBJ rw-, USER 65534 r--, GROUP_OBJ ---, MASK r--, OTHER ---
python3 - "$MNT/s.txt" <<'PYEOF' || fail "verbatim blob check"
import os, struct, sys
p = sys.argv[1]
got = os.getxattr(p, "system.posix_acl_access")
def e(tag, perm, eid): return struct.pack("<HHI", tag, perm, eid)
want = struct.pack("<I", 2) + e(0x01, 6, 0xffffffff) + e(0x02, 4, 65534) \
     + e(0x04, 0, 0xffffffff) + e(0x10, 4, 0xffffffff) + e(0x20, 0, 0xffffffff)
if got != want:
    print("got :", got.hex())
    print("want:", want.hex())
    sys.exit(1)
print("  stored blob is the standard posix_acl_xattr layout, byte-exact")
PYEOF
# listxattr shows it; absent ACL = ENODATA; junk refused
python3 - "$MNT" <<'PYEOF' || fail "xattr edge checks"
import os, sys
M = sys.argv[1]
names = os.listxattr(M + "/s.txt")
assert "system.posix_acl_access" in names, names
open(M + "/plain.txt", "w").close()
try:
    os.getxattr(M + "/plain.txt", "system.posix_acl_access")
    raise SystemExit("expected ENODATA for ACL-less file")
except OSError as e:
    assert e.errno == 61, e          # ENODATA
try:
    os.setxattr(M + "/plain.txt", "system.posix_acl_access",
                bytes.fromhex("0300000001000600ffffffff"))  # version 3
    raise SystemExit("bad version accepted")
except OSError as e:
    assert e.errno in (22, 95), e    # EINVAL (daemon) / EOPNOTSUPP (kernel)
try:
    os.setxattr(M + "/plain.txt", "system.posix_acl_access",
                bytes.fromhex("0200000020000400ffffffff01000600ffffffff04000400ffffffff"))
    raise SystemExit("non-canonical blob accepted")
except OSError as e:
    assert e.errno == 22, e          # EINVAL from the daemon's validator
try:
    # a default ACL on a regular file must be refused (EACCES)
    import struct
    def e(t, p, i): return struct.pack("<HHI", t, p, i)
    blob = struct.pack("<I", 2) + e(1,7,-1 & 0xffffffff) + e(4,5,-1 & 0xffffffff) + e(0x20,5,-1 & 0xffffffff)
    os.setxattr(M + "/plain.txt", "system.posix_acl_default", blob)
    raise SystemExit("default ACL on a file accepted")
except OSError as e:
    assert e.errno == 13, e          # EACCES
print("  listxattr / ENODATA / junk refusal: OK")
PYEOF
echo "  setfacl/getfacl round-trip + verbatim storage: OK"

echo
echo "== [B] enforcement as another uid (sudo -u nobody / setpriv) =="
# B1: mode 0600 with NO ACL: denied; access(2) agrees; root bypasses
echo sekret > "$MNT/b1.txt" && chmod 600 "$MNT/b1.txt"
deny  "nobody cat 0600"        nb cat "$MNT/b1.txt"
allow "nobody test -e (F_OK)"  nb test -e "$MNT/b1.txt"
deny  "nobody test -r 0600"    nb test -r "$MNT/b1.txt"
allow "root cat (bypass)"      sudo -n cat "$MNT/b1.txt"
allow "owner cat"              cat "$MNT/b1.txt"
# B2: ACL grants read (s.txt has u:nobody:r from leg A); write still
# refused (entry has no w, mask r--)
allow "nobody cat with u:nobody:r" nb cat "$MNT/s.txt"
deny  "nobody write with r-- ACL"  nbsh "echo x >> '$MNT/s.txt'"
allow "nobody test -r"             nb test -r "$MNT/s.txt"
deny  "nobody test -w"             nb test -w "$MNT/s.txt"
# B3: mask bounds the named user
setfacl -m u:nobody:rw-,m::rw- "$MNT/s.txt"
allow "write with rw entry+mask"   nbsh "echo x >> '$MNT/s.txt'"
setfacl -m m::r-- "$MNT/s.txt"
deny  "write with mask r--"        nbsh "echo x >> '$MNT/s.txt'"
allow "read with mask r--"         nb cat "$MNT/s.txt"
echo "  named-user ACL + mask bounding: OK"
# B4: group entries — primary group of nobody, then supplementary via setpriv
echo g > "$MNT/g.txt" && chmod 000 "$MNT/g.txt"
setfacl -m g:$NOBODY:r "$MNT/g.txt"
allow "nobody cat via primary-group ACL" nb cat "$MNT/g.txt"
chmod 000 "$MNT/g.txt"; setfacl -b "$MNT/g.txt"; setfacl -m g:$USERGID:r "$MNT/g.txt"
deny  "nobody cat (group user, not a member)" nb cat "$MNT/g.txt"
allow "setpriv +suppl group user"  \
      sudo -n setpriv --reuid $NOBODY --rgid $NOBODY --groups $USERGID cat "$MNT/g.txt"
echo "  group ACLs (primary + supplementary): OK"
# B5: directory traversal (X) vs listing (R)
mkdir "$MNT/priv" && echo in > "$MNT/priv/inner" && chmod 700 "$MNT/priv"
deny  "ls 0700 dir"                nb ls "$MNT/priv"
deny  "cat through 0700 dir"       nb cat "$MNT/priv/inner"
chmod o+x "$MNT/priv"
allow "cat with o+x (traversal)"   nb cat "$MNT/priv/inner"
deny  "ls with o+x only"           nb ls "$MNT/priv"
chmod o+r "$MNT/priv"
allow "ls with o+rx"               nb ls "$MNT/priv"
echo "  traversal vs listing: OK"
# B6: create/unlink — root of the mount is 0755 root-owned: denied;
#     a 0777 dir allows create; +t restricts deletes to owners
deny  "nobody create in mount root" nbsh "echo x > '$MNT/rootfile'"
mkdir "$MNT/pub" && chmod 777 "$MNT/pub"
allow "nobody create in 0777 dir"   nbsh "echo hi > '$MNT/pub/nfile'"
[ "$(stat -c '%u:%g' "$MNT/pub/nfile")" = "$NOBODY:$NOBODY" ] \
    || fail "created file must be owned by its creator"
echo mine > "$MNT/pub/myfile"
allow "nobody rm foreign file in non-sticky 0777" nb rm -f "$MNT/pub/myfile"
echo theirs > "$MNT/pub/nfile2"     # foreign-owned file to delete under +t
chmod +t "$MNT/pub"
deny  "nobody rm foreign file in +t dir"        nb rm -f "$MNT/pub/nfile2"
allow "nobody rm own file in +t dir"            nb rm -f "$MNT/pub/nfile"
echo "  create/unlink/sticky: OK"
# B7: chmod/chown/setfacl ownership
deny  "nobody chmod foreign file"   nb chmod 777 "$MNT/s.txt"
deny  "nobody chown foreign file"   nb chown nobody "$MNT/s.txt"
deny  "nobody setfacl foreign file" nb setfacl -m u:nobody:rwx "$MNT/s.txt"
echo "  chmod/chown/setacl ownership: OK"
# B8: exec permission. The kernel never delegates MAY_EXEC to a
#     default_permissions-less FUSE daemon (verified on 7.1: a 0744 script
#     RUNS as nobody -- exec opens carry no exec flag, so the daemon can
#     only gate exec by readability). What IS enforceable and checked here:
#     access(X_OK) and exec of an unreadable file.
printf '#!/bin/sh\necho ACL-EXEC-OK\n' > "$MNT/run.sh" && chmod 744 "$MNT/run.sh"
deny  "nobody test -x 0744 (access X_OK)"   nb test -x "$MNT/run.sh"
chmod 700 "$MNT/run.sh"
deny  "nobody exec 0700 (unreadable)"       nb "$MNT/run.sh"
chmod 755 "$MNT/run.sh"
[ "$(nb "$MNT/run.sh")" = "ACL-EXEC-OK" ] || fail "exec 0755 script"
echo "  exec permission: OK (X via access(2) + readability gate)"

echo
echo "== [C] default-ACL inheritance on create/mkdir =="
mkdir "$MNT/d"
setfacl -d -m u::rwx,g::rx,o::rx,u:nobody:rwx "$MNT/d" || fail "setfacl -d"
touch "$MNT/d/f"      # umask 022 -> create mode 0644
gf "$MNT/d/f" | tee "$WORK/gf.f"
grep -q 'user:nobody:rwx.*effective:r--' "$WORK/gf.f" || fail "inherited+masked named user"
grep -qx 'mask::r--' "$WORK/gf.f"                     || fail "inherited mask from mode"
[ "$(stat -c '%a' "$MNT/d/f")" = "644" ]              || fail "child mode 644"
allow "nobody read inherited file"   nb cat "$MNT/d/f"
deny  "nobody write inherited file"  nbsh "echo x > '$MNT/d/f'"
mkdir "$MNT/d/sub"
getfacl -p --omit-header -d "$MNT/d/sub" 2>/dev/null | grep -q 'user:nobody:rwx' \
    || fail "subdir must inherit the default ACL as its own default"
gf "$MNT/d/sub" | grep -q 'user:nobody:rwx' || fail "subdir access ACL"
allow "nobody ls inherited subdir (r-x)"  nb ls "$MNT/d/sub"
deny  "nobody create in subdir (r-x)"     nbsh "echo x > '$MNT/d/sub/z'"
python3 - "$MNT/d/g" <<'PYEOF' || fail "mode-0600 create under default ACL"
import os, sys
fd = os.open(sys.argv[1], os.O_CREAT | os.O_WRONLY, 0o600)
os.close(fd)
PYEOF
gf "$MNT/d/g" | grep -qx 'mask::---' || fail "mode 0600 must mask group class to ---"
deny  "nobody read 0600-masked inherited file" nb cat "$MNT/d/g"
mkdir "$MNT/plain" && touch "$MNT/plain/child"
python3 - "$MNT/plain/child" <<'PYEOF' || fail "no default ACL -> no ACL"
import os, sys
try:
    os.getxattr(sys.argv[1], "system.posix_acl_access")
    raise SystemExit("ACL appeared without a default ACL on the parent")
except OSError as e:
    assert e.errno == 61, e    # ENODATA
PYEOF
echo "  inheritance: file masked to mode, subdir default+access, plain child ACL-free: OK"

echo
echo "== [D] chmod folds into the ACL mask (POSIX.1e) =="
echo data > "$MNT/x.txt"
setfacl -m u:nobody:rw-,m::rw- "$MNT/x.txt"
allow "pre-chmod write (mask rw-)"   nbsh "echo x >> '$MNT/x.txt'"
chmod 640 "$MNT/x.txt"
gf "$MNT/x.txt" | tee "$WORK/gf.x"
grep -qx 'mask::r--' "$WORK/gf.x" || fail "chmod 640 must set mask r--"
deny  "write after chmod 640"        nbsh "echo x >> '$MNT/x.txt'"
allow "read after chmod 640"         nb cat "$MNT/x.txt"
chmod 660 "$MNT/x.txt"
gf "$MNT/x.txt" | grep -qx 'mask::rw-' || fail "chmod 660 must set mask rw-"
allow "write after chmod 660"        nbsh "echo x >> '$MNT/x.txt'"
chmod 600 "$MNT/x.txt"
gf "$MNT/x.txt" | grep -qx 'mask::---' || fail "chmod 600 must set mask ---"
deny  "read after chmod 600"         nb cat "$MNT/x.txt"
echo "  chmod -> mask: OK"

echo
echo "== [E] persistence across unmount/remount =="
gf "$MNT/s.txt" > "$WORK/gf.s.pre"
gf "$MNT/d/f"  > "$WORK/gf.f.pre"
mnt_down
fsck_ok
deep_ok
mnt_up
gf "$MNT/s.txt" > "$WORK/gf.s.post"
gf "$MNT/d/f"  > "$WORK/gf.f.post"
cmp "$WORK/gf.s.pre"  "$WORK/gf.s.post"  || fail "s.txt ACL changed across remount"
cmp "$WORK/gf.f.pre"  "$WORK/gf.f.post"  || fail "d/f ACL changed across remount"
deny  "post-remount nobody read s.txt (mask r--, w was denied too)" nbsh "echo x >> '$MNT/s.txt'"
allow "post-remount nobody read s.txt"  nb cat "$MNT/s.txt"
deny  "post-remount nobody write d/f"   nbsh "echo x > '$MNT/d/f'"
allow "post-remount nobody read d/f"    nb cat "$MNT/d/f"
mnt_down
fsck_ok
deep_ok
echo "  ACLs + enforcement identical after remount; fsck/verify clean"

rm -f "$IMG"
echo
echo "ACL E2E: PASS"
