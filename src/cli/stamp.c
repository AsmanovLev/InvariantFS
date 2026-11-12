/* stamp.c — one-shot offline migration of a volume's format generation.
 *
 * WHY THIS EXISTS
 * The 0 -> 1 generation bump did not move a single byte of layout: the
 * superblock carries the marker, the metadata store was already declared
 * (MET0) and VOLF_META was already set. A volume written before the bump is
 * therefore byte-wise a current volume that says "0" in two places (and one
 * mirror of that byte at the tail anchor). The gate refuses it BY NUMBER,
 * which is correct -- but a hand `dd` cannot finish the job:
 *
 *   1. ANC0, the tail anchor, stores format_version AND a CRC32C over itself
 *      (offset 0x78, over 0x00..0x77). Patching the byte without recomputing
 *      the CRC makes the anchor REFUSED_GEOMETRY at every mount.
 *   2. On a two-device volume the anchor is NOT at the end of the image. Its
 *      address is the volume's last GLOBAL block, which lands on dev1 at
 *      local block (total_blocks - 1 - dev_blocks[0]). Writing "the last
 *      block of the file" puts it into live data.
 *   3. Both devices mirror block 0 byte-for-byte, so both copies move
 *      together or the mirror check fires.
 *
 * So this is the same idea as `dd` with the two things dd cannot do: find the
 * anchor correctly, and re-stamp its CRC.
 *
 * SAFETY
 *   - Dry run by default. --apply is required to write anything.
 *   - Refuses a device that /proc/mounts names, and refuses anything that
 *     does not look like the volume it claims to be (magic, superblock CRC,
 *     MET0, VOLF_META, DEVT, anchor CRC, matching geometry).
 *   - The anchor's EXISTING crc is recomputed and compared before anything is
 *     written: a CRC implementation that cannot reproduce what is on the disk
 *     never gets to write one.
 *   - Backs up every block it touches (block 0 of each device, the anchor
 *     block) before the first write, and re-reads everything afterwards.
 *   - Idempotent: a device already stamped is reported, not rewritten, so a
 *     crash between the two block-0 writes is repaired by re-running it.
 *
 * Usage:
 *   invf-stamp [--apply] [--dev1 /dev/sdb1] <dev0|image>
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "invarifs.h"

#define STAMP_BLOCK ((size_t)INVFS_BLOCK_SIZE)

/* Retired flag bits. A volume stamped 0 carries them; mkfs no longer writes
 * them and a reader is told to refuse them once a check exists, so leaving
 * them set only defers the problem. */
#define STAMP_RETIRED_FLAGS (0x00000002u | 0x00000008u)   /* VOLF_META2 | VOLF_ASTV2 */

static const char *g_prog = "invf-stamp";

static void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", g_prog);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int read_at(int fd, uint64_t off, void *buf, size_t len, const char *what)
{
    ssize_t got = pread(fd, buf, len, (off_t)off);
    if (got < 0)
        die("%s: read at %llu failed: %s", what, (unsigned long long)off, strerror(errno));
    if ((size_t)got != len)
        die("%s: short read at %llu (%zd of %zu) -- past the end of the device?",
            what, (unsigned long long)off, got, len);
    return 0;
}

static int write_at(int fd, uint64_t off, const void *buf, size_t len, const char *what)
{
    ssize_t put = pwrite(fd, buf, len, (off_t)off);
    if (put < 0)
        die("%s: write at %llu failed: %s", what, (unsigned long long)off, strerror(errno));
    if ((size_t)put != len)
        die("%s: short write at %llu (%zd of %zu)", what, (unsigned long long)off, put, len);
    if (fsync(fd) != 0)
        die("%s: fsync failed: %s", what, strerror(errno));
    return 0;
}

/* 1 when /proc/mounts names this device as the source of a mount. */
static int device_is_mounted(const char *path)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[1024];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char dev[512];
        if (sscanf(line, "%511s", dev) != 1)
            continue;
        if (strcmp(dev, path) == 0)
            { fclose(f); return 1; }
        if (strlen(dev) >= strlen(base) &&
            strcmp(dev + strlen(dev) - strlen(base), base) == 0)
            { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

static uint64_t dev_size_blocks(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        die("fstat failed: %s", strerror(errno));
    if (!S_ISBLK(st.st_mode)) {
        if (st.st_size <= 0)
            die("not a block device and not a regular file with a size");
        return (uint64_t)st.st_size / STAMP_BLOCK;
    }
    {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end < 0)
            die("lseek failed: %s", strerror(errno));
        if (lseek(fd, 0, SEEK_SET) != 0)
            die("lseek back failed: %s", strerror(errno));
        return (uint64_t)end / STAMP_BLOCK;
    }
}

/* The device must be big enough for the geometry the superblock claims. A
 * half-size device is the difference between "stamp" and "write past the
 * end", and the check is free. */
static void check_size(const char *path, int fd, uint64_t need_blocks)
{
    uint64_t have = dev_size_blocks(fd);
    if (have < need_blocks)
        die("%s holds %llu blocks, the volume claims %llu",
            path, (unsigned long long)have, (unsigned long long)need_blocks);
}

static void backup(const char *dir, const char *tag, int fd, uint64_t off, size_t len)
{
    char path[1024];
    uint8_t *buf = malloc(len);
    int out;
    if (!buf)
        die("out of memory");
    snprintf(path, sizeof path, "%s/%s.block", dir, tag);
    read_at(fd, off, buf, len, "backup read");
    out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
        die("cannot create backup %s: %s", path, strerror(errno));
    if (write(out, buf, len) != (ssize_t)len)
        die("cannot write backup %s: %s", path, strerror(errno));
    fsync(out);
    close(out);
    free(buf);
    printf("  backup: %s (%zu bytes from offset %llu)\n", path, len,
           (unsigned long long)off);
}

/* DEVT's crc covers the WHOLE descriptor with the field zeroed (mkfs and
 * devt_crc both do invfs_crc32c(&t, sizeof t)), unlike the superblock and
 * ANC0, whose spans stop at the field. Getting this wrong is invisible
 * until it meets a real volume, which is how it was found. */
static uint32_t devt_crc_of(const invfs_devt *d)
{
    invfs_devt t = *d;
    t.crc32c = 0;
    return invfs_crc32c(&t, sizeof t);
}

/* Recompute the ANC0 crc the way anchor_crc() does: over the descriptor with
 * the crc field read as zero. Duplicated here on purpose -- this tool must not
 * grow a dependency on the volume being openable, because that is the state it
 * is invoked to repair. */
static uint32_t anc0_crc(const invfs_anc0 *a)
{
    invfs_anc0 t = *a;
    t.crc32c = 0;
    return invfs_crc32c(&t, offsetof(invfs_anc0, crc32c));
}

int main(int argc, char **argv)
{
    const char *dev0_path = NULL, *dev1_path = NULL;
    int apply = 0, a, fd0, fd1 = -1;
    uint8_t blk0[STAMP_BLOCK];
    invfs_superblock sb;
    invfs_devt devt;
    int have_devt = 0;
    uint64_t total, dev0_blocks = 0, anchor_global, anchor_local;
    int anchor_fd;
    uint8_t ancb[STAMP_BLOCK];
    invfs_anc0 anc;
    int sb_needs, flags_needs, anc_needs;

    for (a = 1; a < argc; a++) {
        if (strcmp(argv[a], "--apply") == 0)        { apply = 1; continue; }
        if (strcmp(argv[a], "--dev1") == 0 && a + 1 < argc) { dev1_path = argv[++a]; continue; }
        if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0) {
            printf("usage: %s [--apply] [--dev1 <dev1>] <dev0|image>\n\n"
                   "  Migration of a marker-only format generation (0 -> 1).\n"
                   "  Dry run by default; --apply writes.\n"
                   "  Requires the volume to be UNMOUNTED (run from a livecd\n"
                   "  or an initramfs shell).\n", g_prog);
            return 0;
        }
        if (argv[a][0] == '-')
            die("unknown option %s", argv[a]);
        if (dev0_path)
            die("exactly one device argument expected");
        dev0_path = argv[a];
    }
    if (!dev0_path)
        die("no device given (try --help)");

    /* Refuse a mounted volume only when we would WRITE to it. A dry run is
     * read-only, and refusing it would make the tool useless for the one
     * thing it must be checked against first: the real, mounted volume. */
    if (apply && device_is_mounted(dev0_path))
        die("%s is mounted (per /proc/mounts); unmount it or run from a live environment",
            dev0_path);
    if (apply && dev1_path && device_is_mounted(dev1_path))
        die("%s is mounted (per /proc/mounts); unmount it first", dev1_path);

    /* ---- read and validate block 0 of dev0 ---------------------------- */
    fd0 = open(dev0_path, apply ? O_RDWR : O_RDONLY);
    if (fd0 < 0)
        die("cannot open %s: %s", dev0_path, strerror(errno));
    read_at(fd0, 0, blk0, sizeof blk0, dev0_path);
    if (memcmp(blk0, "InvariFS", 8) != 0)
        die("%s: no InvariFS superblock at block 0", dev0_path);
    memcpy(&sb, blk0, sizeof sb);
    if (invfs_crc32c(blk0, offsetof(invfs_superblock, checksum)) != sb.checksum)
        die("%s: superblock CRC does not match -- refusing to touch a torn "
            "descriptor", dev0_path);
    if (memcmp(blk0 + INVFS_MET0_OFF, "MET0", 4) == 0) {
        uint32_t metver;
        memcpy(&metver, blk0 + INVFS_MET0_OFF + 4, 4);
        if (metver != 1)
            die("%s: MET0 version %u, this tool understands 1", dev0_path, metver);
    } else {
        die("%s: no MET0 descriptor -- this is not a metadata store volume, "
            "and its generation is not a marker-only difference", dev0_path);
    }
    if (!(sb.vol_flags & VOLF_META))
        die("%s: VOLF_META unset (vol_flags=0x%08x) -- not a metadata store "
            "volume", dev0_path, sb.vol_flags);

    total = sb.total_blocks;
    if (total < 2)
        die("%s: total_blocks=%llu is not a volume", dev0_path,
            (unsigned long long)total);

    /* DEVT: device table, present from WP25 on. */
    memcpy(&devt, blk0 + INVFS_DEVT_OFF, sizeof devt);
    if (memcmp(devt.magic, "DEVT", 4) == 0) {
        if (devt_crc_of(&devt) != devt.crc32c)
            die("%s: DEVT CRC does not match", dev0_path);
        have_devt = 1;
        dev0_blocks = devt.dev_blocks[0];
    }

    if (dev1_path) {
        fd1 = open(dev1_path, apply ? O_RDWR : O_RDONLY);
        if (fd1 < 0)
            die("cannot open %s: %s", dev1_path, strerror(errno));
        if (!have_devt)
            die("--dev1 given but %s carries no DEVT table", dev0_path);
    } else if (have_devt && devt.dev_count == 2) {
        die("%s declares %u devices; pass --dev1 (the volume is a two-device "
            "volume and its tail anchor lives on dev1)", dev0_path, devt.dev_count);
    }
    check_size(dev0_path, fd0, have_devt && devt.dev_count == 2 ? dev0_blocks : total);

    /* ---- where the anchor is ----------------------------------------- */
    anchor_global = total - 1;
    anchor_fd = fd0;
    anchor_local = anchor_global;
    if (have_devt && devt.dev_count == 2) {
        if (anchor_global < dev0_blocks)
            die("volume geometry is inconsistent: the global last block %llu "
                "falls inside dev0 (%llu blocks)", (unsigned long long)anchor_global,
                (unsigned long long)dev0_blocks);
        anchor_fd = fd1;
        anchor_local = anchor_global - dev0_blocks;
    }

    read_at(anchor_fd, anchor_local * STAMP_BLOCK, ancb, sizeof ancb,
            (anchor_fd == fd1 ? dev1_path : dev0_path));
    memcpy(&anc, ancb, sizeof anc);
    if (memcmp(anc.magic, INVFS_ANCHOR_MAGIC, 4) != 0)
        die("no ANC0 anchor at global block %llu (local %llu on %s) -- refusing "
            "to guess: the anchor mirrors the root descriptors and a wrong "
            "block here is live data", (unsigned long long)anchor_global,
            (unsigned long long)anchor_local, anchor_fd == fd1 ? dev1_path : dev0_path);
    if (anc.version != INVFS_ANC0_VERSION)
        die("anchor version %u, expected %u", anc.version, INVFS_ANC0_VERSION);
    if (anc.total_blocks != total || anc.block_size != INVFS_BLOCK_SIZE)
        die("anchor geometry does not match the superblock (anchor %llu/%u, "
            "superblock %llu/%u)", (unsigned long long)anc.total_blocks,
            anc.block_size, (unsigned long long)total, INVFS_BLOCK_SIZE);
    if (anc0_crc(&anc) != anc.crc32c)
        die("the anchor's stored CRC (0x%08x) cannot be reproduced from its own "
            "bytes (got 0x%08x): either the anchor is damaged or this tool's "
            "CRC32C is wrong. Nothing was written.", anc.crc32c, anc0_crc(&anc));

    /* ---- decide what needs changing ---------------------------------- */
    sb_needs    = (sb.format_version != (uint8_t)INVFS_FORMAT_VERSION);
    flags_needs = ((sb.vol_flags & STAMP_RETIRED_FLAGS) != 0);
    anc_needs   = (anc.format_version != (uint32_t)INVFS_FORMAT_VERSION);

    printf("%s\n", dev0_path);
    printf("  superblock format_version : %u -> %u\n", sb.format_version,
           sb_needs ? (unsigned)INVFS_FORMAT_VERSION : sb.format_version);
    printf("  vol_flags                 : 0x%08x -> 0x%08x%s\n", sb.vol_flags,
           sb.vol_flags & ~STAMP_RETIRED_FLAGS,
           flags_needs ? "  (clearing VOLF_META2|VOLF_ASTV2)" : "");
    printf("  anchor @ global %llu (local %llu on %s)\n",
           (unsigned long long)anchor_global, (unsigned long long)anchor_local,
           anchor_fd == fd1 ? (dev1_path ? dev1_path : "dev1") : dev0_path);
    printf("  anchor format_version     : %u -> %u\n", anc.format_version,
           anc_needs ? (unsigned)INVFS_FORMAT_VERSION : anc.format_version);
    printf("  anchor crc                : 0x%08x (verified against the block)\n",
           anc.crc32c);

    if (!sb_needs && !flags_needs && !anc_needs) {
        printf("nothing to do: already at generation %u\n", INVFS_FORMAT_VERSION);
        if (fd1 >= 0) close(fd1);
        close(fd0);
        return 0;
    }
    if (!apply) {
        printf("\nDRY RUN: nothing written. Re-run with --apply to write.\n");
        if (fd1 >= 0) close(fd1);
        close(fd0);
        return 0;
    }

    /* ---- back up, then write ----------------------------------------- */
    {
        /* Backups go beside the CWD, not /tmp: a live environment's /tmp is
         * usually tmpfs, so a copy there is gone at the very reboot this
         * migration is followed by. Run this from a persistent directory
         * (a USB stick, or the rescue shell's /root). */
        char dir[1024], tmpl[] = "./invf-stamp-backup.XXXXXX";
        if (!mkdtemp(tmpl))
            die("cannot create a backup directory here (%s) -- run from a "
                "persistent directory", strerror(errno));
        snprintf(dir, sizeof dir, "%s", tmpl);
        printf("\nbacking up:\n");
        backup(dir, "dev0-block0", fd0, 0, STAMP_BLOCK);
        if (fd1 >= 0) {
            uint8_t b1[STAMP_BLOCK];
            read_at(fd1, 0, b1, sizeof b1, dev1_path);
            if (memcmp(b1, blk0, sizeof b1) != 0)
                die("%s block 0 is not the mirror of %s block 0 -- the devices "
                    "disagree, refusing to stamp a split image",
                    dev1_path, dev0_path);
            backup(dir, "dev1-block0", fd1, 0, STAMP_BLOCK);
        }
        backup(dir, "anchor", anchor_fd, anchor_local * STAMP_BLOCK, STAMP_BLOCK);
        printf("restore with: dd if=<backup> of=<dev> bs=%zu count=1 seek=<block>\n",
               STAMP_BLOCK);
    }

    /* Superblock: format_version and the retired flag bits. Both live OUTSIDE
     * the checksum span (0..offsetof(checksum)), so the CRC does not move --
     * but recompute it anyway so a future widening of the span cannot make
     * this silently wrong. */
    memcpy(&sb, blk0, sizeof sb);
    sb.format_version = (uint8_t)INVFS_FORMAT_VERSION;
    sb.vol_flags &= ~STAMP_RETIRED_FLAGS;
    sb.checksum = invfs_crc32c(&sb, offsetof(invfs_superblock, checksum));
    memcpy(blk0, &sb, sizeof sb);
    write_at(fd0, 0, blk0, sizeof blk0, dev0_path);
    if (fd1 >= 0)
        write_at(fd1, 0, blk0, sizeof blk0, dev1_path);

    /* Anchor: the byte AND its CRC. */
    anc.format_version = (uint32_t)INVFS_FORMAT_VERSION;
    anc.crc32c = anc0_crc(&anc);
    memcpy(ancb, &anc, sizeof anc);
    write_at(anchor_fd, anchor_local * STAMP_BLOCK, ancb, sizeof ancb,
             anchor_fd == fd1 ? dev1_path : dev0_path);

    /* ---- verify by re-reading --------------------------------------- */
    {
        uint8_t v0[STAMP_BLOCK], va[STAMP_BLOCK];
        invfs_superblock vs;
        invfs_anc0 va_anc;
        read_at(fd0, 0, v0, sizeof v0, dev0_path);
        memcpy(&vs, v0, sizeof vs);
        if (vs.format_version != (uint8_t)INVFS_FORMAT_VERSION)
            die("verify failed: %s format_version is %u after the write",
                dev0_path, vs.format_version);
        if (vs.vol_flags & STAMP_RETIRED_FLAGS)
            die("verify failed: %s still carries retired flag bits (0x%08x)",
                dev0_path, vs.vol_flags);
        if (invfs_crc32c(v0, offsetof(invfs_superblock, checksum)) != vs.checksum)
            die("verify failed: %s superblock CRC after the write", dev0_path);
        read_at(anchor_fd, anchor_local * STAMP_BLOCK, va, sizeof va,
                anchor_fd == fd1 ? dev1_path : dev0_path);
        memcpy(&va_anc, va, sizeof va_anc);
        if (va_anc.format_version != (uint32_t)INVFS_FORMAT_VERSION)
            die("verify failed: anchor format_version is %u after the write",
                va_anc.format_version);
        if (anc0_crc(&va_anc) != va_anc.crc32c)
            die("verify failed: anchor CRC after the write");
        printf("\nOK: %s is now generation %u (vol_flags=0x%08x), anchor verified.\n",
               dev0_path, vs.format_version, vs.vol_flags);
        if (fd1 >= 0) {
            uint8_t v1[STAMP_BLOCK];
            invfs_superblock vs1;
            read_at(fd1, 0, v1, sizeof v1, dev1_path);
            memcpy(&vs1, v1, sizeof vs1);
            if (vs1.format_version != (uint8_t)INVFS_FORMAT_VERSION ||
                memcmp(v1, v0, sizeof v1) != 0)
                die("verify failed: %s block 0 is not the mirror of %s after "
                    "the write", dev1_path, dev0_path);
            printf("OK: %s mirror verified (0x%08x flags).\n", dev1_path, vs1.vol_flags);
        }
    }

    if (fd1 >= 0) close(fd1);
    close(fd0);
    return 0;
}
