/*
 * invf-absorb — the tool half of WP-ABSORB (see src/cli/absorb.h).
 *
 *   invf-absorb plan    <image> <reserve.list>
 *       Ask the installed packs which filesystem the image is, get their
 *       used-byte map, and write it as the `<start> <count>` list that
 *       `invf-mkfs --reserve` reads. Writes nothing to the image.
 *
 *   invf-absorb publish <volume> <name> <reference> <reserve.list> <prefix_blocks>
 *       Inside an ALREADY BUILT volume (built with that reserve list), publish
 *       the old filesystem as `name`: the prefix comes from <reference> and is
 *       stored once, every reserved range is named where it already lies.
 *
 *   invf-absorb verify  <volume> <name> <reference>
 *       Read `name` back through the engine and compare it with <reference>,
 *       byte for byte. This is the acceptance that matters: a publish that is
 *       not read back and compared has not been proved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* readlink: resolving a sibling tool by /proc/self/exe */
#include <string.h>

#include "absorb.h"
#include "volume.h"

static uint64_t file_size_of(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint64_t n = 0;
    if (!f)
        return 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long v = ftell(f);
        if (v > 0)
            n = (uint64_t)v;
    }
    fclose(f);
    return n;
}

static int cmd_plan(const char *image, const char *list)
{
    absorb_plan_t p;
    int rc;
    memset(&p, 0, sizeof p);
    rc = absorb_probe(image, NULL, &p);
    if (rc <= 0) {
        fprintf(stderr, "invf-absorb plan: refused: %s\n",
                p.why[0] ? p.why : "the image could not be probed");
        return 1;
    }
    rc = absorb_write_reserve_list(image, &p, list);
    if (rc <= 0) {
        fprintf(stderr, "invf-absorb plan: refused: %s\n",
                p.why[0] ? p.why : "no reserve map");
        return 1;
    }
    printf("[absorb] %s claims %s\n", p.pack_name, image);
    printf("  old image      : %s (%llu B)\n", p.out_name,
           (unsigned long long)p.bytes);
    printf("  reserve        : %llu range(s), %llu block(s) of 4096 B\n",
           (unsigned long long)p.used_extents,
           (unsigned long long)p.used_blocks);
    printf("  for the volume : %llu block(s) of garbage\n",
           (unsigned long long)p.free_blocks);
    printf("  list written   : %s\n", list);
    if (p.check_cmd[0])
        printf("  second opinion : %s\n", p.check_cmd);
    return 0;
}

static int cmd_publish(const char *volume, const char *name, const char *ref,
                       const char *list, unsigned long long prefix_blocks)
{
    absorb_plan_t p;
    uint64_t ino;
    memset(&p, 0, sizeof p);
    ino = absorb_publish(volume, name, ref, NULL, list, prefix_blocks,
                         file_size_of(ref), &p);
    if (!ino) {
        fprintf(stderr, "invf-absorb publish: %s\n",
                p.why[0] ? p.why : "publish failed");
        return 1;
    }
    printf("[absorb] published %s as inode %llu inside %s\n", name,
           (unsigned long long)ino, volume);
    return 0;
}

/* Where our sibling tools live: bin/ next to this binary. Resolving it here keeps
 * absorb.c free of any knowledge about installation layout. */
static int sibling(const char *name, char *out, size_t cap)
{
    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash;
    if (n <= 0)
        return -1;
    exe[n] = 0;
    slash = strrchr(exe, '/');
    if (!slash)
        return -1;
    *slash = 0;
    return snprintf(out, cap, "%s/%s", exe, name) < (int)cap ? 0 : -1;
}

/* invf-absorb commit <image|device> <reserve.list> [--out-name N] [--packs DIR]
 *
 * The whole in-place adoption. It refuses unless every step it needs has a
 * precondition it can check first: a pack that claims the image, a prefix to save,
 * enough free space for the copy, and a durable prefix copy before mkfs touches
 * block 0. */
static int cmd_commit(int argc, char **argv)
{
    const char *target = argv[2], *list = argv[3];
    const char *out_name = NULL, *packs = NULL, *mkfs = NULL;
    char mkfsbuf[512];
    absorb_plan_t p;
    int i, rc;

    for (i = 4; i < argc; i++) {
        if (strncmp(argv[i], "--out-name=", 11) == 0) out_name = argv[i] + 11;
        else if (strncmp(argv[i], "--packs=", 8) == 0) packs = argv[i] + 8;
        else if (strncmp(argv[i], "--mkfs=", 7) == 0) mkfs = argv[i] + 7;
        else { fprintf(stderr, "invf-absorb commit: unknown option %s\n", argv[i]); return 2; }
    }
    if (!mkfs) {
        if (sibling("invf-mkfs", mkfsbuf, sizeof mkfsbuf) != 0) {
            fprintf(stderr, "invf-absorb commit: cannot find invf-mkfs beside this "
                            "binary; pass --mkfs=<path>\n");
            return 2;
        }
        mkfs = mkfsbuf;
    }
    memset(&p, 0, sizeof p);
    printf("[absorb] adopting %s in place\n", target);
    rc = absorb_commit(target, out_name, list, mkfs, packs, &p);
    if (rc != 0) {
        fprintf(stderr, "[absorb] REFUSED/FAILED: %s\n",
                p.why[0] ? p.why : "commit failed");
        return 1;
    }
    printf("[absorb] %s now holds %s; the source is still complete inside it\n",
           target, out_name ? out_name : (p.out_name[0] ? p.out_name : "old.img"));
    printf("[absorb] next: read it back and let the filesystem's own checker "
           "confirm, e.g.\n  invf-cat %s %s > old.img && e2fsck -fn old.img\n",
           target, out_name ? out_name : (p.out_name[0] ? p.out_name : "old.img"));
    return 0;
}

static int cmd_verify(const char *volume, const char *name, const char *ref)
{
    invfs_volume *v;
    invfs_inode in;
    uint64_t id, total, off;
    FILE *rf;
    uint8_t *buf, *rb;
    int err = 0;
    size_t chunk = 1u << 20;

    v = vol_open(volume, &err);
    if (!v) {
        fprintf(stderr, "invf-absorb verify: cannot open %s (err %d)\n",
                volume, err);
        return 1;
    }
    /* vol_find_rc, not vol_find: "no such name" and "the lookup could not be"
     * are the same 0 to vol_find, and a VERIFIER must tell them apart -- one
     * says the publish lost the name, the other says the volume cannot be read. */
    {
        int lrc = vol_find_rc(v, name, &id);
        if (lrc != 1) {
            fprintf(stderr, "invf-absorb verify: %s: %s\n", name,
                    lrc == 0 ? "not in the volume" : "unreachable (lookup failed)");
            vol_close(v);
            return 1;
        }
    }
    if (vol_inode_get(v, id, &in) != 1) {
        fprintf(stderr, "invf-absorb verify: %s is not in the volume\n", name);
        vol_close(v);
        return 1;
    }
    total = in.size;
    if (total != file_size_of(ref)) {
        fprintf(stderr, "invf-absorb verify: size %llu vs reference %llu\n",
                (unsigned long long)total,
                (unsigned long long)file_size_of(ref));
        vol_close(v);
        return 1;
    }
    rf = fopen(ref, "rb");
    buf = (uint8_t *)malloc(chunk);
    rb = (uint8_t *)malloc(chunk);
    if (!rf || !buf || !rb) {
        if (rf) fclose(rf);
        free(buf);
        free(rb);
        vol_close(v);
        return 1;
    }
    for (off = 0; off < total; off += chunk) {
        size_t want = (size_t)(total - off < chunk ? total - off : chunk);
        uint64_t got = (uint64_t)vol_read_range(v, id, off, want, buf);
        if (got != want || fread(rb, 1, want, rf) != want) {
            fprintf(stderr, "invf-absorb verify: short read at offset %llu\n",
                    (unsigned long long)off);
            err = 1;
            break;
        }
        if (memcmp(buf, rb, want) != 0) {
            size_t k;
            for (k = 0; k < want; k++)
                if (buf[k] != rb[k]) {
                    fprintf(stderr, "invf-absorb verify: byte %llu differs "
                            "(volume %02x, reference %02x)\n",
                            (unsigned long long)(off + k), buf[k], rb[k]);
                    break;
                }
            err = 1;
            break;
        }
    }
    free(buf);
    free(rb);
    fclose(rf);
    vol_close(v);
    if (!err)
        printf("[absorb] verified: %s is byte-identical to %s (%llu B read back)\n",
               name, ref, (unsigned long long)total);
    return err;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: invf-absorb plan    <image> <reserve.list>\n"
                "       invf-absorb publish <volume> <name> <reference> "
                "<reserve.list> <prefix_blocks>\n"
                "       invf-absorb verify  <volume> <name> <reference>\n"
                "       invf-absorb commit  <image|device> <reserve.list> "
                "[--out-name=N] [--packs=DIR] [--mkfs=PATH]\n"
                "\ncommit is the in-place adoption: it saves the prefix mkfs will\n"
                "overwrite, builds the volume with the used ranges RESERVED, and\n"
                "publishes the old filesystem inside it. It refuses before writing\n"
                "anything when a precondition does not hold.\n");
        return 2;
    }
    if (strcmp(argv[1], "plan") == 0 && argc == 4)
        return cmd_plan(argv[2], argv[3]);
    if (strcmp(argv[1], "publish") == 0 && argc == 7)
        return cmd_publish(argv[2], argv[3], argv[4], argv[5],
                           strtoull(argv[6], NULL, 0));
    if (strcmp(argv[1], "verify") == 0 && argc == 5)
        return cmd_verify(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "commit") == 0 && argc >= 4)
        return cmd_commit(argc, argv);
    fprintf(stderr, "invf-absorb: bad arguments\n");
    return 2;
}
