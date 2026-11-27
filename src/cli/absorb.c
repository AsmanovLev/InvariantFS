/*
 * absorb.c — the pack-driven probe/plan for WP-ABSORB (see absorb.h).
 *
 * Four filesystem-agnostic steps, in this order: find a pack that claims the
 * image (its manifest's sniff.magic/sniff.offset), ask that pack for its byte
 * map (`classify` -> GCFG), keep everything the map does not call garbage, and
 * write that as the reserve list `invf-mkfs --reserve` reads.
 *
 * The commit stage is deliberately absent. An absorb that overwrites block 0
 * before a verified `old.<fs>` exists is worse than no absorb at all, so until
 * that stage lands `mkfs --absorb` plans and refuses to commit -- out loud.
 */
#include "absorb.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blkio.h"

#define MANIFEST_MAX 64

typedef struct {
    char dir[256];
    char name[64];
    char classify[512];
    char absorb_name[ABSORB_NAME_MAX];
    char check[512];
    char magic[64];
    unsigned long long sniff_off;
    int absorb;
} pack_t;

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return s;
}

static void copy_val(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* One manifest. Returns 1 when the pack declares itself a filesystem. */
static int read_manifest(const char *dir, pack_t *pk)
{
    char path[512], line[1024];
    FILE *f;
    int absorb = 0;

    memset(pk, 0, sizeof *pk);
    copy_val(pk->dir, sizeof pk->dir, dir);
    snprintf(path, sizeof path, "%s/manifest", dir);
    f = fopen(path, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *eq, *k, *v;
        if (*s == '#')
            continue;
        eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        k = trim(s);
        v = trim(eq + 1);
        if (strcmp(k, "absorb") == 0) {
            absorb = (v[0] == '1' || v[0] == 'y' || v[0] == 't');
        } else if (strcmp(k, "name") == 0) {
            copy_val(pk->name, sizeof pk->name, v);
        } else if (strcmp(k, "classify") == 0) {
            copy_val(pk->classify, sizeof pk->classify, v);
        } else if (strcmp(k, "absorb_name") == 0) {
            copy_val(pk->absorb_name, sizeof pk->absorb_name, v);
        } else if (strcmp(k, "absorb_check") == 0) {
            copy_val(pk->check, sizeof pk->check, v);
        } else if (strcmp(k, "sniff.magic") == 0) {
            copy_val(pk->magic, sizeof pk->magic, v);
        } else if (strcmp(k, "sniff.offset") == 0) {
            pk->sniff_off = strtoull(v, NULL, 0);
        }
    }
    fclose(f);
    pk->absorb = absorb;
    if (absorb && !pk->absorb_name[0])
        snprintf(pk->absorb_name, sizeof pk->absorb_name, "old.img");
    return absorb;
}

int absorb_find_packs(const char *explicit_dir, char *out, size_t cap,
                      absorb_plan_t *p)
{
    const char *cand[4];
    int i;

    cand[0] = explicit_dir;
    cand[1] = getenv("INVFS_PACKS");
    cand[2] = "tools/codecpacks";
    cand[3] = "../tools/codecpacks";
    for (i = 0; i < 4; i++) {
        DIR *d;
        if (!cand[i] || !*cand[i])
            continue;
        d = opendir(cand[i]);
        if (!d)
            continue;
        closedir(d);
        copy_val(out, cap, cand[i]);
        return 1;
    }
    snprintf(p->why, sizeof p->why,
             "no containerpack directory found (an explicit path, $INVFS_PACKS, "
             "or tools/codecpacks); absorb has no map without one");
    return 0;
}

/* Read the sniff magic at the pack's offset and compare. 1 = claims it. */
static int pack_claims(const pack_t *pk, const char *path, absorb_plan_t *p)
{
    blkio io;
    unsigned char want[32], got[32];
    size_t n, i;
    int rc;

    n = strlen(pk->magic) / 2;
    if (n == 0 || n > sizeof want)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(pk->magic + 2 * i, "%2x", &v) != 1)
            return 0;
        want[i] = (unsigned char)v;
    }
    rc = blkio_open(&io, path, 0);
    if (rc != 0) {
        snprintf(p->why, sizeof p->why, "cannot open %s: %s", path,
                 blkio_strerror(rc));
        return -1;
    }
    if (blkio_seek(&io, pk->sniff_off) != 0 ||
        blkio_read(&io, got, n) != 0) {
        blkio_close(&io);
        return 0;
    }
    blkio_close(&io);
    return memcmp(want, got, n) == 0;
}

/* Little-endian GCFG: "GCFG", u32 version, u32 n_entries, u32 block_size,
 * then n x 24 B {u64 off, u64 len, u8 cls, u8 flags, u16 rsvd}. */
static int write_reserve(const char *gcfg, const char *out_path,
                         absorb_plan_t *p)
{
    FILE *f, *o;
    unsigned char hdr[16], ent[24];
    unsigned n, i;
    uint64_t used = 0, ranges = 0;

    f = fopen(gcfg, "rb");
    if (!f) {
        snprintf(p->why, sizeof p->why, "classify wrote no table at %s", gcfg);
        return 0;
    }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr ||
        memcmp(hdr, "GCFG", 4) != 0) {
        fclose(f);
        snprintf(p->why, sizeof p->why, "the classify table has no GCFG magic");
        return 0;
    }
    n = (unsigned)hdr[8] | ((unsigned)hdr[9] << 8) |
        ((unsigned)hdr[10] << 16) | ((unsigned)hdr[11] << 24);
    o = fopen(out_path, "w");
    if (!o) {
        fclose(f);
        snprintf(p->why, sizeof p->why, "cannot write %s", out_path);
        return 0;
    }
    for (i = 0; i < n; i++) {
        uint64_t off, len, start_blk, nblk;
        unsigned cls;
        if (fread(ent, 1, sizeof ent, f) != sizeof ent)
            break;
        off = (uint64_t)ent[0] | ((uint64_t)ent[1] << 8) |
              ((uint64_t)ent[2] << 16) | ((uint64_t)ent[3] << 24) |
              ((uint64_t)ent[4] << 32) | ((uint64_t)ent[5] << 40) |
              ((uint64_t)ent[6] << 48) | ((uint64_t)ent[7] << 56);
        len = (uint64_t)ent[8] | ((uint64_t)ent[9] << 8) |
              ((uint64_t)ent[10] << 16) | ((uint64_t)ent[11] << 24) |
              ((uint64_t)ent[12] << 32) | ((uint64_t)ent[13] << 40) |
              ((uint64_t)ent[14] << 48) | ((uint64_t)ent[15] << 56);
        cls = ent[16];
        if (len == 0)
            continue;
        if (cls == 1u)       /* garbage: the only class the source does not need */
            continue;
        start_blk = off / 4096u;
        nblk = (len + 4095u) / 4096u;
        fprintf(o, "%llu %llu\n", (unsigned long long)start_blk,
                (unsigned long long)nblk);
        used += nblk;
        ranges++;
    }
    fclose(f);
    fclose(o);
    if (ranges == 0) {
        snprintf(p->why, sizeof p->why,
                 "the classified map reserved nothing, which cannot be right for "
                 "an image with a superblock: refusing");
        return 0;
    }
    p->used_blocks = used;
    p->used_extents = ranges;
    p->free_blocks = p->bytes / 4096u > used ? p->bytes / 4096u - used : 0;
    return 1;
}

int absorb_probe(const char *path, const char *packs_dir, absorb_plan_t *p)
{
    char dir[256];
    DIR *d;
    struct dirent *de;
    int claimed = 0;

    if (!path || !p)
        return -1;
    memset(p, 0, sizeof *p);
    if (!absorb_find_packs(packs_dir, dir, sizeof dir, p))
        return 0;

    d = opendir(dir);
    if (!d) {
        snprintf(p->why, sizeof p->why, "cannot list %s", dir);
        return -1;
    }
    while (!claimed && (de = readdir(d)) != NULL) {
        char sub[512];
        pack_t pk;
        if (de->d_name[0] == '.')
            continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, de->d_name);
        if (!read_manifest(sub, &pk) || !pk.absorb)
            continue;
        {
            blkio io;
            int rc = blkio_open(&io, path, 0);
            if (rc != 0) {
                closedir(d);
                snprintf(p->why, sizeof p->why, "cannot open %s: %s", path,
                         blkio_strerror(rc));
                return -1;
            }
            p->bytes = blkio_capacity(&io);
            blkio_close(&io);
        }
        {
            int c = pack_claims(&pk, path, p);
            if (c < 0) {
                closedir(d);
                return -1;
            }
            if (c) {
                copy_val(p->pack, sizeof p->pack, pk.dir);
                copy_val(p->pack_name, sizeof p->pack_name, pk.name);
                copy_val(p->out_name, sizeof p->out_name, pk.absorb_name);
                copy_val(p->check_cmd, sizeof p->check_cmd, pk.check);
                claimed = 1;
                /* keep the classify template for the caller */
                copy_val(p->why, sizeof p->why, "");
                /* the template travels in a static so the reserve step can run
                 * it without re-reading every manifest */
                {
                    extern char absorb__tpl[512];
                    copy_val(absorb__tpl, 512, pk.classify);
                }
            }
        }
    }
    closedir(d);
    if (!claimed) {
        snprintf(p->why, sizeof p->why,
                 "no installed containerpack declares `absorb = 1` AND claims "
                 "this image by its sniff magic; refusing rather than guessing "
                 "what filesystem it is");
        return 0;
    }
    p->found = 1;
    p->prefix_blocks = 256 + (p->bytes / 4096u / 8u) / 4096u + 64;
    return 1;
}

char absorb__tpl[512];

int absorb_write_reserve_list(const char *path, absorb_plan_t *p,
                              const char *out_path)
{
    char gcfg[512], cmd[2048], rendered[1024];
    const char *t = absorb__tpl;
    size_t o = 0;

    if (!path || !p || !out_path || !p->found)
        return 0;
    if (!absorb__tpl[0]) {
        snprintf(p->why, sizeof p->why,
                 "pack %s declares no classify command, so there is no "
                 "used-byte map to reserve", p->pack_name);
        return 0;
    }
    snprintf(gcfg, sizeof gcfg, "%s.gcfg", out_path);
    while (*t && o + 1 < sizeof rendered) {
        if (strncmp(t, "{in}", 4) == 0) {
            size_t n = strlen(path);
            if (o + n >= sizeof rendered) break;
            memcpy(rendered + o, path, n); o += n; t += 4;
        } else if (strncmp(t, "{out}", 5) == 0) {
            size_t n = strlen(gcfg);
            if (o + n >= sizeof rendered) break;
            memcpy(rendered + o, gcfg, n); o += n; t += 5;
        } else {
            rendered[o++] = *t++;
        }
    }
    rendered[o] = 0;
    snprintf(cmd, sizeof cmd, "%s/%s", p->pack, rendered);
    if (system(cmd) != 0) {
        snprintf(p->why, sizeof p->why,
                 "pack %s refused to classify this image (its own reason is on "
                 "stderr); reserving a map this build did not verify is how the "
                 "old filesystem's blocks get eaten", p->pack_name);
        return 0;
    }
    if (!write_reserve(gcfg, out_path, p)) {
        remove(gcfg);
        return 0;
    }
    remove(gcfg);
    return (int)p->used_extents;
}
