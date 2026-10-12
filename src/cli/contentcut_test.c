/* contentcut_test.c — WP613: torn data segments get a recovery path.
 *
 * The gap this exists for: a data segment whose framed CRC (or length-exact
 * decode) fails made the whole file read EIO, and NO checker named the
 * shape -- the recipe audit stopped at load+parse+coherence, so a sound
 * recipe over a dead segment passed `invf-fsck` silent. The volume reported
 * OK while the file was already unreadable. The fix (segment probe in the
 * audit + SEGMENT verdict + `invf-fsck --cut` prefix salvage in vol_cut.c)
 * mirrors the read path's own checks: a span is good only if its bytes
 * decode to exactly the length the entry claims.
 *
 * Fixture: torn segments are built the honest way -- a payload byte of a
 * live segment is flipped in the image file while the volume is CLOSED
 * (the power-loss shape: bytes on disk stop matching the CRC the header
 * names), then the volume is reopened and must fail loudly.
 *
 * Legs:
 *   A  sound volume (2 multi-segment + 1 single-segment + control) ->
 *      audit clean, reads bit-exact
 *   B  tear seg 2 of big1 and the only seg of single -> both reads EIO;
 *      audit DETECTS both (kind SEGMENT); damage ledger names both
 *      segment-torn
 *   C  dry-run cut -> would_cut=1 (big1), volume untouched (sizes + EIO
 *      unchanged)
 *   D  real cut + flush + close + reopen (the delta/fence durability
 *      path) -> big1 reads back EXACTLY its 128 KiB prefix, single still
 *      EIO at its old size, control bit-exact, audit names only single
 *   E  single is UNCUTTABLE (first span torn: truncating to empty is
 *      deletion wearing a repair flag) -- named, left EIO, size unchanged
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

#include "volume.h"
#include "volume_internal.h"
#include "vol_delta.h"

/* Red-leg compatibility: the SEGMENT kinds arrive with the fix. Defined
 * here so this test still COMPILES against the unpatched engine -- where
 * it must fail its assertions (no detection, no cut), not its build. */
#ifndef INVFS_RECIPE_BAD_SEGMENT
#define INVFS_RECIPE_BAD_SEGMENT 4
#endif
#ifndef INVFS_DMG_SEGMENT_TORN
#define INVFS_DMG_SEGMENT_TORN 6
#endif

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

static uint8_t *mk_pattern(size_t n, unsigned seed)
{
    uint8_t *b = (uint8_t *)malloc(n ? n : 1);
    size_t i;
    if (!b)
        return NULL;
    srand(seed);
    for (i = 0; i < n; i++)
        b[i] = (uint8_t)(rand() & 0xFF);
    return b;
}

static void close_vol(void)
{
    if (g_v) {
        vol_close(g_v);
        g_v = NULL;
    }
}

static int open_vol(void)
{
    int err = 0;
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        fprintf(stderr, "contentcut_test: vol_open failed: err=%d\n", err);
        return -1;
    }
    return 0;
}

/* The pba of recipe entry `idx` of inode `id` (engine session must be open). */
static uint64_t entry_pba(uint64_t id, size_t idx, size_t *n_out)
{
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n = 0;
    uint64_t pba = 0;
    if (vol_inode_get(g_v, id, &in) != 1)
        return 0;
    if (vol_recipe_load(g_v, in.recipe_addr, &blob, &blen) != 0 || !blob)
        return 0;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n) == 0 && ents &&
        idx < n) {
        pba = ents[idx].pba;
        if (n_out)
            *n_out = n;
    }
    free(blob);
    return pba;
}

/* Flip one payload byte of the segment at pba. The volume MUST be closed:
 * this is the disk image being torn underneath a mounted-truth format. */
static int tear_pba(uint64_t pba)
{
    int fd;
    uint8_t b;
    off_t off = (off_t)(pba * 4096 + 8 + 5);
    fd = open(g_img, O_RDWR);
    if (fd < 0)
        return -1;
    if (pread(fd, &b, 1, off) != 1) {
        close(fd);
        return -1;
    }
    b ^= 0xFF;
    if (pwrite(fd, &b, 1, off) != 1) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int read_whole(uint64_t id, uint8_t **d, size_t *n)
{
    *d = NULL;
    *n = 0;
    return vol_read_inode(g_v, id, 0, d, n);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    uint8_t *pbig = NULL, *pctl = NULL, *psingle = NULL;
    size_t bigsz = 200000, ctlsz = 150000, singsz = 10000;
    uint64_t id_big = 0, id_big2 = 0, id_ctl = 0, id_single = 0;
    uint64_t tear_pba_big = 0, tear_pba_single = 0;
    int err = 0;

    printf("contentcut_test: torn segments get detect + cut salvage\n");

    snprintf(g_img, sizeof g_img, "%s/invf-contentcut-test.img", dir);
    unlink(g_img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 root, g_img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        fprintf(stderr, "contentcut_test: vol_open failed: err=%d\n", err);
        return 2;
    }

    pbig = mk_pattern(bigsz, 613001);
    pctl = mk_pattern(ctlsz, 613002);
    psingle = mk_pattern(singsz, 613003);
    if (!pbig || !pctl || !psingle) {
        printf("  out of memory\n");
        return 2;
    }

    /* ---- leg A: a sound volume ------------------------------------ */
    id_big = vol_write_bulk(g_v, "big1.bin", pbig, bigsz, NULL);
    id_big2 = vol_write_bulk(g_v, "big2.bin", pbig, bigsz, NULL);
    id_ctl = vol_write_bulk(g_v, "ctl.bin", pctl, ctlsz, NULL);
    id_single = vol_write_bulk(g_v, "single.bin", psingle, singsz, NULL);
    ok(id_big && id_big2 && id_ctl && id_single, "leg A: four files written");
    {
        size_t n = 0;
        uint64_t p0 = entry_pba(id_big, 0, &n);
        char b[128];
        snprintf(b, sizeof b, "leg A: big1 spans %zu segments (need >=3)",
                 n);
        ok(p0 != 0 && n >= 3, b);
        n = 0;
        (void)entry_pba(id_single, 0, &n);
        snprintf(b, sizeof b, "leg A: single spans %zu segment(s) (need 1)",
                 n);
        ok(n == 1, b);
    }
    {
        invfs_recipe_audit a;
        ok(vol_recipe_audit(g_v, &a) == 0, "leg A: audit completes");
        ok(a.nfault_total == 0, "leg A: audit clean on a sound volume");
        ok(a.checked == 4, "leg A: 4 content files checked");
    }
    {
        uint8_t *d = NULL;
        size_t n = 0;
        ok(read_whole(id_big, &d, &n) == 0 && n == bigsz &&
           memcmp(d, pbig, bigsz) == 0, "leg A: big1 bit-exact");
        free(d);
    }
    /* Remember the victims' segment addresses, then tear while closed. */
    tear_pba_big = entry_pba(id_big, 2, NULL);
    tear_pba_single = entry_pba(id_single, 0, NULL);
    ok(tear_pba_big && tear_pba_single, "leg A: victim pbas resolve");
    close_vol();
    ok(tear_pba(tear_pba_big) == 0, "leg B: seg 2 of big1 torn on disk");
    ok(tear_pba(tear_pba_single) == 0, "leg B: single's only seg torn");
    if (open_vol() != 0)
        return 2;

    /* ---- leg B: honest terminal + loud detection ------------------- */
    {
        uint8_t *d = NULL;
        size_t n = 0;
        ok(read_whole(id_big, &d, &n) != 0, "leg B: big1 reads EIO");
        free(d);
        d = NULL;
        ok(read_whole(id_single, &d, &n) != 0, "leg B: single reads EIO");
        free(d);
    }
    {
        invfs_recipe_audit a;
        size_t i;
        int fbig = 0, fsingle = 0, kinds = 0;
        char b[160];
        ok(vol_recipe_audit(g_v, &a) == 0, "leg B: audit completes");
        snprintf(b, sizeof b, "leg B: audit DETECTS both torn files "
                 "(bad=%llu of checked=%llu)",
                 (unsigned long long)a.nfault_total,
                 (unsigned long long)a.checked);
        ok(a.nfault_total == 2, b);
        for (i = 0; i < a.nfault; i++) {
            if (a.fault[i].id == id_big) {
                fbig = 1;
                if (a.fault[i].kind == INVFS_RECIPE_BAD_SEGMENT)
                    kinds++;
            }
            if (a.fault[i].id == id_single) {
                fsingle = 1;
                if (a.fault[i].kind == INVFS_RECIPE_BAD_SEGMENT)
                    kinds++;
            }
        }
        ok(fbig && fsingle, "leg B: the message names big1 + single");
        ok(kinds == 2, "leg B: both classified SEGMENT (not MISSING/CORRUPT)");
    }
    {
        invfs_damaged_file dmg[INVFS_DMG_MAX];
        int nd, i, fbig = 0, fsingle = 0;
        memset(dmg, 0, sizeof dmg);
        nd = vol_damaged_files(g_v, dmg, INVFS_DMG_MAX);
        ok(nd == 2, "leg B: ledger lists 2 damaged files");
        for (i = 0; i < nd; i++) {
            if (dmg[i].id == id_big && dmg[i].kind == INVFS_DMG_SEGMENT_TORN)
                fbig = 1;
            if (dmg[i].id == id_single &&
                dmg[i].kind == INVFS_DMG_SEGMENT_TORN)
                fsingle = 1;
        }
        ok(fbig && fsingle, "leg B: ledger says segment-torn for both");
    }

    /* ---- leg C: dry-run plans, changes nothing --------------------- */
    {
        invfs_content_cut_report rep;
        invfs_inode in;
        uint8_t *d = NULL;
        size_t n = 0;
        memset(&rep, 0, sizeof rep);
        ok(vol_content_cut(g_v, 1, &rep) == 0, "leg C: dry-run completes");
        ok(rep.would_cut == 1 && rep.cut == 0, "leg C: plan cuts big1 only");
        ok(rep.torn == 2, "leg C: 2 torn files probed");
        ok(vol_inode_get(g_v, id_big, &in) == 1 && in.size == bigsz,
           "leg C: big1 size untouched by the plan");
        ok(read_whole(id_big, &d, &n) != 0, "leg C: big1 still EIO");
        free(d);
    }

    /* ---- leg D: the real cut, durable across close/reopen ---------- */
    {
        invfs_content_cut_report rep;
        memset(&rep, 0, sizeof rep);
        ok(vol_content_cut(g_v, 0, &rep) == 0, "leg D: cut completes");
        ok(rep.cut == 1, "leg D: exactly 1 file cut");
        ok(rep.verify_failed == 0, "leg D: no verify failure");
        ok(vol_flush(g_v) == 0, "leg D: durability flush holds");
    }
    close_vol();
    if (open_vol() != 0)
        return 2;
    {
        uint8_t *d = NULL;
        size_t n = 0;
        invfs_inode in;
        /* 200000 = 3x65536 + tail; seg 2 torn -> prefix is 2x65536. */
        ok(vol_inode_get(g_v, id_big, &in) == 1 && in.size == 131072,
           "leg D: big1 row is the 128 KiB good prefix after reopen");
        ok(read_whole(id_big, &d, &n) == 0 && n == 131072 &&
           memcmp(d, pbig, 131072) == 0,
           "leg D: big1 reads back EXACTLY its surviving spans");
        free(d);
        d = NULL;
        ok(read_whole(id_ctl, &d, &n) == 0 && n == ctlsz &&
           memcmp(d, pctl, ctlsz) == 0, "leg D: control bit-exact");
        free(d);
        d = NULL;
        ok(read_whole(id_big2, &d, &n) == 0 && n == bigsz &&
           memcmp(d, pbig, bigsz) == 0, "leg D: big2 (same bytes, own "
           "segments) bit-exact -- no collateral");
        free(d);
    }
    {
        invfs_recipe_audit a;
        ok(vol_recipe_audit(g_v, &a) == 0, "leg D: audit completes");
        ok(a.nfault_total == 1, "leg D: only single still offends");
        ok(a.nfault >= 1 && a.fault[0].id == id_single &&
           a.fault[0].kind == INVFS_RECIPE_BAD_SEGMENT,
           "leg D: the remainder is single, still SEGMENT");
    }

    /* ---- leg E: single is uncuttable, honestly --------------------- */
    {
        uint8_t *d = NULL;
        size_t n = 0;
        invfs_inode in;
        ok(read_whole(id_single, &d, &n) != 0, "leg E: single still EIO");
        free(d);
        ok(vol_inode_get(g_v, id_single, &in) == 1 && in.size == singsz,
           "leg E: single NOT truncated to empty (refused, size kept)");
    }

    close_vol();
    free(pbig);
    free(pctl);
    free(psingle);
    printf("contentcut_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
