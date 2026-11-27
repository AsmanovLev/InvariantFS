#!/bin/bash
# test-convert-ext4.sh — `invf-convert`: ext4 -> InvariantFS, phase 0.
#
# The tool is two commands in one binary (tools/invf-convert.c):
#   convert  pack classify -> loop-mount the source read-only -> import the TREE
#            into a fresh invf-mkfs volume -> invf-fsck + invf-verify --deep ->
#            cross-check the import against the pack's own member enumeration
#   verify   mount both ends (loop ro / fuse) and compare EVERY path: presence,
#            type, size, mode, uid, gid, mtime (sec+nsec), BLAKE3 of the logical
#            content, symlink targets, hardlink identity, xattrs
#
# What this suite proves, in order:
#
#   leg A  a corpus that exercises every metadata an InvFS volume can hold —
#          300 files of mixed sizes over a directory tree, a >1 MiB file, an
#          empty file, a sparse file (8 MiB logical / 3 bytes written), a
#          non-ASCII name, modes 0600/0644/0755/04755, a 2-name and a 3-name
#          hardlink group, absolute/relative/long symlinks, fifo + char + block
#          devices + a unix socket, xattrs (user.* and a real POSIX ACL blob),
#          sub-second mtimes — converts, and the report must have ZERO
#          differences.
#   leg B  the verifier MUST be able to fail: the destination is mutated
#          (content, mode, mtime, a deleted name, an added name) and verify has
#          to name every one of them and exit 1. A comparison harness with no
#          red leg is a rubber stamp.
#   leg C  negative controls: an ext2 image (no extents), an ext4 with bigalloc
#          (an INCOMPAT feature the pack refuses), and a name the volume cannot
#          represent ('!' is the container-sibling namespace). Each must exit 3,
#          name the reason, and leave NO destination behind.
#   leg D  a real-world rootfs (an Ubuntu/Debian cloud image root partition if
#          one is on the box, or $INVFS_CONVERT_REAL_IMG): counts, timings,
#          p50/p95 per-file verification, and the same zero-diff assertion.
#          Opt-in via INVFS_CONVERT_REAL=1 when the image has to be DERIVED from
#          a .qcow2 (qemu-img + losetup -P + dd, a few minutes); skipped LOUDLY
#          with the exact command when nothing is available.
#
# LOCKED suite (sudo, losetup): loop mounts and a fuse mount, serialised by
# tools/run-e2e.sh. Images live on the disk-backed scratch root, never in
# /dev/shm (a 2.5 GiB rootfs image in RAM is how a suite OOMs a 7.6 GiB box).
#
# Run:  bash tools/run-e2e.sh tools/test-convert-ext4.sh
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
export REPO
. "$REPO/tools/lib-scratch.sh"
. "$REPO/tools/lib-loopmount.sh"

B=${INVFS_BIN:-$REPO/bin}
PACKDIR=$REPO/tools/codecpacks/ext4fs.codecpack
CONVERT=$B/invf-convert
W=${INVFS_CONVERT_WORK:-$(invfs_scratch_root)/wp-convert-$$}
MNT=$W/mnt
export INVFS_CONVERT_PACK=$PACKDIR

pass() { echo "  ok: $*"; }
note() { echo "  -- $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

# mount the destination image through the daemon and WAIT for it: a fixed
# sleep is a race, and the failure it produces ("No such file or directory" from
# stat) blames the volume for a slow daemon start. Prints the daemon's log on a
# refusal, which is where the reason actually is.
fuse_up() {  # $1 = image, $2 = mountpoint, $3 = log
    local img=$1 mnt=$2 log=$3
    mkdir -p "$mnt"
    sudo -n "$B/invf-fuse" "$(readlink -f "$img")" "$mnt" -o attr_t=0 -f >"$log" 2>&1 &
    for _ in $(seq 1 100); do
        grep -q " $mnt " /proc/self/mounts && return 0
        sleep 0.1
    done
    cat "$log" >&2 || true
    fail "invf-fuse never mounted $mnt"
}

fuse_down() {  # $1 = mountpoint
    sudo -n fusermount3 -u "$1" 2>/dev/null || sudo -n umount "$1" 2>/dev/null || true
    for _ in $(seq 1 100); do
        grep -q " $1 " /proc/self/mounts || return 0
        sleep 0.1
    done
}

cleanup() {
    # every mount this suite made, detached by device: a fuse mount whose daemon
    # died still answers `umount` and a loop mount holds its devices otherwise
    for m in "$W"/mnt*; do
        [ -d "$m" ] || continue
        sudo -n fusermount3 -u "$m" 2>/dev/null || sudo -n umount "$m" 2>/dev/null || true
    done
    pkill -f "invf-fuse.*$W" 2>/dev/null || true
    [ -n "${LOOPDEV:-}" ] && { sudo -n losetup -d "$LOOPDEV" 2>/dev/null || true; LOOPDEV=""; }
    for d in $(sudo -n losetup -j "$W" 2>/dev/null | cut -d: -f1); do
        sudo -n losetup -d "$d" 2>/dev/null || true
    done
    sleep 0.3
    rm -rf "$W"
}
trap cleanup EXIT

echo "== invf-convert: ext4 -> InvariantFS (phase 0) =="

# --- preconditions -----------------------------------------------------------
command -v mkfs.ext4 >/dev/null || { echo "FAIL: mkfs.ext4 (e2fsprogs) not installed"; exit 1; }
command -v python3   >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }
command -v sudo      >/dev/null || { echo "FAIL: sudo not installed"; exit 1; }
sudo -n true 2>/dev/null || { echo "FAIL: this suite needs passwordless sudo (loop mounts)"; exit 1; }
[ -x "$CONVERT" ] || fail "$CONVERT is missing: run make"
[ -x "$B/invf-fuse" ] || fail "$B/invf-fuse is missing: run make"
[ "$(id -u)" != 0 ] || note "running as root; the suite assumes the invoking user has sudo"

echo "== build the ext4fs pack helper (-Wall -Wextra -Werror) =="
mkdir -p "$PACKDIR/bin"
WARN=$(cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACKDIR/bin/ext4fs" "$PACKDIR/ext4fs.c" 2>&1) \
    || { echo "FAIL: pack build failed"; echo "$WARN"; exit 1; }
[ -z "$WARN" ] || { echo "FAIL: pack build not warning-clean:"; echo "$WARN"; exit 1; }
pass "pack helper built clean"

rm -rf "$W" && mkdir -p "$W" "$MNT"
cd "$W"

# populates a mounted ext4 root at $1 with the corpus every leg reuses
build_corpus() {
    T=$1
    sudo chown "$(id -u):$(id -g)" "$T"
    # 300 files: 30 dirs x 10 files, sizes 0..64 KiB, so the population is big
    # enough that a dropped subtree cannot hide in a small count
    python3 - "$T" <<'PY'
import os, random, sys
T = sys.argv[1]
random.seed(20261127)
for d in range(30):
    dp = os.path.join(T, "tree", "d%02d" % d)
    os.makedirs(dp, exist_ok=True)
    for f in range(10):
        size = random.choice([0, 1, 17, 4096, 65536]) if f % 5 == 0 else random.randint(1, 65536)
        with open(os.path.join(dp, "f%02d_%02d.bin" % (d, f)), "wb") as fh:
            fh.write(os.urandom(size))
    with open(os.path.join(dp, "note.txt"), "w") as fh:
        fh.write("dir %d\n" % d)
PY
    # > 1 MiB (multi-segment: 64 KiB segments -> 48 of them at 3 MiB)
    head -c 3145728 /dev/urandom > "$T/tree/mega.bin"
    # empty file
    : > "$T/tree/empty.bin"
    # sparse: 8 MiB logical, 3 bytes written in the middle
    python3 - "$T" <<'PY'
import sys
p = sys.argv[1] + "/tree/sparse.bin"
with open(p, "wb") as f:
    f.truncate(8 * 1024 * 1024)
    f.seek(4 * 1024 * 1024)
    f.write(b"mid")
PY
    # non-ASCII name (UTF-8 bytes the volume must carry verbatim)
    printf 'non-ascii\n' > "$T/tree/файл-ü-ß.txt"
    # modes
    head -c 4096 /dev/urandom > "$T/tree/mode600.bin"; chmod 0600 "$T/tree/mode600.bin"
    head -c 4096 /dev/urandom > "$T/tree/mode644.bin"; chmod 0644 "$T/tree/mode644.bin"
    mkdir -p "$T/tree/mode755";                      chmod 0755 "$T/tree/mode755"
    head -c 4096 /dev/urandom > "$T/tree/setuid.bin"; chmod 04755 "$T/tree/setuid.bin"
    # symlinks: relative, absolute, and a 200-byte target
    ln -s "hello-ish" "$T/tree/link-rel"
    ln -s "/usr/lib/nonexistent/absolute" "$T/tree/link-abs"
    ln -s "$(python3 -c 'print("t"*200)')" "$T/tree/link-long"
    # hardlinks: a 2-name group and a 3-name group
    head -c 2048 /dev/urandom > "$T/tree/hl-a"
    ln "$T/tree/hl-a" "$T/tree/hl-b"
    head -c 2048 /dev/urandom > "$T/tree/hl3-a"
    ln "$T/tree/hl3-a" "$T/tree/hl3-b"
    ln "$T/tree/hl3-a" "$T/tree/hl3-c"
    # special files: fifo, char device, block device, unix socket
    mkfifo "$T/tree/fifo"
    sudo mknod "$T/tree/chardev" c 1 3
    sudo mknod "$T/tree/blkdev" b 7 0
    sudo chown "$(id -u):$(id -g)" "$T/tree/chardev" "$T/tree/blkdev"
    python3 - "$T" <<'PY'
import socket, sys
p = sys.argv[1] + "/tree/sock"
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(p)
s.close()          # the inode stays on disk; this is a socket FILE, not a live socket
PY
    # sub-second mtimes, on several types
    touch -d "2020-01-02 03:04:05.123456789" "$T/tree/mode644.bin"
    touch -d "2021-05-06 07:08:09.987654321" "$T/tree/mega.bin"
    touch -d "2019-12-31 23:59:59.000000001" "$T/tree/mode755"
    touch -d "2018-02-03 04:05:06.500000000" "$T/tree/hl-a" "$T/tree/hl-b"
    # xattrs: binary user.* values plus a REAL POSIX ACL blob. The ACL is
    # written as the raw system.posix_acl_access encoding (version-2 header +
    # {tag,perm,id} entries), which is what acl(5) tools would store; the engine
    # transports xattr blobs verbatim and evaluates the ACL daemon-side.
    python3 - "$T" <<'PY'
import os, struct, sys
T = sys.argv[1]
os.setxattr(T + "/tree/mode600.bin", "user.tag", b"secret")
os.setxattr(T + "/tree/mode644.bin", "user.bin", bytes(range(32)))
os.setxattr(T + "/tree/mega.bin", "user.note", b"big file, 3 MiB")
os.setxattr(T + "/tree", "user.dirtag", b"tree root")
# ACL: USER_OBJ rw- | USER 1000 r-- | GROUP_OBJ r-- | MASK r-- | OTHER r--
# (version 2, little endian). The NAMED USER entry is load-bearing for this
# fixture: an ACL equivalent to the mode bits is dropped by the kernel's
# ext4_xattr_set_acl handling (posix_acl_update_mode removes a no-op ACL), and
# the first version of this corpus silently stored no ACL at all -- which the
# leg then reported as a conversion difference. The named entry cannot be
# expressed by mode bits, so the xattr is kept.
blob = struct.pack("<I", 2)
for tag, perm, ident in ((0x01, 6, 0xFFFFFFFF), (0x02, 4, 1000),
                         (0x04, 4, 0xFFFFFFFF), (0x10, 4, 0xFFFFFFFF),
                         (0x20, 4, 0xFFFFFFFF)):
    blob += struct.pack("<HHI", tag, perm, ident)
os.setxattr(T + "/tree/mode644.bin", "system.posix_acl_access", blob)
PY
    sync
}

mkfs_image() {  # $1 = file, $2 = MiB, rest = mkfs args
    local img=$1 mib=$2; shift 2
    dd if=/dev/zero of="$img" bs=1M count="$mib" status=none
    mkfs.ext4 -q -F "$@" "$img"
}

# ---------------------------------------------------------------- leg A ------
echo
echo "== leg A: 192 MiB ext4 corpus -> convert -> verify (zero differences) =="
mkfs_image src.ext4 192 -b 4096
invfs_loop_mount "$PWD/src.ext4" "$MNT"
build_corpus "$MNT"
n_src=$(sudo find "$MNT" -mindepth 1 | wc -l)
read -r n_hlg n_hlname <<<"$(sudo find "$MNT" -type f -links +1 -printf '%i\n' | sort -u | wc -l) $(sudo find "$MNT" -type f -links +1 | wc -l)"
note "corpus: $n_src entries, $n_hlname hardlink names in $n_hlg inodes"
# The fixture must actually CARRY what the legs assert (a fixture that quietly
# lost its ACL blob would turn into a conversion failure the tool did not
# cause). Checked here, on the source, before anything is converted.
sudo python3 - "$MNT" <<'PY' || fail "the fixture lost an xattr on the source"
import os, struct, sys
M = sys.argv[1]
assert os.getxattr(M + "/tree/mode600.bin", "user.tag") == b"secret"
assert os.getxattr(M + "/tree/mode644.bin", "user.bin") == bytes(range(32))
acl = os.getxattr(M + "/tree/mode644.bin", "system.posix_acl_access")
assert len(acl) == 4 + 5 * 8, acl
assert os.getxattr(M + "/tree", "user.dirtag") == b"tree root"
PY
[ "$n_hlg" = "2" ] || fail "the fixture must carry 2 hardlink groups, found $n_hlg"
sudo umount "$MNT"; sudo losetup -d "$LOOPDEV"; LOOPDEV=""

CONVLOG=$W/convert.log
set +e
sudo -n "$CONVERT" convert src.ext4 dst.img >"$CONVLOG" 2>&1
conv_rc=$?
set -e
cat "$CONVLOG"
[ "$conv_rc" = 0 ] || fail "convert exited $conv_rc"
grep -q "^RESULT: converted" "$CONVLOG" || fail "convert did not claim a conversion"
grep -q "gate: invf-fsck exit 0: OK" "$CONVLOG" || fail "invf-fsck did not report OK"
grep -q "gate: invf-verify --deep exit 0: deep: .* 0 corrupt" "$CONVLOG" \
    || fail "invf-verify --deep did not report 0 corrupt"
# the pack oracle: every member inode the pack enumerated is either carried by
# the tree or explained as an inode with no dirent. The arithmetic must close:
#   matched + unmatched == enumerated
read -r matched enumerated <<<"$(sed -n 's/^nk match   : \([0-9]*\) of \([0-9]*\) member.*/\1 \2/p' "$CONVLOG")"
[ -n "$matched" ] && [ -n "$enumerated" ] || fail "no pack-oracle cross-check line"
unmatched=$(sed -n 's/^unmatched  : \([0-9]*\) inode(s).*/\1/p' "$CONVLOG")
unmatched=${unmatched:-0}
[ $((matched + unmatched)) = "$enumerated" ] \
    || fail "pack oracle: $matched carried + $unmatched unmatched != $enumerated enumerated"
[ "$matched" -ge 1 ] || fail "pack oracle: nothing carried"
pass "convert: fsck OK, verify --deep 0 corrupt, pack oracle $matched/$enumerated carried (+$unmatched nameless inode(s))"

# the byte map must tile the image and the carry must fit inside it
grep -q "tiles the image" "$CONVLOG" || fail "the pack's byte map did not tile the source"
carry=$(sed -n 's/.*carry = meta+member = \([0-9]*\) B.*/\1/p' "$CONVLOG")
image=$(sed -n 's/^byte map    : \([0-9]*\) B image.*/\1/p' "$CONVLOG")
[ -n "$carry" ] && [ -n "$image" ] || fail "could not read the carry/image numbers"
[ "$carry" -lt "$image" ] || fail "carry ($carry) is not smaller than the image ($image)"
pass "byte map: carry $carry B of the $image B image (the phase-1 space rule's floor)"

VERLOG=$W/verify.log
set +e
sudo -n "$CONVERT" verify src.ext4 dst.img >"$VERLOG" 2>&1
ver_rc=$?
set -e
cat "$VERLOG"
[ "$ver_rc" = 0 ] || fail "verify exited $ver_rc on a fresh conversion"
grep -q "^RESULT: IDENTICAL" "$VERLOG" || fail "verify did not report IDENTICAL"
grep -q "^DIFFS      : 0" "$VERLOG" || fail "verify reported differences"
grep -q "hardlinks  : [0-9]* source group(s)" "$VERLOG" \
    || fail "the hardlink partition was not checked"
grep -q "the engine's own inode ids carry the same partition" "$VERLOG" \
    || fail "the hardlink partition was not read from the engine"
[ "$(grep -o 'hardlinks  : [0-9]*' "$VERLOG" | grep -o '[0-9]*')" = "2" ] \
    || fail "expected exactly 2 hardlink groups (hl-a/hl-b and hl3-a/b/c)"
grep -q "per-file   : p50" "$VERLOG" || fail "no per-file verification timing"
for c in directories "regular files" symlinks specials; do
    grep -q "$c" "$VERLOG" || fail "the report has no '$c' count"
done
pass "verify: IDENTICAL, 0 differences, hardlink partition + per-file timing reported"

# the counts on both sides must be the SAME numbers, not just present
for c in directories "regular files" symlinks specials; do
    line=$(grep -F "$c" "$VERLOG" | head -1)
    s=$(echo "$line" | sed 's/.*src \([0-9]*\).*/\1/')
    d=$(echo "$line" | sed 's/.*dst \([0-9]*\).*/\1/')
    [ "$s" = "$d" ] || fail "$c: src $s != dst $d"
    [ "$s" -gt 0 ] || fail "$c: nothing of that type in the corpus"
done
pass "every class has the same count on both sides"

# nanosecond mtimes must survive the round trip: `stat` through the mount is
# the only place a user can see them, and it is what the engine promises. The
# expectation is COMPUTED, not hardcoded: `touch -d` reads LOCAL time and the
# mount reports epoch seconds, so a literal here is a timezone assertion (the
# first version of this leg failed on a +0700 box by exactly 25200 s while the
# nanoseconds were right).
MNT2=$W/mnt2
fuse_up dst.img "$MNT2" "$W/fuse2.log"
want=$(date -d "2020-01-02 03:04:05.123456789" +%s).123456789
mtime=$(sudo -n stat -c '%.9Y' "$MNT2/tree/mode644.bin")
[ "$mtime" = "$want" ] \
    || fail "nanosecond mtime through the mount: got $mtime, want $want"
pass "mtime nanoseconds survive: $mtime via the fuse mount"
# setuid + 0600 + xattr + ACL through the mount
[ "$(sudo stat -c '%a' "$MNT2/tree/setuid.bin")" = "4755" ] || fail "setuid bit lost"
[ "$(sudo stat -c '%a' "$MNT2/tree/mode600.bin")" = "600" ] || fail "0600 lost"
[ "$(sudo stat -c '%h' "$MNT2/tree/hl3-a")" = "3" ] || fail "3-name hardlink nlink != 3"
sudo python3 - "$MNT2" <<'PY' || fail "xattrs through the mount differ"
import os, struct, sys
M = sys.argv[1]
assert os.getxattr(M + "/tree/mode600.bin", "user.tag") == b"secret"
assert os.getxattr(M + "/tree/mode644.bin", "user.bin") == bytes(range(32))
acl = os.getxattr(M + "/tree/mode644.bin", "system.posix_acl_access")
assert len(acl) == 4 + 5 * 8, acl                       # 5 entries
assert struct.unpack("<I", acl[:4])[0] == 2
tags = [struct.unpack("<H", acl[4 + 8 * i:6 + 8 * i])[0] for i in range(5)]
assert tags == [0x01, 0x02, 0x04, 0x10, 0x20], tags      # canonical order
PY
pass "xattrs (user.* + POSIX ACL blob) and modes survive the mount"
fuse_down "$MNT2"

# ------------------------------------------------------------------ leg B ----
echo
echo "== leg B: the verifier MUST fail on a mutated destination (differential control) =="
cp dst.img dst-mut.img
MNT2=$W/mnt3
fuse_up dst-mut.img "$MNT2" "$W/fuse3.log"
# content: flip ONE byte in place, so the size stays the same and the only
# difference the verifier can see is the bytes (an append would be reported as a
# size difference, which is a different check)
sudo -n python3 - "$MNT2/tree/mode600.bin" <<'PY'
import sys
p = sys.argv[1]
with open(p, "r+b") as f:
    f.seek(2048)
    b = f.read(1)
    f.seek(2048)
    f.write(bytes([b[0] ^ 0xFF]))
PY
sudo -n chmod 0640 "$MNT2/tree/mode644.bin"                     # mode
sudo -n touch -d "2017-01-01 00:00:00.111111111" "$MNT2/tree/mega.bin"  # mtime
sudo -n rm "$MNT2/tree/empty.bin"                               # missing
sudo -n bash -c "printf 'new\n' > '$MNT2/tree/added.txt'"       # extra
sudo -n python3 -c "import os,sys; os.setxattr('$MNT2/tree/mode600.bin','user.tag',b'tampered')"
sudo -n bash -c "rm '$MNT2/tree/hl3-c'"                         # nlink 3 -> 2
fuse_down "$MNT2"

MUTLOG=$W/verify-mut.log
set +e
sudo -n "$CONVERT" verify src.ext4 dst-mut.img >"$MUTLOG" 2>&1
mut_rc=$?
set -e
grep "^DIFF" "$MUTLOG" | sed 's/^/    /'
[ "$mut_rc" = 1 ] || fail "verify of the mutated volume exited $mut_rc, want 1"
grep -q "^RESULT: DIFFERENT" "$MUTLOG" || fail "verify did not report DIFFERENT"
for kind in content mode mtime missing extra xattr nlink; do
    grep -q "^DIFF $kind" "$MUTLOG" || fail "no '$kind' difference was reported"
done
pass "verify named all 7 injected differences and exited 1"

# ---------------------------------------------------------------- leg C -----
echo
echo "== leg C: negative controls (a refusal must be loud and leave nothing behind) =="

# C1: ext2 — no extents, which the pack refuses by name
mkfs_image ext2.img 32 -t ext2
set +e
sudo -n "$CONVERT" convert ext2.img ext2-dst.img >"$W/c1.log" 2>&1
c1_rc=$?
set -e
cat "$W/c1.log"
[ "$c1_rc" = 3 ] || fail "ext2 refusal exited $c1_rc, want 3"
grep -q "extents feature absent" "$W/c1.log" || fail "the ext2 refusal did not name the reason"
[ ! -e ext2-dst.img ] || fail "a refused conversion left a destination image behind"
pass "ext2 (no extents): exit 3, named reason, no destination left"

# C2: an INCOMPAT feature the pack does not know
if mkfs_image bad.img 64 -b 4096 -O bigalloc 2>/dev/null; then
    set +e
    sudo -n "$CONVERT" convert bad.img bad-dst.img >"$W/c2.log" 2>&1
    c2_rc=$?
    set -e
    cat "$W/c2.log"
    [ "$c2_rc" = 3 ] || fail "bigalloc refusal exited $c2_rc, want 3"
    # bigalloc is a RO_COMPAT feature (0x200); the pack refuses anything it does
    # not know and NAMES the bit, which is the only thing that makes the refusal
    # diagnosable
    grep -Eqi "unknown (INCOMPAT|RO_COMPAT) feature" "$W/c2.log" \
        || fail "the bigalloc refusal did not name the feature"
    [ ! -e bad-dst.img ] || fail "a refused conversion left a destination image behind"
    pass "bigalloc (unknown INCOMPAT): exit 3, named reason, no destination left"
else
    note "bigalloc not available in this mkfs.ext4: leg skipped (reported, not silently)"
fi

# C3: a path the VOLUME cannot represent ('!' is the container-sibling
# namespace, refused at every user-name entry point, WP135)
mkfs_image bang.img 64 -b 4096
invfs_loop_mount "$PWD/bang.img" "$MNT"
sudo chown "$(id -u):$(id -g)" "$MNT"
printf 'hi\n' > "$MNT/a!b.txt"
printf 'ok\n' > "$MNT/normal.txt"
sudo umount "$MNT"; sudo losetup -d "$LOOPDEV"; LOOPDEV=""
set +e
sudo -n "$CONVERT" convert bang.img bang-dst.img >"$W/c3.log" 2>&1
c3_rc=$?
set -e
cat "$W/c3.log"
[ "$c3_rc" = 3 ] || fail "the '!' refusal exited $c3_rc, want 3"
grep -q "reserves for container siblings" "$W/c3.log" || fail "the '!' refusal did not name the reason"
grep -q "removed the destination" "$W/c3.log" || fail "the refused run did not say it removed the destination"
[ ! -e bang-dst.img ] || fail "a refused conversion left a destination image behind"
pass "'!' in a name: exit 3, named reason, destination removed"

# ---------------------------------------------------------------- leg D -----
echo
echo "== leg D: real-world ext4 rootfs (opt-in derivation, loud skip) =="
# Candidates, in order of cost: an already-extracted ext4 rootfs first (a real
# image, seconds to use), then a cloud image that has to be DERIVED (qemu-img +
# losetup -P + dd: minutes, and only with INVFS_CONVERT_REAL=1). The magic
# at offset 1080 is the ext4 superblock's 0xEF53; testing it is what keeps this
# leg from "skipping" a real image that is sitting right there because a qcow2
# happened to be earlier in the list.
ext4_magic() { [ "$(sudo -n dd if="$1" bs=1 skip=1080 count=2 status=none 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "53ef" ]; }

REAL_IMG=${INVFS_CONVERT_REAL_IMG:-}
CLOUD_IMG=""
if [ -z "$REAL_IMG" ]; then
    for c in /var/tmp/real/u2404-root.ext4 /var/tmp/u2404-root.ext4 \
             /var/tmp/debian12-root.ext4 "$REPO/../bench/ubuntu2404.img"; do
        [ -f "$c" ] || continue
        if ext4_magic "$c"; then REAL_IMG=$c; break; fi
        case "$(file -b "$c")" in *qcow*) CLOUD_IMG=$c ;; esac
    done
fi
if [ -z "$REAL_IMG" ] && [ -n "$CLOUD_IMG" ] && [ "${INVFS_CONVERT_REAL:-0}" = "1" ]; then
    note "deriving an ext4 image from $CLOUD_IMG (qemu-img + losetup -P + dd)"
    command -v qemu-img >/dev/null || fail "qemu-img is needed to derive the rootfs image"
    qemu-img convert -O raw "$CLOUD_IMG" "$W/real.raw"
    RLOOP=$(sudo -n losetup -P --find --show "$W/real.raw")
    # the root partition is found by MAGIC, not by number: cloud images put the
    # ESP first and renumber freely
    ROOTP=""
    for p in "${RLOOP}"p*; do
        ext4_magic "$p" && ROOTP=$p
    done
    [ -n "$ROOTP" ] || { sudo -n losetup -d "$RLOOP"; fail "no ext4 partition found in $CLOUD_IMG"; }
    sudo -n dd if="$ROOTP" of="$W/real.ext4" bs=1M conv=sparse status=none
    sudo -n losetup -d "$RLOOP"
    REAL_IMG=$W/real.ext4
    note "real rootfs image: $(du -h "$REAL_IMG" | cut -f1) on disk"
fi
# LEG D IS OPT-IN EVEN WHEN AN IMAGE IS SITTING THERE. A 2.5 GiB rootfs costs
# ~700 s to convert and ~180 s to verify; that is a measurement to run and
# report, not a per-push gate. INVFS_CONVERT_REAL=1 is the switch, and the skip
# below names the image it declined to use so "skipped" is never confused with
# "nothing was available".
if [ "${INVFS_CONVERT_REAL:-0}" = "1" ] && [ -n "$REAL_IMG" ] && [ -f "$REAL_IMG" ] && ext4_magic "$REAL_IMG"; then
    note "real-world leg on $REAL_IMG"
    set +e
    /usr/bin/time -f "  wall %e s" sudo -n "$CONVERT" convert "$REAL_IMG" real-dst.img >"$W/real.log" 2>&1
    r_rc=$?
    set -e
    grep -E "^(byte map|  (meta_structure|garbage|member|carry)|tree import|pack oracle|nk match|unmatched|gate|timing)|wall " "$W/real.log" | sed 's/^/    /'
    [ "$r_rc" = 0 ] || { cat "$W/real.log"; fail "real-world convert exited $r_rc"; }
    grep -q "RESULT: converted" "$W/real.log" || fail "real-world convert did not claim a conversion"
    set +e
    sudo -n "$CONVERT" verify "$REAL_IMG" real-dst.img >"$W/realverify.log" 2>&1
    rv_rc=$?
    set -e
    grep -E "^(entries|  |content|xattrs|timing|per-file|hardlinks|DIFFS|RESULT)" "$W/realverify.log" | sed 's/^/    /'
    [ "$rv_rc" = 0 ] || { cat "$W/realverify.log"; fail "real-world verify exited $rv_rc"; }
    grep -q "RESULT: IDENTICAL" "$W/realverify.log" || fail "real-world verify reported differences"
    pass "real-world rootfs: converted and verified IDENTICAL"
else
    if [ -n "$REAL_IMG" ]; then
        echo "  SKIP (loud): leg D is opt-in. An ext4 rootfs IS available at $REAL_IMG;"
        echo "        re-run with INVFS_CONVERT_REAL=1 to convert and verify it."
    else
        echo "  SKIP (loud): no real ext4 rootfs image available."
        echo "        Looked for: \$INVFS_CONVERT_REAL_IMG, /var/tmp/{real/,}u2404-root.ext4,"
        echo "        <repo>/../bench/ubuntu2404.img (a qcow2: derive it, or set"
        echo "        INVFS_CONVERT_REAL=1 to let this suite derive it). Manual recipe:"
        echo "          qemu-img convert -O raw <cloud>.img real.raw"
        echo "          losetup -P --find --show real.raw    # find the ext4 partition"
        echo "          dd if=/dev/loopNp1 of=real.ext4 bs=1M conv=sparse"
        echo "          INVFS_CONVERT_REAL=1 INVFS_CONVERT_REAL_IMG=real.ext4 bash tools/test-convert-ext4.sh"
    fi
fi

echo
echo "PASS: invf-convert (ext4 -> InvariantFS, phase 0) — every leg green"
