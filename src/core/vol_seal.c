/* vol_seal.c — native v3 content seal (WP201). Par2-inspired, no compat.
 *
 * WHAT SEALED MEANS HERE. At seal time the engine walks the live namespace
 * (vol_walk_strict + receipt — a short walk fails the seal, never a partial
 * one), sorts dirents by name, and streams every REG file's bytes (plus
 * symlink targets) in that order through 64 KiB symbols into groups of k.
 * Each group gains m Reed-Solomon parity symbols (rs.c, GF(2^8)); parity is
 * the hidden file "\x01seal-parity", the manifest (per-file
 * name/type/size/BLAKE3 + per-group shape + CRC32C) is the hidden file
 * "\x01seal-footer". The footer is committed LAST: no valid footer = no
 * seal, and a crash anywhere before that leaves the prior seal (or no seal)
 * behind — never a half-trusted one. After both commits a full re-verify
 * (re-walk, re-stream, re-encode, byte-compare against what is on disk)
 * runs before success is reported: a seal that cannot prove itself is a
 * failure, not a seal.
 *
 * WHAT V1 DOES NOT DO (locked, see the WP doc): no healing — verify reports
 * into the existing counters, never rewrites (seal_recover_segment stays a
 * loud -1); no incremental re-seal — every seal is a full recompute (the
 * allocator's dirty-stripe bitmap is still maintained, v1 just never
 * consults it); no second layer (l2_* report fields stay 0); no par2 byte
 * compat; no giant stripes (k+m <= 256).
 *
 * WHERE THE CONFIG LIVES. vol_redun_config stores a menu-checked (k, algo,
 * m) in v->rd IN MEMORY ONLY (rd_present=1); nothing reaches block 0, so a
 * --dry-run that configures cannot mutate. The on-disk record is the footer
 * prelude; vol_redun_state reports session config first, footer second,
 * defaults last. RDP0 (the v2 block-0 descriptor) is deliberately NOT
 * written: v1 parity names file bytes, not absolute blocks, so a sealed
 * volume stays resizable and there is no v2-meaningful geometry to persist
 * there. (The v->rd reuse as a config carrier wants a comment fix in
 * volume_internal.h/volume.h, which this WP may not touch — noted in TODO.)
 *
 * VERIFY SEMANTICS (invfs_seal_verify, v1 mapping onto the v2 fields):
 *   sealed     groups re-verified against stored parity
 *   mismatched groups whose re-encoded parity differs (data drift OR parity
 *              damage — v1 does not pretend to tell them apart)
 *   missing    manifest files gone from the namespace (per FILE, exact)
 *   extra      live files absent from the manifest (per FILE, exact)
 * Any post-seal write/delete/create makes at least one counter nonzero, so
 * `invf-verify --deep` fails loudly: no silent staleness, ever. A volume
 * with no footer at all reports all zeros (unsealed, silent — seal is
 * opt-in). A footer file that EXISTS but does not strictly parse is also
 * reported unsealed (never trusted) plus one stderr note — absent, not
 * half-believed.
 *
 * RAM DISCIPLINE. File content is never held whole: 64 KiB stream chunks,
 * one group ((k+m)*64 KiB, <= ~1.4 MiB) at a time. The sorted name list is
 * metadata (O(files)), written through ranged-write sessions as produced.
 * Manifest hashes are BLAKE3-256.
 */

#include "volume_internal.h"
#include "vol_seal.h"
#include "vol_walk.h"
#include "vol_fault.h"
#include "rs.h"
#include "blake3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* (Re)Allocate the dirty bitmap and mark every shadow block: the state
 * before that moment is simply not tracked, so the next reseal must be a
 * full pass. */
void seal_dirty_reset(invfs_volume *v)
{
    size_t bytes = (size_t)((v->sb.shadow_zone_blocks + 7) / 8);
    if (!v->seal_dirty)
        v->seal_dirty = (uint8_t *)malloc(bytes ? bytes : 1);
    if (v->seal_dirty)
        memset(v->seal_dirty, 0xFF, bytes);
}


/* Mark the shadow-zone blocks [pba, pba+n) dirty (their stripes need a
 * parity recompute at the next reseal). No-op outside the shadow zone or
 * when no seal config exists (the bitmap is NULL then). */
void seal_dirty_mark(invfs_volume *v, uint64_t pba, uint64_t n)
{
    uint64_t ss = v->sb.shadow_zone_start;
    uint64_t b, end;
    if (!v->seal_dirty || !n) return;
    if (pba + n <= ss || pba >= ss + v->sb.shadow_zone_blocks) return;
    if (pba < ss) { n -= ss - pba; pba = ss; }
    end = pba + n;
    if (end > ss + v->sb.shadow_zone_blocks)
        end = ss + v->sb.shadow_zone_blocks;
    for (b = pba; b < end; b++)
        bit_set(v->seal_dirty, b - ss);
}


/* ---- the retired entry points ------------------------------------- */

/* v1 is detect-only: there is no redundancy the read path could rebuild
 * from, so a failed segment fails exactly as it would unsealed — loudly.
 * (Scrub auto-heal is a later WP; this must not grow a quiet path.) */
int seal_recover_segment(invfs_volume *v, uint64_t pba, uint64_t plen,
                         uint32_t *csize_out, uint8_t **blob_out)
{
    (void)v; (void)pba; (void)plen;
    (void)csize_out; (void)blob_out;
    return -1;
}

/* ================= pure codec half (also the unit-test surface) ========== */

static void put16(uint8_t *p, uint16_t x)
{
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8);
}

static void put32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
}

static void put64(uint8_t *p, uint64_t x)
{
    p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8);
    p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24);
    p[4] = (uint8_t)(x >> 32); p[5] = (uint8_t)(x >> 40);
    p[6] = (uint8_t)(x >> 48); p[7] = (uint8_t)(x >> 56);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/* The menu as data, so the gate (not just the driver) enforces it. */
static const struct { unsigned k, m; } seal_menu_pairs[] = {
    { 20, 1 }, { 9, 1 }, { 8, 2 }, { 6, 2 },
};

static int seal_menu_m(unsigned k)
{
    size_t i;
    for (i = 0; i < sizeof seal_menu_pairs / sizeof seal_menu_pairs[0]; i++)
        if (seal_menu_pairs[i].k == k)
            return (int)seal_menu_pairs[i].m;
    return -1;
}

int seal_menu(int pct, unsigned *k_out, unsigned *m_out)
{
    unsigned k = 9, m = 1;          /* pct 10: the default */
    switch (pct) {
    case 5:  k = 20; m = 1; break;
    case 10: k = 9;  m = 1; break;
    case 20: k = 8;  m = 2; break;
    case 25: k = 6;  m = 2; break;
    default: k = 9;  m = 1; break;  /* unknown -> default, never garbage */
    }
    if (k_out) *k_out = k;
    if (m_out) *m_out = m;
    return 0;
}

void seal_plan(unsigned k, unsigned m, uint64_t data_bytes,
               uint64_t *groups_out, uint64_t *parity_bytes_out)
{
    uint64_t per, g;
    if (!k) k = 9;
    per = (uint64_t)k * SEAL_SYM_BYTES;
    g = per ? (data_bytes + per - 1) / per : 0;
    if (groups_out) *groups_out = g;
    if (parity_bytes_out) *parity_bytes_out = g * (uint64_t)m * SEAL_SYM_BYTES;
}

size_t seal_prelude_enc(uint8_t out[SEAL_PRELUDE_LEN],
                        unsigned k, unsigned m, unsigned algo, uint64_t seq)
{
    if (!out || !k || !m || k + m > 256 || !seq)
        return 0;
    if (algo != RS_ALGO_VM && algo != RS_ALGO_CAUCHY)
        return 0;
    memcpy(out, SEAL_FOOTER_MAGIC, 8);
    put16(out + 8, SEAL_FORMAT_VERSION);
    put16(out + 10, (uint16_t)k);
    put16(out + 12, (uint16_t)m);
    put32(out + 14, SEAL_SYM_BYTES);
    put16(out + 18, (uint16_t)algo);
    put64(out + 20, seq);
    return SEAL_PRELUDE_LEN;
}

size_t seal_entry_enc(uint8_t *out, size_t cap, const char *name,
                      unsigned ftype, uint64_t size, const uint8_t hash[32])
{
    size_t nl, need;
    int hashed = (ftype == SEAL_FT_REG || ftype == SEAL_FT_LNK);
    if (!out || !name) return 0;
    nl = strlen(name);
    if (!nl || nl > INVFS_MAX_NAME) return 0;
    need = 2 + nl + 1 + 8 + (size_t)(hashed ? 32 : 0);
    if (cap < need) return 0;
    put16(out, (uint16_t)nl);
    memcpy(out + 2, name, nl);
    out[2 + nl] = (uint8_t)ftype;
    put64(out + 3 + nl, size);
    if (hashed) {
        if (!hash) return 0;
        memcpy(out + 11 + nl, hash, 32);
    }
    return need;
}

size_t seal_group_enc(uint8_t out[10], unsigned datasyms, uint64_t databytes)
{
    if (!out || !datasyms || datasyms > 256) return 0;
    put16(out, (uint16_t)datasyms);
    put64(out + 2, databytes);
    return 10;
}

size_t seal_trailer_enc(uint8_t out[SEAL_TRAILER_LEN], const uint8_t *body,
                        size_t body_len, uint32_t nfiles, uint32_t ngroups)
{
    /* The CRC covers every footer byte before the CRC field itself — body
     * PLUS the two count words (the parser reads it the same way). */
    uint8_t counts[8];
    uint32_t crc;
    if (!out) return 0;
    put32(counts, nfiles);
    put32(counts + 4, ngroups);
    crc = invfs_crc32c_update(0, body, body ? body_len : 0);
    crc = invfs_crc32c_update(crc, counts, sizeof counts);
    put32(out, nfiles);
    put32(out + 4, ngroups);
    put32(out + 8, crc);
    return SEAL_TRAILER_LEN;
}

size_t seal_parhdr_enc(uint8_t out[SEAL_PARHDR_LEN],
                       unsigned k, unsigned m, uint32_t ngroups, uint64_t seq)
{
    uint32_t hcrc;
    if (!out || !k || !m || k + m > 256) return 0;
    memcpy(out, SEAL_PARITY_MAGIC, 8);
    put16(out + 8, SEAL_FORMAT_VERSION);
    put16(out + 10, (uint16_t)k);
    put16(out + 12, (uint16_t)m);
    put32(out + 14, SEAL_SYM_BYTES);
    put32(out + 18, ngroups);
    put64(out + 22, seq);
    hcrc = invfs_crc32c(out, 30);
    put32(out + 30, hcrc);
    return SEAL_PARHDR_LEN;
}

/* Strict footer parse. Every integer is bounds-checked before it is used;
 * the buffer is untrusted (torn reads) and any anomaly is "not sealed". */
int seal_footer_parse(const uint8_t *buf, size_t len, seal_footinfo *info_out,
                      seal_entry **entries_out, seal_group **groups_out)
{
    seal_footinfo info;
    seal_entry *ents = NULL;
    seal_group *grps = NULL;
    size_t pos;
    uint32_t nf, ng, crc_file, crc_calc;
    uint32_t i;

    if (entries_out) *entries_out = NULL;
    if (groups_out) *groups_out = NULL;
    if (!buf || len < SEAL_PRELUDE_LEN + SEAL_TRAILER_LEN)
        return 1;
    if (memcmp(buf, SEAL_FOOTER_MAGIC, 8) != 0)
        return 1;
    if (get16(buf + 8) != SEAL_FORMAT_VERSION)
        return 1;
    info.k = get16(buf + 10);
    info.m = get16(buf + 12);
    info.sym = get32(buf + 14);
    if (!info.k || !info.m || (unsigned)info.k + (unsigned)info.m > 256)
        return 1;
    if (info.sym != SEAL_SYM_BYTES)
        return 1;
    if (get16(buf + 18) != RS_ALGO_VM && get16(buf + 18) != RS_ALGO_CAUCHY)
        return 1;
    info.seq = get64(buf + 20);
    if (!info.seq)
        return 1;

    /* Trailer first (fixed offset from the end), so absurd counts die
     * before any allocation: entries must fit between prelude and trailer. */
    nf = get32(buf + len - 12);
    ng = get32(buf + len - 8);
    crc_file = get32(buf + len - 4);
    if (nf > 16777216u || ng > 16777216u)
        return 1;
    crc_calc = invfs_crc32c(buf, len - 4);
    if (crc_calc != crc_file)
        return 1;

    /* Walk entries + groups with a cursor; exact consumption or refusal. */
    pos = SEAL_PRELUDE_LEN;
    if (entries_out || groups_out) {
        if (nf && (ents = calloc(nf, sizeof *ents)) == NULL)
            return -1;
        if (ng && (grps = calloc(ng, sizeof *grps)) == NULL) {
            free(ents);
            if (entries_out) *entries_out = NULL;
            return -1;
        }
    }
    for (i = 0; i < nf; i++) {
        uint16_t nl;
        uint8_t ft;
        int hashed;
        if (pos + 2 > len - SEAL_TRAILER_LEN) goto refuse;
        nl = get16(buf + pos);
        if (!nl || nl > INVFS_MAX_NAME) goto refuse;
        if (pos + 2 + nl + 1 + 8 > len - SEAL_TRAILER_LEN) goto refuse;
        ft = buf[pos + 2 + nl];
        hashed = (ft == SEAL_FT_REG || ft == SEAL_FT_LNK);
        if (hashed && pos + 2 + nl + 1 + 8 + 32 > len - SEAL_TRAILER_LEN)
            goto refuse;
        if (ents) {
            memcpy(ents[i].name, buf + pos + 2, nl);
            ents[i].name[nl] = 0;
            ents[i].namelen = nl;
            ents[i].ftype = ft;
            ents[i].size = get64(buf + pos + 3 + nl);
            ents[i].has_hash = hashed;
            if (hashed)
                memcpy(ents[i].hash, buf + pos + 11 + nl, 32);
        }
        pos += 2 + nl + 1 + 8 + (size_t)(hashed ? 32 : 0);
    }
    for (i = 0; i < ng; i++) {
        uint16_t ds;
        uint64_t db;
        if (pos + 10 > len - SEAL_TRAILER_LEN) goto refuse;
        ds = get16(buf + pos);
        db = get64(buf + pos + 2);
        if (!ds || ds > info.k || !db || db > (uint64_t)ds * info.sym)
            goto refuse;
        if (grps) {
            grps[i].datasyms = ds;
            grps[i].databytes = db;
        }
        pos += 10;
    }
    if (pos != len - SEAL_TRAILER_LEN) goto refuse;
    info.nfiles = nf;
    info.ngroups = ng;
    if (info_out) *info_out = info;
    if (entries_out) *entries_out = ents; else free(ents);
    if (groups_out) *groups_out = grps; else free(grps);
    return 0;

refuse:
    free(ents);
    free(grps);
    if (entries_out) *entries_out = NULL;
    if (groups_out) *groups_out = NULL;
    return 1;
}

int seal_parhdr_parse(const uint8_t *buf, size_t len,
                      unsigned *k_out, unsigned *m_out, uint32_t *ngroups_out,
                      uint64_t *seq_out)
{
    unsigned k, m;
    uint32_t ng;
    uint64_t sq;
    if (!buf || len < SEAL_PARHDR_LEN)
        return 1;
    if (memcmp(buf, SEAL_PARITY_MAGIC, 8) != 0)
        return 1;
    if (get16(buf + 8) != SEAL_FORMAT_VERSION)
        return 1;
    k = get16(buf + 10);
    m = get16(buf + 12);
    if (!k || !m || k + m > 256)
        return 1;
    if (get32(buf + 14) != SEAL_SYM_BYTES)
        return 1;
    ng = get32(buf + 18);
    /* 0xFFFFFFFF is the in-progress placeholder a seal writes before the
     * group count is known (rewritten before the footer lands); it must
     * never verify. */
    if (ng == 0xFFFFFFFFu || ng > 16777216u)
        return 1;
    sq = get64(buf + 22);
    if (!sq)
        return 1;
    if (invfs_crc32c(buf, 30) != get32(buf + 30))
        return 1;
    if (k_out) *k_out = k;
    if (m_out) *m_out = m;
    if (ngroups_out) *ngroups_out = ng;
    if (seq_out) *seq_out = sq;
    return 0;
}

/* ================= engine half (volume-coupled) ========================== */

#define SEAL_MAX_NGROUPS 16777216u

/* Forward declarations (defined below vol_seal). */
typedef struct seal_grouper seal_grouper;
typedef struct seal_crc seal_crc;
/* WP401 scrub ledger sink: growable array owned by the pass. A single
 * walk fills it, so duplicate marks of one group (a hole's `missing`
 * upgraded by a later parity disagreement to `mismatched`) deduplicate
 * by scan -- a count-then-fill two-pass overcounts them, because the
 * count pass has no array to scan. NULL sink = legacy verify behavior. */
typedef struct {
    invfs_seal_badgroup *arr;
    size_t n, cap;
} scrub_sink;
static int seal_emit_group(invfs_volume *v, invfs_wsession *pws,
                           seal_grouper *gr, uint64_t *par_off,
                           invfs_wsession *fws, uint64_t *foot_off,
                           seal_crc *fcrc, uint32_t *ngroups,
                           unsigned datasyms, uint64_t databytes);
static int seal_verify_locked(invfs_volume *v, invfs_seal_verify *out,
                              uint64_t expect_seq,
                              scrub_sink *sink, int *untrusted_out);
static int seal_feed_verify(invfs_volume *v, seal_grouper *gr,
                            const uint8_t *data, size_t len, uint64_t pino,
                            uint64_t *poff, uint32_t *gi, uint32_t ngroups,
                            invfs_seal_verify *out, scrub_sink *sink);

/* In-memory config carrier: v->rd with l1_algo==1. (RDP0 is never written;
 * the footer prelude is the on-disk record.) */
static int seal_mem_cfg(const invfs_volume *v, unsigned *k, unsigned *algo,
                        unsigned *m)
{
    if (!v || !v->rd_present || v->rd.l1_algo != 1)
        return 0;
    if (seal_menu_m(v->rd.k1) < 0)
        return 0;
    if (k) *k = v->rd.k1;
    if (m) *m = (unsigned)v->rd.m2;
    if (algo) {
        unsigned a = (unsigned)v->rd.l2_algo;
        *algo = (a == RS_ALGO_CAUCHY || a == RS_ALGO_VM) ? a : RS_ALGO_VM;
    }
    return 1;
}

/* Read+strict-check the footer prelude only. 0 = sealed prelude (info
 * filled), 1 = absent-or-untrusted, -1 = I/O error. */
static int seal_read_prelude(invfs_volume *v, seal_footinfo *info,
                             uint64_t *size_out)
{
    uint64_t ino = 0, fsz = 0, ctime = 0;
    uint8_t pre[SEAL_PRELUDE_LEN];
    int rc, r;
    if (!v || !info)
        return -1;
    rc = vol_find_rc(v, SEAL_FOOTER_NAME, &ino);
    if (rc != 1 || !ino)
        return 1;                       /* absent (or unreadable: unsealed) */
    if (vol_stat_full(v, SEAL_FOOTER_NAME, &ino, &fsz, &ctime) != 0)
        return -1;
    if (fsz < SEAL_PRELUDE_LEN + SEAL_TRAILER_LEN)
        return 1;                       /* torn: never trusted */
    if (size_out) *size_out = fsz;
    r = vol_read_range(v, ino, 0, sizeof pre, pre);
    if (r != (int)sizeof pre)
        return r < 0 ? -1 : 1;
    if (memcmp(pre, SEAL_FOOTER_MAGIC, 8) != 0)
        return 1;
    if (get16(pre + 8) != SEAL_FORMAT_VERSION)
        return 1;
    info->k = get16(pre + 10);
    info->m = get16(pre + 12);
    info->sym = get32(pre + 14);
    if (!info->k || !info->m ||
        (unsigned)info->k + (unsigned)info->m > 256 ||
        info->sym != SEAL_SYM_BYTES)
        return 1;
    if (get16(pre + 18) != RS_ALGO_VM && get16(pre + 18) != RS_ALGO_CAUCHY)
        return 1;
    info->seq = get64(pre + 20);
    if (!info->seq)
        return 1;
    info->nfiles = 0;
    info->ngroups = 0;
    return 0;
}

int vol_redun_state(const invfs_volume *v, uint32_t *k1, int *l2_algo,
                    uint32_t *m2)
{
    unsigned k = 9, a = RS_ALGO_VM, m = 1;
    int live = 0;
    seal_footinfo fi;
    if (k1) *k1 = 9;
    if (l2_algo) *l2_algo = 0;
    if (m2) *m2 = 0;
    if (!v)
        return 0;
    if (seal_mem_cfg(v, &k, &a, &m)) {
        live = 1;
    } else if (seal_read_prelude((invfs_volume *)v, &fi, NULL) == 0) {
        /* A sealed volume re-opened: the footer is the config. */
        uint8_t pre[SEAL_PRELUDE_LEN];
        uint64_t ino = 0;
        invfs_volume *w = (invfs_volume *)v;
        k = fi.k; m = fi.m; a = RS_ALGO_VM;
        if (vol_find_rc(w, SEAL_FOOTER_NAME, &ino) == 1 && ino &&
            vol_read_range(w, ino, 0, sizeof pre, pre) == (int)sizeof pre &&
            (get16(pre + 18) == RS_ALGO_VM ||
             get16(pre + 18) == RS_ALGO_CAUCHY))
            a = get16(pre + 18);
        live = 1;
    }
    if (k1) *k1 = k;
    if (l2_algo) *l2_algo = live ? (int)a : 0;
    if (m2) *m2 = live ? m : 0;
    return live;
}

/* Coerce any (k1, algo, m2) onto the fixed menu. k1==0 keeps the session
 * value (or the default); algo<0 keeps it; m2 is informational (the pair
 * fixes m — a foreign m2 is not a shape we offer). v1 has ONE layer: algo
 * selects the group code (0/default = VM), and "off" is spelled unseal,
 * not l2_algo==0. Memory-only; the footer persists the geometry at seal
 * time, so --dry-run stays read-only. */
void vol_redun_config(invfs_volume *v, uint32_t k1, int l2_algo, uint32_t m2)
{
    unsigned k = 9, a = RS_ALGO_VM;
    int pm;
    if (!v)
        return;
    if (k1 && seal_menu_m(k1) >= 0) {
        k = k1;
    } else if (k1) {
        unsigned fk = 9;
        if (seal_mem_cfg(v, &fk, NULL, NULL))
            k = fk;
        fprintf(stderr, "seal: k1=%u is not on the fixed menu "
                "(20/9/8/6); using k=%u\n", (unsigned)k1, k);
    } else if (seal_mem_cfg(v, &k, NULL, NULL)) {
        /* keep */
    }
    pm = seal_menu_m(k);
    if (l2_algo == RS_ALGO_VM || l2_algo == RS_ALGO_CAUCHY) {
        a = (unsigned)l2_algo;
    } else if (l2_algo < 0) {
        if (!seal_mem_cfg(v, NULL, &a, NULL))
            a = RS_ALGO_VM;
    } else if (l2_algo != 0) {
        fprintf(stderr, "seal: unknown group code %d; using %s\n",
                l2_algo, rs_algo_name(RS_ALGO_VM));
    }
    (void)m2;   /* the pair fixes m; documented above */
    v->rd.l1_algo = 1;
    v->rd.l2_algo = (uint8_t)a;
    v->rd.k1 = (uint16_t)k;
    v->rd.k2 = 0;
    v->rd.m2 = (uint8_t)pm;
    v->rd.pad = 0;
    v->rd.parity_area_hint = 0;
    v->rd_present = 1;
}

/* ---- namespace collection (sorted, receipted) -------------------------- */

typedef struct {
    char name[256];
    uint64_t ino;
    uint32_t type;
    uint64_t size;
} seal_file;

typedef struct {
    seal_file *ents;
    size_t n, cap;
    int oom;
} seal_collect;

static int seal_walk_cb(void *ctx_, const char *path, uint64_t ino,
                        uint32_t type, uint64_t size, int64_t mtime)
{
    seal_collect *c = ctx_;
    (void)mtime;
    if (!path || !path[0])
        return 0;
    if ((unsigned char)path[0] == 0x01)
        return 0;                       /* seal's own files + owners */
    if (strlen(path) > INVFS_MAX_NAME)
        return 0;                       /* cannot be manifested; skip */
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 256;
        seal_file *ne = realloc(c->ents, nc * sizeof *ne);
        if (!ne) { c->oom = 1; return 1; }
        c->ents = ne;
        c->cap = nc;
    }
    snprintf(c->ents[c->n].name, sizeof c->ents[c->n].name, "%s", path);
    c->ents[c->n].ino = ino;
    c->ents[c->n].type = type;
    c->ents[c->n].size = size;
    c->n++;
    return 0;
}

static int seal_name_cmp(const void *a_, const void *b_)
{
    return strcmp(((const seal_file *)a_)->name,
                  ((const seal_file *)b_)->name);
}

/* Collect + sort the coverable namespace. 0 = complete (out filled, caller
 * frees ents), -1 = the walk stopped or OOM (nothing is claimed). */
static int seal_collect_sorted(invfs_volume *v, seal_collect *out)
{
    vol_walk_t w;
    int wrc;
    memset(out, 0, sizeof *out);
    vol_walk_init(&w, v, "seal namespace walk");
    wrc = vol_walk_strict(v, seal_walk_cb, out);
    vol_walk_result(&w, wrc, out->n, out->n);
    if (out->oom || vol_walk_commit(&w) != 0) {
        free(out->ents);
        memset(out, 0, sizeof *out);
        return -1;
    }
    qsort(out->ents, out->n, sizeof out->ents[0], seal_name_cmp);
    return 0;
}

/* ---- streaming group engine (shared by seal + verify) ------------------ */

struct seal_grouper {
    unsigned k, m, algo;
    uint8_t *sym;           /* k data symbols, SEAL_SYM_BYTES each */
    uint8_t *par;           /* m parity symbols */
    uint8_t **dptr;         /* data symbol pointers for rs_encode */
    uint8_t **pptr;         /* parity symbol pointers */
    size_t fill;            /* bytes in the current symbol */
    unsigned nsym;          /* symbols completed in the open group */
    uint64_t group_bytes;   /* valid data bytes in the open group */
};

struct seal_crc {
    uint32_t crc;
    uint64_t body_len;
};

static int seal_grouper_init(seal_grouper *g, unsigned k, unsigned m,
                             unsigned algo)
{
    unsigned i;
    memset(g, 0, sizeof *g);
    g->k = k; g->m = m; g->algo = algo;
    g->sym = calloc(k ? k : 1, SEAL_SYM_BYTES);
    g->par = calloc(m ? m : 1, SEAL_SYM_BYTES);
    g->dptr = malloc(sizeof *g->dptr * (k ? k : 1));
    g->pptr = malloc(sizeof *g->pptr * (m ? m : 1));
    if (!g->sym || !g->par || !g->dptr || !g->pptr) {
        free(g->sym); free(g->par); free(g->dptr); free(g->pptr);
        memset(g, 0, sizeof *g);
        return -1;
    }
    for (i = 0; i < k; i++) g->dptr[i] = g->sym + (size_t)i * SEAL_SYM_BYTES;
    for (i = 0; i < m; i++) g->pptr[i] = g->par + (size_t)i * SEAL_SYM_BYTES;
    return 0;
}

static void seal_grouper_free(seal_grouper *g)
{
    free(g->sym); free(g->par); free(g->dptr); free(g->pptr);
    memset(g, 0, sizeof *g);
}

/* Feed bytes, consuming up to the first group completion. Returns 1 with
 * parity encoded into par[] (shape in datasyms/dbytes) when a group
 * completes — the caller emits, calls seal_grouper_next(), and feeds the
 * REMAINDER (datap and lenp advance past consumed bytes, so driving it in
 * a while loop cannot drop a byte when a group completes mid-chunk);
 * 0 when all input is consumed with no completion; -1 on rs_encode
 * failure (parity untouched then — never partial). */
static int seal_grouper_feed(seal_grouper *g, const uint8_t **datap,
                             size_t *lenp, unsigned *datasyms,
                             uint64_t *dbytes)
{
    const uint8_t *data = *datap;
    size_t len = *lenp;
    while (len) {
        size_t want = SEAL_SYM_BYTES - g->fill;
        size_t take = len < want ? len : want;
        memcpy(g->sym + (size_t)g->nsym * SEAL_SYM_BYTES + g->fill, data,
               take);
        data += take;
        len -= take;
        g->fill += take;
        g->group_bytes += take;
        if (g->fill == SEAL_SYM_BYTES) {
            g->fill = 0;
            g->nsym++;
            if (g->nsym == g->k) {
                if (rs_encode((int)g->algo, g->k, g->m, SEAL_SYM_BYTES,
                              g->dptr, g->pptr) != 0)
                    return -1;
                *datasyms = g->k;
                *dbytes = g->group_bytes;
                *datap = data;
                *lenp = len;
                return 1;
            }
        }
    }
    *datap = data;
    *lenp = len;
    return 0;
}

/* Close a partial tail group (zero-pad, encode). *has = 0 when the group
 * is empty (no tail). Same contract as feed. */
static int seal_grouper_flush(seal_grouper *g, int *has, unsigned *datasyms,
                              uint64_t *dbytes)
{
    *has = 0;
    if (!g->nsym && !g->fill)
        return 0;
    if (g->fill) {
        memset(g->sym + (size_t)g->nsym * SEAL_SYM_BYTES + g->fill, 0,
               SEAL_SYM_BYTES - g->fill);
        g->nsym++;
    }
    if (rs_encode((int)g->algo, g->k, g->m, SEAL_SYM_BYTES,
                  g->dptr, g->pptr) != 0)
        return -1;
    *has = 1;
    *datasyms = g->nsym;
    *dbytes = g->group_bytes;
    return 0;
}

static void seal_grouper_next(seal_grouper *g)
{
    g->fill = 0;
    g->nsym = 0;
    g->group_bytes = 0;
}

/* Read exactly len bytes at off (short only at EOF/error). Bytes read,
 * or -1 on error. */
static int seal_read_full(invfs_volume *v, uint64_t ino, uint64_t off,
                          uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = vol_read_range(v, ino, off + got, len - got, buf + got);
        if (r < 0) return -1;
        if (r == 0) break;
        got += (size_t)r;
    }
    return (int)got;
}

/* Session write of exactly len bytes at off. 0 ok, -1 error. */
static int seal_write_full(invfs_wsession *ws, uint64_t off,
                           const uint8_t *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done > (size_t)1 << 20 ?
                       (size_t)1 << 20 : len - done;
        if (vol_write_range(ws, off + done, buf + done, chunk) != 0)
            return -1;
        done += chunk;
    }
    return 0;
}

static void seal_crc_feed(seal_crc *s, const void *p, size_t n)
{
    s->crc = invfs_crc32c_update(s->crc, p, n);
    s->body_len += n;
}

int vol_seal(invfs_volume *v, int unseal, invfs_seal_report *rep)
{
    if (rep) memset(rep, 0, sizeof *rep);
    if (!v || !rep)
        return -1;

    /* ---- unseal: retire both files, idempotent ---------------------- */
    if (unseal) {
        uint64_t ino = 0;
        uint64_t fsz = 0, ctime = 0;
        uint64_t freed = 0;
        if (v->sb.vol_flags & VOLF_READONLY) {
            fprintf(stderr, "seal: volume is read-only; unseal refuses\n");
            return -1;
        }
        /* Count first (the footer names what is about to go), then drop. */
        if (vol_find_rc(v, SEAL_FOOTER_NAME, &ino) == 1 && ino &&
            vol_stat_full(v, SEAL_FOOTER_NAME, &ino, &fsz, &ctime) == 0 &&
            fsz >= SEAL_PRELUDE_LEN + SEAL_TRAILER_LEN &&
            fsz < (uint64_t)1 << 26) {
            uint8_t *fb = malloc((size_t)fsz);
            if (fb) {
                if (seal_read_full(v, ino, 0, fb, (size_t)fsz) ==
                    (int)fsz) {
                    seal_footinfo fi;
                    if (seal_footer_parse(fb, (size_t)fsz, &fi, NULL,
                                          NULL) == 0)
                        freed = (uint64_t)fi.ngroups * fi.m;
                }
                free(fb);
            }
        }
        vol_delete_file(v, SEAL_PARITY_NAME);
        vol_delete_file(v, SEAL_FOOTER_NAME);
        memset(&v->rd, 0, sizeof v->rd);
        v->rd_present = 0;
        rep->freed = freed;
        return 0;
    }

    /* ---- seal -------------------------------------------------------- */
    {
        unsigned k = 9, m = 1, algo = RS_ALGO_VM;
        seal_footinfo prior;
        uint64_t seq = 1;
        int have_prior = 0;
        seal_collect col;
        seal_grouper gr;
        uint8_t chunk[SEAL_SYM_BYTES];
        invfs_wsession *pws = NULL, *fws = NULL;
        uint64_t par_off = SEAL_PARHDR_LEN, foot_off = 0;
        uint8_t hdr[SEAL_PARHDR_LEN];
        uint32_t ngroups = 0;
        uint64_t data_bytes = 0, parity_bytes = 0;
        seal_crc fcrc;
        size_t fi;
        int gr_init = 0, rc = -1;

        memset(&col, 0, sizeof col);
        memset(&gr, 0, sizeof gr);
        if (v->sb.vol_flags & VOLF_READONLY) {
            fprintf(stderr, "seal: volume is read-only; seal refuses "
                    "(sealing writes parity + manifest)\n");
            return -1;
        }
        /* The prior footer is ALWAYS read when present: it sets the next
         * seq (lineage survives reconfiguration) and is the geometry
         * fallback. Geometry precedence is session config > footer >
         * default — the driver configures every run, so a session-first
         * test here would blind the seq (every seal gen-1). */
        if (seal_read_prelude(v, &prior, NULL) == 0) {
            uint8_t p28[SEAL_PRELUDE_LEN];
            uint64_t ino = 0;
            have_prior = 1;
            seq = prior.seq + 1;
            if (!seq) seq = 1;   /* u64 wrap: never seal at generation 0 */
            if (!seal_mem_cfg(v, &k, &algo, &m)) {
                k = prior.k; m = prior.m;
                if (vol_find_rc(v, SEAL_FOOTER_NAME, &ino) == 1 && ino &&
                    seal_read_full(v, ino, 0, p28, sizeof p28) ==
                    (int)sizeof p28 &&
                    (get16(p28 + 18) == RS_ALGO_VM ||
                     get16(p28 + 18) == RS_ALGO_CAUCHY))
                    algo = get16(p28 + 18);
            }
        } else if (seal_mem_cfg(v, &k, &algo, &m)) {
            /* session config wins (no prior seal to continue) */
        }
        if (seal_collect_sorted(v, &col) != 0) {
            fprintf(stderr, "seal: namespace walk stopped; refusing a "
                    "partial seal\n");
            return -1;
        }
        if (seal_grouper_init(&gr, k, m, algo) != 0) {
            fprintf(stderr, "seal: out of memory\n");
            goto out;
        }
        gr_init = 1;
        /* Sessions open truncate=0 (no-truncate): a crash before a commit
         * leaves the PRIOR version live; a crash after leaves the new
         * bytes with the old (or no) footer — both read as
         * "prior-or-absent", never half-trusted. */
        if (vol_write_begin(v, SEAL_PARITY_NAME, 0, &pws) == 0 || !pws) {
            fprintf(stderr, "seal: cannot stage " SEAL_PARITY_NAME "\n");
            goto out;
        }
        if (vol_write_begin(v, SEAL_FOOTER_NAME, 0, &fws) == 0 || !fws) {
            fprintf(stderr, "seal: cannot stage " SEAL_FOOTER_NAME "\n");
            goto out;
        }
        /* Parity header placeholder (ngroups unknown until the stream
         * ends); rewritten in place after the commit, before the footer. */
        if (!seal_parhdr_enc(hdr, k, m, 0xFFFFFFFFu, seq) ||
            seal_write_full(pws, 0, hdr, sizeof hdr) != 0)
            goto out;
        memset(&fcrc, 0, sizeof fcrc);
        /* Footer prelude (counts live in the trailer, so one pass). */
        {
            uint8_t pb[SEAL_PRELUDE_LEN];
            if (!seal_prelude_enc(pb, k, m, algo, seq) ||
                seal_write_full(fws, 0, pb, sizeof pb) != 0)
                goto out;
            seal_crc_feed(&fcrc, pb, sizeof pb);
            foot_off = sizeof pb;
        }
        /* Pass 1 (hash + manifest): every file's manifest entry is written
         * BEFORE any group entry, so the footer layout stays
         * prelude/entries/groups/trailer. Content hashes stream here;
         * the symbol stream runs in pass 2 below. Two content passes is
         * the price of the layout (seal is rare; the second pass is
         * usually page-cache hot). */
        for (fi = 0; fi < col.n; fi++) {
            seal_file *e = &col.ents[fi];
            uint8_t hash[32];
            uint8_t enc[2 + 256 + 1 + 8 + 32];
            size_t enclen;
            int cover = (e->type == INVFS_ITYP_REG || e->type == INVFS_ITYP_LNK);
            uint64_t left = 0, off = 0;
            blake3_hasher hb;

            blake3_hasher_init(&hb);
            if (e->type == INVFS_ITYP_LNK) {
                uint8_t *t = NULL;
                size_t tl = 0;
                if (vol_read_file(v, e->ino, &t, &tl) != 0) {
                    fprintf(stderr, "seal: cannot read symlink target %s\n",
                            e->name);
                    free(t);
                    goto out;
                }
                blake3_hasher_update(&hb, t ? t : (const uint8_t *)"", tl);
                free(t);
            } else if (e->type == INVFS_ITYP_REG) {
                left = e->size;
            }
            while (left) {
                size_t want = left > sizeof chunk ? sizeof chunk :
                                                    (size_t)left;
                int r = seal_read_full(v, e->ino, off, chunk, want);
                if (r != (int)want) {
                    fprintf(stderr, "seal: short read on %s "
                            "(want %zu, got %d); refusing\n",
                            e->name, want, r);
                    goto out;
                }
                blake3_hasher_update(&hb, chunk, want);
                off += want;
                left -= want;
            }
            if (cover)
                blake3_hasher_finalize(&hb, hash, sizeof hash);
            else
                memset(hash, 0, sizeof hash);
            enclen = seal_entry_enc(enc, sizeof enc, e->name,
                                   e->type == INVFS_ITYP_REG ? SEAL_FT_REG :
                                   e->type == INVFS_ITYP_LNK ? SEAL_FT_LNK :
                                   (unsigned)e->type,
                                   e->type == INVFS_ITYP_LNK ? 0 : e->size,
                                   cover ? hash : NULL);
            if (!enclen) {
                fprintf(stderr, "seal: cannot manifest %s\n", e->name);
                goto out;
            }
            if (seal_write_full(fws, foot_off, enc, enclen) != 0)
                goto out;
            seal_crc_feed(&fcrc, enc, enclen);
            foot_off += enclen;
        }
        /* Pass 2 (symbols + parity): stream covered bytes in the same
         * sorted order into groups; group entries append AFTER every file
         * entry, preserving the footer layout. */
        for (fi = 0; fi < col.n; fi++) {
            seal_file *e = &col.ents[fi];
            uint64_t left = 0, off = 0;
            if (e->type == INVFS_ITYP_LNK) {
                uint8_t *t = NULL;
                size_t tl = 0;
                if (vol_read_file(v, e->ino, &t, &tl) != 0) {
                    fprintf(stderr, "seal: cannot re-read symlink %s\n",
                            e->name);
                    free(t);
                    goto out;
                }
                /* The target bytes join the symbol stream too: every
                 * covered byte sits in exactly one symbol. The loop
                 * re-feeds the remainder after each completion, so a
                 * group completing mid-buffer drops nothing. */
                {
                    const uint8_t *p = t;
                    size_t n = tl;
                    while (n) {
                        int c;
                        unsigned ds = 0;
                        uint64_t db = 0;
                        c = seal_grouper_feed(&gr, &p, &n, &ds, &db);
                        if (c < 0) {
                            free(t);
                            fprintf(stderr, "seal: parity encode failed\n");
                            goto out;
                        }
                        if (c > 0 && seal_emit_group(v, pws, &gr, &par_off,
                                                    fws, &foot_off, &fcrc,
                                                    &ngroups, ds, db) != 0) {
                            free(t);
                            goto out;
                        }
                    }
                }
                free(t);
            } else if (e->type == INVFS_ITYP_REG) {
                left = e->size;
            }
            while (left) {
                size_t want = left > sizeof chunk ? sizeof chunk :
                                                    (size_t)left;
                const uint8_t *p;
                size_t n;
                int r = seal_read_full(v, e->ino, off, chunk, want);
                if (r != (int)want) {
                    fprintf(stderr, "seal: short re-read on %s; "
                            "refusing\n", e->name);
                    goto out;
                }
                p = chunk;
                n = want;
                while (n) {
                    int c;
                    unsigned ds = 0;
                    uint64_t db = 0;
                    c = seal_grouper_feed(&gr, &p, &n, &ds, &db);
                    if (c < 0) {
                        fprintf(stderr, "seal: parity encode failed\n");
                        goto out;
                    }
                    if (c > 0 && seal_emit_group(v, pws, &gr, &par_off,
                                                fws, &foot_off, &fcrc,
                                                &ngroups, ds, db) != 0)
                        goto out;
                }
                data_bytes += want;
                off += want;
                left -= want;
            }
        }
        /* Tail group (zero-padded) if the stream did not end on one. */
        {
            int has = 0;
            unsigned ds = 0;
            uint64_t db = 0;
            if (seal_grouper_flush(&gr, &has, &ds, &db) != 0) {
                fprintf(stderr, "seal: parity encode failed (tail)\n");
                goto out;
            }
            if (has && seal_emit_group(v, pws, &gr, &par_off, fws, &foot_off,
                                       &fcrc, &ngroups, ds, db) != 0)
                goto out;
        }
        parity_bytes = (uint64_t)ngroups * m * SEAL_SYM_BYTES;
        if (ngroups >= SEAL_MAX_NGROUPS) {
            fprintf(stderr, "seal: absurd group count; refusing\n");
            goto out;
        }
        /* Footer trailer (counts + CRC over body AND counts), then commit
         * parity, rewrite its header with the true count, commit footer. */
        {
            uint8_t tr[SEAL_TRAILER_LEN];
            put32(tr, (uint32_t)col.n);
            put32(tr + 4, ngroups);
            seal_crc_feed(&fcrc, tr, 8);   /* counts are covered too */
            put32(tr + 8, fcrc.crc);
            if (seal_write_full(fws, foot_off, tr, sizeof tr) != 0)
                goto out;
            foot_off += sizeof tr;
            if (vol_write_truncate(pws, par_off) != 0 ||
                vol_write_truncate(fws, foot_off) != 0)
                goto out;
            if (vol_write_commit(pws) != 0) {
                fprintf(stderr, "seal: parity commit failed\n");
                goto out;
            }
            pws = NULL;
        }
        /* Crash probe seam: between the parity commit and the footer
         * commit, exactly where a kill -9 would land. */
        if (invfs_vol_fault(SEAL_FAULT_CRASH)) {
            fprintf(stderr, "seal: fault " SEAL_FAULT_CRASH " fired "
                    "(simulated crash before the footer commit)\n");
            vol_write_abort(fws);
            fws = NULL;
            goto out;
        }
        /* True parity header now that ngroups is known. */
        {
            invfs_wsession *hws = NULL;
            if (!seal_parhdr_enc(hdr, k, m, ngroups, seq))
                goto out;
            if (vol_write_begin(v, SEAL_PARITY_NAME, 0, &hws) == 0 || !hws)
                goto out;
            if (seal_write_full(hws, 0, hdr, sizeof hdr) != 0) {
                vol_write_abort(hws);
                goto out;
            }
            if (vol_write_commit(hws) != 0) {
                fprintf(stderr, "seal: parity header commit failed\n");
                goto out;
            }
        }
        if (vol_write_commit(fws) != 0) {
            fprintf(stderr, "seal: footer commit failed\n");
            goto out;
        }
        fws = NULL;
        /* Session geometry (memory only; the footer is the record). */
        v->rd.l1_algo = 1;
        v->rd.l2_algo = (uint8_t)algo;
        v->rd.k1 = (uint16_t)k;
        v->rd.k2 = 0;
        v->rd.m2 = (uint8_t)m;
        v->rd.pad = 0;
        v->rd.parity_area_hint = parity_bytes;
        v->rd_present = 1;
        /* Verify-after-write: prove the seal from disk before claiming it. */
        {
            invfs_seal_verify sv;
            memset(&sv, 0, sizeof sv);
            if (seal_verify_locked(v, &sv, seq, NULL, NULL) != 0 ||
                sv.sealed != ngroups || sv.mismatched || sv.missing ||
                sv.extra) {
                fprintf(stderr, "seal: verify-after-write failed "
                        "(sealed=%llu mismatched=%llu missing=%llu "
                        "extra=%llu); the seal is NOT claimed\n",
                        (unsigned long long)sv.sealed,
                        (unsigned long long)sv.mismatched,
                        (unsigned long long)sv.missing,
                        (unsigned long long)sv.extra);
                goto out;
            }
        }
        rep->stripes = ngroups;
        rep->parity_blocks = parity_bytes / SEAL_SYM_BYTES;
        rep->updated = ngroups;   /* v1: full recompute, always rewritten */
        rep->unchanged = 0;
        rep->added = have_prior ? 0 : ngroups;
        rep->freed = 0;
        rep->unprotected = 0;
        rep->overhead_pct = data_bytes ?
            100.0 * (double)parity_bytes / (double)data_bytes : 0.0;
        rep->dirty_skipped = 0;   /* v1 never consults the dirty bitmap */
        rc = 0;
    out:
        if (pws) vol_write_abort(pws);
        if (fws) vol_write_abort(fws);
        if (gr_init) seal_grouper_free(&gr);
        free(col.ents);
        return rc;
    }
}

/* Write one completed group's parity + its footer group entry. */
static int seal_emit_group(invfs_volume *v, invfs_wsession *pws,
                           seal_grouper *gr, uint64_t *par_off,
                           invfs_wsession *fws, uint64_t *foot_off,
                           seal_crc *fcrc, uint32_t *ngroups,
                           unsigned datasyms, uint64_t databytes)
{
    uint8_t ge[10];
    unsigned i;
    (void)v;
    if (!pws || !gr || !par_off || !fws || !foot_off || !fcrc ||
        !ngroups || !datasyms)
        return -1;
    for (i = 0; i < gr->m; i++)
        if (seal_write_full(pws, *par_off + (uint64_t)i * SEAL_SYM_BYTES,
                            gr->par + (size_t)i * SEAL_SYM_BYTES,
                            SEAL_SYM_BYTES) != 0)
            return -1;
    *par_off += (uint64_t)gr->m * SEAL_SYM_BYTES;
    if (!seal_group_enc(ge, datasyms, databytes))
        return -1;
    if (seal_write_full(fws, *foot_off, ge, sizeof ge) != 0)
        return -1;
    seal_crc_feed(fcrc, ge, sizeof ge);
    *foot_off += sizeof ge;
    (*ngroups)++;
    seal_grouper_next(gr);
    return 0;
}

/* WP401 scrub ledger helpers (the sink type is declared above, next to
 * the forward declarations; grown here, next to its users). */
static int scrub_sink_grow(scrub_sink *s)
{
    size_t nc = s->cap ? s->cap * 2 : 16;
    invfs_seal_badgroup *na;
    if (nc > (size_t)16777216) nc = (size_t)16777216;
    if (s->n >= nc) return -1;
    na = (invfs_seal_badgroup *)realloc(s->arr, nc * sizeof *na);
    if (!na) return -1;
    s->arr = na;
    s->cap = nc;
    return 0;
}
static int scrub_append(scrub_sink *s, uint32_t g, const char *reason)
{
    if (!s) return 0;
    if (s->n == s->cap && scrub_sink_grow(s) != 0) return -1;
    s->arr[s->n].group = g;
    snprintf(s->arr[s->n].reason, sizeof s->arr[s->n].reason, "%s", reason);
    s->n++;
    return 0;
}
static int scrub_mark(scrub_sink *s, uint32_t g, const char *reason)
{
    size_t i;
    if (!s) return 0;
    for (i = 0; i < s->n; i++) {
        if (s->arr[i].group == g) {
            if (!strcmp(reason, "mismatched"))
                snprintf(s->arr[i].reason, sizeof s->arr[i].reason,
                         "mismatched");
            return 0;
        }
    }
    return scrub_append(s, g, reason);
}

/* One completed verify group: compare re-encoded parity to stored bytes.
 * A stored-parity read failure counts the group mismatched (parity damage
 * is loud), and the stream still advances — never trusted, never stuck.
 * WP401: the ledger names an unreadable-parity group "missing" (it
 * cannot be read) while the counter stays mismatched, exactly as verify
 * counts it -- see vol_seal_scrub's contract in volume.h. */
static int seal_feed_verify(invfs_volume *v, seal_grouper *gr,
                            const uint8_t *data, size_t len, uint64_t pino,
                            uint64_t *poff, uint32_t *gi, uint32_t ngroups,
                            invfs_seal_verify *out, scrub_sink *sink)
{
    static uint8_t pb[2 * SEAL_SYM_BYTES];  /* m <= 2 on the fixed menu */
    while (len) {
        int c;
        unsigned ds = 0;
        uint64_t db = 0;
        unsigned i;
        c = seal_grouper_feed(gr, &data, &len, &ds, &db);
        if (c < 0)
            return -1;
        (void)ds; (void)db;
        if (!c)
            continue;
        if (*gi >= ngroups) {
            out->mismatched++;      /* more data than the seal covered */
            if (scrub_mark(sink, *gi, "extra") != 0) return -1;
        } else if (seal_read_full(v, pino, *poff, pb,
                                  (size_t)gr->m * SEAL_SYM_BYTES) !=
                   (int)((size_t)gr->m * SEAL_SYM_BYTES)) {
            out->mismatched++;
            if (scrub_mark(sink, *gi, "missing") != 0) return -1;
        } else {
            for (i = 0; i < gr->m; i++)
                if (memcmp(pb + (size_t)i * SEAL_SYM_BYTES,
                           gr->par + (size_t)i * SEAL_SYM_BYTES,
                           SEAL_SYM_BYTES) != 0) {
                    out->mismatched++;
                    if (scrub_mark(sink, *gi, "mismatched") != 0)
                        return -1;
                    break;
                }
            *poff += (uint64_t)gr->m * SEAL_SYM_BYTES;
        }
        (*gi)++;
        seal_grouper_next(gr);
    }
    return 0;
}

/* Stream one live file's covered bytes into the verify grouper while
 * hashing them. 0 = streamed (hash_out filled), 1 = unreadable (caller
 * counts the damage), -1 = encode failure. */
static int seal_stream_file(invfs_volume *v, const seal_file *e,
                            seal_grouper *gr, uint64_t pino, uint64_t *poff,
                            uint32_t *gi, uint32_t ngroups,
                            invfs_seal_verify *out, uint8_t hash_out[32],
                            uint64_t *bytes_out, scrub_sink *sink)
{
    static uint8_t chunk[SEAL_SYM_BYTES];
    blake3_hasher hb;
    uint64_t nbytes = 0;
    blake3_hasher_init(&hb);
    if (e->type == INVFS_ITYP_LNK) {
        uint8_t *t = NULL;
        size_t tl = 0;
        if (vol_read_file(v, e->ino, &t, &tl) != 0) {
            free(t);
            return 1;
        }
        blake3_hasher_update(&hb, t ? t : (const uint8_t *)"", tl);
        if (seal_feed_verify(v, gr, t ? t : chunk, tl, pino, poff, gi,
                             ngroups, out, sink) != 0) {
            free(t);
            return -1;
        }
        nbytes = tl;
        free(t);
    } else if (e->type == INVFS_ITYP_REG) {
        uint64_t left = e->size, off = 0;
        while (left) {
            size_t want = left > sizeof chunk ? sizeof chunk : (size_t)left;
            int r = seal_read_full(v, e->ino, off, chunk, want);
            if (r != (int)want)
                return 1;
            blake3_hasher_update(&hb, chunk, want);
            if (seal_feed_verify(v, gr, chunk, want, pino, poff, gi,
                                 ngroups, out, sink) != 0)
                return -1;
            nbytes += want;
            off += want;
            left -= want;
        }
    }
    blake3_hasher_finalize(&hb, hash_out, 32);
    if (bytes_out) *bytes_out = nbytes;
    return 0;
}

/* Full re-verify against an expected seq (0 = accept the footer's own).
 * Shared by the seal-time verify-after-write and vol_seal_verify.
 * Returns 0 with counters filled (all-zero = unsealed, silent), -1 only
 * for I/O/namespace failures that make even "unsealed" unclaimable.
 * WP401: sink carries the scrub ledger (NULL = legacy verify behavior,
 * no ledger); untrusted_out (NULL = don't report)
 * distinguishes 1 = no footer file from 2 = footer present but
 * untrusted (the "treated as unsealed, never trusted" shapes). */
static int seal_verify_locked(invfs_volume *v, invfs_seal_verify *out,
                              uint64_t expect_seq,
                              scrub_sink *sink, int *untrusted_out)
{
    uint64_t fino = 0, pino = 0, fsz = 0, psz = 0, ctime = 0;
    uint8_t pre[SEAL_PRELUDE_LEN];
    uint8_t phdr[SEAL_PARHDR_LEN];
    seal_footinfo fi;
    unsigned algo = RS_ALGO_VM, pk = 0, pm = 0;
    uint32_t png = 0, ngt = 0, nft = 0;
    uint64_t pseq = 0;
    uint8_t tr[SEAL_TRAILER_LEN];
    seal_collect col;
    seal_grouper gr;
    int gr_init = 0;
    uint64_t poff = SEAL_PARHDR_LEN;
    uint64_t eoff = SEAL_PRELUDE_LEN;
    uint32_t crc = 0;
    uint32_t gi = 0, eidx = 0;
    size_t li = 0;
    uint64_t data_bytes = 0;
    uint64_t stale_bytes = 0, stale_chg = 0, stale_add = 0, stale_del = 0;

    memset(&col, 0, sizeof col);
    memset(&gr, 0, sizeof gr);
    memset(out, 0, sizeof *out);
    if (untrusted_out) *untrusted_out = 0;
    if (!v)
        return -1;
    if (vol_find_rc(v, SEAL_FOOTER_NAME, &fino) != 1 || !fino) {
        if (untrusted_out) *untrusted_out = 1;
        return 0;                       /* unsealed: all-zero, silent */
    }
    if (vol_stat_full(v, SEAL_FOOTER_NAME, &fino, &fsz, &ctime) != 0)
        return -1;
    if (fsz < SEAL_PRELUDE_LEN + SEAL_TRAILER_LEN ||
        fsz >= (uint64_t)1 << 32) {
        fprintf(stderr, "seal: footer present but implausible (%llu B); "
                "treated as unsealed, never trusted\n",
                (unsigned long long)fsz);
        if (untrusted_out) *untrusted_out = 2;
        return 0;
    }
    if (seal_read_full(v, fino, 0, pre, sizeof pre) != (int)sizeof pre)
        return -1;
    if (memcmp(pre, SEAL_FOOTER_MAGIC, 8) != 0 ||
        get16(pre + 8) != SEAL_FORMAT_VERSION) {
        fprintf(stderr, "seal: footer present but not a v1 footer; treated "
                "as unsealed, never trusted\n");
        if (untrusted_out) *untrusted_out = 2;
        return 0;
    }
    fi.k = get16(pre + 10);
    fi.m = get16(pre + 12);
    fi.sym = get32(pre + 14);
    algo = get16(pre + 18);
    fi.seq = get64(pre + 20);
    if (!fi.k || !fi.m || (unsigned)fi.k + (unsigned)fi.m > 256 ||
        fi.sym != SEAL_SYM_BYTES ||
        (algo != RS_ALGO_VM && algo != RS_ALGO_CAUCHY) || !fi.seq ||
        fi.m > 2) {
        fprintf(stderr, "seal: footer geometry invalid; treated as "
                "unsealed, never trusted\n");
        if (untrusted_out) *untrusted_out = 2;
        return 0;
    }
    if (expect_seq && fi.seq != expect_seq)
        return -1;      /* seal-time: the disk disagrees with what we wrote */
    /* Trailer (needed for counts whatever the parity looks like). */
    if (seal_read_full(v, fino, fsz - SEAL_TRAILER_LEN, tr, sizeof tr) !=
        (int)sizeof tr)
        return -1;
    nft = get32(tr);
    ngt = get32(tr + 4);
    if (nft > 16777216u || ngt > SEAL_MAX_NGROUPS) {
        fprintf(stderr, "seal: footer counts absurd; treated as unsealed, "
                "never trusted\n");
        if (untrusted_out) *untrusted_out = 2;
        return 0;
    }
    fi.nfiles = nft;
    fi.ngroups = ngt;
    if (vol_find_rc(v, SEAL_PARITY_NAME, &pino) != 1 || !pino) {
        /* Footer without parity: every group is missing. Loud, not silent:
         * something deleted the parity out from under a live seal. */
        out->sealed = ngt;
        out->missing = ngt;
        { uint32_t g; for (g = 0; g < ngt; g++)
            if (scrub_append(sink, g, "missing") != 0) goto fail; }
        fprintf(stderr, "seal: sealed-at-gen-%llu but parity file is gone; "
                "%u groups missing\n", (unsigned long long)fi.seq, ngt);
        return 0;
    }
    if (vol_stat_full(v, SEAL_PARITY_NAME, &pino, &psz, &ctime) != 0)
        return -1;
    if (seal_read_full(v, pino, 0, phdr, sizeof phdr) != (int)sizeof phdr)
        return -1;
    if (seal_parhdr_parse(phdr, sizeof phdr, &pk, &pm, &png, &pseq) != 0 ||
        pk != fi.k || pm != fi.m || pseq != fi.seq || png != ngt ||
        psz != SEAL_PARHDR_LEN + (uint64_t)png * pm * SEAL_SYM_BYTES) {
        /* Parity disagrees with the footer: untrusted parity. Every group
         * counts mismatched — loud, and a re-seal heals it. */
        out->sealed = ngt;
        out->mismatched = ngt;
        { uint32_t g; for (g = 0; g < ngt; g++)
            if (scrub_append(sink, g, "mismatched") != 0) goto fail; }
        fprintf(stderr, "seal: sealed-at-gen-%llu but parity disagrees "
                "with the footer; %u groups mismatched\n",
                (unsigned long long)fi.seq, ngt);
        return 0;
    }
    if (seal_grouper_init(&gr, fi.k, fi.m, algo) != 0)
        return -1;
    gr_init = 1;
    if (seal_collect_sorted(v, &col) != 0)
        goto fail;
    crc = invfs_crc32c_update(0, pre, sizeof pre);
    out->sealed = ngt;
    /* Merged loop: manifest entries in footer order against the sorted
     * live list. Covered bytes stream into the grouper in the same order
     * the seal wrote them, so parity compares align. */
    for (eidx = 0; eidx < nft; eidx++) {
        uint8_t h2[2];
        uint8_t eb[2 + 256 + 1 + 8 + 32];
        uint16_t nl;
        uint8_t ft;
        uint64_t sz;
        uint8_t mhash[32];
        int hashed;
        char mname[256];
        int cmp = 1;
        if (seal_read_full(v, fino, eoff, h2, 2) != 2) goto corrupt;
        nl = get16(h2);
        if (!nl || nl > INVFS_MAX_NAME) goto corrupt;
        if (seal_read_full(v, fino, eoff + 2, eb, nl + 1 + 8) !=
            (int)(nl + 1 + 8))
            goto corrupt;
        crc = invfs_crc32c_update(crc, h2, 2);
        crc = invfs_crc32c_update(crc, eb, nl + 1 + 8);
        ft = eb[nl];
        sz = get64(eb + nl + 1);
        hashed = (ft == SEAL_FT_REG || ft == SEAL_FT_LNK);
        if (hashed) {
            if (seal_read_full(v, fino, eoff + 2 + nl + 1 + 8, mhash, 32)
                != 32)
                goto corrupt;
            crc = invfs_crc32c_update(crc, mhash, 32);
        }
        eoff += 2 + nl + 1 + 8 + (uint64_t)(hashed ? 32 : 0);
        memcpy(mname, eb, nl);
        mname[nl] = 0;
        /* New live files sorting before this entry: extra + streamed. */
        while (li < col.n && (cmp = strcmp(col.ents[li].name, mname)) < 0) {
            uint8_t h[32];
            uint64_t nb = 0;
            int sr;
            out->extra++;
            stale_add++;
            stale_bytes += col.ents[li].size;
            if (col.ents[li].type == INVFS_ITYP_REG ||
                col.ents[li].type == INVFS_ITYP_LNK) {
                sr = seal_stream_file(v, &col.ents[li], &gr, pino, &poff,
                                      &gi, ngt, out, h, &nb, sink);
                if (sr < 0) goto fail;
                data_bytes += nb;   /* unreadable: counted below */
            }
            li++;
        }
        if (li < col.n && cmp == 0) {
            seal_file *e = &col.ents[li];
            unsigned eft = e->type == INVFS_ITYP_REG ? SEAL_FT_REG :
                           e->type == INVFS_ITYP_LNK ? SEAL_FT_LNK :
                           e->type;
            uint64_t esz = e->type == INVFS_ITYP_LNK ? 0 : e->size;
            int covered = (eft == SEAL_FT_REG || eft == SEAL_FT_LNK);
            li++;
            if (eft != ft || esz != sz) {
                stale_chg++;
                stale_bytes += e->size > sz ? e->size : sz;
            }
            if (covered && eft == ft && esz == sz && hashed) {
                uint8_t h[32];
                uint64_t nb = 0;
                int sr = seal_stream_file(v, e, &gr, pino, &poff, &gi,
                                          ngt, out, h, &nb, sink);
                if (sr < 0) goto fail;
                data_bytes += nb;
                if (sr > 0 || memcmp(h, mhash, 32) != 0) {
                    stale_chg++;
                    stale_bytes += esz;
                }
            } else if (covered) {
                /* Changed shape: bytes still stream (parity will tell). */
                uint8_t h[32];
                uint64_t nb = 0;
                int sr = seal_stream_file(v, e, &gr, pino, &poff, &gi,
                                          ngt, out, h, &nb, sink);
                if (sr < 0) goto fail;
                data_bytes += nb;
            }
        } else {
            /* Manifested but gone: deleted since the seal. The stream has
             * a hole at the current group: it cannot be proven, so the
             * scrub ledger names it missing (a later parity disagreement
             * on the same group upgrades it to mismatched). */
            out->missing++;
            if (gi < ngt && scrub_mark(sink, gi, "missing") != 0)
                goto fail;
            stale_del++;
            stale_bytes += sz;
        }
    }
    /* Trailing live files: extra + streamed. */
    while (li < col.n) {
        uint8_t h[32];
        uint64_t nb = 0;
        int sr;
        out->extra++;
        stale_add++;
        stale_bytes += col.ents[li].size;
        if (col.ents[li].type == INVFS_ITYP_REG ||
            col.ents[li].type == INVFS_ITYP_LNK) {
            sr = seal_stream_file(v, &col.ents[li], &gr, pino, &poff,
                                  &gi, ngt, out, h, &nb, sink);
            if (sr < 0) goto fail;
            data_bytes += nb;
        }
        li++;
    }
    /* Tail group, then every group the stream never completed. */
    {
        int has = 0;
        unsigned ds = 0;
        uint64_t db = 0;
        static uint8_t pb[2 * SEAL_SYM_BYTES];
        if (seal_grouper_flush(&gr, &has, &ds, &db) != 0)
            goto fail;
        if (has) {
            unsigned i;
            if (gi >= ngt) {
                out->mismatched++;
                if (scrub_mark(sink, gi, "extra") != 0) goto fail;
            } else if (seal_read_full(v, pino, poff, pb,
                                      (size_t)fi.m * SEAL_SYM_BYTES) !=
                       (int)((size_t)fi.m * SEAL_SYM_BYTES)) {
                out->mismatched++;
                if (scrub_mark(sink, gi, "missing") != 0) goto fail;
            } else {
                for (i = 0; i < fi.m; i++)
                    if (memcmp(pb + (size_t)i * SEAL_SYM_BYTES,
                               gr.par + (size_t)i * SEAL_SYM_BYTES,
                               SEAL_SYM_BYTES) != 0) {
                        out->mismatched++;
                        if (scrub_mark(sink, gi, "mismatched") != 0)
                            goto fail;
                        break;
                    }
                poff += (uint64_t)fi.m * SEAL_SYM_BYTES;
            }
            gi++;
        }
        if (gi < ngt) {
            /* Unprovable groups: the live stream ended before reaching
             * them. Counted mismatched, exactly as verify counts them;
             * the ledger names them missing (no content proves them). */
            { uint32_t g; for (g = gi; g < ngt; g++)
                if (scrub_mark(sink, g, "missing") != 0) goto fail; }
            out->mismatched += (uint64_t)(ngt - gi);  /* unprovable groups */
        }
    }
    /* Group entries: bounds sanity + CRC fold. Content truth comes from
     * the parity re-encode above (a lying entry with matching parity is
     * not constructible by accident); deriving expected shapes here from
     * verify-time bytes would double-count every stale group, so shapes
     * are NOT re-derived — only bounded. */
    {
        uint64_t geoff = eoff;
        uint32_t k;
        for (k = 0; k < ngt; k++) {
            uint8_t gb[10];
            uint16_t ds;
            uint64_t db;
            if (seal_read_full(v, fino, geoff, gb, 10) != 10) goto corrupt;
            crc = invfs_crc32c_update(crc, gb, 10);
            geoff += 10;
            ds = get16(gb);
            db = get64(gb + 2);
            if (!ds || ds > fi.k || !db || db > (uint64_t)ds * fi.sym) {
                out->mismatched++;
                if (scrub_mark(sink, k, "mismatched") != 0) goto fail;
            }
        }
        eoff = geoff;
    }
    /* Trailer must sit exactly here with matching counts + CRC (the CRC
     * covers the counts as well — fold them before comparing). */
    {
        uint32_t cf = get32(tr + 8);
        crc = invfs_crc32c_update(crc, tr, 8);
        if (eoff + SEAL_TRAILER_LEN != fsz ||
            get32(tr) != nft || get32(tr + 4) != ngt || cf != crc)
            goto corrupt;
    }
    if (gr_init) seal_grouper_free(&gr);
    free(col.ents);
    if (out->mismatched || out->missing || out->extra) {
        fprintf(stderr, "seal: sealed-at-gen-%llu: %u groups over %llu "
                "data bytes; STALE: %llu uncovered bytes (%llu changed, "
                "%llu added, %llu deleted files; %llu groups mismatched, "
                "%llu missing, %llu extra)\n",
                (unsigned long long)fi.seq, ngt,
                (unsigned long long)data_bytes,
                (unsigned long long)stale_bytes,
                (unsigned long long)stale_chg,
                (unsigned long long)stale_add,
                (unsigned long long)stale_del,
                (unsigned long long)out->mismatched,
                (unsigned long long)out->missing,
                (unsigned long long)out->extra);
    } else {
        fprintf(stderr, "seal: sealed-at-gen-%llu verified clean: %u "
                "groups, %llu data bytes\n",
                (unsigned long long)fi.seq, ngt,
                (unsigned long long)data_bytes);
    }
    return 0;

corrupt:
    if (gr_init) seal_grouper_free(&gr);
    free(col.ents);
    fprintf(stderr, "seal: footer failed its own CRC/shape mid-stream; "
            "treated as unsealed, never trusted\n");
    memset(out, 0, sizeof *out);
    free(sink ? sink->arr : NULL);
    if (sink) memset(sink, 0, sizeof *sink);
    if (untrusted_out) *untrusted_out = 2;
    return 0;

fail:
    if (gr_init) seal_grouper_free(&gr);
    free(col.ents);
    return -1;
}

int vol_seal_verify(invfs_volume *v, invfs_seal_verify *out)
{
    if (!v || !out)
        return -1;
    memset(out, 0, sizeof *out);
    return seal_verify_locked(v, out, 0, NULL, NULL);
}

/* WP401 seal scrub entry point. Strictly read-only: this is the same
 * re-encode-and-compare walk vol_seal_verify runs, plus the per-group
 * ledger. No parity write, no re-seal, no heal -- those belong to
 * vol_seal (WP201), the auto-reseal site (WP403), and --heal (WP402).
 * One walk fills a caller-owned ledger; free(*bad_out) when done (it is
 * NULL when no group drifted). */
int vol_seal_scrub(invfs_volume *v, invfs_seal_badgroup **bad_out,
                   size_t *nbad_out, invfs_seal_verify *totals_out)
{
    invfs_seal_verify tot;
    scrub_sink sink;
    int untrusted = 0;
    int rc;
    if (!v || !bad_out || !nbad_out || !totals_out)
        return -1;
    memset(&tot, 0, sizeof tot);
    memset(totals_out, 0, sizeof *totals_out);
    memset(&sink, 0, sizeof sink);
    *bad_out = NULL;
    *nbad_out = 0;
    rc = seal_verify_locked(v, &tot, 0, &sink, &untrusted);
    if (rc != 0 || untrusted) {
        free(sink.arr);
        return rc != 0 ? -1 : untrusted; /* 1 unsealed, 2 torn footer */
    }
    *totals_out = tot;
    *bad_out = sink.arr;         /* NULL when no bad groups */
    *nbad_out = sink.n;
    return 0;
}

/* v1 is detect-only: there is nothing to repair with. (Declared in
 * volume.h; no in-tree caller today. Defined here so the contract lives
 * next to the seal instead of as a second refusal stub.) */
int vol_seal2_repair(invfs_volume *v, invfs_seal2_repair *rep)
{
    (void)v;
    if (rep) memset(rep, 0, sizeof *rep);
    fprintf(stderr, "seal: v1 is detect-only; there is no parity to rebuild "
            "from (scrub auto-heal is a later WP)\n");
    return -1;
}
