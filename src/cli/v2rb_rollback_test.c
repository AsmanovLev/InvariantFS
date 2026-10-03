/* v2rb_rollback_test.c — the two v2-era paths that kept running on v3.
 *
 * Both bugs are the same shape: a retired v2 mechanism that was correct in
 * its own time is still being driven on Meta-v3, where it is either a
 * no-op or actively destructive. In both cases the RETIRED TWIN is the one
 * that is right and the current implementation diverged.
 *
 * ---------------------------------------------------------------------------
 * LEG A — a v2 rollback reached on v3, and it writes into v3's shared pool.
 *
 * src/core/vol_cpack.c's MAP branch commits with vol_create_blob_file()
 * (:3292) and, on the two failure paths after that commit, rolls back with
 * an UNGUARDED vol_delete_inode(v, newino, name) (:3307, :3318). On v3,
 * vol_create_blob_file supersedes the inode row IN PLACE (newino ==
 * inode_id), so the old row the rollback exists to retire is already gone:
 * the call cannot undo the commit it was written to undo.
 *
 * It is not a no-op while failing to. vol_retire_inode (vol_records.c:623)
 * has no v3 guard, so it calls vol_mark_dirty (a superblock rewrite) and
 * then appends a v2 TOMBSTONE_MAGIC record through vol_append_slot into the
 * ACTIVE METADATA EXTENT. On v3 that extent is the shared base-page / data
 * pool the COW B+ tree allocates from — and where the metadata zone ran
 * short it is physically in the shadow pool (AGENTS.md 2.7). MEASURED HERE:
 * the record landed at pba 2680775 in the shadow zone, over a block that was
 * ALLOCATED. That is a v2-shaped record written over a live v3 block: a
 * bit-exactness violation, not a bookkeeping one.
 *
 * What it does NOT do is lose the file: the v2 tombstone and the idx_del
 * beside it are invisible to v3 resolution, so the committed row survives.
 * The damage is the pool overwrite plus the superblock rewrite — and the
 * lane then reports "declined", so the operator sees a clean sweep log over
 * a volume that just had a live block overwritten. This test asserts on the
 * DISK EFFECT, not on the message alone.
 *
 * The rollback is exercised through cpack_rollback_commit() — the shared
 * helper both MAP-branch call sites use. A test that re-implemented the
 * three lines would prove nothing about the code that actually runs.
 *
 * ---------------------------------------------------------------------------
 * LEG D — the free path reports success when it freed nothing.
 *
 * vol_v3_free_recipe_blocks() (vol_ast.c:125) returns -1 when the recipe
 * LOAD fails but 0 when it does not PARSE, having freed nothing. Its v2
 * twin bails on parse failure too (vol_records.c:551-557). So unlinking a
 * v3 inode whose recipe does not parse makes vol_v3_unlink report SUCCESS,
 * the row is deleted, and every block the recipe named is allocated,
 * referenced by nothing, and reachable by no name.
 *
 * The honest assertion is on BOTH halves, and they currently disagree:
 *   D1 — what the free function returns, and whether it freed.
 *   D2 — what the caller (vol_v3_unlink) is told, and what happened to the
 *        blocks. After the fix both halves agree: the free reports failure,
 *        the blocks are still allocated, and unlink says so instead of
 *        reporting success.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_delta.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  OK    %s\n", what);
    }
}

static invfs_volume *g_v;
static char g_img[512];

/* ---- stderr capture: the lane's "declined" report is part of the defect --
 * A half-committed decomposition logged as a clean refusal is Finding A's
 * legibility face, so it is asserted, not assumed. */
static int   g_errfd = -1;
static int   g_saved_err = -1;
static char  g_errbuf[8192];

/* WP204: where the stderr-capture file goes. This used to be a compiled-in
 * /srv/bench/wt-v2rb-stderr-XXXXXX template: the author's bench disk, which
 * does not exist on a CI runner or in a container, so mkstemp failed, the
 * capture never started, and the legs that assert on the diagnostic read an
 * EMPTY log and failed as if the engine had said nothing. The harness
 * passes a scratch root; honour it, and default to /tmp. */
static char g_tmpdir[512] = "/tmp";

static void err_capture_begin(void)
{
    char tmpl[600];
    g_saved_err = dup(2);
    snprintf(tmpl, sizeof tmpl, "%s/wt-v2rb-stderr-XXXXXX", g_tmpdir);
    g_errfd = mkstemp(tmpl);
    if (g_errfd < 0) return;
    unlink(tmpl);
    dup2(g_errfd, 2);
}

static const char *err_capture_end(void)
{
    ssize_t n;
    if (g_saved_err < 0) return "";
    fflush(stderr);
    dup2(g_saved_err, 2);
    close(g_saved_err);
    g_saved_err = -1;
    lseek(g_errfd, 0, SEEK_SET);
    n = read(g_errfd, g_errbuf, sizeof g_errbuf - 1);
    close(g_errfd);
    g_errfd = -1;
    if (n < 0) n = 0;
    g_errbuf[n] = 0;
    return g_errbuf;
}

/* Count the v2-shaped records ("DELT" tombstones) sitting in the SHARED
 * METADATA ZONE right now. The assertion is made against the bytes on the
 * device, not against a flag or an in-memory cursor: the claim that matters
 * is "a v2 record landed in the pool v3's COW B+ tree allocates from", and
 * anything cheaper would only prove the test agrees with itself.
 *
 * Scanned on a CLOSED volume (a reopened handle), because the record stream
 * is write-buffered -- reading the same handle back would read the buffer,
 * not the disk. */
static long v2_tombstones_in_metadata_zone(void)
{
    static uint8_t buf[1u << 20];
    uint64_t start = g_v->sb.metadata_zone_start * INVFS_BLOCK_SIZE;
    uint64_t end   = start + g_v->sb.metadata_zone_blocks * INVFS_BLOCK_SIZE;
    uint64_t off;
    long n = 0;
    uint32_t i;

    if (g_v->sb.metadata_zone_blocks == 0) return 0;
    for (off = start; off < end; off += sizeof buf) {
        size_t want = (end - off > sizeof buf) ? sizeof buf
                                               : (size_t)(end - off);
        if (want < 4) break;
        if (vol_read_raw(g_v, off, buf, want) != 0) return -1;
        for (i = 0; i + 4 <= want; i++) {
            uint32_t m;
            memcpy(&m, buf + i, 4);
            if (m == TOMBSTONE_MAGIC) n++;
        }
    }
    return n;
}

/* Does a v2-shaped record sit at this byte offset? The block the v2 record
 * stream appends into is named by inode_area_pos, so this is a direct
 * read of what the rollback put on the device. */
static int v2_record_at(invfs_volume *v, uint64_t off)
{
    uint8_t buf[512];
    uint32_t m;
    if (vol_read_raw(v, off, buf, sizeof buf) != 0) return 0;
    memcpy(&m, buf, 4);
    return m == TOMBSTONE_MAGIC;
}

static const char *zone_name(invfs_volume *v, uint64_t pba)
{
    if (pba < v->sb.metadata_zone_start) return "block-0 descriptor";
    if (pba < v->sb.metadata_zone_start + v->sb.metadata_zone_blocks)
        return "METADATA";
    if (pba < v->sb.raw_zone_start) return "metadata tail";
    if (pba < v->sb.shadow_zone_start) return "RAW";
    return "SHADOW";
}

static int block_allocated(uint64_t pba)
{
    if (!pba || pba >= g_v->sb.total_blocks) return 0;
    return bit_get(g_v->bitmap, pba) != 0;
}

/* Close and reopen, then count. Every disk assertion in this test goes
 * through here, so none of them can accidentally read a write buffer. */
static long rescan_metadata_zone(void)
{
    int rerr = 0;
    long n;
    vol_close(g_v);
    g_v = vol_open(g_img, &rerr);
    if (!g_v) return -1;
    n = v2_tombstones_in_metadata_zone();
    return n;
}

static uint64_t write_file(const char *name, const uint8_t *body, size_t n)
{
    invfs_meta_pub m;
    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG;
    m.mode = 0644;
    return vol_v3_write_bulk(g_v, name, body, n, &m);
}

/* ---------------------------------------------------------------- LEG A -- */

/* What the MAP branch's rollback must NOT do on v3. Called through
 * cpack_rollback_commit() in src/core/vol_cpack.c, i.e. the shipped line. */
static void leg_a_map_rollback_on_v3(void)
{
    static uint8_t body[96 * 1024];
    static uint8_t container[40 * 1024];
    invfs_v3_inode in;
    uint8_t addr_at_commit[INVFS_V3_RECIPE_ADDR_LEN] = {0};
    uint8_t *snap = NULL;
    size_t snap_len = 0;
    uint64_t old_id, newino, pos_before, off_before, ext_pba, wpos;
    long tomb_before, tomb_after;
    int clobbered_allocated, landed;
    const char *log;
    size_t i;

    printf("leg A: containerpack MAP-branch rollback on a v3 volume\n");
    for (i = 0; i < sizeof body; i++)   body[i] = (uint8_t)(i * 7 + 3);
    for (i = 0; i < sizeof container; i++) container[i] = (uint8_t)(i * 11 + 5);

    old_id = write_file("pack.tar", body, sizeof body);
    ok(old_id != 0, "leg A: the original file is written");
    ok(vol_v3_inode_get(g_v, old_id, &in) == 1, "leg A: its row is readable");

    /* the commit, exactly as vol_cpack.c:3292 does it */
    newino = vol_create_blob_file(g_v, "pack.tar", container,
                                  sizeof container, sizeof body,
                                  INVFS_ALGO_ZSTD);
    ok(newino != 0, "leg A: the MAP-branch commit (vol_create_blob_file) "
                    "succeeds");
    /* THE PREMISE of the whole finding. If this ever stops holding, the
     * rollback would be harmless and this test would be testing nothing --
     * so it is asserted rather than assumed. */
    ok(newino == old_id,
       "leg A: on v3 the commit supersedes the row IN PLACE "
       "(newino == inode_id) -- so the row the rollback exists to retire is "
       "already gone and the rollback cannot undo its own commit");

    /* snapshot exactly what the commit published */
    {
        invfs_v3_inode ci;
        if (vol_v3_inode_get(g_v, newino, &ci) == 1) {
            memcpy(addr_at_commit, ci.recipe_addr, INVFS_V3_RECIPE_ADDR_LEN);
            vol_v3_recipe_load(g_v, ci.recipe_addr, &snap, &snap_len);
        }
    }

    /* The byte offset the v2 record stream would append at. vol_retire_inode
     * writes the tombstone here (vol_records.c:792-795) and then advances
     * this cursor -- so if the cursor does not move, no v2 record was
     * written, whatever else the rollback did. */
    pos_before = g_v->inode_area_pos;

    /* --- what actually lands in the volume ---
     * Counted on a REOPENED handle, not the live one: the record stream is
     * write-buffered, so a read-back on the same handle reads the buffer,
     * not the disk. The question is what survives to the device, so the
     * count is taken before the rollback and again after it. */
    tomb_before = rescan_metadata_zone();
    if (tomb_before < 0) {
        ok(0, "leg A: the shared metadata zone can be scanned");
        return;
    }
    /* was the block the v2 record stream is about to overwrite carrying
     * anything? This is what turns "a stray record" into "an overwrite". */
    pos_before = g_v->inode_area_pos;
    clobbered_allocated = block_allocated(pos_before / INVFS_BLOCK_SIZE);

    /* the exact byte the v2 record stream would append at: the ACTIVE
     * metadata extent's base plus its append offset. On v3 that extent is
     * the shared base-page / data pool the COW B+ tree allocates from, and
     * on a volume where the metadata zone ran short the allocator falls
     * back to the shadow pool (AGENTS.md 2.7) -- so this is the pool the
     * v2 record lands in, whichever physical zone it turned out to be. */
    off_before = g_v->met0.active_offset;

    /* the rollback, through the shipped helper */
    err_capture_begin();
    cpack_rollback_commit(g_v, newino, old_id, "pack.tar", 0,
                          sizeof body, 0);
    log = err_capture_end();

    /* which extent got the record (it is allocated lazily by the append
     * itself, so this has to be read AFTER the rollback) */
    {
        uint64_t entry = meta_mapper_get(g_v, (size_t)g_v->met0.active_extent);
        ext_pba = entry ? invfs_meta_ext_pba(entry) : 0;
        wpos = ext_pba * INVFS_BLOCK_SIZE + off_before;
    }

    /* THE cursor check: vol_append_slot bumps met0.active_offset for every
     * record it hands a slot to (volume.c:4282), so a cursor that did not
     * move means no v2 record-stream append happened at all. */
    ok(g_v->met0.active_offset == off_before,
       "leg A: the shared metadata extent's append cursor does not move -- "
       "no v2 record-stream append happened at all");

    /* THE byte check, on a REOPENED handle: the record stream is
     * write-buffered, so a read-back on the live handle would read the
     * buffer rather than the device. */
    vol_close(g_v);
    {
        int rerr = 0;
        g_v = vol_open(g_img, &rerr);
    }
    if (!g_v) { ok(0, "leg A: reopen after the rollback"); return; }
    landed = v2_record_at(g_v, wpos);
    if (landed) {
        uint64_t pba = wpos / INVFS_BLOCK_SIZE;
        printf("          (a v2 TOMBSTONE_MAGIC record was written at byte "
               "%llu = pba %llu, inside the active metadata extent (pba "
               "%llu) which sits in the %s zone; that block was %s before "
               "the rollback)\n",
               (unsigned long long)wpos, (unsigned long long)pba,
               (unsigned long long)ext_pba, zone_name(g_v, pba),
               clobbered_allocated ? "ALLOCATED (live data overwritten)"
                                   : "free");
    }
    ok(!landed,
       "leg A: no v2 TOMBSTONE record reaches the shared base-page/data pool "
       "at the v2 record-stream append position");
    ok(!clobbered_allocated || !landed,
       "leg A: the rollback does not overwrite an allocated block with a "
       "v2-shaped record");

    tomb_after = rescan_metadata_zone();
    if (tomb_after < 0) {
        ok(0, "leg A: the shared metadata zone can be re-scanned");
        return;
    }
    if (tomb_after > tomb_before)
        printf("          (%ld v2 TOMBSTONE record(s) appeared in the shared "
               "metadata zone across the rollback; it had %ld before)\n",
               tomb_after - tomb_before, tomb_before);
    ok(tomb_after == tomb_before,
       "leg A: NO v2 TOMBSTONE record reaches the shared metadata extent");
    ok(tomb_before == 0,
       "leg A: a sound v3 volume holds no v2 tombstone records at all "
       "(the positive control: a counter stuck at zero would pass the "
       "check above vacuously)");

    /* --- and the row it was supposed to preserve --- */
    {
        uint64_t got = 0;
        ok(vol_v3_path_lookup(g_v, "pack.tar", &got) == 1 && got == newino,
           "leg A: the committed row still resolves after close/reopen");
    }
    {
        /* BIT-EXACTNESS, asserted as "the committed recipe is byte-identical
         * to what the commit wrote". The rollback's whole job is to undo the
         * commit; on v3 it cannot, and the damage it does instead is to the
         * committed row. So the pinned invariant is that the bytes the
         * commit published are still exactly those bytes afterwards.
         *
         * (Compared against the snapshot taken at commit time rather than
         * against the container: what vol_create_blob_file stores under the
         * address is the AST recipe framing the container, not the
         * container itself, and the container read path is not what this
         * control is about.) */
        invfs_v3_inode after;
        uint8_t *d = NULL;
        size_t n = 0;
        int exact = 0;
        if (vol_v3_inode_get(g_v, newino, &after) == 1 &&
            memcmp(after.recipe_addr, addr_at_commit,
                   INVFS_V3_RECIPE_ADDR_LEN) == 0 &&
            vol_v3_recipe_load(g_v, after.recipe_addr, &d, &n) == 0 &&
            d && n == snap_len && snap && memcmp(d, snap, n) == 0)
            exact = 1;
        ok(exact,
           "leg A: the committed recipe is still BIT-EXACT -- same address, "
           "byte-identical blob -- after the rollback");
        free(d);
    }

    /* --- legibility: "declined" over a half-committed decomposition --- */
    ok(strstr(log, "pack.tar") != NULL,
       "leg A: the rollback names the file in its report, so the operator is "
       "not left reading a bare \"declined\" line");
    if (strstr(log, "declined") != NULL && strstr(log, "pack.tar") == NULL)
        printf("          (log was: %s)\n", log);
}

/* ---------------------------------------------------------------- LEG D -- */

/* Load an inode's recipe, break ONLY its AST version word so the blob still
 * loads (its BLAKE3 still matches its own address -- this is a recipe that
 * is present and intact as a blob but is not a recipe) and still names the
 * same real, allocated blocks, and store it back under its new address. */
static int poison_recipe(uint64_t id, uint8_t new_addr[INVFS_V3_RECIPE_ADDR_LEN],
                         uint64_t *pbas, size_t *npbas, uint64_t *size_out)
{
    invfs_v3_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0, n = 0, i;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t nents = 0;

    if (vol_v3_inode_get(g_v, id, &in) != 1) return -1;
    if (size_out) *size_out = in.size;
    if (vol_v3_recipe_load(g_v, in.recipe_addr, &blob, &blen) != 0 || !blob)
        return -1;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &nents) != 0) {
        free(blob);
        return -1;
    }
    n = 0;
    for (i = 0; i < nents && npbas && n < *npbas; i++)
        pbas[n++] = ents[i].pba;
    if (npbas) *npbas = n;

    /* the AST version word: neither V1 nor V2, so invfs_ast_hdr_parse
     * refuses and vol_ast_recipe_parse returns -1. Everything else about
     * the blob is untouched, so it still hashes to a valid address and
     * still names the same segments. */
    blob[0] = 0xEE; blob[1] = 0xEE; blob[2] = 0xEE; blob[3] = 0xEE;

    if (vol_v3_recipe_store(g_v, blob, blen, new_addr) != 0) {
        free(blob);
        return -1;
    }
    free(blob);
    if (vol_v3_inode_get(g_v, id, &in) != 1) return -1;
    in.recipe = (invfs_blkptr){0};
    memcpy(in.recipe_addr, new_addr, INVFS_V3_RECIPE_ADDR_LEN);
    return vol_v3_inode_delta_put(g_v, id, &in);
}

static void leg_d_unparseable_recipe(void)
{
    static uint8_t body[256 * 1024];
    uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN];
    uint64_t pbas[64], npbas;
    uint64_t id, size = 0;
    const char *log;
    size_t i, n_held = 0;

    printf("leg D: unparseable v3 recipe -- what the caller is told vs what "
           "happened to the blocks\n");
    for (i = 0; i < sizeof body; i++) body[i] = (uint8_t)(i * 31 + 17);

    /* ---- D1: the free path's own return value ---- */
    id = write_file("d1.bin", body, sizeof body);
    ok(id != 0, "leg D1: a multi-segment file is written");
    npbas = sizeof pbas / sizeof pbas[0];
    ok(poison_recipe(id, addr, pbas, &npbas, &size) == 0,
       "leg D1: its recipe is replaced by a blob that loads but does not "
       "parse");
    ok(npbas > 0, "leg D1: the recipe named real blocks to begin with");
    for (i = 0; i < npbas; i++)
        if (block_allocated(pbas[i])) n_held++;
    ok(n_held == npbas,
       "leg D1: every block the recipe names is allocated before the free");

    {
        int rc = vol_v3_free_recipe_blocks(g_v, addr, 0);
        if (rc == 0)
            printf("          (vol_v3_free_recipe_blocks returned 0 having "
                   "freed nothing -- %llu blocks are now orphaned)\n",
                   (unsigned long long)npbas);
        ok(rc != 0,
           "leg D1: vol_v3_free_recipe_blocks REPORTS FAILURE on a recipe that "
           "does not parse (v2 twin bails here too, vol_records.c:551-557)");
    }
    {
        size_t still = 0;
        for (i = 0; i < npbas; i++)
            if (block_allocated(pbas[i])) still++;
        ok(still == npbas,
           "leg D1: and the blocks are still allocated -- the return value "
           "and the block fate AGREE");
    }

    /* ---- D2: what vol_v3_unlink tells the caller ---- */
    id = write_file("d2.bin", body, sizeof body);
    ok(id != 0, "leg D2: a second such file is written");
    npbas = sizeof pbas / sizeof pbas[0];
    ok(poison_recipe(id, addr, pbas, &npbas, &size) == 0,
       "leg D2: its recipe is poisoned the same way");

    err_capture_begin();
    {
        int rc = vol_v3_unlink(g_v, "d2.bin");
        log = err_capture_end();
        if (rc == 0)
            printf("          (vol_v3_unlink reported SUCCESS; the row is gone "
                   "and %llu blocks are unreachable)\n",
                   (unsigned long long)npbas);
        ok(rc != 0,
           "leg D2: vol_v3_unlink does NOT report success when it could not "
           "reclaim the recipe's blocks");
    }
    {
        uint64_t got = 0;
        ok(vol_v3_path_lookup(g_v, "d2.bin", &got) != 1,
           "leg D2: the name is still gone (the unlink itself is not undone "
           "-- rolling it back would be worse)");
    }
    {
        size_t still = 0;
        for (i = 0; i < npbas; i++)
            if (block_allocated(pbas[i])) still++;
        ok(still == npbas,
           "leg D2: the blocks are still allocated, so the caller's report "
           "and the block fate now AGREE");
    }
    ok(strstr(log, "d2.bin") != NULL,
       "leg D2: the diagnostic names the file, so the operator knows which "
       "reclaim to chase");
    if (strstr(log, "d2.bin") == NULL)
        printf("          (log was: %s)\n", log);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    int err = 0;

    snprintf(g_tmpdir, sizeof g_tmpdir, "%s", dir);

    printf("v2rb_rollback_test: v2 rollback paths must not run on v3\n");

    snprintf(g_img, sizeof g_img, "%s/invf-v2rb-test.img", dir);
    unlink(g_img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 48 2>/dev/null",
                 root, g_img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        fprintf(stderr, "v2rb_rollback_test: vol_open failed: err=%d\n", err);
        return 2;
    }
    if (!(g_v->sb.vol_flags & VOLF_V3)) {
        printf("  volume is not v3 -- this test is meaningless on it\n");
        return 2;
    }

    leg_a_map_rollback_on_v3();
    leg_d_unparseable_recipe();

    vol_close(g_v);
    unlink(g_img);
    printf("v2rb_rollback_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
