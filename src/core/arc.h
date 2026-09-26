/*
 * arc.h — Adaptive Replacement Cache for reconstructed file contents.
 *
 * Portable C99, no platform dependencies, no internal locking: the volume
 * layer is caller-serialized (Dokan holds g_lock, FUSE holds g_io_lock around
 * every vol_* call) and this follows the same discipline.
 *
 * WHY A CACHE IS LOAD-BEARING HERE, NOT AN OPTIMISATION
 *
 * A transcoded file is not stored as decodable segments -- it is a blob plus a
 * sibling recipe, and the only thing that can turn it back into the original
 * is a whole-file reconstruction. A mount reads through vol_read_range, one
 * 64 KB callback at a time, so serving a window means reconstructing the file
 * and throwing all but 64 KB of it away. For a container that is quadratic;
 * for FLAC/PNG it means shelling out to MAC.exe or djxl per callback. The
 * cache is what makes ranged reads of transcoded files viable at all.
 *
 * WHY ARC RATHER THAN LRU
 *
 * The two access patterns here are hostile to each other. A media player or a
 * build tool re-reads the same few files constantly (frequency). A backup
 * sweep or a directory checksum walks every file once (recency, and pure
 * pollution). Plain LRU lets the one-shot walk evict the working set on every
 * pass. ARC (Megiddo & Modha, USENIX FAST '03) keeps both: T1 holds things
 * seen once, T2 things seen twice or more, and the ghost lists B1/B2 record
 * what was evicted so the split between them can be re-tuned from the misses
 * it actually suffers. A scan lands in T1 and evicts from T1.
 *
 * WHY STALE BYTES CANNOT BE SERVED
 *
 * The key is the inode id, and inode ids are never reused: they come from a
 * monotonic v->next_inode_id, no path updates a record's data in place, and
 * every overwrite goes through delete+create and gets a fresh id. So a cached
 * entry can only ever be the content of the inode it was decoded from.
 * arc_invalidate exists to give the budget back on delete, not for safety.
 */
#ifndef INVFS_ARC_H
#define INVFS_ARC_H

#include <stdint.h>
#include <stddef.h>

typedef struct invfs_arc invfs_arc;

typedef struct {
    uint64_t hits;        /* arc_get found the content */
    uint64_t misses;      /* arc_get did not */
    uint64_t ghost_hits;  /* miss on something we had evicted (B1/B2) */
    uint64_t inserts;
    uint64_t evictions;   /* entries demoted to ghost, data freed */
    uint64_t refused;     /* too large to cache without thrashing */
    uint64_t invalidated;
    size_t   bytes;       /* live data held now (T1+T2) */
    size_t   budget;
    size_t   t1_bytes, t2_bytes;
    size_t   p;           /* current adaptive target for T1, in bytes */
    uint32_t entries;     /* live entries now */
    uint32_t ghosts;      /* ghost entries now (B1+B2) */
} invfs_arc_stats;

/* budget in bytes; 0 returns NULL (cache disabled -- callers treat NULL as
   "no cache" rather than branching on a flag) */
invfs_arc *arc_create(size_t budget_bytes);
void       arc_destroy(invfs_arc *a);

/* On a hit returns 1 and points *data / *len at the cached content. The pointer
   is BORROWED and stays valid until the next arc_put/arc_invalidate/arc_clear/
   arc_destroy on this cache -- arc_get itself never frees anything, so copying
   out of it before the next mutation is enough. Returns 0 on a miss.
   NULL cache is a miss. */
int  arc_get(invfs_arc *a, uint64_t key, const uint8_t **data, size_t *len);

/* Insert. TAKES OWNERSHIP of `data` (must be malloc'd) on success, and frees
   it if the entry is refused for size -- so the caller never has to know
   which happened. NULL cache frees `data` and returns. */
void arc_put(invfs_arc *a, uint64_t key, uint8_t *data, size_t len);

void arc_invalidate(invfs_arc *a, uint64_t key);
void arc_clear(invfs_arc *a);
void arc_stats(const invfs_arc *a, invfs_arc_stats *out);

#endif /* INVFS_ARC_H */
