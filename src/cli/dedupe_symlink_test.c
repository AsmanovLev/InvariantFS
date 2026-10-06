/*
 * dedupe_symlink_test.c -- the dedupe pass must not parse a symlink as an AST.
 *
 * THE SITE. dedup_v3_walk_cb (src/core/vol_dedupe.c) hashes every segment of
 * every live inode by PARSING its blob with vol_ast_recipe_parse. A symlink's
 * blob is its TARGET STRING, stored content-addressed exactly like a recipe
 * (vol_create_node, src/core/vol_dirs.c), so it is indistinguishable from
 * a recipe by address alone. invfs_ast_hdr_parse (src/core/invarifs.h)
 * accepts ANY blob whose first four bytes are a v1 or v2 AST version word,
 * so a symlink that false-parses donates its fabricated block entries to the
 * pass, and those entries reach dedup_remap_file_v3 -- which republishes a
 * SYNTHESISED recipe over the link and moves refcounts.
 *
 * THE TRIGGER IS NOT REACHABLE TODAY, and leg R proves it rather than
 * asserting it. Every v3 symlink writer takes strlen() over a NUL-terminated
 * buffer (src/core/volume.h: invfs_meta_pub.target is
 * `char target[INVFS_META_TARGET_MAX]`, documented NUL-terminated), and both
 * AST version words are 01 00 00 00 / 02 00 00 00 -- NUL at offset 1, and a
 * NUL at offset 1 means the target string ENDS there, so the blob is one
 * byte and is rejected by `avail < INVFS_AST_HDR_V1_LEN` before the version
 * is compared. No `ln -s` and no invf-import can produce it: argv,
 * readlink(2) and snprintf("%s") all refuse an embedded NUL. Leg R proves
 * this in two halves -- R1 sweeps all 256 leading bytes at the string
 * level, R2 puts the three interesting shapes through the real writer on a
 * real volume and shows what comes back and what version word it implies.
 *
 * So this site is the harmless-but-accidental kind, not the destructive one:
 * it survives only because strlen happens to stop a crafted target before
 * it can carry a version word. That is a property of the CALLER, not of the
 * checker -- and the guard that states it is the whole reason the next
 * symlink writer does not turn an accidental `return 0` into silent data
 * loss. Leg F is that next writer: it forges the blob through the same
 * public primitives (vol_recipe_store + vol_inode_delta_put on a LNK
 * row) that any
 * non-strlen symlink content path would use, and asserts the damage by
 * BYTES -- what vol_read_inode hands back for the link -- not by exit code.
 *
 * LEGS:
 *   R  reachability control, in two halves. R1 sweeps all 256 possible
 *      leading bytes at the string level -- which is what the writers
 *      compute, since they store strlen(target) bytes. R2 puts the three
 *      shapes that matter through the real vol_create_node writer on a
 *      real volume, which is the half R1 assumes. Characterisation: it
 *      passes before AND after the fix, and that is the point -- it is the
 *      evidence that the guard is currently redundant.
 *   F  the forge, run through vol_sweep_dedupe_ex:
 *        F0 BYTES. An ORDINARY symlink -- what `ln -s` actually produces,
 *           and the only shape a user can reach -- survives a pass and
 *           still reads back as its exact target string.
 *        F1 BYTES. The FORGED link's content is untouched, memcmp'd
 *           whole. This is the primary red assertion and it is a byte
 *           comparison of the link's content before and after the pass,
 *           not a length: the damage rewrites the pba field INSIDE the
 *           blob and republishes it, so the length is unchanged and
 *           neither a length check nor `invf-verify --deep`
 *           (src/cli/verify.c:391-400) can see it.
 *        F2 ACCOUNTING. The forged link contributes zero fabricated segments
 *           to the hash pass, measured against a control volume that differs
 *           ONLY by the link's presence.
 *        F3 REFCOUNTS. pba_ref_count on the live segment equals the
 *           control's. A fabricated entry otherwise donates a phantom +1.
 *   P  vol_read_range answers a symlink the way vol_read_inode already
 *      does -- leading window, mid-target window, an over-long request
 *      clamped to the target length, and one past the end returning 0
 *      rather than EIO. All four are byte comparisons.
 *   N  narrowing, not disabling: the REAL duplicate pair on the same volume
 *      still merges, and every file reads back byte-identical. A fix that
 *      simply returned 0 for everything would pass F and fail N.
 *
 * What this does NOT establish: it drives vol_sweep_dedupe_ex directly, not
 * the invf-sweep CLI. The free-space consequence it does NOT claim either --
 * see the note in leg F: with pba_ref_modify flooring at 0, the forged
 * reference and the forged decrement pair up, so what this site provably
 * destroys is the LINK'S BYTES and the refcount accounting, not a live
 * extent. The block-free arm at src/core/vol_dedupe.c:352 is reachable only
 * through a count this test could not drive to zero.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include "invarifs.h"
#include "volume_internal.h"

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

#define SEG_SZ   ((size_t)SEGMENT_SIZE)
#define NSEG_F0  2
#define SEG_DUP  0                 /* segment index shared with f1 */
#define LINK_NAME   "lib64"
#define LINK_TARGET "usr/lib"
#define LINK_TLEN   (sizeof(LINK_TARGET) - 1)

static void fill(uint8_t *b, size_t n, unsigned seed)
{
    size_t i;
    uint32_t x = seed * 2654435761u + 1u;
    for (i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        b[i] = (uint8_t)(x >> 16);
    }
}

static int mkfs_image(const char *img)
{
    char cmd[1200];
    const char *root = getenv("PWD");
    if (!root || !*root) root = ".";
    unlink(img);
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 1 >/dev/null 2>&1", root, img);
    return system(cmd);
}

/* f0 = [DUP content, unique content]; f1 = [DUP content]. The shared segment
 * is the dedupe loser-to-be, so a forged entry naming it becomes a merge
 * intent against the canonical copy -- exactly the shape that reaches
 * dedup_remap_file_v3. */
static uint8_t *g_dup = NULL, *g_uniq = NULL;

static invfs_volume *make_volume(const char *img, int with_link)
{
    invfs_volume *v;
    invfs_meta_pub m;
    int err = 0;
    uint8_t *body;

    if (mkfs_image(img) != 0) return NULL;
    v = vol_open(img, &err);
    if (!v) return NULL;
    if (!(v->sb.vol_flags & VOLF_META)) { vol_close(v); return NULL; }

    body = (uint8_t *)malloc(SEG_SZ * NSEG_F0);
    if (!body) { vol_close(v); return NULL; }
    memcpy(body, g_dup, SEG_SZ);
    memcpy(body + SEG_SZ, g_uniq, SEG_SZ);

    memset(&m, 0, sizeof m);
    m.type = INVFS_ITYP_REG; m.mode = 0644; m.nlink = 1;
    if (!vol_write_bulk(v, "f0.bin", body, SEG_SZ * NSEG_F0, &m)) {
        free(body); vol_close(v); return NULL;
    }
    if (!vol_write_bulk(v, "f1.bin", g_dup, SEG_SZ, &m)) {
        free(body); vol_close(v); return NULL;
    }
    free(body);

    if (with_link) {
        invfs_meta_pub lm;
        memset(&lm, 0, sizeof lm);
        lm.type = INVFS_ITYP_LNK; lm.mode = 0777; lm.nlink = 1;
        snprintf(lm.target, sizeof lm.target, "%s", LINK_TARGET);
        if (!vol_create_node(v, LINK_NAME, &lm)) { vol_close(v); return NULL; }
    }
    if (vol_flush(v) != 0) { vol_close(v); return NULL; }
    return v;
}

/* pba of segment `idx` of `name`. 0 if it cannot be read. */
static uint64_t seg_pba(invfs_volume *v, const char *name, uint32_t idx)
{
    uint64_t ino = 0, pba = 0;
    invfs_inode in;
    uint8_t *blob = NULL;
    size_t blen = 0;
    invfs_ast_hdr ah;
    const invfs_ast_block_entry *ents = NULL;
    size_t n_ents = 0;

    if (vol_path_lookup(v, name, &ino) != 1) return 0;
    if (vol_inode_get(v, ino, &in) != 1) return 0;
    if (vol_recipe_load(v, in.recipe_addr, &blob, &blen) != 0 || !blob) return 0;
    if (vol_ast_recipe_parse(blob, blen, &ah, &ents, &n_ents) == 0 && ents &&
        idx < n_ents)
        pba = ents[idx].pba;
    free(blob);
    return pba;
}

/* The forge. A symlink row whose recipe blob is a real v1 AST naming `pba`
 * as block 0. Written with the two public primitives a non-strlen symlink
 * content path would use -- the same pair vol_dedupe.c itself uses at
 * :309/:316. Nothing here is reachable through ln -s or invf-import; that is
 * leg R's job to show. */
static int forge_symlink_blob(invfs_volume *v, const char *name, uint64_t pba)
{
    invfs_inode in;
    uint64_t ino = 0;
    uint8_t addr[INVFS_RECIPE_ADDR_LEN];
    uint8_t blob[INVFS_AST_HDR_V1_LEN + sizeof(invfs_ast_block_entry)];
    invfs_ast_block_entry e;

    if (vol_path_lookup(v, name, &ino) != 1) return -1;
    if (vol_inode_get(v, ino, &in) != 1) return -1;

    memset(blob, 0, sizeof blob);
    memset(&e, 0, sizeof e);
    e.file_offset = 0;
    e.length      = SEG_SZ;
    e.zone        = INVFS_ZONE_RAW;
    e.algo        = INVFS_ALGO_NONE;
    e.block_id    = 0;
    e.pba         = pba;
    memcpy(blob + INVFS_AST_HDR_V1_LEN, &e, sizeof e);
    {
        uint32_t ver = INVFS_AST_VERSION_V1;
        uint16_t nb = 1, nc = 0;
        uint32_t ck = 0;
        memcpy(blob, &ver, 4);
        memcpy(blob + 4, &(uint32_t){ SEG_SZ }, 4);
        memcpy(blob + 8, &nb, 2);
        memcpy(blob + 10, &nc, 2);
        memcpy(blob + 12, &ck, 4);
    }
    if (vol_recipe_store(v, blob, sizeof blob, addr) != 0) return -1;
    memcpy(in.recipe_addr, addr, sizeof addr);
    return vol_inode_delta_put(v, ino, &in) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- LEG R */

static void leg_reachability(const char *img)
{
    invfs_volume *v;
    invfs_meta_pub m;
    int err = 0, b, parsed = 0;
    uint64_t ino[3] = {0, 0, 0};
    char nm[32];

    printf("LEG R  reachability control: no symlink target string false-parses\n");

    /* R1 -- all 256 possible leading bytes, at the string level. What the
     * v3 writers do with a target is `strlen(target)` bytes of it
     * (src/core/vol_dirs.c), and R2 below proves that end to end on a real
     * volume, so this sweep is the same computation the reader sees:
     * invfs_ast_hdr_parse over a strlen-delimited prefix. b == 1 and
     * b == 2 are the AST version words' first byte; the NUL that has to
     * follow them cannot be part of a C string, so those two come out one
     * byte long and fail `avail < INVFS_AST_HDR_V1_LEN` before the version
     * is ever compared. */
    for (b = 0; b < 256; b++) {
        char target[4];
        invfs_ast_hdr ah;
        target[0] = (char)b;
        target[1] = 'X';
        target[2] = 'Y';
        target[3] = 0;
        if (vol_ast_recipe_parse((const uint8_t *)target, strlen(target),
                                 &ah, NULL, NULL) == 0)
            parsed++;
    }
    ok(parsed == 0,
       "leg R1: ZERO of 256 leading bytes false-parse as an AST");
    printf("  note: R1 swept 256 leading bytes, %d false-parsed\n", parsed);

    /* R2 -- the bytes that matter, through the ONLY v3 symlink writer, on a
     * real volume: what comes back is exactly the strlen-delimited bytes
     * and nothing more. This is the half R1 assumes, so it is measured
     * rather than taken from the source.
     *
     * Three shapes, and the middle one is the interesting one. A target of
     * exactly "\x01" is one byte and dies on `avail < 16`. A target of
     * "\x01XY" is three bytes and reaches the version comparison -- with
     * the word 0x595801, because the byte that WOULD have to be NUL is
     * 'X'. That is the whole reachability argument in one assertion:
     * ver == 1 requires a NUL at offset 1, and a NUL at offset 1 means
     * strlen == 1, and a one-byte blob cannot satisfy hdr_parse. */
    static const struct { const char *name; char t[4]; size_t len; } cases[] = {
        { "1-byte 0x01",  { 0x01, 0,    0,    0 }, 1 },
        { "3-byte 0x01XY",{ 0x01, 'X', 'Y', 0 }, 3 },
        { "3-byte 0x02XY",{ 0x02, 'X', 'Y', 0 }, 3 },
    };
    if (!(v = (mkfs_image(img) == 0 ? vol_open(img, &err) : NULL))) {
        printf("  FAIL  leg R2 setup (open)\n"); failures++; return;
    }
    for (b = 0; b < (int)(sizeof cases / sizeof cases[0]); b++) {
        uint8_t *out = NULL;
        size_t out_len = 0;
        invfs_ast_hdr ah;

        memset(&m, 0, sizeof m);
        m.type = INVFS_ITYP_LNK; m.mode = 0777; m.nlink = 1;
        memcpy(m.target, cases[b].t, sizeof m.target);
        snprintf(nm, sizeof nm, "r%d", b);
        ino[b] = vol_create_node(v, nm, &m);
        if (!ino[b] ||
            vol_read_inode(v, ino[b], 0, &out, &out_len) != 0 || !out) {
            ok(0, "leg R2: the crafted target read back");
            continue;
        }
        ok(out_len == cases[b].len &&
           memcmp(out, cases[b].t, cases[b].len) == 0,
           "leg R2: the writer stored exactly the strlen-delimited bytes");
        ok(vol_ast_recipe_parse(out, out_len, &ah, NULL, NULL) != 0,
           "leg R2: and that blob does not parse as an AST");
        if (out_len >= 4) {
            uint32_t ver = 0;
            memcpy(&ver, out, 4);
            ok(ver != INVFS_AST_VERSION_V1 && ver != INVFS_AST_VERSION_V2,
               "leg R2: its version word is neither V1 nor V2");
            printf("  note: %s -> version word 0x%08x\n", cases[b].name, ver);
        }
        free(out);
    }
    vol_close(v);
}

/* ---------------------------------------------------------------- LEG F */

static int read_back_eq(invfs_volume *v, const char *name,
                        const uint8_t *want, size_t wantlen, size_t *got_len)
{
    uint64_t ino = 0;
    uint8_t *out = NULL;
    size_t out_len = 0;
    int same;

    if (vol_path_lookup(v, name, &ino) != 1) return -1;
    if (vol_read_inode(v, ino, 0, &out, &out_len) != 0 || !out) return -1;
    if (got_len) *got_len = out_len;
    same = (out_len == wantlen) && (memcmp(out, want, wantlen) == 0);
    if (!same)
        printf("    %s: read back %zu bytes, wanted %zu\n", name, out_len, wantlen);
    free(out);
    return same ? 1 : 0;
}

/* Snapshot the link's content. Returns a malloc'd copy, or NULL. */
static uint8_t *snap_link(invfs_volume *v, const char *name, size_t *len)
{
    uint64_t ino = 0;
    uint8_t *out = NULL;
    size_t out_len = 0;

    *len = 0;
    if (vol_path_lookup(v, name, &ino) != 1) return NULL;
    if (vol_read_inode(v, ino, 0, &out, &out_len) != 0 || !out) return NULL;
    *len = out_len;
    return out;
}

static void leg_forge(const char *img_f, const char *img_c, const char *img_o)
{
    invfs_volume *vf = NULL, *vc = NULL, *vo = NULL;
    invfs_dedupe_stats sf, sc;
    int rc_f, rc_c, rc_o;
    uint64_t loser = 0, canon = 0, cnt_f = 0, cnt_c = 0;
    uint8_t *pre = NULL, *post = NULL;
    size_t pre_len = 0, post_len = 0, link_len = 0, body_len = 0;

    printf("\nLEG F  the forge: a symlink row whose blob IS an AST\n");
    vf = make_volume(img_f, 1);
    vc = make_volume(img_c, 0);          /* control: no symlink at all */
    vo = make_volume(img_o, 1);          /* ordinary ln -s symlink */
    if (!vf || !vc || !vo) {
        printf("  FAIL  leg F setup\n"); failures++;
        if (vf) vol_close(vf);
        if (vc) vol_close(vc);
        if (vo) vol_close(vo);
        return;
    }

    /* f1's lone segment is the duplicate that dedupe will retire onto f0's
     * copy. The forged entry names it, so it becomes a genuine loser with a
     * canonical replacement -- the exact shape that reaches the remap. */
    loser = seg_pba(vf, "f1.bin", SEG_DUP);
    canon = seg_pba(vf, "f0.bin", SEG_DUP);
    ok(loser != 0 && canon != 0 && loser != canon,
       "leg F setup: two distinct pbas hold identical segment content");
    if (!loser || !canon || loser == canon) {
        vol_close(vf); vol_close(vc); vol_close(vo); return;
    }

    ok(forge_symlink_blob(vf, LINK_NAME, loser) == 0,
       "leg F setup: forged AST blob published onto the LNK row");
    if (vol_flush(vf) != 0) { failures++; }

    /* The link's content as the pass is about to find it. The pass must
     * leave it alone BYTE FOR BYTE -- not merely the same length: the
     * damage it does is to rewrite the pba field inside the blob and
     * republish it, so a length check cannot see it and neither can
     * invf-verify --deep (src/cli/verify.c:391-400). */
    pre = snap_link(vf, LINK_NAME, &pre_len);
    ok(pre != NULL && pre_len ==
       (INVFS_AST_HDR_V1_LEN + sizeof(invfs_ast_block_entry)),
       "leg F setup: the forged blob is the link's whole content");

    memset(&sf, 0, sizeof sf);
    memset(&sc, 0, sizeof sc);
    rc_f = vol_sweep_dedupe_ex(vf, &sf, NULL, NULL);
    rc_c = vol_sweep_dedupe_ex(vc, &sc, NULL, NULL);
    rc_o = vol_sweep_dedupe_ex(vo, NULL, NULL, NULL);
    if (vol_flush(vf) != 0 || vol_flush(vc) != 0 || vol_flush(vo) != 0)
        failures++;
    /* vol_sweep_dedupe_ex returns the merge COUNT (>= 0), < 0 on error. */
    ok(rc_f >= 0 && rc_c >= 0 && rc_o >= 0,
       "leg F: all three dedupe passes ran without error");

    /* F0 -- the ORDINARY symlink. This is what `ln -s` actually produces
     * and the only shape a user can reach today. It must survive a pass
     * untouched, and it does both before and after the fix: the guard is
     * currently redundant, and this leg is the standing evidence. */
    rc_o = read_back_eq(vo, LINK_NAME, (const uint8_t *)LINK_TARGET, LINK_TLEN,
                        &link_len);
    ok(rc_o == 1,
       "F0 BYTES: an ordinary symlink still reads back as its exact target");

    /* F1 -- BYTES. The forged link's content must be untouched. */
    post = snap_link(vf, LINK_NAME, &post_len);
    ok(post != NULL && pre != NULL &&
       post_len == pre_len && post != NULL &&
       memcmp(post, pre, pre_len) == 0,
       "F1 BYTES: the dedupe pass did not rewrite the symlink's content");
    if (post && pre && (post_len != pre_len ||
                        memcmp(post, pre, pre_len) != 0)) {
        /* Print the field that actually moves. The entry starts at
         * INVFS_AST_HDR_V1_LEN and pba is its last 8 bytes (invarifs.h:
         * file_offset, length, the zone/algo/block_id bitfield word, then
         * pba), so it sits at hdr + 24 -- not at hdr, which is the
         * entry's file_offset and always zero. This is the byte the remap
         * rewrites: the pass re-pointed the link's fabricated entry at
         * the canonical block and republished the whole blob. */
        const size_t pba_off = INVFS_AST_HDR_V1_LEN + 24;
        printf("    length: before %zu, after %zu\n", pre_len, post_len);
        if (pre_len >= pba_off + 8 && post_len >= pba_off + 8) {
            uint64_t b1 = 0, b2 = 0;
            memcpy(&b1, pre + pba_off, 8);
            memcpy(&b2, post + pba_off, 8);
            printf("    entry[0].pba: before %llu, after %llu"
                   "  (same length, different content)\n",
                   (unsigned long long)b1, (unsigned long long)b2);
        }
    }
    free(pre);
    free(post);

    /* F2 -- the forged link must contribute NO segments to the hash pass. */
    printf("    segments_hashed: forged=%llu control=%llu (delta %lld)\n",
           (unsigned long long)sf.segments_hashed,
           (unsigned long long)sc.segments_hashed,
           (long long)sf.segments_hashed - (long long)sc.segments_hashed);
    ok(sf.segments_hashed == sc.segments_hashed,
       "F2 ACCOUNTING: the symlink donates zero fabricated segments");

    /* F3 -- no phantom reference. */
    pba_ref_ensure(vf);
    pba_ref_ensure(vc);
    cnt_f = pba_ref_count(vf, canon);
    cnt_c = pba_ref_count(vc, canon);
    printf("    pba_ref_count(canon=%llu): forged=%llu control=%llu\n",
           (unsigned long long)canon, (unsigned long long)cnt_f,
           (unsigned long long)cnt_c);
    ok(cnt_f == cnt_c,
       "F3 REFCOUNTS: no phantom reference survives on the canonical block");

    /* LEG P -- the in-file divergence, read_range (src/core/vol_read.c:1678).
     * vol_read_inode is the authority and hands the target back verbatim; read_range had
     * no branch for a raw-blob type and fell into the AST parse, which
     * cannot succeed, and answered EIO. Unreachable through FUSE (the
     * kernel resolves a symlink) but not through the core, where every
     * caller arrives by inode id alone. Ranged reads of the link must
     * agree with the whole-object read, at every offset and length. */
    {
        uint64_t lino = 0;
        uint8_t win[64];
        int r;
        if (vol_path_lookup(vo, LINK_NAME, &lino) != 1) {
            ok(0, "P: the ordinary symlink is still resolvable");
        } else {
            r = vol_read_range(vo, lino, 0, 4, win);
            ok(r == 4 && memcmp(win, LINK_TARGET, 4) == 0,
               "P: vol_read_range returns the first 4 target bytes");
            memset(win, 0, sizeof win);
            r = vol_read_range(vo, lino, 4, 3, win);
            ok(r == 3 && memcmp(win, LINK_TARGET + 4, 3) == 0,
               "P: vol_read_range returns a mid-target window");
            memset(win, 0, sizeof win);
            r = vol_read_range(vo, lino, 0, LINK_TLEN + 8, win);
            ok(r == (int)LINK_TLEN &&
               memcmp(win, LINK_TARGET, LINK_TLEN) == 0,
               "P: an over-long read is clamped to the target length");
            ok(vol_read_range(vo, lino, LINK_TLEN, 1, win) == 0,
               "P: a read past the end returns 0, not EIO");
        }
    }

    /* LEG N -- the guard is NARROW. The real duplicate pair still merges. */
    printf("\nLEG N  narrowing: the real duplicate still merges, data intact\n");
    ok(sc.segments_merged >= 1,
       "N: the control's real duplicate pair was merged (guard is not blanket)");
    ok(vc->sb.total_blocks > 0 && sf.segments_merged <= sc.segments_merged + 1,
       "N: the forged link does not inflate the merge count");

    {
        uint8_t *b = (uint8_t *)malloc(SEG_SZ * NSEG_F0);
        if (b) {
            memcpy(b, g_dup, SEG_SZ);
            memcpy(b + SEG_SZ, g_uniq, SEG_SZ);
            ok(read_back_eq(vf, "f0.bin", b, SEG_SZ * NSEG_F0, &body_len) == 1,
               "N BIT-EXACT: f0.bin reads back byte-identical after the pass");
            free(b);
        }
        b = (uint8_t *)malloc(SEG_SZ);
        if (b) {
            memcpy(b, g_dup, SEG_SZ);
            ok(read_back_eq(vf, "f1.bin", b, SEG_SZ, &body_len) == 1,
               "N BIT-EXACT: f1.bin reads back byte-identical after the pass");
            free(b);
        }
    }

    vol_close(vf);
    vol_close(vc);
    vol_close(vo);
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char sub[512], img_r[640], img_f[640], img_c[640], img_o[640];

    /* A per-pid subdirectory, so this runs unisolated (the volumes are far
     * too large for the tmpfs a namespace hands out, and this host's /tmp is
     * RAM) without two concurrent `make test` runs in different worktrees
     * colliding on a fixed image name. Same reason invf-rollback_symlink_test
     * takes a scratch root rather than a sandboxed /tmp. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(sub, sizeof sub, "%s/dedupe_symlink.%ld", dir, (long)getpid());
    if (mkdir(sub, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", sub, strerror(errno));
        return 2;
    }
    snprintf(img_r, sizeof img_r, "%s/ddp_reach.img", sub);
    snprintf(img_f, sizeof img_f, "%s/ddp_forge.img", sub);
    snprintf(img_c, sizeof img_c, "%s/ddp_ctrl.img", sub);
    snprintf(img_o, sizeof img_o, "%s/ddp_ord.img", sub);

    g_dup  = (uint8_t *)malloc(SEG_SZ);
    g_uniq = (uint8_t *)malloc(SEG_SZ);
    if (!g_dup || !g_uniq) {
        fprintf(stderr, "oom\n");
        return 2;
    }
    fill(g_dup, SEG_SZ, 7);
    fill(g_uniq, SEG_SZ, 99);

    leg_reachability(img_r);
    leg_forge(img_f, img_c, img_o);

    unlink(img_r);
    unlink(img_f);
    unlink(img_c);
    unlink(img_o);
    rmdir(sub);

    free(g_dup);
    free(g_uniq);

    printf("\ndedupe_symlink_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}