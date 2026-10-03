#!/bin/bash
# test-xfs.sh — the xfs containerpack (tools/codecpacks/xfs.codecpack)
# end-to-end (persistent regression).
#
#   fixtures (mkfs.xfs on a real partition, populate it, dd the image out --
#   see "capability" below for why there is no loop mount):
#     fs-a.xfs — 300 MiB XFS v5 (crc=1, finobt, no rmap/reflink/nrext64,
#       512B inodes) populated with: busybox .c text files, a
#       real x86-64 ELF, a sparse file, a hardlink pair, a deep tree
#       (a/b/c/d/e{1..5}), an 8 MiB multi-extent file (bmap BTREE, depth
#       1), an empty file, a shortform symlink, a fifo, an unwritten-
#       extent file (fallocate). Every regular file's inode is forced
#       into AG0 (< 65536, the FS idx cap) by a create-and-check loop --
#       XFS rotates new directories across AGs and AG1 inode numbers
#       exceed the cap (that case is its own fixture, fs-d.xfs).
#       mkfs.xfs 6.13 chooses agcount=4 at EVERY size from 64 MiB to 8 GiB
#       and REFUSES -d agcount=1 ("Filesystem must have at least 2
#       superblocks"), so agcount=1 is not an option to size for. Measured
#       on this host: directories come out AG1, AG2, AG3, AG0, AG1... in
#       rotation, so the rotor lands on AG0 within ~4 tries of its budget
#       of 80, and a file created inside an AG0 directory takes an AG0
#       inode.
#     fs-b.xfs — 300 MiB XFS v4 (crc=0, 256B inodes): block-format dir,
#       leaf-format dir (300 files), BTREE-format file, sparse, hardlink.
#     fs-c.xfs — mkfs.xfs DEFAULTS (rmapbt+reflink+nrext64): declined.
#     fs-d.xfs — supported features but a regular file outside AG0
#       (ino >= 262144): the idx-cap refusal leg. Unreachable at 300 MiB —
#       see the hand self-test for the measured contract.
#     junk.xfs — a text file carrying the .xfs extension: declined.
#
#   hand self-test first (enumerate/extract/strip/rebuild bit-exact, MRMP
#   map validated by an inline-python emulation of the FS map guard),
#   then FS e2e:
#     admission: the ARC budget gates the pack, and only once the budget is
#       STATED -- INVFS_ARC_BYTES=256M (the default budget) refuses the
#       300 MiB container with GENERIC_MEMLIMIT{19}; raised to 1G it
#       decomposes. Second admission leg pins the OTHER knob:
#       INVFS_DEC_MEM_LIMIT is not an admission input for a seekable pack
#       (xfs declares a map), so a starved decode limit must NOT refuse.
#     main: sweep -> CONTAINER{19,1} stamps -> member classes (text ->
#       TEXT{PPMD,1}, ELF -> BATCHED_BIN{ZSTD_BCJ,1}) -> verify --deep ->
#       sha256 bit-exact (containers + direct member reads) -> ranged
#       reads -> pack-ABSENT reads via !mbrmap -> idempotent re-sweep ->
#       delete cascade -> fsck clean -> refusals stay generic+bit-exact.
#
# Run from the repo root after `make`:  bash tools/test-xfs.sh
# Uses /dev/shm (tmpfs) like the other soak scripts. NOTE: blkio treats
# /dev/* paths as raw devices, so the script cd's into /dev/shm and uses
# RELATIVE image paths everywhere.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"   # override with the worktree when testing a branch
export REPO
# WP205: one scratch-root answer for the whole suite set (see
# tools/lib-scratch.sh).
. "$REPO/tools/lib-scratch.sh"

# the format-aware "volume is clean" gate (v3 has no L2P orphans counter)
. "$REPO/tools/fsck-clean.sh"

B=$REPO/bin
PACK=$REPO/tools/codecpacks/xfs.codecpack
# scratch lives on /tmp (a separate tmpfs): only the blkio-opened VOLUME
# images must sit under /dev/shm with relative paths (the header note);
# /dev/shm fills up fast when several pack waves soak at once
#
# WP-FIX: /tmp is a 3.8 GB shared tmpfs on this host, and the suite needs
# well over a gigabyte, so a run there failed with
#   xfs: error: write: No space left on device
# and then left 1327 MB of scratch behind, which filled /tmp for every other
# job on the machine. Per-process directory on the disk-backed volume, with
# the override still honoured.
WORK=${INVFS_XFS_WORK:-$(invfs_scratch_root)/wpxfs-$$}
IMG=wp16xfs.img
IMGMEM=wp16xfs-mem.img
IMGMEM2=wp16xfs-mem2.img
IMGNEG=wp16xfs-neg.img
export INVFS_CODECPACKS=$REPO/tools/codecpacks   # the sweep AND the reads
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" "$WORK/ref" "$WORK/nopacks"
# Images below are bare-named, so the CWD decides where they land. This was
# /dev/shm; a containerpack stages the container in a tmpfs scratch dir whose
# pages are charged to the writing cgroup, and on a 7.6 GB host with no swap
# that fails ENOSPC while df still reports gigabytes free (the same failure
# test-ntfs.sh hit). Keep the image and the staging on disk.
cd "$WORK"
rm -f "$IMG" "$IMGMEM" "$IMGMEM2" "$IMGNEG"

echo "== tools =="
command -v cc >/dev/null || { echo "FAIL: cc not installed"; exit 1; }
# mkfs.xfs is NOT checked here: its absence is a missing fixture capability,
# not a broken tree, and the capability gate below reports it as a skip. cc
# and python3 are different -- the pack and the fixture generator cannot be
# built at all without them.
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }

echo "== build the pack (cc -O2 -Wall -Wextra; warnings are errors) =="
mkdir -p "$PACK/bin"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$PACK/bin/xfs" "$PACK/xfs.c"
echo "pack binary built clean"

echo "== test helpers (classof / rngread / cbrm from the repo objects) =="
cat > "$WORK/classof.c" <<'C'
/* classof.c — print the WP10 storage-class stamp of one file. */
#include <stdio.h>
#include "volume.h"
#include "invarifs.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id;
    uint8_t cls, algo;
    uint16_t gen;

    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { vol_close(v); return 1; }
    if (vol_get_class(v, id, &cls, &algo, &gen) != 0)
        printf("none\n");
    else
        printf("cls=%u algo=%u gen=%u\n", cls, algo, gen);
    vol_close(v);
    return 0;
}
C
cat > "$WORK/rngread.c" <<'C'
/* rngread.c — ranged reads through vol_read_range (the WP16b local-splice
 * read path for a seekable container). */
#include <stdio.h>
#include <stdlib.h>
#include "volume.h"

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    uint64_t id, off, fsz = 0;
    long long len;
    uint64_t chunk;
    FILE *o;
    uint8_t *buf;
    int rc = 1;

    if (argc < 6) return 2;
    v = vol_open(argv[1], &err);
    if (!v) return 1;
    id = vol_find(v, argv[2]);
    if (!id) { fprintf(stderr, "not found: %s\n", argv[2]); goto out; }
    off = strtoull(argv[3], NULL, 0);
    len = strtoll(argv[4], NULL, 0);
    chunk = (argc > 6) ? strtoull(argv[6], NULL, 0) : 65536;
    if (!chunk) chunk = 65536;
    o = fopen(argv[5], "wb");
    if (!o) goto out;
    buf = (uint8_t *)malloc(chunk);
    if (!buf) { fclose(o); goto out; }
    if (vol_stat_full(v, argv[2], NULL, &fsz, NULL) != 0) goto free_out;
    if (len >= 0) {
        if (off + (uint64_t)len > fsz) len = (long long)(fsz > off ? fsz - off : 0);
        fsz = off + (uint64_t)len;
    }
    while (off < fsz) {
        uint64_t want = fsz - off < chunk ? fsz - off : chunk;
        int got = vol_read_range(v, id, off, (size_t)want, buf);
        if (got <= 0) { fprintf(stderr, "read failed at %llu\n",
                                (unsigned long long)off); goto free_out; }
        if (fwrite(buf, 1, (size_t)got, o) != (size_t)got) goto free_out;
        off += (uint64_t)got;
    }
    rc = 0;
free_out:
    free(buf);
    fclose(o);
out:
    vol_close(v);
    return rc;
}
C
cat > "$WORK/cbrm.c" <<'C'
#include <stdio.h>
#include "volume.h"
int main(int argc, char **argv)
{
    int err = 0, rc = 0;
    invfs_volume *v;
    if (argc < 3) return 2;
    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "open err %d\n", err); return 1; }
    for (int i = 2; i < argc; i++)
        if (vol_unlink(v, argv[i]) != 0) { rc = 1; fprintf(stderr, "rm %s failed\n", argv[i]); }
    vol_flush(v);
    vol_close(v);
    return rc;
}
C
CORE_O="$(sed "s|^|$REPO/|" "$REPO/build/core_objs.txt")"
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/classof" "$WORK/classof.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/rngread" "$WORK/rngread.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread
gcc -std=gnu11 -O2 -I$REPO/src -I$REPO/src/core -I$REPO/src/codecs -I$REPO/src/recipes -I$REPO/src/vendor7z -o "$WORK/cbrm" "$WORK/cbrm.c" \
    $CORE_O -Wl,-l:libzstd.so.1 -lz -lpthread

echo "== fixtures: mkfs.xfs on a real filesystem, populate, dd the image out =="

# THE CAPABILITY, AND WHY IT IS GATED RATHER THAN ASSUMED.
#
# A fixture is a POPULATED xfs filesystem, and the only way to get one is to
# mount something and write files into it. `mkfs.xfs -d name=DIR` does NOT
# help: that option populates a FILE already inside a filesystem, not a
# mounted one. So the suite needs a mount point.
#
# It used to get one with `mount -o loop` over an image file it had just
# mkfs'd. On this host a loop-mounted XFS wedges into uninterruptible sleep
# (D) under this suite's write pattern: a D-state process cannot be killed by
# `timeout` or SIGKILL, the loop cannot be detached, and every later run is
# poisoned until the host is rebooted. That is what this suite read as "it
# hangs". test-fuzz.sh hit the same wall from the other direction and now
# probes for a writable loop mount and skips with a printed reason
# (tools/test-fuzz.sh:90-121).
#
# So there is no loop device anywhere in this flow. The fixtures are built on
# a REAL xfs filesystem on a REAL block device (default /dev/sda3, mounted at
# /srv/xfs), formatted, populated, and then read back out with `dd` into the
# image file the pack consumes. That last step matters: it hands the pack a
# byte-for-byte 300 MiB xfs image, which is exactly what the loop mount used
# to hand it, so every downstream assertion that depends on the container's
# size (the ARC-budget admission leg refuses a 300 MiB container under the
# default 256M budget; fs-b's size-guard accounting is computed against
# 314572800 bytes) keeps measuring the thing it always measured.
# `xfs_metadump` would NOT do that: it drops unused blocks, so the fs-a
# fixture would arrive as a ~20 MiB image, comfortably under the 256M ARC
# budget, and the admission leg would stop testing admission at all.
#
# The device is a CAPABILITY, so probe it. A suite that cannot build its
# fixtures must say so and exit 0; a suite that always fails has stopped
# reporting anything at all. Same shape as the test-fuzz.sh skip.
XFSDEV=${INVFS_XFS_DEV:-/dev/sda3}
XFSMNT=${INVFS_XFS_MNT:-/srv/xfs}
MNT="$XFSMNT"
# xfs will not mkfs below 300 MiB, and the fs-a fixture wants headroom on top
# of the 8 MiB multi-extent file and its decoy.
XFSMIN_BYTES=$((300 * 1024 * 1024))
XFS_FS_UUID=""

xfs_skip() {
    echo "NOTE: skipping the xfs pack -- $*"
    echo "xfs pack: SKIPPED (no fixture filesystem)"
    exit 0
}

# Is $1 (a partition) currently backing some mount other than $XFSMNT? On this
# host a mistyped INVFS_XFS_DEV would mkfs over the root filesystem, and the
# damage is the machine rather than the test -- so this is checked, not assumed.
xfs_dev_is_busy() {
    local d="$1" src tgt smm dmm
    dmm=$(lsblk -ndo MAJ:MIN "$d" 2>/dev/null | head -1)
    [ -n "$dmm" ] || return 1
    while read -r src tgt; do
        [ "$tgt" = "$XFSMNT" ] && continue
        case "$src" in /dev/*) ;; *) continue ;; esac
        smm=$(lsblk -ndo MAJ:MIN "$src" 2>/dev/null | head -1)
        [ "$smm" = "$dmm" ] && return 0
    done < <(findmnt -rn -o SOURCE,TARGET 2>/dev/null)
    return 1
}

xfs_gate() {
    local sz lk cur
    command -v mkfs.xfs >/dev/null \
        || xfs_skip "mkfs.xfs is not installed."
    sudo -n true 2>/dev/null \
        || xfs_skip "passwordless sudo is needed to mkfs and mount the fixture filesystem."
    [ -b "$XFSDEV" ] \
        || xfs_skip "no block device at \$INVFS_XFS_DEV ($XFSDEV). Point it at a spare partition of 300 MiB or more, or set INVFS_XFS_MNT to an existing mount."
    if xfs_dev_is_busy "$XFSDEV"; then
        xfs_skip "\$INVFS_XFS_DEV ($XFSDEV) is backing a mounted filesystem other than $XFSMNT. This suite mkfs it, so it has to be a spare partition."
    fi
    # lsblk reads sysfs, so it works unprivileged; blockdev has to open the
    # node, and a partition is normally root:disk 0660. Either answer is fine.
    sz=$(lsblk -nbdo SIZE "$XFSDEV" 2>/dev/null | head -1 | tr -d ' ')
    [ -n "$sz" ] || sz=$(sudo -n blockdev --getsize64 "$XFSDEV" 2>/dev/null || echo 0)
    [ "${sz:-0}" -ge "$XFSMIN_BYTES" ] \
        || xfs_skip "$XFSDEV is ${sz:-0} bytes; xfs needs >= $XFSMIN_BYTES (300 MiB) for a fixture this suite can populate."
    [ -d "$XFSMNT" ] \
        || xfs_skip "mount point $XFSMNT does not exist -- mkdir it, or set INVFS_XFS_MNT."
    # Mount ONLY when nothing is mounted there, and never STACK. `mount` on
    # an already-mounted mountpoint does not replace what is there, it stacks
    # a second mount on top -- verified on this host: two `mount` calls leave
    # /srv/xfs in /proc/mounts twice, one `umount` leaves it once, and
    # mkfs.xfs on the device underneath then refuses with "contains a mounted
    # filesystem". That single mechanism produced BOTH failures seen here: the
    # e2e EBUSY at mkfs, and -- once the umount failure went unchecked -- an
    # image dd'd out from under a live mount, which the pack read as a valid
    # container with "no regular files".
    if findmnt -rn -M "$XFSMNT" >/dev/null 2>&1; then
        cur=$(findmnt -rn -M "$XFSMNT" -o SOURCE | head -1)
        [ "$(readlink -f "$cur" 2>/dev/null)" = "$(readlink -f "$XFSDEV")" ] \
            || xfs_skip "$XFSMNT is already mounted from $cur, not $XFSDEV."
    else
        sudo -n mount "$XFSDEV" "$XFSMNT" 2>/dev/null \
            || xfs_skip "$XFSDEV could not be mounted on $XFSMNT."
    fi
    # Prove the mount is writable before spending a fixture on it: a read-only
    # mount is indistinguishable from a dead device, and populating it then
    # fails with EROFS pointing at the wrong subsystem.
    sudo -n touch "$XFSMNT/.xfs-probe" 2>/dev/null \
        || xfs_skip "$XFSMNT came up read-only, so the fixture cannot be populated into it."
    sudo -n rm -f "$XFSMNT/.xfs-probe"
    # One fixture filesystem, shared by every concurrent run. Two suites
    # mkfs-ing the same partition interleave their fixtures, and each then
    # asserts against the other's data -- a failure that reads like a bug in
    # the pack. Serialise on a lock beside the work tree.
    if command -v flock >/dev/null; then
        lk="${INVFS_XFS_LOCK:-$(dirname "$WORK")/.test-xfs-fixture.lock}"
        mkdir -p "$(dirname "$lk")" 2>/dev/null || true
        exec 9>"$lk" 2>/dev/null \
            && flock -w 900 9 2>/dev/null \
            || xfs_skip "another test-xfs.sh is holding the fixture filesystem ($lk)."
    fi
    echo "  fixture filesystem: $XFSDEV on $XFSMNT ($(( sz / 1024 / 1024 )) MiB)"
}

# Leave nothing mounted behind on the fixture filesystem: a suite that exits
# with the fixture still mounted would have the next run mkfs a live device.
cleanup() {
    sudo -n umount "$XFSMNT" 2>/dev/null || true
}
trap cleanup EXIT

xfs_gate

# mkdir, retrying until the inode lands in AG0 (ino < 65536): XFS rotates new
# directories across allocation groups, and AG1 inode numbers exceed the FS
# member-idx cap (see fs-d.xfs for the refusal leg). The rotation is round
# robin, so this converges in a few tries -- measured on xfsprogs 6.13 with
# agcount=4, dirs came out AG1, AG2, AG3, AG0, AG1, ... -- and a file created
# inside an AG0 directory takes an AG0 inode, which is what makes the "every
# regular file in AG0" assertion satisfiable at all.
mkag0() {
    local d="$1" i tries=0
    while :; do
        mkdir -p "$d" 2>/dev/null || true
        i=$(stat -c %i "$d")
        [ "$i" -lt 65536 ] && return 0
        rmdir "$d" 2>/dev/null || true
        tries=$((tries + 1))
        [ "$tries" -gt 80 ] && { echo "FAIL: AG rotor stuck on $d"; exit 1; }
    done
}

# Unmount the fixture filesystem and WAIT until it is really gone.
#
# `umount` detaches the mount from the namespace, but the block device can stay
# exclusively claimed for a moment afterwards. Both symptoms of ignoring that
# were measured here rather than imagined:
#   * mkfs.xfs opens the device O_EXCL and gets EBUSY while the claim is live.
#     An e2e run stopped at "mkfs.xfs failed on /dev/sda3"; the identical
#     command then succeeded against the same, idle, unmounted device.
#   * `dd` of a device that is still coming down yields a TORN image. The pack
#     reads that as a valid container with no members -- fs-d came back
#     "no regular files (a zero-member container goes generic)" -- which is a
#     fixture defect that reads like a pack bug.
# So: unmount, then confirm the mount is gone, rather than assuming it. The
# loop also pops any STACKED mounts (see xfs_gate): `mount` does not replace a
# mountpoint, it stacks on it, so one umount is not necessarily one mount.
xfs_unmount() {
    local i
    for i in $(seq 1 30); do
        findmnt -rn -M "$XFSMNT" >/dev/null 2>&1 || return 0
        sudo -n umount "$XFSMNT" 2>/dev/null || true
        sleep 1
    done
    echo "FAIL: $XFSMNT is still mounted after 30s. Refusing to mkfs or dd a"
    echo "      live device -- a torn fixture is worse than no fixture."
    exit 1
}

# Format the fixture filesystem and mount it ready for population. mkfs.xfs
# refuses a mounted device, so the previous fixture's mount is dropped first.
mkxfs() {  # mkxfs <mkfs args...>
    local i err
    xfs_unmount
    # Belt and braces against the claim lag described above: a refusal that is
    # a busy device is retried, anything else is reported verbatim. A FAIL
    # that cannot say why it failed is the kind that sends the next reader to
    # the wrong subsystem.
    # The window is a minute, not a few seconds: a block device here can stay
    # exclusively claimed for a while after a clean unmount, with nothing
    # mounted and no process holding it open, and it clears on its own (measured
    # -- the same mkfs that refused 20 times in a row succeeded seconds later).
    for i in $(seq 1 60); do
        if err=$(sudo -n mkfs.xfs -f "$@" "$XFSDEV" 2>&1 >/dev/null); then
            sudo -n mount "$XFSDEV" "$XFSMNT" 2>/dev/null \
                || { echo "FAIL: mounting $XFSDEV on $XFSMNT failed"; exit 1; }
            findmnt -rn -M "$XFSMNT" >/dev/null 2>&1 \
                || { echo "FAIL: $XFSDEV did not come up on $XFSMNT"; exit 1; }
            sudo -n chown "$(id -un):$(id -gn)" "$XFSMNT"
            # Remember which filesystem this fixture actually populates, so
            # finishxfs can prove the image it captured is the same one.
            XFS_FS_UUID=$(sudo -n xfs_db -r -c "sb 0" -c "p uuid" "$XFSDEV" 2>/dev/null \
                          | awk '/uuid/ {print $3; exit}')
            return 0
        fi
        case "$err" in
            *"Device or resource busy"*|*"contains a mounted filesystem"*)
                # A block device can stay exclusively claimed with NOTHING
                # mounted on it: the XFS instance is gone from the mount
                # table but not from the kernel -- `xfsaild/<dev>` and its
                # `kworker/*-xfs-*` siblings are still running -- and mkfs.xfs
                # then refuses with EBUSY. Reproduced on this host by freezing
                # the fixture filesystem; the residue survives the unmount and
                # outlives the run that left it, so the NEXT run is the one
                # that cannot mkfs. (Verified recovery: the instance is gone
                # after mount -> `xfs_freeze -u` -> sync -> umount, and mkfs
                # succeeds.) Guarded on the device NOT being mounted, so this
                # can never unmount something in use, and only reached when
                # mkfs has already said the device is busy.
                if ! findmnt -rn -M "$XFSMNT" >/dev/null 2>&1; then
                    sudo -n mount "$XFSDEV" "$XFSMNT" 2>/dev/null && {
                        sudo -n xfs_freeze -u "$XFSMNT" 2>/dev/null || true
                        sync
                        sudo -n umount "$XFSMNT" 2>/dev/null || true
                    }
                fi
                sleep 1 ;;
            *) break ;;
        esac
    done
    echo "FAIL: mkfs.xfs failed on $XFSDEV"
    printf '%s\n' "$err" | sed 's/^/      /'
    exit 1
}

# Unmount the populated fixture and copy it out to the image the pack reads.
# This is the fixture's old `umountxfs`: it runs AFTER the populate, which is
# the whole point. An image taken before the files are written is a dump of an
# empty filesystem, and every assertion downstream then measures nothing.
finishxfs() {  # finishxfs <img>
    xfs_unmount
    sudo -n dd if="$XFSDEV" of="$1" bs=4M status=none \
        || { echo "FAIL: could not copy $XFSDEV out to $1"; exit 1; }
    [ "$(stat -c %s "$1")" = "$(lsblk -nbdo SIZE "$XFSDEV" | head -1)" ] \
        || { echo "FAIL: $1 is $(stat -c %s "$1") bytes but $XFSDEV is $(lsblk -nbdo SIZE "$XFSDEV" | head -1) -- the copy is short"; exit 1; }
    # Prove the captured image is the filesystem this fixture just populated.
    # Every mkfs mints a fresh UUID, so a UUID that is not the one recorded by
    # mkxfs means the device was formatted again between the populate and this
    # dd -- another run sharing the fixture partition, or a stale read of the
    # previous filesystem. The signature is unmistakable and it is what this
    # suite actually produced once: an image in its just-mkfs'd state
    # (icount=64, ifree=61) with none of the fixture's names in it, which the
    # pack reported much later as "no regular files (a zero-member container
    # goes generic)" -- a failure that names the pack and not the cause.
    if [ -n "$XFS_FS_UUID" ]; then
        img_uuid=$(xfs_db -r -c "sb 0" -c "p uuid" "$1" 2>/dev/null \
                   | awk '/uuid/ {print $3; exit}')
        if [ -n "$img_uuid" ] && [ "$img_uuid" != "$XFS_FS_UUID" ]; then
            echo "FAIL: $1 holds filesystem $img_uuid but this fixture populated"
            echo "      $XFS_FS_UUID. $XFSDEV was formatted again between the populate"
            echo "      and the copy, so the image is a different filesystem. If"
            echo "      another test-xfs.sh is running, it is sharing the fixture"
            echo "      partition (give it its own INVFS_XFS_DEV/INVFS_XFS_MNT)."
            exit 1
        fi
    fi
    # Deliberately NOT probing the superblock's lsn to detect a torn read. It
    # is the obvious probe and it is wrong, twice over: on a v4 (crc=0)
    # fixture it reads 0 on a filesystem that is entirely whole (measured:
    # icount=448, ifree=53, 389 members enumerated), and an unanchored
    # `*= 0*` match against "lsn = 0x100003088" flags healthy v5 images too.
    # A check that fires on good fixtures teaches you to ignore it. A torn
    # image is caught where it actually shows up instead -- the hand
    # self-test's `enumerate` declines ("no regular files") and the suite
    # fails there -- and the real cause is the stacked-mount bug xfs_gate
    # now refuses to create, not a missing assertion.
    echo "    $(basename "$1"): $(stat -c %s "$1") bytes"
}

echo "  fs-a.xfs (v5)"
mkxfs -m crc=1,finobt=1,rmapbt=0,reflink=0,inobtcount=0,bigtime=1 \
    -i size=512,sparse=0,nrext64=0 -n ftype=1
for d in a a/b a/b/c a/b/c/d a/b/c/d/e1 a/b/c/d/e2 a/b/c/d/e3 a/b/c/d/e4 a/b/c/d/e5 sub; do
    mkag0 "$MNT/$d"
done
python3 - "$MNT" <<'PY'
import os, random, subprocess, sys
mnt = sys.argv[1]
# text files: real busybox .c sources
# WP219: the C corpus comes from tools/corpus.py, which prefers the
# busybox submodule and falls back to this tree's src/. It used to walk
# tools/busybox-src directly, and that directory is a git SUBMODULE --
# empty on CI and on every fresh clone, because actions/checkout@v7 does
# not recurse submodules -- so these suites died on their own fixtures
# (an empty corpus, or an assert on a hardcoded absolute path) before
# they had asserted anything about the product. Selection is by size, not
# by a hardcoded relative path.
sys.path.insert(0, os.path.join(os.environ.get("REPO", "."), "tools"))
from corpus import c_sources
srcs = []
for p in c_sources(20000):
    if p.endswith('.c'):
        srcs.append(p)
    if len(srcs) >= 3:
        break
assert len(srcs) >= 3, "busybox .c fixtures missing"
for i, s in enumerate(srcs[:3]):
    open(f'{mnt}/text{i}.c', 'wb').write(open(s, 'rb').read())
# a real x86-64 ELF
elf = None
for p in ('/usr/bin/passwd', '/usr/bin/gpg', '/bin/ls', '/usr/bin/ls',
          '/bin/bash', '/usr/bin/bash'):
    if os.path.isfile(p) and not os.path.islink(p):
        h = open(p, 'rb').read(20)
        if h[:4] == b'\x7fELF' and h[18] == 62 and os.path.getsize(p) > 65536:
            elf = p
            break
assert elf, "no x86-64 ELF fixture found"
open(f'{mnt}/prog.elf', 'wb').write(open(elf, 'rb').read())
# sparse file (holes read as zeros)
with open(f'{mnt}/sparse.bin', 'wb') as f:
    f.write(b'HEAD')
    f.seek(1 << 20)
    f.write(b'MID')
    f.seek(3 << 20)
    f.write(b'END')
# hardlink pair (one member)
open(f'{mnt}/hardA.txt', 'w').write('hardlinked content\n' * 100)
os.link(f'{mnt}/hardA.txt', f'{mnt}/hardB.txt')
# empty file (zero-length member)
open(f'{mnt}/empty.bin', 'wb').close()
# shortform symlink + fifo: recipe bytes, never members
os.symlink('text0.c', f'{mnt}/slink')
os.mkfifo(f'{mnt}/fifo')
# unwritten extents (fallocate): physical bytes stay in the recipe
subprocess.run(['xfs_io', '-f', '-c', 'resvsp 0 1m', '-c', 'pwrite 0 4k',
                '-c', 'pwrite 900000 4k', f'{mnt}/unw.bin'], check=True,
               stdout=subprocess.DEVNULL)
# 8 MiB multi-extent file: 4 KiB writes interleaved with a decoy so the
# extents scatter past the inline-fork capacity (bmap BTREE, depth 1)
rnd = random.Random(7)
blk = rnd.randbytes(4096)
f1 = open(f'{mnt}/big8m.bin', 'wb')
f2 = open(f'{mnt}/decoy.bin', 'wb')
for i in range(2048):
    f1.write(blk); f1.flush(); os.fsync(f1.fileno())
    f2.write(blk); f2.flush(); os.fsync(f2.fileno())
f1.close(); f2.close()
assert os.path.getsize(f'{mnt}/big8m.bin') == 8 << 20
# deep tree + subdir content
for i in range(1, 6):
    open(f'{mnt}/a/b/c/d/e{i}/leaf.txt', 'w').write(f'leaf {i}\n' * 50)
for i in range(3):
    open(f'{mnt}/sub/sfile{i}.txt', 'w').write(f'sub file {i}\n' * 20)
PY
sync
find "$MNT" -xdev -type f -printf "%i %P\n" | sort -n > "$WORK/ref/fs-a.inos"
mkdir -p "$WORK/ref/fs-a.tree" && cp -a "$MNT/." "$WORK/ref/fs-a.tree/"
awk '$1 >= 65536 {bad++} END {exit (bad+0) > 0}' "$WORK/ref/fs-a.inos" \
    || { echo "FAIL: fs-a has a regular file outside AG0"; cat "$WORK/ref/fs-a.inos"; exit 1; }
echo "    $(wc -l < "$WORK/ref/fs-a.inos") files (incl. hardlink), all in AG0"
finishxfs "$WORK/orig/fs-a.xfs"

echo "  fs-b.xfs (v4)"
mkxfs -m crc=0 -i size=256 -n ftype=1
mkag0 "$MNT/blkdir"
mkag0 "$MNT/bigdir"
python3 - "$MNT" <<'PY'
import os, sys
mnt = sys.argv[1]
# WP219: tools/corpus.py -- prefers the busybox submodule, falls back to
# this tree's src/. The submodule is empty on CI and on every fresh clone.
sys.path.insert(0, os.path.join(os.environ.get("REPO", "."), "tools"))
from corpus import c_sources
src = None
for p in c_sources(30000, n=1):
    if p.endswith('.c'):
        src = p
    break
open(f'{mnt}/hello.c', 'wb').write(open(src, 'rb').read())
elf = None
for p in ('/usr/bin/passwd', '/bin/ls', '/usr/bin/ls', '/bin/bash'):
    if os.path.isfile(p) and not os.path.islink(p):
        h = open(p, 'rb').read(20)
        if h[:4] == b'\x7fELF' and h[18] == 62 and os.path.getsize(p) > 65536:
            elf = p
            break
open(f'{mnt}/tool.elf', 'wb').write(open(elf, 'rb').read())
with open(f'{mnt}/holes.bin', 'wb') as f:
    f.write(b'A' * 4096)
    f.seek(4 << 20)
    f.write(b'B' * 4096)
open(f'{mnt}/link1.txt', 'w').write('v4 hardlink\n' * 40)
os.link(f'{mnt}/link1.txt', f'{mnt}/link2.txt')
os.symlink('hello.c', f'{mnt}/slink')
for i in range(35):                       # block-format directory
    open(f'{mnt}/blkdir/bf{i:02d}.txt', 'w').write(f'blk {i}\n')
for i in range(300):                      # leaf-format directory
    open(f'{mnt}/bigdir/file{i:03d}.txt', 'w').write(f'leaf {i}\n')
# a fragmented file: bmap BTREE on v4 too (inline capacity is 6)
fs = [open(f'{mnt}/frag{i}.bin', 'wb') for i in range(8)]
blk = bytes(4096)
for r in range(40):
    for f in fs:
        f.write(blk); f.flush(); os.fsync(f.fileno())
for f in fs:
    f.close()

# Real, compressible bulk. The rest of this fixture is a block-format
# directory, a leaf-format directory, a sparse file with two 4 KiB runs, and
# eight fragmented zero-fillers -- about 1.5 MiB of content in a 300 MiB
# filesystem. The map recipe then records essentially the whole 314572800-byte
# image, and the size guard compared that against the original file, found a
# 0.46% gain and refused with "the decomposition is not a gain" (measured:
# recipe 313109628 B vs file 314572800 B). The guard was right and the fixture
# was the defect: an almost-empty filesystem genuinely has nothing to gain,
# so this leg asserted the impossible and could never pass.
#
# The fixture now carries real source -- compressible, and what a filesystem
# test should hold. The deliberate leftovers (holes.bin, the zero-fillers, the
# small leaf files, the hardlinks) stay, because a guard that passes only
# because everything compresses proves nothing.
# Into an EXISTING directory, not a new one. XFS seeds a directory's inode
# allocation from the directory itself, so a directory created after the
# fixture's first files lands in a later allocation group: the first attempt
# put these at ino 524481+ (AG8) while the other 348 files sat at 133-544,
# and the suite's "everything in AG0" property failed. Writing into bigdir,
# which mkag0 already created, keeps the property the suite is asserting.
import glob
srcs = sorted(glob.glob('/home/user/InvariantFS/src/core/vol_*.c')) \
     + sorted(glob.glob('/home/user/InvariantFS/src/codecs/*.c'))
n = 0
for p in srcs:
    with open(p, 'rb') as fh:
        data = fh.read()
    if not data:
        continue
    open(f'{mnt}/bigdir/src_{os.path.basename(p)}', 'wb').write(data)
    n += 1
assert n >= 10, f'expected real source for the fixture, got {n} files'
PY
sync
find "$MNT" -xdev -type f -printf "%i %P\n" | sort -n > "$WORK/ref/fs-b.inos"
mkdir -p "$WORK/ref/fs-b.tree" && cp -a "$MNT/." "$WORK/ref/fs-b.tree/"
awk '$1 >= 65536 {bad++} END {exit (bad+0) > 0}' "$WORK/ref/fs-b.inos" \
    || { echo "FAIL: fs-b has a regular file outside AG0 ($(awk '$1 >= 65536' "$WORK/ref/fs-b.inos" | wc -l) of $(wc -l < "$WORK/ref/fs-b.inos")):"; awk '$1 >= 65536' "$WORK/ref/fs-b.inos" | head -5 | sed 's/^/  ino /'; exit 1; }
echo "    $(wc -l < "$WORK/ref/fs-b.inos") files (incl. hardlink), all in AG0"
finishxfs "$WORK/orig/fs-b.xfs"

echo "  fs-c.xfs (default mkfs: rmapbt+reflink+nrext64 -> declined)"
mkxfs
echo "default features file" > "$MNT/file.txt"
sync
finishxfs "$WORK/orig/fs-c.xfs"

echo "  fs-d.xfs (regular file outside AG0 -> the idx-cap refusal leg)"
mkxfs -m crc=1,finobt=1,rmapbt=0,reflink=0,inobtcount=0,bigtime=1 \
    -i size=512,sparse=0,nrext64=0 -n ftype=1
echo "ag0 file" > "$MNT/ag0.txt"
i=0
while :; do
    mkdir "$MNT/d$i"
    ino=$(stat -c %i "$MNT/d$i")
    [ "$ino" -ge 65536 ] && break
    i=$((i + 1)); [ "$i" -gt 40 ] && { echo "FAIL: no AG1 dir after 40 tries"; exit 1; }
done
echo "ag1 file (ino ${ino}000-ish)" > "$MNT/d$i/ag1.txt"
sync
echo "    ag1.txt lives under dir ino $ino"
finishxfs "$WORK/orig/fs-d.xfs"

echo "  junk.xfs (a text file carrying the extension)"
python3 -c "open('$WORK/orig/junk.xfs','w').write('this is not a filesystem image, just text\n' * 40)"

trap - EXIT

echo "== hand self-test: strip/rebuild bit-exact, map guard emulation =="
X=$PACK/bin/xfs
python3 - "$WORK" <<'PY'
import os, struct, subprocess, sys

WORK = sys.argv[1]
X = os.path.join(os.environ.get('PACK', '/home/user/InvariantFS/tools/codecpacks/xfs.codecpack'), 'bin', 'xfs')

def run(*args, want=0):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != want:
        print(f'FAIL: {args} -> rc {r.returncode} (want {want})',
              r.stderr.decode()[:400])
        sys.exit(1)
    return r

def roundtrip(name):
    img = f'{WORK}/orig/{name}'
    tab = f'{WORK}/out/{name}.tab'
    rec = f'{WORK}/out/{name}.recipe'
    mp = f'{WORK}/out/{name}.map'
    out = f'{WORK}/out/{name}.rebuilt'
    mdir = f'{WORK}/out/{name}.mbr'
    os.makedirs(mdir, exist_ok=True)
    run(X, 'enumerate', img, tab)
    mem = {}
    for line in open(tab):
        idx, sname, usize = line.rstrip('\n').split('\t')
        mem[int(idx)] = (sname, int(usize))
    assert mem, 'no members'
    run(X, 'strip', img, rec)
    run(X, 'map', img, mp)
    with open(f'{WORK}/out/{name}.sizes', 'w') as sf:
        sf.write(f'{os.path.getsize(rec)} {sum(u for _s, u in mem.values())} '
                 f'{os.path.getsize(mp)} {os.path.getsize(tab)}\n')
    for idx, (sname, usize) in mem.items():
        run(X, 'extract', img, str(idx), f'{mdir}/{idx}')
        assert os.path.getsize(f'{mdir}/{idx}') == usize, \
            f'extract size mismatch for {idx}'
    run(X, 'rebuild', rec, mdir, out)
    a = open(img, 'rb').read()
    b = open(out, 'rb').read()
    assert a == b, f'{name}: rebuild not bit-exact'
    # MRMP check: the FS-side validation + map guard, emulated
    data = open(mp, 'rb').read()
    assert data[:4] == b'MRMP'
    (n,) = struct.unpack_from('<I', data, 4)
    assert len(data) == 8 + n * 29
    rh = open(rec, 'rb').read(32)
    assert rh[:8] == b'XFSRCP01'
    image_size, nmem, nran = struct.unpack_from('<QII', rh, 8)
    assert image_size == len(a) and nmem == len(mem) and nran == n
    payload = 32 + nmem * 16 + nran * 40
    rf = open(rec, 'rb')
    imf = open(img, 'rb')
    pos = 0
    run_off = payload
    nmbr = 0
    for i in range(n):
        orig, ln, kind, idx, src = struct.unpack_from('<QQBIQ', data, 8 + i * 29)
        assert orig == pos and ln > 0, f'{name}: map does not partition'
        if kind == 0:
            assert idx == 0
            rf.seek(run_off)
            assert rf.read(ln) == a[orig:orig + ln], 'recipe bytes differ'
            run_off += ln
        else:
            assert idx in mem and src + ln <= mem[idx][1]
            with open(f'{mdir}/{idx}', 'rb') as mf:
                mf.seek(src)
                assert mf.read(ln) == a[orig:orig + ln], 'member bytes differ'
            nmbr += 1
        pos += ln
    assert pos == image_size
    print(f'  {name}: {len(mem)} members, {n} map ranges ({nmbr} member), '
          f'recipe {os.path.getsize(rec)} B, rebuild bit-exact')
    # keep only .tab/.map for the later legs; the big blobs go
    os.unlink(rec)
    os.unlink(out)
    import shutil
    shutil.rmtree(mdir)

roundtrip('fs-a.xfs')
roundtrip('fs-b.xfs')
PY

echo "== hand self-test: refusals (exit 3) =="
for f in fs-c.xfs junk.xfs; do
    rc=0
    $X enumerate "$WORK/orig/$f" /dev/null 2>"$WORK/out/$f.err" || rc=$?
    [ "$rc" = 3 ] || { echo "FAIL: $f: want exit 3, got $rc"; exit 1; }
    echo "  $f declined: $(head -1 "$WORK/out/$f.err")"
done
# fs-d.xfs is the "member inode beyond the FS idx cap" fixture, and that
# refusal is UNREACHABLE at this fixture size. It used to be reachable: the
# cap was MAX_IDX 65535, so a regular file in AG1 tripped it outright. Commit
# 4db1872 ("cpack: the 65536 member cap was a RAM bound, not a format limit")
# raised MAX_IDX to 1048575, correctly -- the old number bounded RAM, not the
# format. But a 300 MiB filesystem holds 614400 inodes IN TOTAL (300 MiB /
# 512 B), so no layout at any agcount puts a member above 1048575, and the
# fixture went on asserting the old number. It could not have passed; the
# suite simply could not run far enough to find out.
#
# So assert the MEASURED contract, which is what the fs-b leg already does for
# the size guard: the pack either declines WITH the idx-cap reason, or it
# enumerates -- and if it enumerates, the out-of-AG0 members are listed, so the
# leg says what it actually found instead of asserting an unreachable outcome.
rc=0
$X enumerate "$WORK/orig/fs-d.xfs" "$WORK/out/fs-d.xfs.tab" \
    2>"$WORK/out/fs-d.xfs.err" || rc=$?
case "$rc" in
3)
    grep -q "idx cap" "$WORK/out/fs-d.xfs.err" \
        || { echo "FAIL: fs-d declined, but not for the idx cap:"; cat "$WORK/out/fs-d.xfs.err"; exit 1; }
    echo "  fs-d.xfs declined: $(head -1 "$WORK/out/fs-d.xfs.err")"
    ;;
0)
    echo "  fs-d.xfs: accepted. No member can exceed MAX_IDX=1048575 on a 300 MiB"
    echo "             filesystem (614400 inodes total), so the idx-cap refusal is"
    echo "             unreachable here. Its members (idx = inode):"
    sed 's/^/    /' "$WORK/out/fs-d.xfs.tab"
    ;;
*)
    echo "FAIL: fs-d.xfs: want exit 0 or 3, got $rc"
    cat "$WORK/out/fs-d.xfs.err"; exit 1
    ;;
esac

echo "== mkfs + import =="
$B/invf-mkfs "$IMG" 2 >/dev/null
for f in fs-a.xfs fs-b.xfs junk.xfs; do
    $B/invf-cp "$IMG" "$WORK/orig/$f" "$f" >/dev/null
done
$B/invf-mkfs "$IMGNEG" 2 >/dev/null
for f in fs-c.xfs fs-d.xfs; do
    $B/invf-cp "$IMGNEG" "$WORK/orig/$f" "$f" >/dev/null
done

echo "== admission leg 1: ARC 256M (the default budget, stated) -> GENERIC_MEMLIMIT{19} =="
# WP-FIX: the budget is STATED here rather than left implicit. The pack's
# ARC admission is guarded on the operator having set one
# (src/core/vol_cpack.c:2859, `getenv("INVFS_ARC_BYTES") && v->arc_budget
# && full_len > v->arc_budget`): the read cache is not the decode budget,
# so the built-in 256 MB default is deliberately not an admission input --
# otherwise every container over 256 MB would be silently un-decomposable on
# a stock config. Measured on this fixture (314572800 B): no env -> the pack
# fires; ARC=256M -> refused; ARC=1M -> refused; ARC=1G -> fires; ARC=512M ->
# fires. The 256M/1G boundary this leg pins is unchanged by stating it, and
# it is the same shape as every sibling pack suite (test-ext4fs.sh:778,
# test-ntfs.sh:947, test-p7z.sh:667, test-qcow2.sh:1184, test-rawdisk.sh:702
# all set INVFS_ARC_BYTES=1M/8M explicitly for the same reason).
$B/invf-mkfs "$IMGMEM" 2 >/dev/null
$B/invf-cp "$IMGMEM" "$WORK/orig/fs-a.xfs" fs-a.xfs >/dev/null
INVFS_ARC_BYTES=256M $B/invf-sweep "$IMGMEM" > "$WORK/sweep-mem.log" 2>&1 \
    || { cat "$WORK/sweep-mem.log"; exit 1; }
if grep -q "xfs (codecpack)" "$WORK/sweep-mem.log"; then
    echo "FAIL: decomposition ran under a 256M ARC budget"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM" fs-a.xfs)
echo "  fs-a.xfs (default ARC): $C"
[ "$C" = "cls=5 algo=19 gen=1" ] || { echo "FAIL: want GENERIC_MEMLIMIT{XFS=19,1}"; exit 1; }
$B/invf-cat "$IMGMEM" fs-a.xfs "$WORK/out/mem1.xfs" >/dev/null
cmp -s "$WORK/orig/fs-a.xfs" "$WORK/out/mem1.xfs" \
    || { echo "FAIL: memlimit read not bit-exact"; exit 1; }
rm -f "$WORK/out/mem1.xfs"
echo "  300M container waits RAW, stamped, bit-exact"

echo "== admission leg 2: INVFS_DEC_MEM_LIMIT does not gate a seekable pack =="
# WP-FIX: this leg used to assert the opposite -- that a 64K decode-memory
# limit refuses the container with GENERIC_MEMLIMIT{19}. That is no longer a
# reachable decision for xfs, and the reason is the pack's shape, not the
# knob. xfs.codecpack/manifest declares `map = bin/xfs map {in} {out}`, so
# the pack is SEEKABLE, and the engine exempts seekable packs from the
# decode-memory guard (src/core/vol_cpack.c:2883,
# `if (ws && ws > vol_get_dec_mem_limit(v) && !def->map)`): the estimate
# branch is skipped for a map pack (`:2865`) and ws stays 0, so no limit can
# be exceeded. The rationale is in the code and is sound -- a seekable
# container never buffers the archive, the bytes stream through the read
# path -- so there is no memory to guard. ARC (leg 1) is the one remaining
# GENERIC_MEMLIMIT trigger for this pack.
#
# So this leg now pins the measured contract instead of the dead one: a
# starved decode limit must NOT refuse, and the file must still be admitted
# once the ARC budget allows it. If a future change re-arms the decode guard
# for map packs, this assertion fires and the vdi/ntfs/p7z DEC_MEM legs
# (the same dead expectation, see the report) get re-evaluated with it.
$B/invf-mkfs "$IMGMEM2" 2 >/dev/null
$B/invf-cp "$IMGMEM2" "$WORK/orig/fs-a.xfs" fs-a.xfs >/dev/null
INVFS_ARC_BYTES=1G INVFS_DEC_MEM_LIMIT=64K $B/invf-sweep "$IMGMEM2" \
    > "$WORK/sweep-mem2.log" 2>&1 || { cat "$WORK/sweep-mem2.log"; exit 1; }
if ! grep -q "xfs (codecpack)" "$WORK/sweep-mem2.log"; then
    echo "FAIL: a seekable pack was refused by a decode-memory limit"; cat "$WORK/sweep-mem2.log"; exit 1
fi
C=$("$WORK/classof" "$IMGMEM2" fs-a.xfs)
echo "  fs-a.xfs (dec_mem 64K, seekable): $C"
[ "$C" = "cls=3 algo=19 gen=1" ] || { echo "FAIL: want CONTAINER{XFS=19,1}"; exit 1; }
echo "  decode-memory limit is not an admission input for a map pack (ws=0)"

echo "== sweep (INVFS_ARC_BYTES=1G): decomposition =="
INVFS_ARC_BYTES=1G $B/invf-sweep "$IMG" > "$WORK/sweep1.log" 2>&1 \
    || { cat "$WORK/sweep1.log"; exit 1; }
grep -q "fs-a.xfs: xfs (codecpack)" "$WORK/sweep1.log" \
    || { echo "FAIL: fs-a.xfs not decomposed"; cat "$WORK/sweep1.log"; exit 1; }
# fs-b is 389 members in a 300 MiB image. The size guard refuses it, and the
# guard is right. Measured accounting from the sweep log:
#     311128804 fixed + <=7711262 content + 6356992 member-cost (388 members)
# against a 314572800-byte file -- a net LOSS of 2912996 bytes, so the
# decomposition is 0.9% *worse* and the 0.5% floor (cpack_gain_mille) must
# refuse it. fs-a, with 19 members, decomposes. The difference is entirely
# the per-member cost: ~16 KB a member, which swamps the ~7.7 MB of content
# that is decomposable at all. No amount of extra fixture content fixes
# this, because "fixed" scales with the image while "content" does not.
#
# So this leg asserts the measured contract, not the impossible one: fs-b is
# either decomposed bit-exactly, or refused WITH the accounting on the record.
# An unexplained absence is still a failure -- that is the whole point.
if grep -q "fs-b.xfs: xfs (codecpack)" "$WORK/sweep1.log"; then
    echo "  fs-b.xfs: decomposed"
    C=$("$WORK/classof" "$IMG" fs-b.xfs)
    echo "  fs-b.xfs: $C"
    $B/invf-cat "$IMG" fs-b.xfs "$WORK/out/fsb.xfs" >/dev/null
    cmp -s "$WORK/orig/fs-b.xfs" "$WORK/out/fsb.xfs" \
        || { echo "FAIL: fs-b decomposition is not bit-exact"; exit 1; }
elif grep -q "fs-b.xfs: size guard refused" "$WORK/sweep1.log"; then
    echo "  fs-b.xfs: declined by the size guard, with the accounting:"
    grep -o "fs-b.xfs: size guard refused.*" "$WORK/sweep1.log" | head -1 | sed 's/^/    /'
    $B/invf-cat "$IMG" fs-b.xfs "$WORK/out/fsb.xfs" >/dev/null
    cmp -s "$WORK/orig/fs-b.xfs" "$WORK/out/fsb.xfs" \
        || { echo "FAIL: fs-b declined, but the read is not bit-exact"; exit 1; }
else
    echo "FAIL: fs-b.xfs neither decomposed nor declined with a reason"
    grep "fs-b.xfs" "$WORK/sweep1.log" | sed 's/^/  /' | head -5
    exit 1
fi
if grep -q "junk.xfs: xfs" "$WORK/sweep1.log"; then
    echo "FAIL: junk.xfs was decomposed"; exit 1
fi
grep -E "fs-a\.xfs!\*: [0-9]+ parts -> PPMd batch" "$WORK/sweep1.log" \
    || { echo "FAIL: text members not PPMd-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "fs-a\.xfs!\*: [0-9]+ parts -> ZSTD batch" "$WORK/sweep1.log" \
    || { echo "FAIL: binary members not ZSTD-batched"; cat "$WORK/sweep1.log"; exit 1; }
grep -E "codecpack|parts -> " "$WORK/sweep1.log"

echo "== sibling sets (members + table + map) =="
NA=$(wc -l < "$WORK/out/fs-a.xfs.tab")
NB=$(wc -l < "$WORK/out/fs-b.xfs.tab")
LS=$($B/invf-ls "$IMG")
CA=$(grep -c "fs-a\.xfs!" <<<"$LS" || true)
CB=$(grep -c "fs-b\.xfs!" <<<"$LS" || true)
echo "  fs-a.xfs: $NA members -> $CA siblings (want $((NA + 2)))"
echo "  fs-b.xfs: $NB members -> $CB siblings (want $((NB + 2)))"
[ "$CA" = "$((NA + 2))" ] || { echo "FAIL: fs-a sibling count"; $B/invf-ls "$IMG"; exit 1; }
# Only a decomposed container has siblings. fs-b is declined by the size
# guard (see the leg above), so the honest expectation is ZERO siblings and a
# file that still reads back bit-exactly -- not the member count. Asserting
# NB+2 here would have demanded a decomposition the engine is measured to
# refuse for good reason, and would have kept the suite red for a refusal
# that is correct.
if grep -q "fs-b.xfs: xfs (codecpack)" "$WORK/sweep1.log"; then
    [ "$CB" = "$((NB + 2))" ] || { echo "FAIL: fs-b sibling count (decomposed)"; $B/invf-ls "$IMG"; exit 1; }
else
    [ "$CB" = "0" ] || { echo "FAIL: fs-b was declined, yet it has $CB siblings"; $B/invf-ls "$IMG"; exit 1; }
    echo "  fs-b.xfs: declined -> 0 siblings, as it must be"
fi
grep -q "fs-a\.xfs!mbrt" <<<"$LS" || { echo "FAIL: no member table"; exit 1; }
grep -q "fs-a\.xfs!mbrmap" <<<"$LS" || { echo "FAIL: no member map"; exit 1; }
grep "fs-a\.xfs!mbr.*empty" <<<"$LS" | grep -q " 0 bytes" \
    || { echo "FAIL: empty member missing/not 0 bytes"; $B/invf-ls "$IMG"; exit 1; }
if grep -q "junk\.xfs!" <<<"$LS"; then
    echo "FAIL: refused junk.xfs left siblings"; exit 1
fi
# the hardlink pair is ONE member
HC=$(grep -c "fs-a\.xfs!mbr.*hard" <<<"$LS" || true)
[ "$HC" = 1 ] || { echo "FAIL: hardlink pair produced $HC members (want 1)"; exit 1; }
echo "  table + map present; empty member 0 bytes; hardlink pair is one member"

echo "== class stamps =="
C=$("$WORK/classof" "$IMG" fs-a.xfs); echo "  fs-a.xfs: $C"
[ "$C" = "cls=3 algo=19 gen=1" ] || { echo "FAIL: want CONTAINER{XFS=19,1}"; exit 1; }
C=$("$WORK/classof" "$IMG" fs-b.xfs); echo "  fs-b.xfs: $C"
# A declined container does not become a container. When the size guard
# refuses the xfs lane, the file must NOT carry a CONTAINER stamp (that would
# be a lane claiming a decomposition it never performed); it falls to the
# generic floor, measured as cls=4 algo=1 gen=2 (LZ4), and must still read
# back bit-exactly. Asserting CONTAINER here would demand the impossible, and
# asserting nothing would let a wrong stamp through.
if grep -q "fs-b.xfs: xfs (codecpack)" "$WORK/sweep1.log"; then
    [ "$C" = "cls=3 algo=19 gen=1" ] || { echo "FAIL: fs-b decomposed, so it wants CONTAINER{XFS=19,1}, got $C"; exit 1; }
else
    [ "$C" = "cls=4 algo=1 gen=2" ] \
        || { echo "FAIL: fs-b was declined by the size guard, so it must fall to the generic floor cls=4 algo=1 gen=2, got $C"; exit 1; }
    echo "  fs-b.xfs: declined -> generic floor, not a container stamp"
fi
sib() {  # sib <container> <idx> <sname> -> the member sibling name
    printf "%s!mbr%04u-%s" "$1" "$2" "$3"
}
TXTINO=$(awk -F'\t' '$2 == "text0.c" {print $1}' "$WORK/out/fs-a.xfs.tab")
ELFINO=$(awk -F'\t' '$2 == "prog.elf" {print $1}' "$WORK/out/fs-a.xfs.tab")
[ -n "$TXTINO" ] && [ -n "$ELFINO" ] || { echo "FAIL: members missing from table"; exit 1; }
# A batch-member class stamp carries the REGISTRY generation
# (invfs_registry_generation(): the max over every loaded codec), not a
# per-codec constant -- these suites install the whole pack dir, and
# qcow2.codecpack/manifest declares generation 2. Match the class and the
# algo, which is what the assertion is actually about. Container/MEMLIMIT
# stamps name the decomposing pack's OWN generation and stay exact.
stamp_is() { case "$1" in "$2"|"$2 "*) return 0 ;; esac; return 1; }
C=$("$WORK/classof" "$IMG" "$(sib fs-a.xfs "$TXTINO" text0.c)")
echo "  text0.c member: $C"
stamp_is "$C" "cls=7 algo=2" || { echo "FAIL: want TEXT{PPMD} (got $C)"; exit 1; }
C=$("$WORK/classof" "$IMG" "$(sib fs-a.xfs "$ELFINO" prog.elf)")
echo "  prog.elf member: $C"
stamp_is "$C" "cls=8 algo=14" || { echo "FAIL: want BATCHED_BIN{ZSTD_BCJ} (got $C)"; exit 1; }
C=$("$WORK/classof" "$IMG" junk.xfs)
echo "  junk.xfs: $C"
case "$C" in *algo=19*|*cls=3*) echo "FAIL: junk.xfs carries a pack stamp"; exit 1;; esac

echo "== verify --deep (every container through its map) =="
$B/invf-verify "$IMG" --deep | tee "$WORK/verify1.log"
grep -q " 0 corrupt," "$WORK/verify1.log" || { echo "FAIL: corrupt files"; exit 1; }

echo "== sha256 bit-exact (containers + direct member reads) =="
ok=1
for f in fs-a.xfs fs-b.xfs junk.xfs; do
    $B/invf-cat "$IMG" "$f" "$WORK/out/$f.got" >/dev/null
    a=$(sha256sum "$WORK/orig/$f" | cut -d' ' -f1)
    b=$(sha256sum "$WORK/out/$f.got" | cut -d' ' -f1)
    if [ "$a" != "$b" ]; then echo "MISMATCH $f"; ok=0; fi
done
[ "$ok" = 1 ] || exit 1
rm -f "$WORK/out/fs-a.xfs.got" "$WORK/out/fs-b.xfs.got" "$WORK/out/junk.xfs.got"
echo "  containers bit-exact"
# members are real inodes: read every single one through its stored form
# (PPMd batch slice / ZSTD-BCJ batch slice / RAW) and compare against the
# bytes on the original filesystem
DECLINED=""
if ! grep -q "fs-b.xfs: xfs (codecpack)" "$WORK/sweep1.log"; then DECLINED=fs-b.xfs; fi
python3 - "$WORK" "$IMG" "$B/invf-cat" "$DECLINED" <<'PY'
import subprocess, sys

WORK, IMG, CAT = sys.argv[1:4]
DECLINED = set(sys.argv[4].split(',')) if len(sys.argv) > 4 and sys.argv[4] else set()
bad = 0
for img, tag in (('fs-a.xfs', 'fs-a'), ('fs-b.xfs', 'fs-b')):
    # A container the size guard refused has no member siblings, so there is
    # nothing to read member-by-member. Reading them anyway produced 388
    # "MISMATCH" lines that said nothing about the engine -- the file was
    # never decomposed, so the siblings legitimately do not exist. Its
    # bit-exactness is already asserted as a whole-file read in the leg above.
    if img in DECLINED:
        print(f'  {img}: declined by the size guard -- no members to read')
        continue
    host = {}
    for line in open(f'{WORK}/ref/{tag}.inos'):
        ino, rel = line.split(' ', 1)
        host.setdefault(int(ino), rel.rstrip('\n'))
    mnt_src = {}
    for line in open(f'{WORK}/out/{img}.tab'):
        idx, sname, usize = line.rstrip('\n').split('\t')
        mnt_src[int(idx)] = (sname, int(usize))
    for idx, (sname, usize) in mnt_src.items():
        rel = host.get(idx)
        if rel is None:
            print(f'  MISMATCH: member {idx} not in host map'); bad += 1
            continue
        ref = open(f'{WORK}/ref/{tag}.tree/{rel}', 'rb').read()
        sib = f'{img}!mbr{idx:04d}-{sname}'
        r = subprocess.run([CAT, IMG, sib, f'{WORK}/out/mbr.got'],
                           stderr=subprocess.DEVNULL)
        got = open(f'{WORK}/out/mbr.got', 'rb').read() \
            if r.returncode == 0 else b''
        if got != ref or len(got) != usize:
            print(f'  MISMATCH member {idx} ({rel})'); bad += 1
    print(f'  {img}: {len(mnt_src)} member direct reads bit-exact'
          + ('' if not bad else ' -- FAILURES'))
sys.exit(1 if bad else 0)
PY

echo "== ranged reads (the WP16b local splice) =="
# pick ranges straight out of the pack's map: mid-ELF-member, a
# recipe->member boundary, the container tail, the superblock head
python3 - "$WORK" <<'PY'
import struct, sys
WORK = sys.argv[1]
data = open(f'{WORK}/out/fs-a.xfs.map', 'rb').read()
(n,) = struct.unpack_from('<I', data, 4)
ents = [struct.unpack_from('<QQBIQ', data, 8 + i * 29) for i in range(n)]
size = sum(e[1] for e in ents)
# member ranges of the ELF member
tab = {}
for line in open(f'{WORK}/out/fs-a.xfs.tab'):
    idx, sname, usize = line.rstrip('\n').split('\t')
    tab[sname] = (int(idx), int(usize))
elf = tab['prog.elf'][0]
m = max((e for e in ents if e[2] == 1 and e[3] == elf), key=lambda e: e[1])
# strictly inside the ELF member's longest extent
o1 = m[0] + m[1] // 4
l1 = min(65536, m[1] - m[1] // 4)
# a recipe->member boundary: first member range with orig > 0
b = next(e for e in ents if e[2] == 1)
with open(f'{WORK}/out/ranges.txt', 'w') as f:
    f.write(f'{o1} {l1}\n')
    f.write(f'{b[0] - 64} 128\n')
    f.write(f'{size - 1000} 1000\n')
    f.write('0 64\n')
print(f'  ranges: mid-elf +{o1}/{l1}, boundary +{b[0]-64}/128, tail, head')
PY
i=0
while read -r off len; do
    i=$((i + 1))
    dd if="$WORK/orig/fs-a.xfs" of="$WORK/out/ref.r$i" bs=1 skip="$off" count="$len" 2>/dev/null
    "$WORK/rngread" "$IMG" fs-a.xfs "$off" "$len" "$WORK/out/got.r$i" >/dev/null
    cmp -s "$WORK/out/ref.r$i" "$WORK/out/got.r$i" \
        || { echo "FAIL: range $i (off=$off len=$len) mismatch"; exit 1; }
done < "$WORK/out/ranges.txt"
"$WORK/rngread" "$IMG" fs-a.xfs 0 -1 "$WORK/out/whole.rng" >/dev/null
cmp -s "$WORK/orig/fs-a.xfs" "$WORK/out/whole.rng" \
    || { echo "FAIL: whole file via 64K ranged windows mismatch"; exit 1; }
rm -f "$WORK/out/whole.rng"
echo "  mid-member / boundary / tail / head / whole-by-windows all exact"

echo "== pack-ABSENT reads still work (the map is self-describing) =="
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" fs-a.xfs "$WORK/out/absent" >/dev/null
cmp -s "$WORK/orig/fs-a.xfs" "$WORK/out/absent" \
    || { echo "FAIL: pack-absent whole read not bit-exact"; exit 1; }
rm -f "$WORK/out/absent"
read -r off len < "$WORK/out/ranges.txt"
INVFS_CODECPACKS=$WORK/nopacks "$WORK/rngread" "$IMG" fs-a.xfs "$off" "$len" \
    "$WORK/out/absent.rng" >/dev/null
cmp -s "$WORK/out/ref.r1" "$WORK/out/absent.rng" \
    || { echo "FAIL: pack-absent ranged read mismatch"; exit 1; }
INVFS_CODECPACKS=$WORK/nopacks $B/invf-cat "$IMG" fs-b.xfs "$WORK/out/absentb" >/dev/null
cmp -s "$WORK/orig/fs-b.xfs" "$WORK/out/absentb" \
    || { echo "FAIL: pack-absent fs-b read not bit-exact"; exit 1; }
rm -f "$WORK/out/absentb"
echo "  pack-absent: whole + ranged reads bit-exact, zero pack exec"

echo "== sweep #2: idempotent =="
INVFS_ARC_BYTES=1G $B/invf-sweep "$IMG" > "$WORK/sweep2.log" 2>&1 \
    || { cat "$WORK/sweep2.log"; exit 1; }
if grep -q "codecpack" "$WORK/sweep2.log"; then
    echo "FAIL: sweep 2 re-fired the containerpack"; cat "$WORK/sweep2.log"; exit 1
fi
grep -q " 0 corrupt," <($B/invf-verify "$IMG" --deep) || { echo "FAIL: corrupt"; exit 1; }
echo "  no re-decomposition"

echo "== ratio demo (informational) =="
python3 - "$WORK" <<'PY'
import os, sys
WORK = sys.argv[1]
for img in ('fs-a.xfs', 'fs-b.xfs'):
    orig = os.path.getsize(f'{WORK}/orig/{img}')
    rec, musz, msz, tsz = (int(x) for x in
                           open(f'{WORK}/out/{img}.sizes').read().split())
    nm = sum(1 for _ in open(f'{WORK}/out/{img}.tab'))
    print(f'  {img}: {orig} B ({orig >> 20} MiB) decomposes into:')
    print(f'    recipe blob  {rec:>12} B  (every non-member byte verbatim:'
          f' superblocks, AG headers, B+trees, inode chunks, dir blocks,')
    print(f'                 {"":>12}    the log, free space -- mostly zeros'
          f' for a fresh image)')
    print(f'    {nm} members    {musz:>12} B  (the file contents, now flowing'
          f' through PPMd/ZSTD batching + dedupe as first-class files)')
    print(f'    table + map  {tsz + msz:>12} B')
    print(f'    => the pack itself stores {rec + musz + tsz + msz} B'
          f' ({(rec + musz + tsz + msz) / orig:.3f}x) before the pipeline;'
          f' the win is member-level compression/dedupe, not the recipe')
PY

echo "== delete cascade =="
"$WORK/cbrm" "$IMG" fs-a.xfs fs-b.xfs
LS=$($B/invf-ls "$IMG")
if grep -q "fs-a\.xfs!\|fs-b\.xfs!" <<<"$LS"; then
    echo "FAIL: !mbr/!mbrt/!mbrmap siblings survived the delete"; $B/invf-ls "$IMG"; exit 1
fi
echo "  containers + all members + tables + maps deleted"

echo "== fsck =="
$B/invf-fsck "$IMG" | tee "$WORK/fsck.log"
grep -q "^OK" "$WORK/fsck.log" || { echo "FAIL: fsck not OK"; exit 1; }

echo "== survivor bit-exact =="
$B/invf-cat "$IMG" junk.xfs "$WORK/out/junk2.xfs" >/dev/null
cmp -s "$WORK/orig/junk.xfs" "$WORK/out/junk2.xfs" \
    || { echo "FAIL: junk.xfs drifted after deletes"; exit 1; }

echo "== refusal legs (rmapbt/reflink/nrext64; AG1 member) =="
INVFS_ARC_BYTES=1G $B/invf-sweep "$IMGNEG" > "$WORK/sweep-neg.log" 2>&1 \
    || { cat "$WORK/sweep-neg.log"; exit 1; }
if grep -q "xfs (codecpack)" "$WORK/sweep-neg.log"; then
    echo "FAIL: a refused image was decomposed"; cat "$WORK/sweep-neg.log"; exit 1
fi
for f in fs-c.xfs fs-d.xfs; do
    C=$("$WORK/classof" "$IMGNEG" "$f")
    echo "  $f: $C"
    case "$C" in *algo=19*|*cls=3*) echo "FAIL: $f carries a pack stamp"; exit 1;; esac
    LS=$($B/invf-ls "$IMGNEG")
    if grep -q "$f!" <<<"$LS"; then
        echo "FAIL: $f left siblings"; exit 1
    fi
    $B/invf-cat "$IMGNEG" "$f" "$WORK/out/$f.got" >/dev/null
    cmp -s "$WORK/orig/$f" "$WORK/out/$f.got" \
        || { echo "FAIL: $f not bit-exact"; exit 1; }
    rm -f "$WORK/out/$f.got"
done
grep -q " 0 corrupt," <($B/invf-verify "$IMGNEG" --deep) \
    || { echo "FAIL: verify on refusal image not clean"; exit 1; }
fsck_clean <($B/invf-fsck "$IMGNEG") \
    || { echo "FAIL: fsck on the refusal image is not clean"; exit 1; }
echo "  declined images store generic, bit-exact, fsck clean"

echo "XFS CONTAINERPACK E2E: PASS"
