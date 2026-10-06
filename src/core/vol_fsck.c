/* vol_fsck.c — fsck/repair: rebuild L2P + used-bitmap from the inode
 * area. Split from volume.c. */

#include "volume_internal.h"
#include "vol_metabuf.h"
#include "vol_btree.h"
#include "vol_delta.h"
#include "vol_reclaim.h"
#include "vol_spt0.h"
#include "vol_anchor.h"


/* The batch accumulator and the real vol_tz_flush/vol_tz_gc live with the
 * rest of the WP10 write path, right after the storage-class helpers (they
 * need meta_rewrite/vol_stamp_class/vol_delete_inode). */

/* WP-M4: the v3 validation path (RT30 + base-tree walk). Defined after the
 * v2 helpers; vol_fsck_scan dispatches to it for VOLF_META volumes. WP86 added
 * the containment walk and the -f repair behind the same entry point. */
static int fsck_scan(invfs_volume *v, invfs_fsck_report *rep, int fix,
                        int discard_reachable);

/* ================= fsck / repair =================
 * WP27 note on physical addressing: AST entries store the segment's pba
 * (24B -> 32B); the physical extent derives from the segment's framed
 * header (seg_extent). The L2P journal is the owner-scoped WAL only. So a
 * destroyed WAL is fully rebuildable from the owner records, and the
 * bitmap is a rebuildable cache: fsck recomputes it from
 * (metadata zone + live records' extents + live owner WAL), complete.
 * What fsck does:
 *   - verify inode-record CRCs and record lengths (bad_recs)
 *   - verify that every live AST entry carries a valid pba (l2p_miss --
 *     the field name kept for the report; under v2 it means "entry with
 *     an invalid pba or an underivable extent")
 *   - WP22d: fold the live set to the consistent cut (a record with an
 *     invalid-pba entry is hidden; the name falls back to its newest
 *     valid version) and, with -f, QUARANTINE the broken versions
 *     (position-kill tombstones, newest first) so the cut is permanent
 *   - rebuild the used-bitmap from (metadata zone + live records' extents
 *     + live owner WAL): orphans (allocated, unreferenced) are freed,
 *     missing (referenced, free in bitmap) are restored
 *   - rewrite the WAL from the live owner records (journal compact)
 *   - mark the superblock CLEAN
 * With fix=1 all fixes are applied and persisted. */

/* WP27: an entry's physical extent, by owner class. The reten registry's
 * length field carries BLOCKS (the ranges are raw blocks, no frame);
 * seal parity is one block; the tier/rawm copies carry the span in bytes;
 * everything else is a framed segment (the header read derives it). */





/* ---- live-record set ----------------------------------------------------
 * Liveness must mirror the consistent cut vol_open applies (the scan-set
 * in volume.c) EXACTLY, or fsck and the read path disagree about the same
 * volume -- the WP22c/F2 lesson. The set is built with per-name version
 * stacks: an INOD with an invalid-pba entry is BROKEN and never live (the
 * name falls back to its newest valid version); a position-kill DELT
 * removes its version unless that version's successor is broken (a torn
 * retire keeps its fallback). */

/* Append a quarantine tombstone (v2 position-kill) for one broken record.
 * The record stays in the append-only area but every future scan drops it
 * from the name's version stack, which is what makes the cut permanent.
 * WP70: route through vol_append_slot so mapper volumes update both
 * met0.active_offset and inode_area_pos -- a bare io_write at
 * inode_area_pos left met0.active_offset stale, and the next vol_open
 * reset inode_area_pos from met0.active_offset, trimming the active
 * extent before the tombstone and making the walk miss it. */


/* WP22d/H6 repair: a name with no fully-valid version is not lost whole
 * when the invalid entries form a SUFFIX of the newest record -- repair
 * by truncation to the longest valid prefix: append a new record
 * version with the tail entries dropped, then position-kill the old one.
 * The readable head survives and the volume reaches CLEAN (quarantine
 * would destroy data that is still fine). Returns 0 when truncated,
 * 1 when the damage is not a clean suffix (caller quarantines),
 * -1 on io/alloc failure. */


/* WP22d content-level cut (fsck -f + INVFS_FSCK_CONTENT=1): read every
 * non-TEXT segment of the record at rec_pos through the CRC'd path and
 * mirror the read path's per-algo shape checks. A segment whose bytes
 * were torn by a drop window AFTER its record landed fails here: the
 * pba-level cut cannot see it, the segment CRC + csize checks can.
 * Returns 1 when any segment fails. */



/* WP49: pass-1 body fed by the bounded, index-ordered namespace walk
 * (position-driven vol_inode_next can cycle on a non-monotonic mapper
 * table). bad_cb keeps fsck's torn/CRC-bad record accounting. */





int vol_fsck_scan(invfs_volume *v, invfs_fsck_report *rep, int fix)
{
    return vol_fsck_scan_ex(v, rep, fix, 0);
}

int vol_fsck_scan_ex(invfs_volume *v, invfs_fsck_report *rep, int fix,
                     int discard_reachable)
{
    /* WP-M4: the checker validates the RT30 root descriptor and walks the
     * base B+-tree. WP86 gave it a repair (the quarantined key ranges are
     * excised, the delta is folded back in, the loss is named).
     *
     * Known gap (NOT WP86, reported as a follow-up): the content cut (fix +
     * INVFS_FSCK_CONTENT=1) and the seal2 repair (--repair) ran on the v2
     * inode-record stream only, so a data segment whose bytes were torn
     * after its recipe landed cannot be quarantined here yet. Both of those
     * were deleted with that stream; the gap is now a gap in the v3
     * checker, not a branch that was never taken. */
    return fsck_scan(v, rep, fix, discard_reachable);
}

/* ================= WP-M4/WP86: metadata-v3 fsck ==========================
 * A v3 volume's namespace lives in the immutable base B+-tree anchored by the
 * RT30 root descriptor (design-meta-v3.md §7/§12/§16). This checker validates
 * the root double-slot and walks every reachable base page.
 *
 * A page whose blkptr.checksum/gen does not match its bytes is a hard read
 * error (mbuf_read_ptr), exactly like the read path -- it is not repairable
 * and the tree keeps no second copy of it. WP-M4 stopped the walk there, which
 * made one torn page fatal twice over: the report counted ONE bad page (the
 * pages behind it were never walked) and `fix` did nothing, so the volume
 * stayed broken forever.
 *
 * WP86 CONTAINS the damage instead:
 *
 *   report  btree_check_tolerant skips the unreadable page, records the key
 *           range it owned, and keeps verifying the rest of the tree, so the
 *           report names every bad page and every quarantined key range. A
 *           key inside a quarantined range reads EIO on the read path
 *           (mbuf_root_read / btree_search already refuse a bad page): it is
 *           never presented as absent, and never as data.
 *   -f      the quarantined ranges are EXCISED (btree_excise): the parent
 *           entry that points at the unreadable page is dropped, every other
 *           key keeps its page, and the delta is folded back in -- which
 *           restores every quarantined key the delta still holds. What is left
 *           -- keys that existed only in the damaged page -- is gone, and the
 *           pass says so with the exact key range, the key count it could
 *           not recover, and the NAMES it could attribute. The damage is
 *           never silenced: the pass that finds it always exits nonzero
 *           (the v2 -f contract), and a *structural* failure is refused
 *           outright rather than "repaired".
 *
 * What no repair can do: a torn ROOT page with no valid sibling slot leaves
 * the base tree unreachable. Only the delta is readable then, and rebuilding
 * the base from the delta alone would silently drop every key that was folded
 * before it -- so -f refuses, loudly, and says what is still readable. */
static void fsck_note(invfs_fsck_report *rep, const char *msg)
{
    rep->damaged = 1;
    fprintf(stderr, "fsck(v3): %s\n", msg);
}

/* The same sentence on stderr, WITHOUT the damage verdict. A condition an
 * operator should be able to see but must not be charged for. The split
 * between "reported" and "damaged" is the whole point: one predicate that
 * does both is how a healthy volume gets reported as broken, and an operator
 * told the filesystem is damaged reaches for interventions the volume never
 * needed. */
static void fsck_info(const char *msg)
{
    fprintf(stderr, "fsck(v3): %s\n", msg);
}

/* Validate the RT30 descriptor itself (magic/version/page_size/CRC). Returns
 * 0 = valid, 1 = absent (an empty base, the RDP0 convention),
 * 2 = WP86: present but torn -- damage, NOT an empty base; the caller must
 * not walk a tree it cannot name, -1 = io error. On success v->rt is
 * populated via mbuf_rt30_load. */
static int fsck_rt30(invfs_volume *v, invfs_fsck_report *rep)
{
    int rc = mbuf_rt30_load(v);
    if (rc < 0) {
        fsck_note(rep, "RT30 root descriptor unreadable (io error)");
        return -1;
    }
    if (rc == 1) {
        rep->rt30_bad = 1;
        fsck_note(rep, "RT30 root descriptor absent -- the base was "
                          "never written (empty)");
        return 1;
    }
    if (rc == 2) {
        rep->rt30_bad = 1;
        fsck_note(rep, "RT30 root descriptor is TORN (version/CRC) -- the "
                          "base tree it names is unreachable; only the delta "
                          "(recent writes) is readable");
        return 2;
    }
    rep->root_seq = v->rt.seq;
    if (v->rt.page_size != INVFS_BLOCK_SIZE) {
        char b[128];
        rep->rt30_bad = 1;
        snprintf(b, sizeof b, "RT30 page_size=%u is not the supported "
                 "%u-byte page (D3)",
                 (unsigned)v->rt.page_size, (unsigned)INVFS_BLOCK_SIZE);
        fsck_note(rep, b);
        return 1;
    }
    if (v->rt.delta_pba && v->rt.delta_pba >= v->sb.total_blocks) {
        fsck_note(rep, "RT30 delta_pba is out of range");
    }
    return 0;
}

/* Read a root slot page and check it against the RT30 slot value. A slot
 * naming a page that does not validate is "torn". Returns 0 = valid (fills
 * *gen_out), 1 = empty slot, 2 = torn, 3 = free (WP-D), -1 = io error.
 *
 * WP-D: 3 is its own code because a freed page is not torn and reporting it
 * as such is a lie an operator acts on. The bytes are perfect -- that is
 * the whole reason this is a separate finding -- so "bad page CRC/magic"
 * would send them to a media-recovery procedure for a volume whose real
 * fault is a reclaim whose liveness predicate was wrong. slots_torn
 * keeps meaning torn; the freed slot is counted as reachable_free,
 * whose name is the finding.
 *
 * TODO(WP-M4): RT30 stores only a pba per slot, not a blkptr, so the WP
 * doc's "blkptr.checksum does not match" cannot be checked literally; the
 * page's own self-CRC is the available check. A slot-level checksum/gen
 * would catch a slot repointed at a different-but-valid page, and belongs
 * with the root-publish format if the design wants it. */
static int fsck_slot_page(invfs_volume *v, uint64_t pba,
                             uint64_t *gen_out)
{
    uint8_t page[INVFS_BLOCK_SIZE];
    const invfs_page_hdr *h;
    if (!pba)
        return 1;
    if (pba >= v->sb.total_blocks)
        return 2;               /* out of range: torn/foreign slot */
    /* WP-D: allocation before integrity, so a freed slot can never be
     * adopted as the live root on the strength of the bytes it still
     * carries. (mbuf_root_read makes the same call on the read path.) */
    if (mbuf_page_allocated(v, pba) == 0)
        return 3;
    if (mbuf_read(v, pba, page) != 0)
        return -1;
    h = mbuf_page_chdr(page);
    if (!mbuf_page_validate(page))
        return 2;
    if (gen_out)
        *gen_out = h->gen;
    return 0;
}

/* Build a verified blkptr (pba + the page's own checksum/gen + root flags) for
 * a page that RT30 or SPT0 names by pba alone. 0 = ok, -1 = the page does not
 * read or does not validate. A blkptr whose checksum/gen are left zero does NOT
 * work here: mbuf_read_ptr compares them against the page header, so it would
 * reject every valid page. */
static int fsck_ptr_at(invfs_volume *v, uint64_t pba, invfs_blkptr *out)
{
    uint8_t page[INVFS_BLOCK_SIZE];

    memset(out, 0, sizeof *out);
    if (!pba || mbuf_read(v, pba, page) != 0)
        return -1;
    /* WP-D: same call as the slot path, for the same reason -- a page the
     * bitmap reports free is not a pointer fsck should hand to a tree
     * walk, a savepoint comparison, or -f's reachability diff. The subtree
     * walk behind those uses mbuf_read_ptr and is hardened already; this
     * is the root page itself, which is read directly. */
    if (mbuf_page_allocated(v, pba) == 0)
        return -1;
    if (!mbuf_page_validate(page))
        return -1;
    mbuf_ptr_set(out, pba, page,
                 mbuf_page_chdr(page)->level == INVFS_PAGE_LEVEL_LEAF
                 ? INVFS_BP_ROOT | INVFS_BP_LEAF
                 : INVFS_BP_ROOT | INVFS_BP_INTERNAL);
    return 0;
}

/* Select the winning root from the RT30 double slot. A slot is a candidate
 * only when its block is ALLOCATED (WP-D) and its page validates; among
 * candidates the higher header gen wins, tie -> the slot seq parity points
 * at (the most recently published), but ONLY when the tie is between two
 * DIFFERENT pages: two slots naming the same page are one root with two
 * names -- reported, not damage (see the branch below), because there is
 * nothing to choose between them. A non-empty slot whose page does not
 * validate is torn and reported; a slot whose block is free is reported
 * separately (the bytes are fine, the ownership is not). Returns 0 = ok
 * (possibly empty, *root_out.pba == 0), -1 = io error. */
static int fsck_root(invfs_volume *v, invfs_fsck_report *rep,
                        invfs_blkptr *root_out)
{
    uint64_t best_pba = 0, best_gen = 0;
    uint8_t page[INVFS_BLOCK_SIZE];
    int have = 0, i;
    memset(root_out, 0, sizeof *root_out);

    for (i = 0; i < 2; i++) {
        uint64_t pba = v->rt.root_slot[i];
        uint64_t gen = 0;
        int rc = fsck_slot_page(v, pba, &gen);
        if (rc == 1)
            continue;               /* empty slot */
        if (rc < 0)
            return -1;
        if (rc == 2) {
            char b[160];
            rep->slots_torn++;
            snprintf(b, sizeof b, "root_slot[%d] pba %llu is torn "
                     "(bad page CRC/magic) -- slot ignored",
                     i, (unsigned long long)pba);
            fsck_note(rep, b);
            continue;
        }
        if (rc == 3) {
            /* WP-D: the page is intact and the volume does not own it. A
             * reclaim with a wrong liveness predicate freed a block a live
             * slot still names -- this is the observable fingerprint of
             * that bug, and it is worth more than the torn count because
             * the bytes are perfect and nothing else reports it. */
            char b[192];
            rep->reachable_free++;
            snprintf(b, sizeof b, "root_slot[%d] pba %llu is FREE in the "
                     "allocation bitmap but its bytes still pass CRC -- the "
                     "block was reclaimed while a live root named it; slot "
                     "ignored", i, (unsigned long long)pba);
            fsck_note(rep, b);
            continue;
        }
        if (!have || gen > best_gen) {
            have = 1;
            best_pba = pba;
            best_gen = gen;
        } else if (gen == best_gen) {
            /* Equal gen is the AMBIGUOUS shape only when the two slots name
             * two DIFFERENT pages. When they name the SAME page there is no
             * ambiguity to resolve: it is one root with two slot names, and
             * there is no choice to make.
             *
             * That shape is reached by an ordinary shipped path, not by
             * damage. mbuf_root_publish writes slot `seq & 1` and then bumps
             * seq (vol_metabuf.c:479-481), so consecutive publishes alternate
             * slots; when a pass publishes NO new root, the rollback's
             * mbuf_root_publish (vol_spt0.c:1299) writes the still-current
             * root into the OTHER slot, and both slots then name one page at
             * one generation. The gen comes out of the page's own header
             * (fsck_slot_page above), so that state lands on
             * `gen == best_gen` by construction. Charging it as damage
             * reported a clean volume as DAMAGED (exit 3) -- the failure an
             * operator acts on, in the direction of doing too much.
             *
             * So: the same page is REPORTED (an operator who sees the two
             * slots line up deserves to be told why nothing was flagged) and
             * NOT charged. The two-different-pages shape below keeps the
             * flag exactly as it was -- that one is real (a torn or
             * ambiguous publish after a crash), and silencing it would trade
             * a false positive for silent corruption. */
            if (pba == best_pba) {
                char b[160];
                rep->slots_same_root++;
                snprintf(b, sizeof b, "both RT30 root slots name the SAME "
                          "root (pba %llu, gen %llu) -- one root, not an "
                          "ambiguous publish",
                          (unsigned long long)pba, (unsigned long long)gen);
                fsck_info(b);
            } else {
                rep->slots_ambiguous++;
                fsck_note(rep, "two DIFFERENT RT30 root pages valid at the "
                                  "same gen (ambiguous publish; seq parity "
                                  "used)");
                if ((uint32_t)i == (uint32_t)(v->rt.seq & 1u))
                    best_pba = pba;
            }
        }
    }
    if (!have)
        return 0;                   /* empty base: clean */
    /* Re-read the winning page to seed its blkptr checksum/flags. */
    if (mbuf_read(v, best_pba, page) != 0)
        return -1;
    mbuf_ptr_set(root_out, best_pba, page,
                 mbuf_page_chdr(page)->level == INVFS_PAGE_LEVEL_LEAF
                 ? INVFS_BP_ROOT | INVFS_BP_LEAF
                 : INVFS_BP_ROOT | INVFS_BP_INTERNAL);
    return 0;
}

/* Allocator cross-check: a reachable page must be marked allocated in the
 * metadata bitmap (the v2 "bitmap divergence" analogue). */
static void fsck_bitmap_check(invfs_volume *v, invfs_fsck_report *rep,
                                 uint64_t pba)
{
    if (v->bitmap && !bit_get(v->bitmap, pba)) {
        char b[128];
        rep->reachable_free++;
        snprintf(b, sizeof b, "reachable base page pba %llu is FREE in the "
                 "metadata bitmap", (unsigned long long)pba);
        fsck_note(rep, b);
    }
}

/* The repair's lost-name scan needs the volume handle in a callback. */
static invfs_volume *g_fsck_v;

/* WP86: how many quarantined key ranges hold a key the delta still covers.
 * Those are the ones the repair gets BACK (the fold re-applies them); every
 * other key in a quarantined range existed only in the unreadable page and is
 * gone. Counted before the fold, from the delta index. */
typedef struct {
    const bt_quarantine *q;
    uint64_t recovered;
} fsck_dq_ctx;

static int fsck_delta_in_q_cb(void *ctx_, const uint8_t *key, uint16_t klen,
                              const delta_ref *ref)
{
    fsck_dq_ctx *c = (fsck_dq_ctx *)ctx_;
    int i;
    (void)ref;
    for (i = 0; i < c->q->n; i++) {
        const bt_range *r = &c->q->range[i];
        int ge_lo = 1, lt_hi = 1;
        /* byte-lexicographic compare against the range bounds; keys are short,
         * so a memcmp on the common prefix plus a length tie-break is exact */
        uint16_t m;
        int cc;
        if (r->lo_n) {
            m = klen < r->lo_n ? klen : r->lo_n;
            cc = m ? memcmp(key, r->lo, m) : 0;
            if (cc < 0 || (cc == 0 && klen < r->lo_n))
                ge_lo = 0;
        }
        if (ge_lo && r->hi_n) {
            m = klen < r->hi_n ? klen : r->hi_n;
            cc = m ? memcmp(key, r->hi, m) : 0;
            if (cc > 0 || (cc == 0 && klen >= r->hi_n))
                lt_hi = 0;
        }
        if (ge_lo && lt_hi) {
            c->recovered++;
            return 0;
        }
    }
    return 0;
}

/* WP86: after the repair, name the damage. A dirent whose inode row is gone is
 * a file the volume can no longer show -- the only loss an operator can act on,
 * so it is counted and printed instead of leaving a silently shorter directory.
 * A quarantined dirent range means the names themselves are lost, which is
 * reported too (as unattributable). */
typedef struct {
    uint64_t lost;
    uint64_t shown;
} fsck_names_ctx;

static int fsck_lost_dirent_cb(void *ctx_, const char *name, size_t nlen,
                               uint64_t child)
{
    fsck_names_ctx *c = (fsck_names_ctx *)ctx_;
    invfs_inode in;
    int rc;

    if (!nlen || !child)
        return 0;
    if ((unsigned char)name[0] == 0x01)
        return 0;                       /* internal owner/registry entry */
    rc = vol_inode_get(g_fsck_v, child, &in);
    if (rc < 0)
        return 0;                       /* still unreadable: not "lost" yet */
    if (rc == 1)
        return 0;
    c->lost++;
    if (c->shown < 32)
        fprintf(stderr, "fsck(v3): LOST: %s (inode %llu: its base page is "
                        "unreadable; the data is not recoverable)\n",
                name, (unsigned long long)child);
    c->shown++;
    return 0;
}

static void fsck_lost_names(invfs_volume *v, invfs_fsck_report *rep)
{
    fsck_names_ctx c;
    int rc;

    memset(&c, 0, sizeof c);
    g_fsck_v = v;
    rc = vol_dirent_scan(v, INVFS_ROOT_INO, fsck_lost_dirent_cb, &c);
    if (rc != 0) {
        fprintf(stderr, "fsck(v3): UNATTRIBUTABLE LOSS: the directory entries "
                        "that named the lost files were themselves inside a "
                        "quarantined range, so the names cannot be listed. "
                        "Every key in the quarantined range is gone; restore "
                        "the volume from a backup if the names matter.\n");
        return;
    }
    rep->lost_names = c.lost;
    if (c.lost)
        fprintf(stderr, "fsck(v3): %llu name(s) lost with the quarantined "
                        "pages%s\n", (unsigned long long)c.lost,
                c.lost > c.shown ? " (list truncated)" : "");
    else
        fprintf(stderr, "fsck(v3): no name is left pointing at a lost inode "
                        "row (the entries that named them were inside the "
                        "quarantined range too)\n");
}

/* ================================================================== */
/* WP: excision liveness -- may this key RANGE be dropped at all?      */
/* ================================================================== */
/* The repair's unit of destruction is a QUARANTINED KEY RANGE, not a page.
 * The page walk learns that interval from the parent's separators and never
 * learns what was IN it, so btree_excise drops [lo, hi) blind. That is sound
 * only if nothing a live object still needs lives in the interval -- and the
 * check that would have established it did not exist, which is why a torn leaf
 * at the HIGH end of the keyspace could take both files' recipe blobs with it
 * while their inode rows and dirents (in readable pages) went on resolving:
 *
 *   - a recipe blob is stored under 0x04 || blake3_256(blob)[32] in the SAME
 *     tree, so nothing in the tree POINTS at it; the only thing in the volume
 *     that says it must exist is the 32-byte address in the inode row;
 *   - xattrs are keyed 0x03 || inode:u64 || name_len || name, again with the
 *     reference living in the row / the key, not in a walkable pointer;
 *   - and a key that is GONE leaves no trace in any page's own CRC, so every
 *     structural check the pass runs still passes afterwards.
 *
 * So the gate is a FORWARD reachability pass over everything that is still
 * readable, asking for each key such an object requires:
 *
 *   readable inode row  ->  0x04 || row.recipe_addr        (its content)
 *                           the 0x03 keys its xattrs occupy, resolved to the
 *                           actual names -- the per-inode SPACE only picks
 *                           which inodes are worth enumerating (see below)
 *   readable dirent     ->  the child inode's 8-byte row key
 *
 * The dirent direction is the same defect one level up: a readable name whose
 * ROW is inside the range is a file that loses its content and keeps its name.
 * It is what turns the WP86 phase-A volume -- whose torn leaf holds the rows of
 * 17 named files, their dirents all readable -- from a 17-file loss into a
 * refusal.
 *
 * Any such key inside a quarantined range REFUSES that range. The volume is
 * left exactly as it was -- damaged, nothing destroyed, every refused key
 * still on the page it was on -- because a key that reads EIO is recoverable
 * (restore the image, or the page) and a key that has been excised is gone.
 * That is the whole ordering rule: establish liveness FIRST, mutate second.
 *
 * What this cannot prove is as stated as what it can: a name inside a
 * quarantined dirent range is invisible from here, so a hardlink whose SECOND
 * name is in the range is not detected. --discard-reachable is the operator's
 * explicit, separately-worded decision to proceed past whatever the proof
 * cannot clear. */
#define FSCK_EXCISE_FAULT_MAX 32

typedef struct {
    uint64_t id;
    uint64_t size;
    uint32_t kind;               /* INVFS_EXCISE_BLOCK_* */
    char     name[256];
} fsck_excise_fault;

enum {
    INVFS_EXCISE_BLOCK_RECIPE = 0,  /* the 0x04 blob that holds its content */
    INVFS_EXCISE_BLOCK_XATTR  = 1,  /* the 0x03 keys that hold its xattrs   */
    INVFS_EXCISE_BLOCK_ROW    = 2   /* the row a surviving NAME resolves to */
};

typedef struct {
    const bt_quarantine *q;
    uint8_t  refused[BT_QUARANTINE_MAX];
    fsck_excise_fault fault[FSCK_EXCISE_FAULT_MAX];
    uint64_t nfault, nfault_total;
    uint64_t live;               /* live rows asked */
    uint64_t xa_unknown;         /* xattr enumerations that hit the damage */
} fsck_live_ctx;

/* What fsck_excise_live_cb's xattr enumeration callback needs to build a key
 * and name the offender it would destroy. */
typedef struct {
    fsck_live_ctx *c;
    uint64_t id;
    uint64_t size;
    const char *name;
} fsck_excise_live_cb_ctx;

static void fsck_be64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (56 - 8 * i));
}

/* Mark every quarantined range that holds the key `lo` (or, when `hi_n` is
 * set, any key of the space [lo,hi)), and record the first offender that
 * reaches each one. One offender per (inode, kind) is enough: the point of
 * the list is that the operator sees WHICH files, not how many pages. */
static void fsck_excise_mark(fsck_live_ctx *c, uint64_t id, uint32_t kind,
                             const char *name, uint64_t size,
                             const uint8_t *lo, uint16_t lo_n,
                             const uint8_t *hi, uint16_t hi_n)
{
    int j, hit = 0, known = 0;
    uint64_t i;

    for (j = 0; j < c->q->n; j++) {
        int overlaps = hi_n ? btree_quarantine_overlaps(c->q, lo, lo_n, hi, hi_n)
                            : btree_quarantine_has(c->q, lo, lo_n);
        if (!overlaps)
            continue;
        c->refused[j] = 1;
        hit = 1;
    }
    if (!hit)
        return;
    for (i = 0; i < c->nfault; i++)
        if (c->fault[i].id == id && c->fault[i].kind == kind)
            known = 1;
    if (!known && c->nfault < FSCK_EXCISE_FAULT_MAX) {
        fsck_excise_fault *f = &c->fault[c->nfault++];
        f->id = id;
        f->size = size;
        f->kind = kind;
        snprintf(f->name, sizeof f->name, "%s", name ? name : "");
    }
    c->nfault_total++;
}

/* One inode's xattrs are a contiguous key run: 0x03 || id || name_len ||
 * name, ordered by name_len then name (the WP-M7 note in invarifs.h), so the
 * whole space is [0x03||id||0x0000, 0x03||(id+1)||0x0000). That SPACE is a
 * filter, not a claim: an inode with no xattrs owns no key in it, and a
 * quarantined range that swallows the space must not block on that alone. It
 * is a filter because a rightmost torn leaf is quarantined as [lo, +inf) --
 * and every 0x03 key sorts above such a lo, so a space test alone would refuse
 * every range on every volume, repair included. So the space picks the inodes
 * worth looking at and the enumeration below decides. */
static int fsck_excise_xattr_cb(void *ctx_, const char *name, size_t nlen)
{
    fsck_excise_live_cb_ctx *c = (fsck_excise_live_cb_ctx *)ctx_;
    uint8_t k[11 + INVFS_MAX_NAME + 4];
    uint16_t kn;

    if (nlen == 0 || nlen > INVFS_MAX_NAME)
        return 0;
    kn = (uint16_t)(11 + nlen);
    k[0] = (uint8_t)INVFS_XATTR_KEY_PREFIX;
    fsck_be64(k + 1, c->id);
    k[9] = (uint8_t)(nlen >> 8);
    k[10] = (uint8_t)(nlen & 0xff);
    memcpy(k + 11, name, nlen);
    /* A value too big for one record continues at the same key with a 0x00
     * marker and a u16 BE chunk index (xattr_chunk_key), so this name's
     * records run from `k` to `k || 0x00 || 0xFF 0xFF` INCLUSIVE. An exclusive
     * upper bound that covers the last chunk index has to be one byte past
     * that, and nothing sorts at or above `... 0xFF 0xFF 0x00` that the format
     * can produce. */
    k[11 + nlen] = 0x00;
    k[12 + nlen] = 0xFF;
    k[13 + nlen] = 0xFF;
    k[14 + nlen] = 0x00;
    fsck_excise_mark(c->c, c->id, INVFS_EXCISE_BLOCK_XATTR, c->name, c->size,
                     k, kn, k + 11 + nlen, (uint16_t)(nlen + 4));
    return 0;
}

/* Every key a live inode's own row requires. A row that cannot be read
 * requires nothing: it is itself inside the damage, and no object left in the
 * volume still says its recipe must exist. */
static int fsck_excise_live_cb(invfs_volume *v, uint64_t id, const char *name,
                               void *ctx_)
{
    fsck_excise_live_cb_ctx *lc = (fsck_excise_live_cb_ctx *)ctx_;
    fsck_live_ctx *c = lc->c;
    static const uint8_t zero[INVFS_RECIPE_ADDR_LEN];
    invfs_inode in;
    uint8_t rk[1 + INVFS_RECIPE_ADDR_LEN];
    uint8_t rhi[1 + INVFS_RECIPE_ADDR_LEN + 3];
    uint8_t xlo[11], xhi[11];
    static char nbuf[300];

    if (vol_inode_get(v, id, &in) != 1)
        return 0;                       /* row unreadable/absent: no claim */
    c->live++;
    if (name && !name[0]) {
        snprintf(nbuf, sizeof nbuf, "path too long to show");
        name = nbuf;
    }

    if (in.type != INVFS_ITYP_DIR && in.size &&
        memcmp(in.recipe_addr, zero, INVFS_RECIPE_ADDR_LEN) != 0) {
        /* The manifest key 0x04 || addr, and -- for a recipe too big to be one
         * value -- its 0x04 || addr || 0x00 || idx:u16 continuation chunks
         * (recipe_chunk_key). Either way it is the file's content, and it
         * is addressed by nothing but the row that is being asked about. */
        rk[0] = (uint8_t)INVFS_RECIPE_KEY_PREFIX;
        memcpy(rk + 1, in.recipe_addr, INVFS_RECIPE_ADDR_LEN);
        memcpy(rhi, rk, sizeof rk);
        rhi[sizeof rk] = 0xFF;
        rhi[sizeof rk + 1] = 0xFF;
        rhi[sizeof rk + 2] = 0xFF;
        fsck_excise_mark(c, id, INVFS_EXCISE_BLOCK_RECIPE, name, in.size,
                         rk, (uint16_t)sizeof rk, rhi, (uint16_t)sizeof rhi);
    }
    xlo[0] = (uint8_t)INVFS_XATTR_KEY_PREFIX;
    fsck_be64(xlo + 1, id);
    xlo[9] = 0;
    xlo[10] = 0;
    xhi[0] = (uint8_t)INVFS_XATTR_KEY_PREFIX;
    fsck_be64(xhi + 1, id + 1);
    xhi[9] = 0;
    xhi[10] = 0;
    if (btree_quarantine_overlaps(c->q, xlo, (uint16_t)sizeof xlo,
                                  xhi, (uint16_t)sizeof xhi)) {
        lc->id = id;
        lc->size = in.size;
        lc->name = name;
        if (vol_xattr_scan(v, id, fsck_excise_xattr_cb, lc) != 0) {
            /* The enumeration could not complete -- the scan's own descent
             * reached the damage. That is not a reason to assume the inode has
             * no xattrs; it is exactly the case where nothing can be proven,
             * so the ranges overlapping its 0x03 key space stay refused. */
            c->xa_unknown++;
            fsck_excise_mark(c, id, INVFS_EXCISE_BLOCK_XATTR, name, in.size,
                             xlo, (uint16_t)sizeof xlo, xhi,
                             (uint16_t)sizeof xhi);
        }
    }
    return 0;
}

/* A name that still resolves requires the inode row it resolves to, and a
 * directory's own children are reached the same way -- so the scan descends.
 * The descent is a WORKLIST, not a recursive call inside the scan callback:
 * vol_dirent_scan is a btree_scan, and re-entering a scan from its own
 * callback is a shape nothing else in the tree walk does. The visited set is on
 * inode ids (a directory cannot be its own ancestor), and the worklist is
 * bounded, so the walk terminates whatever the volume says. */
#define FSCK_EXCISE_DIRS_MAX 1024
typedef struct {
    invfs_volume *v;
    fsck_live_ctx *c;
    uint64_t queue[FSCK_EXCISE_DIRS_MAX];
    int qn, qi;
    int rc;
} fsck_excise_dir_ctx;

static int fsck_excise_dir_cb(void *ctx_, const char *name, size_t nlen,
                              uint64_t child)
{
    fsck_excise_dir_ctx *d = (fsck_excise_dir_ctx *)ctx_;
    invfs_inode in;
    uint8_t k[8];
    int i;

    if (!nlen || !child)
        return 0;
    if ((unsigned char)name[0] == 0x01)
        return 0;                       /* internal owner/registry entry */
    fsck_be64(k, child);
    fsck_excise_mark(d->c, child, INVFS_EXCISE_BLOCK_ROW, name, 0,
                     k, 8, NULL, 0);
    /* a subdirectory names its children exactly as hard as the root does */
    if (d->qn >= FSCK_EXCISE_DIRS_MAX)
        return 0;
    if (vol_inode_get(d->v, child, &in) != 1 || in.type != INVFS_ITYP_DIR)
        return 0;
    for (i = 0; i < d->qn; i++)
        if (d->queue[i] == child)
            return 0;                   /* already queued: no second visit */
    d->queue[d->qn++] = child;
    return 0;
}

static void fsck_excise_dir_walk(fsck_excise_dir_ctx *d)
{
    if (d->rc)
        return;
    d->queue[0] = INVFS_ROOT_INO;
    d->qn = 1;
    d->qi = 0;
    while (d->qi < d->qn && !d->rc) {
        uint64_t dir = d->queue[d->qi++];
        if (vol_dirent_scan(d->v, dir, fsck_excise_dir_cb, d) != 0)
            d->rc = -1;
    }
    if (d->qn >= FSCK_EXCISE_DIRS_MAX)
        fprintf(stderr, "fsck(v3): excision liveness: more than %d "
                        "directories; the dirent walk stopped there\n",
                FSCK_EXCISE_DIRS_MAX);
}

/* Fill `safe` with the quarantined ranges the liveness pass cleared. Returns
 * 0 when at least one range is excisable, -1 when none is (the volume is then
 * left untouched). `discard_reachable` skips the gate entirely -- and says so
 * loudly, because it is the operator trading data for a mountable volume. */
static int fsck_excise_safety(invfs_volume *v, invfs_fsck_report *rep,
                                 const bt_quarantine *q, bt_quarantine *safe,
                                 int discard_reachable)
{
    fsck_live_ctx c;
    fsck_excise_live_cb_ctx lc;
    int i, refused = 0;

    memset(safe, 0, sizeof *safe);
    safe->bad_pages = q->bad_pages;
    if (!q->n)
        return 0;

    memset(&c, 0, sizeof c);
    memset(&lc, 0, sizeof lc);
    c.q = q;
    lc.c = &c;
    if (discard_reachable) {
        fprintf(stderr, "fsck(v3): --discard-reachable: excising every "
                        "quarantined key range even where a live inode still "
                        "needs a key inside it. Every key in those ranges is "
                        "about to be DESTROYED -- the files it belongs to lose "
                        "their content, and no later pass can bring it back.\n");
        for (i = 0; i < q->n; i++)
            safe->range[safe->n++] = q->range[i];
        return 0;
    }

    /* Pass 1: what every live inode row itself requires. */
    if (vol_iter_live_inodes(v, fsck_excise_live_cb, &lc) != 0)
        rep->excise_partial = 1;
    /* Pass 2: what every surviving name requires. */
    {
        fsck_excise_dir_ctx d;
        memset(&d, 0, sizeof d);
        d.v = v;
        d.c = &c;
        fsck_excise_dir_walk(&d);
        if (d.rc)
            rep->excise_partial = 1;
    }
    rep->excise_live = c.live;

    for (i = 0; i < q->n; i++) {
        if (c.refused[i])
            refused++;
        else
            safe->range[safe->n++] = q->range[i];
    }
    rep->excise_refused = (uint64_t)refused;
    rep->excise_blocked = c.nfault_total;
    if (!refused)
        return 0;

    for (i = 0; i < (int)c.nfault; i++) {
        const fsck_excise_fault *f = &c.fault[i];
        char b[512];
        if (f->kind == INVFS_EXCISE_BLOCK_RECIPE)
            snprintf(b, sizeof b,
                     "inode %llu (%s): its live row names a recipe blob at key "
                     "0x04 || BLAKE3(recipe), and that key is inside a "
                     "quarantined key range -- excising the range would destroy "
                     "the %llu byte(s) of content the file still has, while its "
                     "name and its row went on resolving",
                     (unsigned long long)f->id,
                     f->name[0] ? f->name : "no name resolves to it",
                     (unsigned long long)f->size);
        else if (f->kind == INVFS_EXCISE_BLOCK_XATTR)
            snprintf(b, sizeof b,
                     "inode %llu (%s): its live row has xattr records in the "
                     "0x03 || inode key range, and that range is inside a "
                     "quarantined key range -- excising it would drop extended "
                     "attributes the volume can still name but not read",
                     (unsigned long long)f->id,
                     f->name[0] ? f->name : "no name resolves to it");
        else
            snprintf(b, sizeof b,
                     "inode %llu (%s): a directory entry outside the damage "
                     "still resolves to it, so its inode row must exist -- and "
                     "that row key is inside a quarantined key range. Excising "
                     "the range would leave the name pointing at nothing",
                     (unsigned long long)f->id,
                     f->name[0] ? f->name : "no name resolves to it");
        fsck_note(rep, b);
    }
    if (c.nfault_total > c.nfault)
        fprintf(stderr, "fsck(v3): excision liveness: %llu blocked live "
                        "inode(s) (the list above is truncated at %d)\n",
                (unsigned long long)c.nfault_total, FSCK_EXCISE_FAULT_MAX);
    if (c.xa_unknown)
        fprintf(stderr, "fsck(v3): excision liveness: %llu inode(s) could not "
                        "have their xattr records enumerated (the scan's own "
                        "descent reached the damaged page), so the ranges "
                        "overlapping their 0x03 key space are not cleared\n",
                (unsigned long long)c.xa_unknown);
    fprintf(stderr, "fsck(v3): excision REFUSED for %d of %d quarantined key "
                    "range(s): a live inode still needs a key inside them, and "
                    "a range that still holds it cannot be dropped without "
                    "destroying content the volume can still read. Those keys "
                    "stay on the page they are on (they read EIO, which is "
                    "recoverable; an excised key is gone). Restore the image "
                    "or the page, or re-run with --discard-reachable to "
                    "excise anyway and lose those files' content.\n",
            refused, q->n);
    if (rep->excise_partial)
        fprintf(stderr, "fsck(v3): excision liveness is PARTIAL: a walk above "
                        "failed, so the ranges marked excisable are cleared by "
                        "a FLOOR, not a proof. Treat any future loss as "
                        "unexplained by this pass.\n");
    return safe->n ? 0 : -1;
}

/* WP86: the -f repair. Excise the quarantined ranges, publish the new root,
 * reclaim what the dropped subtrees held, and fold the delta back in so every
 * quarantined key the delta still covers comes back.
 *
 * Crash safety is the fold's, unchanged: nothing is published until the new
 * pages and the allocation bitmap are durable, so a crash before the publish
 * leaves the volume exactly as it was (damaged but not worse), and a crash
 * after it replays the delta against the new base idempotently. */
static int fsck_repair(invfs_volume *v, invfs_fsck_report *rep,
                          invfs_blkptr old_root, const bt_quarantine *q)
{
    invfs_blkptr new_root = old_root, nr;
    fsck_dq_ctx dq;
    int changed;

    /* A live save point pins pages this repair is about to free, and it pins a
     * root whose tree is damaged by definition. WP86: it is detected and
     * dropped here, never used (spt0_restore refuses such a save point too). */
    if (v->savepoint_live) {
        fprintf(stderr, "fsck(v3): dropping the save point (its base tree is "
                        "damaged and cannot be used as a rollback target)\n");
        if (spt0_drop(v) < 0) {
            fsck_note(rep, "could not drop the damaged save point");
            return -1;
        }
    }

    memset(&dq, 0, sizeof dq);
    dq.q = q;
    (void)vol_delta_iter(v, fsck_delta_in_q_cb, &dq);
    rep->keys_quarantined = dq.recovered;

    changed = btree_excise(v, old_root, q, &new_root);
    if (changed < 0) {
        fsck_note(rep, "quarantine excision failed (the volume is "
                          "unchanged; re-run with free space)");
        return -1;
    }
    if (changed == 0) {
        fsck_note(rep, "no quarantined range could be excised -- the base "
                          "tree is unchanged");
        return 0;
    }

    /* structure-before-reference (WP-M3): the new pages and the allocation
     * bitmap are durable before RT30 names the new root. */
    if (vol_bitmap_flush(v) != 0 || vmux_barrier(v, "fsck v3 repair pages") < 0) {
        fsck_note(rep, "could not make the repaired pages durable; the "
                          "root was NOT published (the volume is unchanged)");
        return -1;
    }
    if (mbuf_root_publish(v, new_root.pba, new_root.gen) != 0) {
        fsck_note(rep, "RT30 refused the repaired root; the volume is "
                          "unchanged");
        return -1;
    }

    /* Reachability diff against the new root. The save point is gone, so
     * nothing is pinned: the dropped subtrees' pages (including the damaged
     * one) and the rewritten path are freed. */
    (void)vol_reclaim_bump_epoch();
    (void)vol_reclaim_drain(v);
    (void)vol_reclaim_mark_and_free(v, old_root, new_root,
                                    (invfs_blkptr){0, 0, 0, 0});

    /* Fold the delta back in: this restores every quarantined key the delta
     * still holds, and leaves the delta empty as a normal fold does. */
    if (vol_delta_count(v) > 0 && vol_fold(v) != 0)
        fprintf(stderr, "fsck(v3): warning: the delta could not be folded "
                        "back in after the repair; its records are still in "
                        "the delta and will be re-applied by the next fold\n");

    /* What is left is gone: name it. */
    fsck_lost_names(v, rep);
    rep->repaired = 1;
    (void)nr;
    return 0;
}

/* WP118: run the nlink/fan-in audit and turn it into the report + the
 * operator-facing lines. Every offender is named (up to
 * INVFS_NLINK_FAULT_MAX of them) with its inode id, its nlink, how many
 * names actually resolve to it, and one of those names -- the volume
 * cannot say WHICH name is the intruder (that is a judgement about
 * history, not a fact in the volume), so it says what it knows instead.
 * A mismatch is damage: it sets damaged, so the verdict is DAMAGED and
 * the exit code is 3, exactly as for every other finding here. */
static void fsck_nlink_report(invfs_volume *v, invfs_fsck_report *rep)
{
    invfs_nlink_audit a;
    uint64_t i;

    if (vol_nlink_audit(v, &a) != 0) {
        fsck_note(rep, "the nlink/fan-in accounting could not be completed "
                          "(the namespace walk failed); the pass below is "
                          "partial");
        return;
    }
    rep->nlink_names = a.names;
    rep->nlink_inodes = a.inodes;
    rep->nlink_missing = a.missing_names;
    rep->nlink_stale = a.stale_dirents;
    rep->nlink_dead = a.dead_names;
    rep->nlink_orphans = a.orphan_rows;
    rep->nlink_faults = a.nfault_total;
    rep->nlink_bad = a.mismatch;
    if (a.orphan_rows) {
        /* not damage (see vol_nlink_audit): a crash between the row and
         * its dirent leaves a row nobody names, and no repair removes it. */
        fprintf(stderr, "fsck(v3): nlink/fan-in: %llu live inode(s) have NO "
                        "directory entry naming them (orphan rows). Not "
                        "counted as damage: the v3 write order is row then "
                        "dirent, so a crash between the two leaves one, and "
                        "-f cannot remove it. The volume lists the files it "
                        "has; these rows are unreachable.\n",
                (unsigned long long)a.orphan_rows);
    }
    if (!a.mismatch)
        return;

    rep->damaged = 1;
    for (i = 0; i < a.nfault; i++) {
        const invfs_nlink_fault *f = &a.fault[i];
        const char *what = !strcmp(f->reason, "missing-name")
                         ? "a name is MISSING (nlink counts links no "
                           "directory entry accounts for)"
                         : !strcmp(f->reason, "dead")
                         ? "a name points at an inode row that is not live"
                         : "STALE DIRENT (more names resolve to this inode "
                           "than its nlink accounts for -- a name landed on "
                           "an inode it does not belong to, and the content "
                           "under the older name is gone)";
        fprintf(stderr, "fsck(v3): nlink/fan-in: inode %llu (%s): nlink %u, "
                        "%u name(s) resolve to it -- %s%s%s\n",
                (unsigned long long)f->id, f->name[0] ? f->name : "?",
                f->nlink, f->fanin, what,
                a.nfault_total > a.nfault ? " [list truncated]" : "", "");
    }
    if (a.missing_names)
        fprintf(stderr, "fsck(v3): nlink/fan-in: %llu name(s) missing -- the "
                        "inode rows claim %llu more link(s) than the "
                        "directory tree has entries for\n",
                (unsigned long long)a.missing_names,
                (unsigned long long)a.missing_names);
    if (a.stale_dirents)
        fprintf(stderr, "fsck(v3): nlink/fan-in: %llu stale dirent(s) -- "
                        "directory entries resolve to inodes whose rows do "
                        "not account for them; this is the shape of the "
                        "WP111b data loss (one name's content replaced by "
                        "another's)\n",
                (unsigned long long)a.stale_dirents);
    if (a.dead_names)
        fprintf(stderr, "fsck(v3): nlink/fan-in: %llu name(s) point at an "
                        "inode row that is not live\n",
                (unsigned long long)a.dead_names);
    fprintf(stderr, "fsck(v3): nlink/fan-in: %llu name(s) over %llu live "
                    "inode(s); %llu inode(s) do not balance. NOT REPAIRABLE "
                    "by -f: which of the colliding names is the intruder is "
                    "not decidable from the volume.\n",
            (unsigned long long)a.names, (unsigned long long)a.inodes,
            (unsigned long long)a.nfault_total);
}

/* ANC0: what the volume did about its tail anchor, stated in the verdict.
 *
 * This is the one place the "say it out loud" rule from vol_cpack pays off
 * for the anchor, because an open that silently recovered would leave the
 * operator believing a DAMAGED volume is healthy. It is damaged: block 0's
 * RT30 did not validate, and the only reason the tree is walkable is the
 * copy at the tail. So the pass records it as damage -- the data is
 * readable and the report says so -- and invf-fsck exits nonzero. An
 * operator who wants a clean bill of health has to replace block 0.
 *
 * The opposite case is just as important: a volume whose tail holds NO
 * anchor (every volume formatted before the anchor existed) says so and
 * nothing else. fsck must not invent a missing anchor, and a volume that
 * never had one is not damaged by not having one. */
static void fsck_anchor(invfs_volume *v, invfs_fsck_report *rep)
{
    char b[320];

    if (v->anchor_adopted) {
        rep->anchor_restored = 1;
        snprintf(b, sizeof b,
                 "ANC0: the volume opened on its TAIL ANCHOR at block %llu "
                 "-- block 0's root descriptor did not validate. The base "
                 "tree is readable and is being walked, but this image is "
                 "DAMAGED: block 0 must be replaced (re-format, or restore "
                 "block 0 from a backup) before it is sound",
                 (unsigned long long)anchor_pba(v));
        fsck_note(rep, b);
        return;
    }
    if (v->anchor_refresh_failed) {
        rep->anchor_stale = 1;
        snprintf(b, sizeof b,
                 "ANC0: a refresh of the tail anchor at block %llu FAILED "
                 "during this session. The anchor is now older than the root "
                 "descriptor it mirrors, so a loss of block 0 would fall back "
                 "to a stale root. The volume itself is sound",
                 (unsigned long long)anchor_pba(v));
        fsck_note(rep, b);
        return;
    }
    if (v->anchor_state == INVFS_ANCHOR_REFUSED_GEOMETRY) {
        rep->anchor_refused = 1;
        fsck_note(rep, "ANC0: the tail block holds an anchor whose "
                          "geometry fingerprint does not match this volume; "
                          "it was REFUSED, not adopted. Refusal is not "
                          "absence -- the block is not this volume's");
    } else if (v->anchor_state == INVFS_ANCHOR_REFUSED_DAMAGE) {
        rep->anchor_refused = 1;
        fsck_note(rep, "ANC0: the tail block holds an ANC0 descriptor that "
                          "failed its own CRC. It was REFUSED, not adopted; "
                          "the block is damaged, not absent");
    }
}

/* The recipe resolvability audit. Every check above looks at the
 * base tree, the pages, the bitmap or the NAMES; none of them resolves the
 * content address an inode row carries. So a volume that had lost a recipe
 * blob -- the key gone from the tree, or its bytes no longer hashing to the
 * address that names them -- walked clean here: every page CRC verified,
 * every fan-in balanced, verdict OK, and invf-verify --deep plus the read
 * path both failing on a file the volume still listed. Each offender is
 * named with its id, its size and (when one resolves) its path.
 *
 * A recipe blob is addressed by the hash of its own contents, so there is
 * nothing to rebuild it from: this is damage, -f does not touch it, and the
 * verdict is DAMAGED either way.
 *
 * Skipped for the same reason the nlink audit above is: with a quarantined
 * key range in the tree, a recipe that cannot be loaded may simply be in
 * that range -- reported as damage that -f can address, not as a lost blob
 * it cannot. */
static void fsck_recipe_report(invfs_volume *v, invfs_fsck_report *rep)
{
    invfs_recipe_audit a;
    uint64_t i;

    if (vol_recipe_audit(v, &a) != 0) {
        rep->recipe_partial = 1;
        fsck_note(rep, "the recipe resolvability check could not be "
                          "completed (the live-inode walk failed); the count "
                          "below is a floor, not a total");
        return;
    }
    rep->recipe_checked = a.checked;
    rep->recipe_bad = a.bad;
    for (i = 0; i < a.nfault; i++) {
        const invfs_recipe_fault *f = &a.fault[i];
        char b[512];
        snprintf(b, sizeof b,
                 "inode %llu (%s): the recipe blob its row names cannot be "
                 "read -- %s. The file is %llu byte(s) of content the volume "
                 "no longer has, and %s",
                 (unsigned long long)f->id,
                 f->name[0] ? f->name : "no name resolves to it",
                 f->kind == INVFS_RECIPE_BAD_CORRUPT
                     ? "the blob is present but does not parse as a recipe"
                     : "the blob is MISSING from the base tree, shadowed by a "
                       "delta delete, or its bytes no longer hash to the "
                       "address that names them",
                 (unsigned long long)f->size,
                 f->name[0] ? "invf-verify --deep will list it as corrupt"
                            : "no directory entry names it, so nothing the "
                              "operator can read points at it");
        fsck_note(rep, b);
    }
    if (a.bad && a.nfault_total > a.nfault)
        fprintf(stderr, "fsck(v3): recipe blobs: %llu unreadable (the list "
                        "above is truncated at %d)\n",
                (unsigned long long)a.nfault_total, INVFS_RECIPE_BAD_MAX);
    else if (a.bad)
        fprintf(stderr, "fsck(v3): recipe blobs: %llu of %llu live inode(s) "
                        "with content cannot be read\n",
                (unsigned long long)a.bad, (unsigned long long)a.checked);
}

static int fsck_scan(invfs_volume *v, invfs_fsck_report *rep, int fix,
                        int discard_reachable)
{
    invfs_blkptr root;
    bt_stat st;
    bt_quarantine q;
    char err[128];
    int rc, i;

    fsck_anchor(v, rep);
    rc = fsck_rt30(v, rep);
    if (rc < 0)
        return -1;
    if (rc == 2) {
        /* WP86: the descriptor is torn, so no root can be named. Only the
         * delta is readable, and rebuilding the base from the delta alone
         * would drop every key folded before it -- refuse, loudly. */
        rep->root_lost = 1;
        return 0;
    }
    if (rc != 0) {
        /* RT30 absent or a page size this build cannot address: the base is
         * not walkable. Damage (if any) is already recorded; report and stop
         * rather than read 4 KiB pages under a 16 KiB descriptor. */
        return 0;
    }
    if (fsck_root(v, rep, &root) < 0)
        return -1;

    if (root.pba == 0) {
        /* No valid root page. Empty base when no slot names anything;
         * WP86: damage when a slot named a page that does not validate. */
        if (v->rt.root_slot[0] || v->rt.root_slot[1]) {
            char b[192];
            rep->root_lost = 1;
            snprintf(b, sizeof b,
                     "no valid base root: root_slot[0]=%llu root_slot[1]=%llu "
                     "and neither page validates -- the base tree is "
                     "unreachable; only the delta (recent writes) is readable",
                     (unsigned long long)v->rt.root_slot[0],
                     (unsigned long long)v->rt.root_slot[1]);
            fsck_note(rep, b);
        } else if (!rep->damaged) {
            fprintf(stderr, "fsck(v3): base tree empty (clean)\n");
        }
        return 0;
    }

    /* WP86: a live save point over a damaged base is a trap -- it looks like a
     * rollback target and is not one. Report it, and refuse the repair while
     * it is live (fsck_repair drops it, but only under -f). */
    if (v->savepoint_live) {
        invfs_blkptr pinned;
        char e2[128];
        e2[0] = 0;
        if (fsck_ptr_at(v, v->spt0.base_root, &pinned) != 0 ||
            btree_check(v, pinned, NULL, e2, sizeof e2) != 0) {
            char b[192];
            rep->savepoint_bad = 1;
            snprintf(b, sizeof b, "a live save point pins a DAMAGED base tree "
                     "(base_root %llu) -- invf-rollback will refuse it",
                     (unsigned long long)v->spt0.base_root);
            fsck_note(rep, b);
        }
    }

    err[0] = 0;
    rc = btree_check_tolerant(v, root, &st, &q, err, sizeof err);
    rep->pages_walked = st.n_pages;
    rep->keys = st.nkeys;
    rep->quarantined = (uint64_t)q.n;
    if (rc != 0) {
        char b[256];
        if (err[0])
            snprintf(b, sizeof b, "base tree walk failed: %s", err);
        else
            snprintf(b, sizeof b, "base tree walk failed (io error)");
        fsck_note(rep, b);
        /* A structural failure (cycle, shared child, level/ordering) is not
         * media damage and is NOT excised: -f refuses rather than reshape a
         * tree the engine itself built wrong. */
        if (strstr(err, "cycle or shared child"))
            rep->cycles++;
        else
            rep->bad_pages += q.bad_pages ? q.bad_pages : 1;
        return 0;
    }
    if (q.n) {
        /* big enough for two hex keys (3*BT_QUARANTINE_KEY_MAX+1 bytes each)
         * plus the fixed text and the pba -- at 256 the tail carrying
         * "QUARANTINED (those keys read EIO...)" was cut off, which is the
         * part the operator has to read */
        char b[2048];
        rep->bad_pages += q.bad_pages;
        for (i = 0; i < q.n; i++) {
            char lo[3 * BT_QUARANTINE_KEY_MAX + 1];
            char hi[3 * BT_QUARANTINE_KEY_MAX + 1];
            size_t o = 0;
            int j;
            for (j = 0; j < q.range[i].lo_n && o + 3 < sizeof lo; j++)
                o += (size_t)snprintf(lo + o, sizeof lo - o, "%02x",
                                      q.range[i].lo[j]);
            lo[o] = 0;
            if (q.range[i].hi_unbounded) {
                snprintf(hi, sizeof hi, "+inf");
            } else {
                o = 0;
                for (j = 0; j < q.range[i].hi_n && o + 3 < sizeof hi; j++)
                    o += (size_t)snprintf(hi + o, sizeof hi - o, "%02x",
                                          q.range[i].hi[j]);
                hi[o] = 0;
            }
            snprintf(b, sizeof b,
                     "base page pba %llu is unreadable: key range [%s, %s) is "
                     "QUARANTINED (those keys read EIO; everything else is "
                     "readable)", (unsigned long long)q.range[i].pba, lo, hi);
            fsck_note(rep, b);
        }
        if (q.qfull) {
            char b[192];
            snprintf(b, sizeof b, "the quarantine set is incomplete (more "
                     "unreadable pages than it holds, or a key range longer "
                     "than %u bytes): this report is partial and -f will "
                     "refuse to repair", (unsigned)BT_QUARANTINE_KEY_MAX);
            fsck_note(rep, b);
        }
    }

    /* Allocator cross-check: a reachable page must be marked allocated in the
     * metadata bitmap (the v2 "bitmap divergence" analogue). Only the
     * root page is checked here: the check does not expose the visited set,
     * and btree_reclaim (which could mark it) frees pages, which is out of
     * scope. TODO(WP-M4): full reachable-set/bitmap cross-check. */
    fsck_bitmap_check(v, rep, root.pba);

    /* WP118: nlink vs dirent fan-in. Every check above looks at the base
     * tree, the pages and the bitmap; none of them compares the NAMES the
     * volume claims to have against the link counts its inode rows carry, so
     * a volume where two names share one inode -- silently replacing one
     * file's content with another's, the WP111b data loss -- passed every one
     * of them. fsck_nlink_report names the inode and the discrepancy.
     *
     * Skipped when the tree itself is damaged: a name inside a quarantined
     * key range does not resolve, so the fan-in side of every comparison
     * would be short by exactly the lost keys and each would read as a
     * missing name. The damage is already reported above; a namespace audit
     * on top of it would be noise, and a wrong one. */
    if (q.n || q.qfull) {
        fprintf(stderr, "fsck(v3): nlink/fan-in check SKIPPED: the base tree "
                        "is damaged, so a name that does not resolve cannot be "
                        "told from a lost one\n");
        fprintf(stderr, "fsck(v3): recipe resolvability check SKIPPED for "
                        "the same reason: a recipe that will not load may be "
                        "inside a quarantined key range, which is damage -f "
                        "can address\n");
    } else {
        fsck_nlink_report(v, rep);
        fsck_recipe_report(v, rep);
    }

    if (!fix || !q.n)
        return 0;
    if (q.qfull) {
        fsck_note(rep, "refusing to repair: the damage exceeds what one pass "
                          "can quarantine");
        return 0;
    }
    /* ORDERING, and it is the whole point of this gate: prove the ranges hold
     * nothing a live object still needs BEFORE the excision that cannot be
     * undone touches anything. A range that fails the proof is left in place,
     * so the key stays on the page it was on and still reads EIO -- the one
     * state from which the operator can get the bytes back. */
    {
        bt_quarantine sq;
        if (fsck_excise_safety(v, rep, &q, &sq, discard_reachable) != 0 ||
            sq.n == 0)
            return 0;                   /* refused: the volume is untouched */
        return fsck_repair(v, rep, root, &sq);
    }
}
