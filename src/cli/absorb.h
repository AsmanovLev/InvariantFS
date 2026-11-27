/*
 * absorb.h — WP-ABSORB: adopt an existing filesystem in place, pack-driven.
 *
 * `invf-mkfs --absorb <image|device>` turns the target into an InvFS volume that
 * KEEPS the old filesystem as a file inside it (`old.ext4` at the root), exact
 * on the blocks that filesystem allocates. It is a niche operation, so it lives
 * in its own module rather than inside mkfs.
 *
 * NOTHING here knows what ext4 or xfs is. The pack declares it:
 *
 *     absorb       = 1                      this pack's image is a filesystem
 *     absorb_name  = old.ext4               optional, default old.img
 *     absorb_check = bin/e2fsck -n {in}     optional, a second opinion
 *     classify     = bin/ext4fs classify {in} {out}    the used-byte map (GCFG)
 *     sniff.magic / sniff.offset                        already how the engine
 *                                                       picks a pack
 *
 * and the core does four filesystem-agnostic things: detect (sniff), ask for the
 * map (classify), reserve what the map does not call garbage, and verify by
 * reading the result back and comparing bytes. A pack that cannot classify
 * refuses with its own reason -- which is a named refusal here, never an empty
 * map (an empty map reserves nothing and lets the new volume eat the old
 * filesystem).
 *
 * The order is the design:
 *   1. probe  — sniff + the map; refuses, writes nothing;
 *   2. plan   — the reserved ranges, the space they leave, the prefix mkfs will
 *               overwrite; dry by construction;
 *   3. commit — (next stage) prefix saved durably, mkfs with those ranges
 *               RESERVED, `old.<fs>` published, verified, then the prefix
 *               dropped. A crash before verification leaves the source
 *               recoverable: the reserved blocks were never handed out.
 */
#ifndef INVFS_ABSORB_H
#define INVFS_ABSORB_H

#include <stdint.h>
#include <stddef.h>

#define ABSORB_NAME_MAX 64

typedef struct {
    /* probe */
    int      found;                            /* 1 = a filesystem pack claimed it */
    char     pack[256];                        /* pack directory that claimed it */
    char     pack_name[64];                    /* the pack's `name` */
    char     out_name[ABSORB_NAME_MAX];        /* `absorb_name`, default old.img */
    char     check_cmd[512];                   /* `absorb_check`, may be empty */
    uint64_t bytes;                            /* target size */
    /* plan */
    uint64_t used_blocks;                      /* 4096-byte blocks not garbage */
    uint64_t used_extents;                     /* ranges in the reserve list */
    uint64_t free_blocks;                      /* what the new volume may use */
    uint64_t prefix_blocks;                    /* mkfs overwrites this much */
    char     why[256];                         /* refusal reason, "" when fine */
} absorb_plan_t;

/* One containerpack that declares itself a filesystem, as the converter needs
 * it: the sniff magic to recognise an image by, and the manifest's own command
 * lines to ask for the map and the members. NOTHING here names ext4 or xfs --
 * that was the whole point of moving it out of invf-convert. */
typedef struct {
    char     dir[256];                 /* the pack directory */
    char     name[64];                 /* manifest `name` */
    char     magic[64];                /* `sniff.magic`, hex, no 0x */
    unsigned long long sniff_off;      /* `sniff.offset`, default 0 */
    char     classify[512];            /* `classify`  template */
    char     enumerate[512];           /* `enumerate` template */
    char     extract[512];             /* `extract`   template */
} absorb_pack_desc_t;

/* 1 = this pack's sniff magic is where the manifest says it is, 0 = not,
 * -1 = the image could not be read. */
int absorb_pack_matches(const absorb_pack_desc_t *pk, const char *image);

/* Read ONE pack directory's manifest into *out. Returns 1 when it parsed, 0 when
 * the directory has no readable manifest. Used when the caller pinned a pack and
 * therefore must be told about THAT pack, not about whichever one would be picked. */
int absorb_pack_load(const char *packdir, absorb_pack_desc_t *out);

/* Pick the installed pack that claims `image`. `packs_dir` may be NULL for the
 * repository default. Returns 1 with *out filled, 0 when nothing claims the
 * image (why filled by name), -1 on error. */
int absorb_pack_pick(const char *packs_dir, const char *image,
                     absorb_pack_desc_t *out, char *why, size_t whycap);

/* Fill `pack_dir` from an explicit path, $INVFS_PACKS, or the repo's
 * tools/codecpacks. Returns 1 = found, 0 = not (why filled). */
int absorb_find_packs(const char *explicit_dir, char *out, size_t cap,
                      absorb_plan_t *p);

/* 1 = a pack claims this image (and the map was produced),
 * 0 = refused (why filled), -1 = could not read the target.
 * Writes only to the pack's own output file; never to the target. */
int absorb_probe(const char *path, const char *packs_dir, absorb_plan_t *p);

/* The reserve list, one "<start_block> <count>" line per range, in the format
 * `invf-mkfs --reserve` reads. Returns the number of ranges, 0 = refused,
 * -1 = error. */
int absorb_write_reserve_list(const char *path, absorb_plan_t *p,
                              const char *out_path);

/* Publish the old filesystem inside an ALREADY BUILT volume as `out_name`.
 *
 * The volume must have been made with those ranges RESERVED (`mkfs --reserve`),
 * so nothing it did could have touched them. The content is assembled from two
 * places and nothing is copied twice:
 *   - the PREFIX (`prefix_blocks`) mkfs overwrote: read from `prefix_src`,
 *     stored once into freshly allocated blocks;
 *   - every reserved range: named WHERE IT ALREADY IS (pba == device offset), so
 *     this publishes a file over bytes that never moved.
 *
 * Returns the inode id, or 0 with `why` filled. The caller verifies by reading
 * the file back; this function does not claim success on its own. */
uint64_t absorb_publish(const char *volume, const char *out_name,
                        const char *prefix_src, const char *reserve_list,
                        uint64_t prefix_blocks, uint64_t file_size,
                        absorb_plan_t *p);

#endif /* INVFS_ABSORB_H */
