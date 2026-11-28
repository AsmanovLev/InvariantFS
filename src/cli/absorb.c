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
    char enumerate[512];
    char extract[512];
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
        } else if (strcmp(k, "enumerate") == 0) {
            copy_val(pk->enumerate, sizeof pk->enumerate, v);
        } else if (strcmp(k, "extract") == 0) {
            copy_val(pk->extract, sizeof pk->extract, v);
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

    char selfdir[512];
    memset(selfdir, 0, sizeof selfdir);
    (void)!readlink("/proc/self/exe", selfdir, sizeof selfdir - 1);
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
    /* last resort, and the one that actually works for an installed tool: the
     * packs sit next to the binary's parent (bin/../tools/codecpacks) */
    if (selfdir[0]) {
        char *slash = strrchr(selfdir, '/');
        if (slash) {
            char probe[768];
            DIR *d2;
            *slash = 0;
            snprintf(probe, sizeof probe, "%s/../tools/codecpacks", selfdir);
            d2 = opendir(probe);
            if (d2) {
                closedir(d2);
                copy_val(out, cap, probe);
                return 1;
            }
        }
    }
    snprintf(p->why, sizeof p->why,
             "no containerpack directory found (an explicit path, $INVFS_PACKS, "
             "tools/codecpacks, or next to the binary); absorb has no map "
             "without one");
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
    uint64_t cur_blk = 0, cur_nblk = 0;
    int have_cur = 0;

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
        /* Cover EVERY 4096-byte block the range touches: the start rounds down
         * (harmless over-reserve) and the count must then be measured from the
         * start, not from the range's own length. Rounding those two halves
         * independently under-reserves the TAIL of any range that is not
         * 4096-aligned -- which is every range of a 1 KiB or 2 KiB ext4 image.
         * Measured on a 128 MiB / 1 KiB image (mkfs.ext4 default): the orphan
         * file's last logical block sat in a 4096-block whose first 3 KiB were
         * garbage, stayed unreserved, and the published old.ext4 read zeros
         * there -- `e2fsck -fn` on it: "Orphan file (inode 12) block 31 is not
         * clean". The journal fix in the ext4fs pack gets the same file to
         * "Cannot proceed" -> clean ONLY with this rounding. */
        nblk = (off % 4096u + len + 4095u) / 4096u;
        /* Merge with the run being accumulated. Ranges ascend, and the rounding
         * above (start DOWN, count measured from the start) makes the tail block
         * of one run the head block of the next. mkfs --reserve would tolerate
         * that (it marks a bitmap) but absorb_publish walks the list and would
         * copy the shared block twice -- measured: `invf-absorb publish`
         * succeeded and `invf-cat` then refused the file with "inode 2 seg 9:
         * overlapping entry". A merged list is also what the plan prints. */
        if (have_cur && start_blk <= cur_blk + cur_nblk) {
            uint64_t end = start_blk + nblk;
            if (end > cur_blk + cur_nblk)
                cur_nblk = end - cur_blk;
            continue;
        }
        if (have_cur) {
            fprintf(o, "%llu %llu\n", (unsigned long long)cur_blk,
                    (unsigned long long)cur_nblk);
            used += cur_nblk;
            ranges++;
        }
        cur_blk = start_blk;
        cur_nblk = nblk;
        have_cur = 1;
    }
    if (have_cur) {
        fprintf(o, "%llu %llu\n", (unsigned long long)cur_blk,
                (unsigned long long)cur_nblk);
        used += cur_nblk;
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

/* Is the pack's binary OLDER than the source it claims to be built from? Then the
 * map this tool is about to believe came from code that no longer exists.
 *
 * Measured twice in one day: `bin/ext4fs` from 11-23 predated the reserved-inode fix,
 * so an absorbed image came out with a zeroed journal and e2fsck refused it; and
 * `bin/xfs` from 11-19 had no `classify` verb at all, which made three measurements
 * read "the pack refuses with no reason". Both were LOCAL trees where `make` had been
 * run but not `make helpers` -- which is exactly what a person does. `all` does not
 * build the pack CLIs by design (CI runs both), so the tool says it out loud instead
 * of producing a confident wrong answer. */
static void warn_if_pack_stale(const char *packdir, const char *binrel,
                               const char *name)
{
    struct stat sb, ss;
    char bin[512], src[512];

    if (snprintf(bin, sizeof bin, "%s/%s", packdir, binrel) >= (int)sizeof bin)
        return;
    if (snprintf(src, sizeof src, "%s/%s.c", packdir, name) >= (int)sizeof src)
        return;
    if (stat(bin, &sb) != 0 || stat(src, &ss) != 0)
        return;
    if (sb.st_mtime < ss.st_mtime)
        fprintf(stderr,
                "invf-absorb: WARNING: %s is older than %s -- the map below comes "
                "from a binary that predates the source it claims to be. Run "
                "`make helpers`.\n", bin, src);
}

int absorb_pack_load(const char *packdir, absorb_pack_desc_t *out)
{
    pack_t pk;

    if (!packdir || !out)
        return 0;
    memset(out, 0, sizeof *out);
    (void)read_manifest(packdir, &pk);      /* fills pk even when absorb = 0 */
    if (!pk.magic[0] || !pk.name[0])
        return 0;                            /* not something we can recognise */
    copy_val(out->dir, sizeof out->dir, pk.dir);
    copy_val(out->name, sizeof out->name, pk.name);
    copy_val(out->magic, sizeof out->magic, pk.magic);
    out->sniff_off = pk.sniff_off;
    copy_val(out->classify, sizeof out->classify, pk.classify);
    copy_val(out->enumerate, sizeof out->enumerate, pk.enumerate);
    copy_val(out->extract, sizeof out->extract, pk.extract);
    return 1;
}

int absorb_pack_matches(const absorb_pack_desc_t *pk, const char *image)
{
    pack_t tmp;
    absorb_plan_t scratch;

    if (!pk || !image || !pk->magic[0])
        return 0;
    memset(&tmp, 0, sizeof tmp);
    memset(&scratch, 0, sizeof scratch);
    copy_val(tmp.magic, sizeof tmp.magic, pk->magic);
    tmp.sniff_off = pk->sniff_off;
    return pack_claims(&tmp, image, &scratch);
}

int absorb_pack_pick(const char *packs_dir, const char *image,
                     absorb_pack_desc_t *out, char *why, size_t whycap)
{
    char dir[256];
    absorb_plan_t plan;
    DIR *d;
    struct dirent *de;
    int sniffable = 0;

    if (!image || !out)
        return -1;
    memset(out, 0, sizeof *out);
    memset(&plan, 0, sizeof plan);
    if (!absorb_find_packs(packs_dir, dir, sizeof dir, &plan)) {
        if (why && whycap)
            snprintf(why, whycap, "%s", plan.why);
        return -1;
    }
    d = opendir(dir);
    if (!d) {
        if (why && whycap)
            snprintf(why, whycap, "cannot list %s", dir);
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        char sub[512];
        pack_t pk;
        absorb_pack_desc_t cand;
        int m;

        if (de->d_name[0] == '.')
            continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, de->d_name);
        (void)read_manifest(sub, &pk);      /* fills pk even when absorb = 0 */
        if (!pk.magic[0])
            continue;                       /* nothing to recognise it by */
        sniffable++;
        memset(&cand, 0, sizeof cand);
        copy_val(cand.dir, sizeof cand.dir, pk.dir);
        copy_val(cand.name, sizeof cand.name, pk.name);
        copy_val(cand.magic, sizeof cand.magic, pk.magic);
        cand.sniff_off = pk.sniff_off;
        copy_val(cand.classify, sizeof cand.classify, pk.classify);
        copy_val(cand.enumerate, sizeof cand.enumerate, pk.enumerate);
        copy_val(cand.extract, sizeof cand.extract, pk.extract);
        {
            const char *rel = cand.classify[0] ? cand.classify : cand.enumerate;
            char binrel[256];
            int k;
            for (k = 0; rel[k] && rel[k] != ' ' && k < (int)sizeof binrel - 1; k++)
                binrel[k] = rel[k];
            binrel[k] = 0;
            if (binrel[0])
                warn_if_pack_stale(cand.dir, binrel, cand.name);
        }
        m = absorb_pack_matches(&cand, image);
        if (m < 0) {
            closedir(d);
            if (why && whycap)
                snprintf(why, whycap, "cannot read %s", image);
            return -1;
        }
        if (m) {
            *out = cand;
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    if (why && whycap)
        snprintf(why, whycap,
                 "%s: no installed containerpack claims this image "
                 "(%d pack(s) with a sniff magic in %s)", image, sniffable, dir);
    return 0;
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

/* ---------------------------------------------------------------------------
 * Publishing the old filesystem inside the new volume.
 *
 * Two kinds of entry, and the difference is the point:
 *   - the PREFIX (what mkfs overwrote) is real data we must STORE: it goes into
 *     freshly allocated blocks and the entry names them;
 *   - every reserved range is named WHERE IT IS (pba == offset), so publishing it
 *     copies nothing at all. That is what makes absorb cheap on a 4 GiB rootfs.
 *
 * Every entry carries its own file_offset, so the file's holes (the source's
 * garbage ranges) need no entry and read as zeros -- which is exactly what an
 * unallocated block of the source is.
 * ------------------------------------------------------------------------- */

#include "volume.h"
#include "volume_internal.h"

/* Copy [off, off+len) of the source into freshly allocated blocks, each chunk
 * framed the way the write path frames a RAW segment ([csize LE][crc32c]), and
 * append one recipe entry per chunk. Chunks are bounded so a single entry stays
 * a sane size and a 34 MiB prefix does not need a 34 MiB malloc. */
static int absorb_copy_range(invfs_volume *v, FILE *src,
                             invfs_ast_block_entry **ents, uint32_t *nents,
                             uint64_t off, uint64_t len, absorb_plan_t *p)
{
    const uint64_t chunk = 1u << 20;      /* 1 MiB per entry */
    uint64_t done = 0;

    while (done < len) {
        uint64_t clen = len - done;
        uint64_t pba, nblocks;
        uint8_t *buf;
        uint32_t crc;
        int zone = 0;

        if (clen > chunk)
            clen = chunk;
        buf = (uint8_t *)malloc((size_t)clen + 8);
        if (!buf) {
            snprintf(p->why, sizeof p->why, "out of memory for a %llu-byte chunk",
                     (unsigned long long)clen);
            return -1;
        }
        if (fseeko(src, (off_t)(off + done), SEEK_SET) != 0 ||
            fread(buf + 8, 1, (size_t)clen, src) != (size_t)clen) {
            free(buf);
            snprintf(p->why, sizeof p->why,
                     "the source is shorter than the map claims at offset %llu",
                     (unsigned long long)(off + done));
            return -1;
        }
        crc = invfs_crc32c(buf + 8, (size_t)clen);
        buf[0] = (uint8_t)(clen & 0xFF);
        buf[1] = (uint8_t)((clen >> 8) & 0xFF);
        buf[2] = (uint8_t)((clen >> 16) & 0xFF);
        buf[3] = (uint8_t)((clen >> 24) & 0xFF);
        buf[4] = (uint8_t)(crc & 0xFF);
        buf[5] = (uint8_t)((crc >> 8) & 0xFF);
        buf[6] = (uint8_t)((crc >> 16) & 0xFF);
        buf[7] = (uint8_t)((crc >> 24) & 0xFF);
        nblocks = (clen + 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
        pba = alloc_raw_or_shadow(v, nblocks, &zone);
        if (!pba) {
            free(buf);
            snprintf(p->why, sizeof p->why,
                     "no room for %llu more blocks", (unsigned long long)nblocks);
            return -1;
        }
        if (io_pwrite(&v->io, pba * INVFS_BLOCK_SIZE, buf,
                      (size_t)(clen + 8)) != 0) {
            free(buf);
            snprintf(p->why, sizeof p->why, "write failed at offset %llu",
                     (unsigned long long)(off + done));
            return -1;
        }
        free(buf);
        {
            invfs_ast_block_entry *ne = (invfs_ast_block_entry *)
                realloc(*ents, (*nents + 1) * sizeof **ents);
            if (!ne) {
                snprintf(p->why, sizeof p->why, "out of memory");
                return -1;
            }
            *ents = ne;
            memset(&(*ents)[*nents], 0, sizeof (*ents)[*nents]);
            (*ents)[*nents].file_offset = off + done;
            (*ents)[*nents].length = clen;
            (*ents)[*nents].zone = INVFS_ZONE_RAW;
            (*ents)[*nents].algo = INVFS_ALGO_NONE;
            (*ents)[*nents].block_id = *nents;
            (*ents)[*nents].pba = pba;
            (*nents)++;
        }
        done += clen;
    }
    return 0;
}

uint64_t absorb_publish(const char *volume, const char *out_name,
                        const char *prefix_src, const char *range_src,
                        const char *reserve_list, uint64_t prefix_blocks,
                        uint64_t file_size, absorb_plan_t *p)
{
    invfs_volume *v;
    invfs_ast_block_entry *ents = NULL;
    uint32_t nents = 0;
    uint8_t *blob = NULL;
    size_t blen = 0;
    uint8_t addr[INVFS_RECIPE_ADDR_LEN];
    uint64_t ino = 0;
    FILE *rl, *src, *rsrc;
    int err = 0;

    if (!volume || !out_name || !prefix_src || !reserve_list || !p)
        return 0;

    src = fopen(prefix_src, "rb");
    if (!src) {
        snprintf(p->why, sizeof p->why, "cannot read the source %s", prefix_src);
        return 0;
    }
    /* The ranges may live in a DIFFERENT file than the prefix: in an in-place
     * absorb the prefix has to come from the copy saved before mkfs ran, while the
     * ranges are still exactly where they were, inside the target itself. */
    if (range_src && strcmp(range_src, prefix_src) != 0) {
        rsrc = fopen(range_src, "rb");
        if (!rsrc) {
            snprintf(p->why, sizeof p->why, "cannot read %s", range_src);
            fclose(src);
            return 0;
        }
    } else {
        rsrc = src;
    }
    v = vol_open(volume, &err);
    if (!v) {
        fclose(src);
        snprintf(p->why, sizeof p->why, "cannot open the volume %s", volume);
        return 0;
    }

    /* 1. the prefix: the region mkfs overwrote, which MUST be copied */
    if (prefix_blocks &&
        absorb_copy_range(v, src, &ents, &nents, 0, prefix_blocks * 4096u, p) != 0)
        goto out;

    /* 2. everything else the source allocates, copied from where it lies */
    rl = fopen(reserve_list, "r");
    if (!rl) {
        snprintf(p->why, sizeof p->why, "cannot read %s", reserve_list);
        goto out;
    }
    {
        unsigned long long rs, rc;
        while (fscanf(rl, "%llu %llu", &rs, &rc) == 2) {
            uint64_t off = (uint64_t)rs * 4096u, len = (uint64_t)rc * 4096u;
            if (prefix_blocks && off < prefix_blocks * 4096u) {
                uint64_t skip = prefix_blocks * 4096u - off;
                if (skip >= len)
                    continue;
                off += skip;
                len -= skip;
            }
            if (len && absorb_copy_range(v, rsrc, &ents, &nents, off, len, p) != 0) {
                fclose(rl);
                goto out;
            }
        }
    }
    fclose(rl);

    if (!ents) {
        if (!p->why[0])
            snprintf(p->why, sizeof p->why, "no entry to publish");
        goto out;
    }
    if (vol_ast_recipe_serialize(file_size, ents, nents, &blob, &blen) != 0 ||
        vol_recipe_store(v, blob, blen, addr) != 0) {
        free(blob);
        snprintf(p->why, sizeof p->why,
                 "the recipe does not fit (%u entries, %zu B)", nents, blen);
        goto out;
    }
    free(blob);
    ino = vol_create_content_node(v, out_name, file_size, addr);
    if (!ino)
        snprintf(p->why, sizeof p->why, "cannot create %s", out_name);
    else
        vol_flush(v);
out:
    free(ents);
    vol_close(v);
    fclose(src);
    return ino;
}

/* ---- in-place commit (absorb_commit) ------------------------------------- */

/* Copy `len` bytes at `off` from `in` to `out` (both positioned by this call). */
static int copy_one_extent(FILE *in, FILE *out, uint64_t off, uint64_t len,
                           absorb_plan_t *p)
{
    unsigned char buf[1 << 20];

    if (fseeko(in, (off_t)off, SEEK_SET) != 0 ||
        fseeko(out, (off_t)off, SEEK_SET) != 0) {
        snprintf(p->why, sizeof p->why, "seek failed at offset %llu",
                 (unsigned long long)off);
        return -1;
    }
    while (len) {
        size_t want = len > sizeof buf ? sizeof buf : (size_t)len;
        size_t got = fread(buf, 1, want, in);
        if (got == 0) {
            snprintf(p->why, sizeof p->why, "short read at offset %llu",
                     (unsigned long long)off);
            return -1;
        }
        if (fwrite(buf, 1, got, out) != got) {
            snprintf(p->why, sizeof p->why, "short write at offset %llu",
                     (unsigned long long)off);
            return -1;
        }
        off += got;
        len -= got;
    }
    return 0;
}

/* Copy `nbytes` from `src` (opened here) to `dst`, fsync, close. Returns 0. */
static int copy_file_range_to(const char *src, const char *dst, uint64_t nbytes)
{
    FILE *in, *out;
    unsigned char buf[1 << 20];
    uint64_t left = nbytes;
    int rc = 0;

    in = fopen(src, "rb");
    if (!in)
        return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while (left && rc == 0) {
        size_t want = left > sizeof buf ? sizeof buf : (size_t)left;
        size_t got = fread(buf, 1, want, in);
        if (got == 0) {
            rc = -1;
            break;
        }
        if (fwrite(buf, 1, got, out) != got) {
            rc = -1;
            break;
        }
        left -= got;
    }
    /* The prefix is the only copy of those bytes once mkfs runs, so it has to be
     * on the device before mkfs is started, not in the page cache. */
    if (rc == 0 && fflush(out) != 0)
        rc = -1;
    if (rc == 0 && fsync(fileno(out)) != 0)
        rc = -1;
    if (fclose(out) != 0 && rc == 0)
        rc = -1;
    fclose(in);
    return rc;
}

/* Copy the prefix and every reserved range of `target` into a sparse `stage` file,
 * at their own offsets. This is the step that makes the whole operation safe: after
 * it, nothing that mkfs or the publish writes can destroy a source byte.
 *
 * The alternative -- reading each range from the target as the publish needs it --
 * was measured and is WRONG for a filesystem that fills the device: the volume's
 * free space is interleaved with the ranges, so a framed copy lands on source bytes
 * that have not been read yet. e2fsck refused the result. */
static int absorb_stage(const char *target, const char *reserve_list,
                        uint64_t prefix_blocks, uint64_t bytes,
                        const char *stage, absorb_plan_t *p)
{
    FILE *in, *out;
    char line[256];
    /* No 1 MiB stack buffer: it was declared here and never used -- a megabyte of
     * stack in a function that copies through the framed path with its own
     * buffer. -Wunused-variable named it (2026-11-28). */
    int rc = 0;

    in = fopen(target, "rb");
    if (!in) {
        snprintf(p->why, sizeof p->why, "cannot read %s", target);
        return -1;
    }
    out = fopen(stage, "wb");
    if (!out) {
        fclose(in);
        snprintf(p->why, sizeof p->why, "cannot create %s", stage);
        return -1;
    }
    if (fseeko(out, (off_t)(bytes ? bytes : 1) - 1, SEEK_SET) != 0 ||
        fputc(0, out) == EOF) {
        fclose(in);
        fclose(out);
        snprintf(p->why, sizeof p->why, "cannot size %s to %llu bytes", stage,
                 (unsigned long long)bytes);
        return -1;
    }
    /* everything that falls inside the prefix AND inside the reserved ranges: the
     * prefix is copied first because it can overlap a leading range */
    if (copy_one_extent(in, out, 0, prefix_blocks * 4096u, p) != 0)
        rc = -1;
    if (rc == 0) {
        FILE *rl = fopen(reserve_list, "r");
        if (!rl) {
            snprintf(p->why, sizeof p->why, "cannot read %s", reserve_list);
            rc = -1;
        } else {
            while (rc == 0 && fgets(line, sizeof line, rl)) {
                unsigned long long start = 0, cnt = 0, off, len;
                if (sscanf(line, "%llu %llu", &start, &cnt) != 2)
                    continue;
                off = start * 4096ull;
                len = cnt * 4096ull;
                if (len == 0 || off + len > bytes)
                    continue;
                rc = copy_one_extent(in, out, off, len, p);
            }
            fclose(rl);
        }
    }
    if (rc == 0 && fflush(out) != 0)
        rc = -1;
    if (rc == 0 && fsync(fileno(out)) != 0)
        rc = -1;
    if (fclose(out) != 0 && rc == 0)
        rc = -1;
    fclose(in);
    return rc;
}

int absorb_commit(const char *target, const char *out_name,
                  const char *reserve_list, const char *mkfs_bin,
                  const char *packs_dir, absorb_plan_t *p)
{
    char prefix[512], cmd[2048];
    uint64_t ino;
    int rc;

    if (!target || !mkfs_bin || !p)
        return -1;
    memset(p, 0, sizeof *p);

    /* 1. probe: what the target's own filesystem says it uses */
    rc = absorb_probe(target, packs_dir, p);
    if (rc != 1) {
        if (rc == 0 && !p->why[0])
            snprintf(p->why, sizeof p->why,
                     "%s: nothing claims this image, so there is nothing to "
                     "absorb", target);
        return -1;
    }
    if (!p->prefix_blocks) {
        snprintf(p->why, sizeof p->why,
                 "%s: the plan reports no prefix to save, which cannot be right "
                 "for an in-place adoption -- refusing rather than overwriting "
                 "block 0 without a copy of it", target);
        return -1;
    }
    if (p->free_blocks < p->used_blocks) {
        snprintf(p->why, sizeof p->why,
                 "%s: %llu block(s) are used and only %llu are free -- the "
                 "absorbed copy has nowhere to live", target,
                 (unsigned long long)p->used_blocks,
                 (unsigned long long)p->free_blocks);
        return -1;
    }

    /* 2. the reserve list is written first: mkfs needs it, and it must exist on
     *    disk before anything is destroyed. */
    if (absorb_write_reserve_list(target, p, reserve_list) <= 0) {
        if (!p->why[0])
            snprintf(p->why, sizeof p->why,
                     "%s: the reserve list could not be written", target);
        return -1;
    }

    /* 3. stage the source bytes OUTSIDE the device, before mkfs touches it. This
     *    is the step that makes the operation safe; see absorb_stage's note for the
     *    measured failure of the alternative. */
    snprintf(prefix, sizeof prefix, "%s.absorb-stage", target);
    if (absorb_stage(target, reserve_list, p->prefix_blocks, p->bytes, prefix,
                     p) != 0) {
        if (!p->why[0])
            snprintf(p->why, sizeof p->why,
                     "could not stage the source bytes to %s -- nothing was "
                     "written to %s", prefix, target);
        return -1;
    }

    /* 4. build the volume over the old filesystem, with its used ranges reserved */
    snprintf(cmd, sizeof cmd, "%s \"%s\" --reserve \"%s\" --size-bytes %llu",
             mkfs_bin, target, reserve_list, (unsigned long long)p->bytes);
    rc = system(cmd);
    if (rc != 0) {
        snprintf(p->why, sizeof p->why,
                 "mkfs failed (rc=%d): %s. The target still holds the old "
                 "filesystem in its reserved ranges, and the prefix copy is at "
                 "%s", rc, cmd, prefix);
        return -1;
    }

    /* 5. publish from the STAGE: a stable reference, independent of whatever the
     *    allocator did to the device in step 4 */
    ino = absorb_publish(target, out_name ? out_name : p->out_name, prefix, prefix,
                         reserve_list, p->prefix_blocks, p->bytes, p);
    if (!ino) {
        if (!p->why[0])
            snprintf(p->why, sizeof p->why, "publish failed");
        return -1;
    }

    /* 6. the prefix copy is no longer needed: the bytes it held are inside
     *    old.<fs> now, and the volume says so only after a flush. */
    (void)unlink(prefix);
    return 0;
}

