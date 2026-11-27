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

#endif /* INVFS_ABSORB_H */
