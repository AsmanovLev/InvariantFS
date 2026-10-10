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
static int seal_emit_group(invfs_volume *v, invfs_wsession *pws,
                           seal_grouper *gr, uint64_t *par_off,
                           invfs_wsession *fws, uint64_t *foot_off,
                           seal_crc *fcrc, uint32_t *ngroups,
                           unsigned datasyms, uint64_t databytes);
static int seal_verify_locked(invfs_volume *v, invfs_seal_verify *out,
                              uint64_t expect_seq);
static int seal_feed_verify(invfs_volume *v, seal_grouper *gr,
                            const uint8_t *data, size_t len, uint64_t pino,
                            uint64_t *poff, uint32_t *gi, uint32_t ngroups,
                            invfs_seal_verify *out);

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
            if (seal_verify_locked(v, &sv, seq) != 0 ||
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

/* One completed verify group: compare re-encoded parity to stored bytes.
 * A stored-parity read failure counts the group mismatched (parity damage
 * is loud), and the stream still advances — never trusted, never stuck. */
static int seal_feed_verify(invfs_volume *v, seal_grouper *gr,
                            const uint8_t *data, size_t len, uint64_t pino,
                            uint64_t *poff, uint32_t *gi, uint32_t ngroups,
                            invfs_seal_verify *out)
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
        } else if (seal_read_full(v, pino, *poff, pb,
                                  (size_t)gr->m * SEAL_SYM_BYTES) !=
                   (int)((size_t)gr->m * SEAL_SYM_BYTES)) {
            out->mismatched++;
        } else {
            for (i = 0; i < gr->m; i++)
                if (memcmp(pb + (size_t)i * SEAL_SYM_BYTES,
                           gr->par + (size_t)i * SEAL_SYM_BYTES,
                           SEAL_SYM_BYTES) != 0) {
                    out->mismatched++;
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
                            uint64_t *bytes_out)
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
                             ngroups, out) != 0) {
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
                                 ngroups, out) != 0)
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
 * for I/O/namespace failures that make even "unsealed" unclaimable. */
static int seal_verify_locked(invfs_volume *v, invfs_seal_verify *out,
                              uint64_t expect_seq)
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
    if (!v)
        return -1;
    if (vol_find_rc(v, SEAL_FOOTER_NAME, &fino) != 1 || !fino)
        return 0;                       /* unsealed: all-zero, silent */
    if (vol_stat_full(v, SEAL_FOOTER_NAME, &fino, &fsz, &ctime) != 0)
        return -1;
    if (fsz < SEAL_PRELUDE_LEN + SEAL_TRAILER_LEN ||
        fsz >= (uint64_t)1 << 32) {
        fprintf(stderr, "seal: footer present but implausible (%llu B); "
                "treated as unsealed, never trusted\n",
                (unsigned long long)fsz);
        return 0;
    }
    if (seal_read_full(v, fino, 0, pre, sizeof pre) != (int)sizeof pre)
        return -1;
    if (memcmp(pre, SEAL_FOOTER_MAGIC, 8) != 0 ||
        get16(pre + 8) != SEAL_FORMAT_VERSION) {
        fprintf(stderr, "seal: footer present but not a v1 footer; treated "
                "as unsealed, never trusted\n");
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
        return 0;
    }
    fi.nfiles = nft;
    fi.ngroups = ngt;
    if (vol_find_rc(v, SEAL_PARITY_NAME, &pino) != 1 || !pino) {
        /* Footer without parity: every group is missing. Loud, not silent:
         * something deleted the parity out from under a live seal. */
        out->sealed = ngt;
        out->missing = ngt;
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
                                      &gi, ngt, out, h, &nb);
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
                                          ngt, out, h, &nb);
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
                                          ngt, out, h, &nb);
                if (sr < 0) goto fail;
                data_bytes += nb;
            }
        } else {
            /* Manifested but gone: deleted since the seal. */
            out->missing++;
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
                                  &gi, ngt, out, h, &nb);
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
            } else if (seal_read_full(v, pino, poff, pb,
                                      (size_t)fi.m * SEAL_SYM_BYTES) !=
                       (int)((size_t)fi.m * SEAL_SYM_BYTES)) {
                out->mismatched++;
            } else {
                for (i = 0; i < fi.m; i++)
                    if (memcmp(pb + (size_t)i * SEAL_SYM_BYTES,
                               gr.par + (size_t)i * SEAL_SYM_BYTES,
                               SEAL_SYM_BYTES) != 0) {
                        out->mismatched++;
                        break;
                    }
                poff += (uint64_t)fi.m * SEAL_SYM_BYTES;
            }
            gi++;
        }
        if (gi < ngt)
            out->mismatched += (uint64_t)(ngt - gi);  /* unprovable groups */
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
            if (!ds || ds > fi.k || !db || db > (uint64_t)ds * fi.sym)
                out->mismatched++;
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
    return seal_verify_locked(v, out, 0);
}

/* v1 is detect-only: there is nothing to repair with. (Declared in
 * volume.h; no in-tree caller today. Defined here so the contract lives
 * next to the seal instead of as a second refusal stub.)
 *
 * WP402: v1 detect-only still holds for AUTOMATIC paths (scrub, sweep,
 * read path never rewrite). Explicit repair is vol_seal_heal below -- new
 * functions only; nothing above this line changed for it. */
int vol_seal2_repair(invfs_volume *v, invfs_seal2_repair *rep)
{
    (void)v;
    if (rep) memset(rep, 0, sizeof *rep);
    fprintf(stderr, "seal: v1 is detect-only; there is no parity to rebuild "
            "from (scrub auto-heal is a later WP)\n");
    return -1;
}

/* ================= WP402 heal (explicit --heal only) ======================
 *
 * WHY THIS EXISTS. Owner decision: parity drift is repaired ONLY by an
 * explicit heal command -- healing takes long (full group re-reads +
 * reconstruction + verify) and must never stall a sweep, a mount, or a
 * read. WP401's scrub finds and names bad groups (`seal-heal-needed
 * <group-id> <reason>`); this half rebuilds them. No auto-heal anywhere:
 * not in scrub, not in sweep, not in the read path.
 *
 * SHAPE. vol_seal_heal(v, gids, ngids, dry, rep):
 *   manifest (strict footer parse + parity header + cross-checks) ->
 *   per-file audit (size + BLAKE3 vs the manifest; a symlink's sealed
 *   length is its live length only when its hash matches, never trusted
 *   blind) -> per-group detection (the same recompute verify uses:
 *   re-encode each group over sealed-layout bytes, memcmp stored parity)
 *   -> NULL gids heals every bad group, explicit gids heal exactly those
 *   -> per group, data-intact rewrites parity, else a bounded erasure
 *   search (e <= m) with whole-file-hash arbitration; a reconstruction
 *   that does not verify is DISCARDED, never written -> reverify every
 *   targeted group. One bad group never aborts the rest.
 *
 * WHY HASHES ARBITRATE (not parity memcmp). An rs_decode with a wrong
 * erasure set still returns parity-consistent bytes by construction, so a
 * recompute-vs-stored memcmp cannot pick the right candidate. The
 * manifest's per-file BLAKE3s can: exactly the true erasure set decodes
 * to bytes whose files hash-match. Bytes outside the healed group source
 * live -- including not-yet-healed groups -- and the hash FAILS if those
 * are wrong, so a drifted file spanning two bad groups arbitrates
 * instead of deadlocking. The literal recompute memcmp still runs as a
 * sanity assert plus the post-write reverify.
 *
 * WHY EVERY ERASED BYTE NEEDS A WITNESS. Coverage is byte-precise, not
 * slot-precise: a file covering part of a slot says nothing about the
 * slot's other bytes (measured: slot-granular cover credited a file for
 * a whole slot and wrote decode-polluted bytes into the uncovered part).
 * An erased byte is proven only by a matched file holding it, or by the
 * pad-zero check. Write-back writes matched files' full spans and
 * nothing else; every touched file is re-hashed, so audit state is
 * earned and the reverify reports the truth. Files no candidate can
 * prove -- spanning an unhealed group with unreadable bytes, implicating
 * symlink targets, type-changed names -- defer or refuse, honestly.
 * (An earlier shape completed stale slots through the next group's
 * parity; that check is vacuous -- both parities use the same RS row,
 * so every group-consistent candidate satisfies it -- and is gone.)
 *
 * WHAT IT DOES NOT DO. Footer never rewritten (shapes unchanged, so the
 * seal generation is untouched); symlink targets never rewritten (a group
 * whose erased symbols implicate symlink bytes is refused); extra live
 * files are left alone (they are staleness, not drift); directories and
 * other 0-byte entries never enter the stream.
 *
 * CRASH CONTRACT. Each group touches files XOR parity, never both, and
 * every session commit is atomic, so a killed heal leaves healed-or-prior
 * per group, never half-written. INVFS_FAULT="vol_seal_heal_crash:1"
 * simulates the kill after the first group's commit (the image-G probe).
 * INVFS_HEAL_SLOW_MS=<ms> sleeps that long after each group's commit so
 * a real kill -9 can land mid-heal; unset (production) sleeps nothing.
 */

#include <time.h>

/* INVFS_FAULT site armed after a heal group's commit: fires exactly as a
 * kill -9 there would (committed groups healed, the rest prior). */
#define SEAL_HEAL_FAULT_CRASH "vol_seal_heal_crash"

#define HEAL_UNK_OFF ((uint64_t)0xFFFFFFFFFFFFFFFFull)

typedef enum {
    HF_INTACT = 0,    /* live bytes == sealed bytes (size + hash prove it) */
    HF_CHANGED,       /* present, readable, size or hash differs */
    HF_MISSING,       /* no such live name */
    HF_UNREADABLE,    /* present but content cannot be read */
    HF_WRONGTYPE      /* present but not the manifest kind (never sourced) */
} heal_fstate;

typedef struct {
    uint64_t ino;     /* live inode, 0 when !present */
    uint64_t lsize;   /* live size */
    uint32_t ltype;   /* live type (INVFS_ITYP_*) */
    int present;
    heal_fstate st;
    uint64_t elen;    /* covered stream length (REG: manifest size;
                       * LNK: hash-confirmed target length) */
    uint64_t estart;  /* stream offset, HEAL_UNK_OFF while unmappable */
    int len_ok;       /* elen is the sealed length */
    uint8_t *ldata;   /* INTACT LNK target bytes (owned, may be NULL/empty) */
    size_t ldata_len;
} heal_file;

typedef struct {
    seal_footinfo fi;
    unsigned algo;
    seal_entry *ents;     /* nft, manifest order == stream order */
    seal_group *grps;     /* ngt */
    heal_file *files;     /* nft, parallel to ents */
    uint64_t *goff;       /* ngt, stream offset of each group */
    uint64_t pino;        /* live parity inode */
    uint64_t mapped;      /* stream prefix with known layout */
    uint64_t total;       /* full stream bytes (valid iff mapped == total) */
} heal_manifest;

static void heal_manifest_free(heal_manifest *hm)
{
    uint32_t i;
    if (!hm) return;
    free(hm->ents);
    free(hm->grps);
    if (hm->files)
        for (i = 0; i < hm->fi.nfiles; i++)
            free(hm->files[i].ldata);
    free(hm->files);
    free(hm->goff);
    memset(hm, 0, sizeof *hm);
}

/* Chunked content hash of a live REG file. 0 = hashed (hash_out filled),
 * 1 = unreadable (short/error), -1 = bad arguments. Never trusts size. */
static int heal_hash_reg(invfs_volume *v, uint64_t ino, uint64_t size,
                         uint8_t hash_out[32])
{
    static uint8_t chunk[SEAL_SYM_BYTES];
    blake3_hasher hb;
    uint64_t off = 0;
    if (!v || !ino || !hash_out) return -1;
    blake3_hasher_init(&hb);
    while (off < size) {
        size_t want = size - off > sizeof chunk ? sizeof chunk :
                                                    (size_t)(size - off);
        int r = seal_read_full(v, ino, off, chunk, want);
        if (r != (int)want) return 1;
        blake3_hasher_update(&hb, chunk, want);
        off += want;
    }
    blake3_hasher_finalize(&hb, hash_out, 32);
    return 0;
}

/* Load + audit the manifest. 0 = ready, 1 = no valid seal (nothing to
 * heal from; message printed), -1 = I/O error (message printed). */
static int heal_manifest_load(invfs_volume *v, heal_manifest *hm)
{
    uint64_t fino = 0, fsz = 0, psz = 0, ctime = 0;
    uint8_t *fbuf = NULL, phdr[SEAL_PARHDR_LEN];
    unsigned pk = 0, pm = 0;
    uint32_t png = 0;
    uint64_t pseq = 0, fseq = 0;
    uint32_t i;
    uint64_t off;
    memset(hm, 0, sizeof *hm);
    if (vol_find_rc(v, SEAL_FOOTER_NAME, &fino) != 1 || !fino) {
        fprintf(stderr, "heal: not sealed (no " SEAL_FOOTER_NAME
                "); nothing to heal\n");
        return 1;
    }
    if (vol_stat_full(v, SEAL_FOOTER_NAME, &fino, &fsz, &ctime) != 0 ||
        fsz < SEAL_PRELUDE_LEN + SEAL_TRAILER_LEN ||
        fsz >= (uint64_t)1 << 32) {
        fprintf(stderr, "heal: seal footer unreadable; nothing to heal\n");
        return 1;
    }
    fbuf = malloc((size_t)fsz);
    if (!fbuf) { fprintf(stderr, "heal: out of memory\n"); return -1; }
    if (seal_read_full(v, fino, 0, fbuf, (size_t)fsz) != (int)fsz) {
        fprintf(stderr, "heal: seal footer unreadable; nothing to heal\n");
        free(fbuf);
        return 1;
    }
    if (seal_footer_parse(fbuf, (size_t)fsz, &hm->fi, &hm->ents,
                          &hm->grps) != 0) {
        fprintf(stderr, "heal: seal footer invalid; treated as unsealed, "
                "never trusted\n");
        free(fbuf);
        return 1;
    }
    free(fbuf);
    fbuf = NULL;
    if (!hm->fi.nfiles || !hm->fi.ngroups) {
        fprintf(stderr, "heal: seal covers nothing; nothing to heal\n");
        heal_manifest_free(hm);
        return 1;
    }
    /* Footer prelude carries the group code (same read verify uses). */
    {
        uint64_t ino2 = 0;
        uint8_t pre[SEAL_PRELUDE_LEN];
        hm->algo = RS_ALGO_VM;
        if (vol_find_rc(v, SEAL_FOOTER_NAME, &ino2) == 1 && ino2 &&
            seal_read_full(v, ino2, 0, pre, sizeof pre) ==
            (int)sizeof pre &&
            (get16(pre + 18) == RS_ALGO_VM ||
             get16(pre + 18) == RS_ALGO_CAUCHY))
            hm->algo = get16(pre + 18);
        fseq = get64(pre + 20);
    }
    if (vol_find_rc(v, SEAL_PARITY_NAME, &hm->pino) != 1 || !hm->pino) {
        fprintf(stderr, "heal: seal parity file is gone; nothing to "
                "rebuild from\n");
        heal_manifest_free(hm);
        return 1;
    }
    if (vol_stat_full(v, SEAL_PARITY_NAME, &hm->pino, &psz, &ctime) != 0 ||
        seal_read_full(v, hm->pino, 0, phdr, sizeof phdr) !=
        (int)sizeof phdr ||
        seal_parhdr_parse(phdr, sizeof phdr, &pk, &pm, &png, &pseq) != 0 ||
        pk != hm->fi.k || pm != hm->fi.m || png != hm->fi.ngroups ||
        pseq != fseq ||
        psz != SEAL_PARHDR_LEN + (uint64_t)png * pm * SEAL_SYM_BYTES) {
        fprintf(stderr, "heal: seal parity disagrees with the footer; "
                "refusing (re-seal heals this)\n");
        heal_manifest_free(hm);
        return 1;
    }
    hm->files = calloc(hm->fi.nfiles, sizeof *hm->files);
    hm->goff = malloc(sizeof *hm->goff * hm->fi.ngroups);
    if (!hm->files || !hm->goff) {
        fprintf(stderr, "heal: out of memory\n");
        heal_manifest_free(hm);
        return -1;
    }
    off = 0;
    for (i = 0; i < hm->fi.ngroups; i++) {
        hm->goff[i] = off;
        off += hm->grps[i].databytes;
    }
    /* Per-file audit + layout. REG lengths are manifest-authoritative;
     * a symlink's sealed length is knowable only through its hash. The
     * first unconfirmed length ends the mapped prefix: every later
     * entry stays HEAL_UNK_OFF (positions past an unknown length are
     * unknowable), and only groups inside the prefix are mappable. */
    hm->mapped = 0;
    hm->total = 0;
    {
    for (i = 0; i < hm->fi.nfiles; i++) {
        seal_entry *e = &hm->ents[i];
        heal_file *f = &hm->files[i];
        uint64_t lid = 0;
        int frc;
        f->estart = HEAL_UNK_OFF;
        if (e->ftype != SEAL_FT_REG && e->ftype != SEAL_FT_LNK) {
            f->st = HF_INTACT;   /* 0-byte entry: nothing to audit */
            f->elen = 0;
            f->len_ok = 1;
        } else if ((frc = vol_find_rc(v, e->name, &lid)) < 0) {
            fprintf(stderr, "heal: lookup failed for %s\n", e->name);
            heal_manifest_free(hm);
            return -1;
        } else if (frc != 1 || !lid) {
            f->st = HF_MISSING;
            f->elen = (e->ftype == SEAL_FT_REG) ? e->size : 0;
            f->len_ok = (e->ftype == SEAL_FT_REG);
        } else {
            invfs_inode in;
            f->present = 1;
            f->ino = lid;
            if (vol_inode_get(v, lid, &in) != 1) {
                fprintf(stderr, "heal: cannot stat %s\n", e->name);
                heal_manifest_free(hm);
                return -1;
            }
            f->lsize = in.size;
            f->ltype = in.type;
            if (e->ftype == SEAL_FT_REG && in.type != INVFS_ITYP_REG) {
                f->st = HF_WRONGTYPE;
                f->elen = e->size;
                f->len_ok = 1;
            } else if (e->ftype == SEAL_FT_LNK &&
                       in.type != INVFS_ITYP_LNK) {
                f->st = HF_WRONGTYPE;
                f->elen = 0;
                f->len_ok = 0;
            } else if (e->ftype == SEAL_FT_REG) {
                uint8_t h[32];
                int hr = -1;
                if (f->lsize == e->size)
                    hr = heal_hash_reg(v, lid, f->lsize, h);
                if (hr == 0 && memcmp(h, e->hash, 32) == 0) {
                    f->st = HF_INTACT;
                } else if (hr == 1 || f->lsize != e->size) {
                    f->st = (hr == 1) ? HF_UNREADABLE : HF_CHANGED;
                } else {
                    f->st = HF_CHANGED;
                }
                f->elen = e->size;
                f->len_ok = 1;
            } else { /* SEAL_FT_LNK, live type matches */
                uint8_t *t = NULL;
                size_t tl = 0;
                if (vol_read_file(v, lid, &t, &tl) != 0) {
                    f->st = HF_UNREADABLE;
                    free(t);
                } else {
                    blake3_hasher hb;
                    uint8_t h[32];
                    blake3_hasher_init(&hb);
                    blake3_hasher_update(&hb, t ? t : (uint8_t *)"", tl);
                    blake3_hasher_finalize(&hb, h, sizeof h);
                    if (memcmp(h, e->hash, 32) == 0) {
                        f->st = HF_INTACT;
                        f->ldata = t;
                        f->ldata_len = tl;
                        t = NULL;
                        f->elen = tl;
                        f->len_ok = 1;
                    } else {
                        f->st = HF_CHANGED;
                    }
                    free(t);
                }
                if (!f->len_ok) f->elen = 0;
            }
        }
        if (f->len_ok && hm->total == 0) {
            f->estart = hm->mapped;
            hm->mapped += f->elen;
        } else {
            /* unconfirmed length (or anything after one): unmappable */
            f->estart = HEAL_UNK_OFF;
            hm->total = 1;   /* latch: the prefix ended here */
        }
    }
    }
    if (hm->total == 0) hm->total = hm->mapped;
    else hm->total = HEAL_UNK_OFF;
    return 0;
}

/* Group g mappable: its whole databyte range sits in the known prefix. */
static int heal_group_mappable(const heal_manifest *hm, uint32_t g)
{
    uint64_t end;
    if (!hm || g >= hm->fi.ngroups) return 0;
    if (hm->mapped == HEAL_UNK_OFF) return 0;
    end = hm->goff[g] + hm->grps[g].databytes;
    return end <= hm->mapped;
}

/* Source one stream span into buf. Entries are manifest-ordered and groups
 * are visited in order, so a cursor does the entry lookup in one pass.
 * Unknown bytes are zero-filled and counted in *unknown_out. */
typedef struct { uint32_t ei; } heal_cursor;
static int heal_source_span(invfs_volume *v, heal_manifest *hm,
                            heal_cursor *cur, uint64_t soff, uint8_t *buf,
                            size_t len, uint64_t *unknown_out)
{
    size_t done = 0;
    uint64_t unk = 0;
    if (!v || !hm || !cur || !buf) return -1;
    if (unknown_out) *unknown_out = 0;
    while (done < len) {
        uint64_t pos = soff + done;
        /* advance past entries that end at/before pos */
        while (cur->ei < hm->fi.nfiles) {
            heal_file *f = &hm->files[cur->ei];
            if (f->estart == HEAL_UNK_OFF) break;
            if (pos < f->estart + f->elen) break;
            cur->ei++;
        }
        if (cur->ei >= hm->fi.nfiles) {
            /* padding tail: sealed zeros */
            memset(buf + done, 0, len - done);
            done = len;
            break;
        } else {
            heal_file *f = &hm->files[cur->ei];
            seal_entry *e = &hm->ents[cur->ei];
            uint64_t foff, avail, take;
            if (f->estart == HEAL_UNK_OFF || pos < f->estart) {
                /* unmappable hole: unknown */
                take = len - done;
                if (f->estart != HEAL_UNK_OFF && pos < f->estart &&
                    f->estart - pos < take)
                    take = (size_t)(f->estart - pos);
                memset(buf + done, 0, take);
                unk += take;
                done += take;
                continue;
            }
            foff = pos - f->estart;
            avail = f->elen - foff;
            take = len - done < avail ? len - done : (size_t)avail;
            if (e->ftype != SEAL_FT_REG && e->ftype != SEAL_FT_LNK) {
                memset(buf + done, 0, take);  /* cannot happen: elen 0 */
                done += take;
                continue;
            }
            if (f->st == HF_INTACT) {
                if (e->ftype == SEAL_FT_LNK) {
                    if (foff + take > f->ldata_len) {
                        memset(buf + done, 0, take);
                        unk += take;
                    } else {
                        memcpy(buf + done, f->ldata + foff, take);
                    }
                    done += take;
                    continue;
                }
                /* INTACT REG: live bytes ARE sealed bytes */
                if (foff >= f->lsize) {
                    memset(buf + done, 0, take);
                    unk += take;
                    done += take;
                    continue;
                }
                if (foff + take > f->lsize)
                    take = (size_t)(f->lsize - foff);
            } else if (f->st == HF_CHANGED && e->ftype == SEAL_FT_REG) {
                /* Changed but readable: live bytes source the stream
                 * (the recompute then mismatches, as it must). A file
                 * whose size changed sources only its live prefix; the
                 * sealed tail past EOF is unknown. */
                if (!f->present || foff >= f->lsize) {
                    memset(buf + done, 0, take);
                    unk += take;
                    done += take;
                    continue;
                }
                if (foff + take > f->lsize)
                    take = (size_t)(f->lsize - foff);
            } else {
                /* MISSING / UNREADABLE / WRONGTYPE / bad LNK: unknown */
                memset(buf + done, 0, take);
                unk += take;
                done += take;
                continue;
            }
            {
                int r = seal_read_full(v, f->ino, foff, buf + done, take);
                if (r != (int)take) {
                    memset(buf + done, 0, take);
                    unk += take;
                }
                done += take;
            }
        }
    }
    if (unknown_out) *unknown_out = unk;
    return 0;
}

/* Tail-group stale slots (a WP201 quirk, replicated not judged): the seal
 * and verify stream every group through ONE grouper whose symbol buffers
 * are never cleared between groups, so a tail group T>0 encodes slots
 * [datasyms,k) holding group T-1's bytes at the same slot positions --
 * not zeros. (T==0 seals from calloc, so its stale is zeros, and verify
 * replays the same quirk from its own calloc, which is why the two
 * agree.) Healing must source the same bytes or its recompute disagrees
 * with verify's on intact data. Fills dst's slots [ds,k); 0 = filled
 * (zeros for T==0), -1 = some stale byte unknown (caller defers until
 * group T-1 is intact-or-healed, then the live bytes ARE the sealed
 * ones -- healed files read back through the committed sessions). */
static int heal_tail_stale(invfs_volume *v, heal_manifest *hm, uint32_t g,
                           uint8_t *dst)
{
    unsigned k = hm->fi.k, ds = hm->grps[g].datasyms;
    heal_cursor cur;
    uint64_t soff, unk = 0;
    size_t len;
    if (ds > k) ds = k;
    len = ((size_t)k - ds) * SEAL_SYM_BYTES;
    if (!len) return 0;
    if (g == 0) {
        memset(dst + (size_t)ds * SEAL_SYM_BYTES, 0, len);
        return 0;
    }
    /* group g-1 is full (only the last group is partial), so its slots
     * [ds,k) are a contiguous sealed stream range. */
    soff = hm->goff[g - 1] + (uint64_t)ds * SEAL_SYM_BYTES;
    memset(&cur, 0, sizeof cur);
    if (heal_source_span(v, hm, &cur, soff, dst + (size_t)ds *
                         SEAL_SYM_BYTES, len, &unk) != 0)
        return -1;
    return unk ? -1 : 0;
}

/* Per-group detection. Returns 0 (good) or 1 (bad, reason filled).
 * Mirrors the verify recompute: re-encode the sealed-layout bytes and
 * memcmp stored parity -- plus, when files != 0, the file-state rule,
 * so a change the parity cannot see (nullspace) still names the group.
 * The post-heal reverify passes files == 0: a healed group's own bytes
 * must recompute, but drift living in another (unhealed) group must not
 * fail this group's verdict. */
static int heal_detect_group(invfs_volume *v, heal_manifest *hm,
                             uint32_t g, char reason[48], int files)
{
    uint64_t goff, gdb, poff;
    uint8_t *sym = NULL, *par = NULL, **dptr = NULL, **pptr = NULL;
    uint8_t expect[2 * SEAL_SYM_BYTES];
    unsigned k, m, i, bad_state = 0;
    uint32_t e;
    heal_cursor cur;
    int rc = 1;
    if (!v || !hm || g >= hm->fi.ngroups) return 1;
    if (reason) reason[0] = 0;
    k = hm->fi.k; m = hm->fi.m;
    goff = hm->goff[g]; gdb = hm->grps[g].databytes;
    if (!heal_group_mappable(hm, g)) {
        if (reason) snprintf(reason, 48, "unmappable layout");
        return 1;
    }
    /* file-state rule first (detection only): the honest reason beats
     * a memcmp. Skipped for reverify -- see the function contract. */
    if (files) { for (e = 0; e < hm->fi.nfiles; e++) {
        heal_file *f = &hm->files[e];
        seal_entry *en = &hm->ents[e];
        uint64_t es, ee;
        if (en->ftype != SEAL_FT_REG && en->ftype != SEAL_FT_LNK)
            continue;
        if (!f->len_ok || f->estart == HEAL_UNK_OFF) continue;
        es = f->estart; ee = es + f->elen;
        if (es >= goff + gdb || ee <= goff) continue;
        if (f->st != HF_INTACT) {
            bad_state = 1;
            if (reason) {
                const char *r = "content drift";
                if (f->st == HF_MISSING) r = "file missing";
                else if (f->st == HF_UNREADABLE) r = "unreadable content";
                else if (f->st == HF_WRONGTYPE) r = "type changed";
                snprintf(reason, 48, "%s", r);
            }
            break;
        }
    }
    }
    sym = calloc(k ? k : 1, SEAL_SYM_BYTES);
    par = malloc((m ? m : 1) * (size_t)SEAL_SYM_BYTES);
    dptr = malloc(sizeof *dptr * (k ? k : 1));
    pptr = malloc(sizeof *pptr * (m ? m : 1));
    if (!sym || !par || !dptr || !pptr) goto out;
    for (i = 0; i < k; i++) dptr[i] = sym + (size_t)i * SEAL_SYM_BYTES;
    for (i = 0; i < m; i++) pptr[i] = par + (size_t)i * SEAL_SYM_BYTES;
    memset(&cur, 0, sizeof cur);
    {
        /* stream the group in symbol steps (bounded RAM, huge files OK) */
        uint64_t left = gdb, pos = goff;
        unsigned s = 0;
        uint64_t unk_total = 0;
        while (left) {
            size_t want = left > SEAL_SYM_BYTES ? SEAL_SYM_BYTES :
                                                  (size_t)left;
            uint64_t unk = 0;
            if (heal_source_span(v, hm, &cur, pos, sym +
                                 (size_t)s * SEAL_SYM_BYTES, want, &unk) != 0)
                goto out;
            unk_total += unk;
            pos += want;
            left -= want;
            s++;
        }
        if (unk_total || bad_state) {
            /* the file-state rule above may have named a sharper
             * reason already; keep it, else fall back. */
            if (reason && !reason[0])
                snprintf(reason, 48, "%s", bad_state ? "content drift" :
                         "unreadable content");
            rc = 1;
            goto out;
        }
        /* tail stale slots: the previous group's bytes, not zeros
         * (WP201 quirk -- see heal_tail_stale). Unprovable without
         * group g-1 intact-or-healed, so then the group stays named. */
        if (heal_tail_stale(v, hm, g, sym) != 0) {
            if (reason) snprintf(reason, 48, "parity-mismatch");
            rc = 1;
            goto out;
        }
    }
    poff = SEAL_PARHDR_LEN + (uint64_t)g * m * SEAL_SYM_BYTES;
    if (seal_read_full(v, hm->pino, poff, expect, (size_t)m * SEAL_SYM_BYTES)
        != (int)((size_t)m * SEAL_SYM_BYTES)) {
        if (reason) snprintf(reason, 48, "parity unreadable");
        goto out;
    }
    if (rs_encode((int)hm->algo, k, m, SEAL_SYM_BYTES, dptr, pptr) != 0)
        goto out;
    for (i = 0; i < m; i++)
        if (memcmp(expect + (size_t)i * SEAL_SYM_BYTES,
                   par + (size_t)i * SEAL_SYM_BYTES, SEAL_SYM_BYTES) != 0) {
            if (reason) snprintf(reason, 48, "parity-mismatch");
            goto out;
        }
    rc = 0;
out:
    free(sym); free(par); free(dptr); free(pptr);
    return rc;
}

/* Byte source for validation/assembly: group g reads the candidate,
 * every other byte reads live (healed groups committed, so live IS
 * sealed there; unhealed groups read live too -- the whole-file hash is
 * the arbiter, and it fails if those bytes are wrong). Missing or
 * short files fail the span (caller skips the file). Returns 1 iff the
 * full manifest span assembled (I/O ok). */
static int heal_file_span(invfs_volume *v, heal_manifest *hm,
                          const uint8_t *healed, uint32_t ngroups,
                          uint32_t g, const uint8_t *cand,
                          uint32_t ei, uint8_t *out)
{
    seal_entry *e = &hm->ents[ei];
    heal_file *f = &hm->files[ei];
    uint64_t span = (e->ftype == SEAL_FT_REG) ? e->size : f->elen;
    uint64_t done = 0;
    (void)ngroups;
    (void)healed;
    if (e->ftype != SEAL_FT_REG && e->ftype != SEAL_FT_LNK) return 1;
    if (!f->len_ok || f->estart == HEAL_UNK_OFF) return 0;
    if (e->ftype == SEAL_FT_LNK) {
        /* LNK validation is slice-wise at the call site; full spans of
         * changed symlinks are never assembled (never rewritten). */
        return (f->st == HF_INTACT) ? 1 : 0;
    }
    while (done < span) {
        uint64_t pos = f->estart + done;
        uint32_t gi;
        uint64_t gb, ge, take;
        /* locate the group holding pos */
        gi = 0;
        while (gi + 1 < hm->fi.ngroups &&
               hm->goff[gi + 1] <= pos)
            gi++;
        gb = hm->goff[gi];
        ge = gb + hm->grps[gi].databytes;
        if (pos >= ge) { /* past the last databyte: padding, no file */
            return 0;
        }
        take = ge - pos;
        if (take > span - done) take = span - done;
        if (gi == g) {
            uint64_t coff = pos - gb;
            memcpy(out + done, cand + coff, (size_t)take);
        } else {
            int r;
            if (!f->present || !f->ino) return 0;
            r = seal_read_full(v, f->ino, done, out + done,
                               (size_t)take);
            if (r != (int)take) return 0;
        }
        done += take;
    }
    return 1;
}

/* Validate one decode candidate for group g. 1 = verified, 0 = discard.
 *
 * The arbiter is the manifest's per-file BLAKE3s over jointly sourced
 * bytes: the candidate inside g, live bytes everywhere else. Live bytes
 * of not-yet-healed groups are trusted provisionally -- and the hash
 * FAILS if they are wrong, which is what lets a drifted file spanning
 * two bad groups arbitrate instead of deadlocking. A candidate is
 * verified when every erased byte is PROVEN: held by a file whose
 * whole-span hash matches, or padding the zero check proves. Anything
 * less is discarded, never written. (An earlier shape completed stale
 * slots through the next group's parity; that check is vacuous -- both
 * parities use the same RS row, so every group-consistent candidate
 * satisfies it -- and is gone.)
 *
 * matched_out (optional, pre-sized nfiles, zeroed by caller) receives
 * one bit per file whose hash matched; write-back uses it to write only
 * proven files. NULL skips the recording. */
static int heal_validate_candidate(invfs_volume *v, heal_manifest *hm,
                                   uint32_t g, const uint8_t *sym,
                                   const uint8_t *erased,
                                   uint8_t *matched_out)
{
    uint64_t goff = hm->goff[g], gdb = hm->grps[g].databytes;
    unsigned k = hm->fi.k;
    uint32_t e;
    unsigned s;
    /* padding zeros: the partial symbol's pad is always sealed zeros;
     * full pad slots are zeros only for group 0 (calloc) -- a tail T>0
     * holds the previous group's bytes there (WP201 quirk), sourced
     * known and untouched by the decode, so unchecked here. */
    {
        uint64_t ds = hm->grps[g].datasyms;
        uint64_t p;
        if (ds > k) ds = k;
        for (p = gdb; p < ds * SEAL_SYM_BYTES; p++)
            if (sym[p] != 0) return 0;
        if (g == 0)
            for (p = ds * SEAL_SYM_BYTES;
                 p < (uint64_t)k * SEAL_SYM_BYTES; p++)
                if (sym[p] != 0) return 0;
    }
    /* per-file joint hash -- tiered: only files holding bytes in an
     * erased slot constrain the candidate. A file that merely
     * intersects the group (drifted elsewhere, bytes here kept live)
     * must neither prove nor veto: its bytes are preserved unwritten,
     * and hashing it would veto the true candidate with drift from
     * another group. Unassemblable files (unreadable/missing live bytes
     * outside g) are skipped: their erased bytes stay unproven and sink
     * the candidate below. Intact symlink slices overlapped by erased
     * slots must decode back identically (checked, credited, never
     * written); any other symlink implication refuses. */
    for (e = 0; e < hm->fi.nfiles; e++) {
        seal_entry *en = &hm->ents[e];
        heal_file *f = &hm->files[e];
        uint64_t es, ee, is, ie, q;
        int touches_erased = 0;
        uint8_t *span;
        blake3_hasher hb;
        uint8_t h[32];
        int ok;
        if (en->ftype != SEAL_FT_REG && en->ftype != SEAL_FT_LNK)
            continue;
        if (!f->len_ok || f->estart == HEAL_UNK_OFF) continue;
        es = f->estart; ee = es + f->elen;
        if (es >= goff + gdb || ee <= goff) continue;
        is = es < goff ? goff : es;
        ie = ee > goff + gdb ? goff + gdb : ee;
        for (q = (is - goff) / SEAL_SYM_BYTES;
             q < k && (uint64_t)q * SEAL_SYM_BYTES < ie - goff; q++)
            if (erased[q]) { touches_erased = 1; break; }
        if (!touches_erased) continue;
        if (en->ftype != SEAL_FT_REG) {
            /* Symlink bytes in an erased slot: intact targets are
             * hash-proven live bytes, so every overlapped slice must
             * decode back identically (checked, never written -- this
             * constrains wrong erasure sets for free). A drifted,
             * missing, or unreadable target refuses the group: symlink
             * targets are never rewritten. (In practice a changed
             * target also breaks the layout first -- its sealed length
             * is unconfirmable -- so the unmappable refusal usually
             * fires before this one; both are clean.) */
            uint64_t q;
            if (f->st != HF_INTACT) return 0;
            for (q = 0; q < k; q++) {
                uint64_t qs0 = goff + q * SEAL_SYM_BYTES;
                uint64_t qs1 = qs0 + SEAL_SYM_BYTES;
                uint64_t a = is > qs0 ? is : qs0;
                uint64_t b = ie < qs1 ? ie : qs1;
                if (!erased[q] || a >= b) continue;
                if (a - es + (b - a) > f->ldata_len) return 0;
                if (memcmp(sym + (a - goff), f->ldata + (a - es),
                           (size_t)(b - a)) != 0)
                    return 0;
            }
            /* verified: these bytes are proven (intact target decoded
             * back identically). Recorded so coverage credits them;
             * write-back still never touches symlinks. */
            if (matched_out) matched_out[e] = 1;
            continue;
        }
        span = malloc(en->size ? (size_t)en->size : 1);
        if (!span) return 0;
        ok = heal_file_span(v, hm, NULL, 0, g, sym, e, span);
        if (ok) {
            blake3_hasher_init(&hb);
            if (en->size)
                blake3_hasher_update(&hb, span, (size_t)en->size);
            blake3_hasher_finalize(&hb, h, sizeof h);
            ok = (memcmp(h, en->hash, 32) == 0);
        } else {
            ok = -1;   /* unassemblable: skip, do not fail */
        }
        free(span);
        if (ok == 0) return 0;
        if (ok == 1 && matched_out) matched_out[e] = 1;
    }
    /* coverage: every erased byte must lie in a matched file's range or
     * in checked padding. Byte-precise (not slot-precise): a file
     * covering part of a slot says nothing about the rest. Entry ranges
     * tile the stream in order, so one sweep per erased slot finds gaps. */
    for (s = 0; s < k; s++) {
        uint64_t sb0, sb1, uncovered;
        if (!erased[s]) continue;
        sb0 = goff + (uint64_t)s * SEAL_SYM_BYTES;
        sb1 = sb0 + SEAL_SYM_BYTES;
        if (sb1 > goff + gdb) sb1 = goff + gdb;  /* pad checked above */
        uncovered = sb0;
        for (e = 0; e < hm->fi.nfiles && uncovered < sb1; e++) {
            uint64_t a, b;
            unsigned ft = hm->ents[e].ftype;
            if (ft != SEAL_FT_REG && ft != SEAL_FT_LNK) continue;
            if (!matched_out || !matched_out[e]) continue;
            if (!hm->files[e].len_ok ||
                hm->files[e].estart == HEAL_UNK_OFF)
                continue;
            a = hm->files[e].estart;
            b = a + hm->files[e].elen;
            if (a < sb0) a = sb0;
            if (b > sb1) b = sb1;
            if (a >= b || a > uncovered) continue;
            if (b > uncovered) uncovered = b;
        }
        if (uncovered < sb1) return 0;
    }
    return 1;
}

/* Re-audit one REG file after write-back: fresh lookup + full hash.
 * Audit state is EARNED (re-read), never assumed from what was written.
 * 0 = audited (st set, maybe INTACT), -1 = lookup hard error. */
static int heal_refresh_reg(invfs_volume *v, heal_manifest *hm, uint32_t e)
{
    seal_entry *en = &hm->ents[e];
    heal_file *f = &hm->files[e];
    uint64_t lid = 0;
    invfs_inode in;
    uint8_t h[32];
    int frc;
    if (en->ftype != SEAL_FT_REG) return 0;
    frc = vol_find_rc(v, en->name, &lid);
    if (frc < 0) return -1;
    f->present = 0;
    f->ino = 0;
    if (frc != 1 || !lid) { f->st = HF_MISSING; return 0; }
    if (vol_inode_get(v, lid, &in) != 1) return -1;
    f->present = 1;
    f->ino = lid;
    f->lsize = in.size;
    f->ltype = in.type;
    if (in.type != INVFS_ITYP_REG) { f->st = HF_WRONGTYPE; return 0; }
    if (in.size != en->size) { f->st = HF_CHANGED; return 0; }
    if (heal_hash_reg(v, lid, in.size, h) != 0) {
        f->st = HF_UNREADABLE;
        return 0;
    }
    f->st = (memcmp(h, en->hash, 32) == 0) ? HF_INTACT : HF_CHANGED;
    return 0;
}

/* Write one healed REG file's full manifest span. 0 ok, -1 error. */
static int heal_write_file(invfs_volume *v, const char *name,
                           const uint8_t *span, uint64_t size,
                           uint64_t *bytes_out)
{
    invfs_wsession *ws = NULL;
    uint64_t done = 0;
    if (vol_write_begin(v, name, 1, &ws) == 0 || !ws) return -1;
    while (done < size) {
        size_t chunk = size - done > (size_t)1 << 20 ? (size_t)1 << 20 :
                                                       (size_t)(size - done);
        if (vol_write_range(ws, done, span + done, chunk) != 0) {
            vol_write_abort(ws);
            return -1;
        }
        done += chunk;
    }
    if (vol_write_truncate(ws, size) != 0) {
        vol_write_abort(ws);
        return -1;
    }
    if (vol_write_commit(ws) != 0) return -1;
    if (bytes_out) *bytes_out += size;
    return 0;
}

/* Test-only pacing: INVFS_HEAL_SLOW_MS=<ms> sleeps after each group's
 * commit so a real kill -9 can land mid-heal. Unset: no sleep. */
static void heal_slow_gate(void)
{
    const char *s = getenv("INVFS_HEAL_SLOW_MS");
    if (s && *s) {
        long ms = strtol(s, NULL, 10);
        struct timespec ts;
        if (ms < 0) ms = 0;
        if (ms > 5000) ms = 5000;
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
}

/* Heal one bad group. Returns 1 healed-parity, 2 healed-data, 0 refused
 * (message printed), -1 hard error (message printed). */
static int heal_one_group(invfs_volume *v, heal_manifest *hm,
                          uint8_t *healed, uint32_t g,
                          invfs_seal_heal_report *rep)
{
    unsigned k = hm->fi.k, m = hm->fi.m;
    uint64_t goff = hm->goff[g], gdb = hm->grps[g].databytes;
    uint64_t poff = SEAL_PARHDR_LEN + (uint64_t)g * m * SEAL_SYM_BYTES;
    uint8_t *sym = NULL, *par = NULL, **blks = NULL, *present = NULL;
    uint8_t *erased = NULL, *matched = NULL;
    unsigned i, e0 = 0, ep = 0;
    uint32_t e;
    int is_parity_path = 1;
    int rc = 0;
    /* unmappable groups (symlink length unconfirmed) refuse cleanly:
     * positions are unknowable, so nothing here is provable. */
    if (!heal_group_mappable(hm, g)) {
        fprintf(stderr, "heal: group %u: unmappable layout (a preceding "
                "symlink's sealed length is unconfirmed); refusing\n", g);
        return 0;
    }
    /* live-type guard: write-back replaces REG rows or creates missing
     * names; anything live-but-not-REG under a covered name refuses. */
    for (e = 0; e < hm->fi.nfiles; e++) {
        heal_file *f = &hm->files[e];
        seal_entry *en = &hm->ents[e];
        uint64_t es, ee;
        if (en->ftype != SEAL_FT_REG && en->ftype != SEAL_FT_LNK)
            continue;
        if (!f->len_ok || f->estart == HEAL_UNK_OFF) continue;
        es = f->estart; ee = es + f->elen;
        if (es >= goff + gdb || ee <= goff) continue;
        if (f->st != HF_INTACT) is_parity_path = 0;
        if (f->present && f->st != HF_INTACT && f->st != HF_MISSING) {
            unsigned want = (en->ftype == SEAL_FT_REG) ? INVFS_ITYP_REG :
                                                        INVFS_ITYP_LNK;
            if (f->ltype != want) {
                fprintf(stderr, "heal: group %u: %s changed type; "
                        "refusing (not a rewrite)\n", g, en->name);
                return 0;
            }
        }
        if (en->ftype == SEAL_FT_LNK && f->st != HF_INTACT) {
            fprintf(stderr, "heal: group %u: symlink %s drifted; "
                    "refusing (targets never rewritten)\n", g, en->name);
            return 0;
        }
    }
    sym = calloc(k ? k : 1, SEAL_SYM_BYTES);
    par = malloc((m ? m : 1) * (size_t)SEAL_SYM_BYTES);
    blks = malloc(sizeof *blks * (k + m > 0 ? k + m : 1));
    present = malloc(k + m > 0 ? k + m : 1);
    erased = calloc(k ? k : 1, 1);
    if (!sym || !par || !blks || !present || !erased) {
        fprintf(stderr, "heal: group %u: out of memory\n", g);
        rc = -1;
        goto out;
    }
    /* fill data symbols from the sealed layout; unknown -> erasure */
    {
        heal_cursor cur;
        memset(&cur, 0, sizeof cur);
        for (i = 0; i < k; i++) {
            uint64_t soff = goff + (uint64_t)i * SEAL_SYM_BYTES;
            uint64_t send = soff + SEAL_SYM_BYTES;
            uint64_t gend = goff + gdb;
            uint8_t *dst = sym + (size_t)i * SEAL_SYM_BYTES;
            uint64_t unk = 0;
            if (soff >= gend) {
                memset(dst, 0, SEAL_SYM_BYTES);  /* pad slot: stale
                                                 * filled below for T>0 */
                continue;
            }
            if (send > gend) {
                /* last partial symbol: bytes then sealed zeros */
                size_t nb = (size_t)(gend - soff);
                if (heal_source_span(v, hm, &cur, soff, dst, nb, &unk)
                    != 0) { rc = -1; goto out; }
                memset(dst + nb, 0, SEAL_SYM_BYTES - nb);
                if (unk) erased[i] = 1;
                continue;
            }
            if (heal_source_span(v, hm, &cur, soff, dst, SEAL_SYM_BYTES,
                                 &unk) != 0) { rc = -1; goto out; }
            if (unk) erased[i] = 1;
        }
    }
    for (i = 0; i < k; i++) if (erased[i]) e0++;
    /* tail stale slots (WP201 quirk): unknown until group g-1 is
     * intact-or-healed -- defer, the fixpoint retries. */
    if (heal_tail_stale(v, hm, g, sym) != 0) {
        rc = 0;
        goto out;
    }
    /* stored parity in place */
    if (seal_read_full(v, hm->pino, poff, par, (size_t)m * SEAL_SYM_BYTES)
        != (int)((size_t)m * SEAL_SYM_BYTES))
        memset(par, 0, (size_t)m * SEAL_SYM_BYTES), ep = m;
    if (is_parity_path && !e0 && !ep) {
        /* data proven intact: recompute + rewrite the parity symbols */
        uint8_t **dptr = malloc(sizeof *dptr * (k ? k : 1));
        uint8_t **pptr = malloc(sizeof *pptr * (m ? m : 1));
        invfs_wsession *ws = NULL;
        if (!dptr || !pptr) {
            free(dptr); free(pptr);
            fprintf(stderr, "heal: group %u: out of memory\n", g);
            rc = -1;
            goto out;
        }
        for (i = 0; i < k; i++)
            dptr[i] = sym + (size_t)i * SEAL_SYM_BYTES;
        for (i = 0; i < m; i++)
            pptr[i] = par + (size_t)i * SEAL_SYM_BYTES;
        if (rs_encode((int)hm->algo, k, m, SEAL_SYM_BYTES, dptr,
                      pptr) != 0) {
            free(dptr); free(pptr);
            fprintf(stderr, "heal: group %u: encode failed\n", g);
            rc = -1;
            goto out;
        }
        free(dptr); free(pptr);
        if (vol_write_begin(v, SEAL_PARITY_NAME, 0, &ws) == 0 || !ws) {
            fprintf(stderr, "heal: group %u: cannot stage parity\n", g);
            rc = -1;
            goto out;
        }
        for (i = 0; i < m; i++)
            if (seal_write_full(ws, poff + (uint64_t)i * SEAL_SYM_BYTES,
                                par + (size_t)i * SEAL_SYM_BYTES,
                                SEAL_SYM_BYTES) != 0) {
                vol_write_abort(ws);
                fprintf(stderr, "heal: group %u: parity write failed\n",
                        g);
                rc = -1;
                goto out;
            }
        if (vol_write_commit(ws) != 0) {
            fprintf(stderr, "heal: group %u: parity commit failed\n", g);
            rc = -1;
            goto out;
        }
        rep->bytes_rewritten += (uint64_t)m * SEAL_SYM_BYTES;
        printf("[heal] group %u: parity rewritten (data intact), "
               "verified\n", g);
        healed[g] = 1;
        rc = 1;
        goto out;
    }
    /* data path: bounded erasure search, hashes arbitrate.
     * Decode needs k survivors of k+m: known data erasures (e0) plus
     * unreadable parity (ep) plus hypothesized extras must fit in m. */
    if (e0 + ep > m) {
        fprintf(stderr, "heal: group %u: cannot reconstruct: needs %u, "
                "has %u\n", g, e0 + ep, m);
        rc = 0;
        goto out;
    }
    /* per-file match map for the accepted candidate (validation fills
     * it; write-back writes only matched files). */
    matched = calloc(hm->fi.nfiles ? hm->fi.nfiles : 1, 1);
    if (!matched) {
        fprintf(stderr, "heal: group %u: out of memory\n", g);
        rc = -1;
        goto out;
    }
    {
        uint8_t *work = malloc((k ? k : 1) * (size_t)SEAL_SYM_BYTES);
        unsigned *cand_idx = malloc(sizeof *cand_idx * (k ? k : 1));
        unsigned ncand = 0, extra, max_extra;
        int found = 0;
        if (!work || !cand_idx) {
            free(work); free(cand_idx);
            fprintf(stderr, "heal: group %u: out of memory\n", g);
            rc = -1;
            goto out;
        }
        for (i = 0; i < k; i++) {
            uint64_t soff = goff + (uint64_t)i * SEAL_SYM_BYTES;
            if (!erased[i] && soff < goff + gdb) cand_idx[ncand++] = i;
        }
        /* try e0 (+ep) first, then widen the hypothesis to fill m.
         * The vacuous round (no erasures at all) is skipped when live
         * files already prove drift: with nothing hypothesized there is
         * nothing to verify, and a parity-matching drift (nullspace)
         * must be searched, not nodded through. */
        max_extra = m - e0 - ep;
        if (max_extra > ncand) max_extra = ncand;
        {
            unsigned extra_start = 0;
            if (e0 == 0) {
                for (e = 0; e < hm->fi.nfiles; e++) {
                    uint64_t es, ee;
                    if (hm->ents[e].ftype != SEAL_FT_REG) continue;
                    if (!hm->files[e].len_ok ||
                        hm->files[e].estart == HEAL_UNK_OFF)
                        continue;
                    es = hm->files[e].estart;
                    ee = es + hm->files[e].elen;
                    if (es >= goff + gdb || ee <= goff) continue;
                    if (hm->files[e].st != HF_INTACT) {
                        extra_start = 1;
                        break;
                    }
                }
            }
            for (extra = extra_start; extra <= max_extra; extra++) {
            /* iterate C(ncand, extra) subsets, lexicographic */
            unsigned *pick = malloc(sizeof *pick * (extra ? extra : 1));
            unsigned pi;
            int more;
            if (!pick) {
                free(work); free(cand_idx);
                fprintf(stderr, "heal: group %u: out of memory\n", g);
                rc = -1;
                goto out;
            }
            for (pi = 0; pi < extra; pi++) pick[pi] = pi;
            more = 1;
            if (extra > ncand) more = 0;
            if (extra == 0) more = 2;  /* exactly one round, no subset */
            while (more) {
                uint8_t *eset;
                unsigned ne = e0 + extra;
                memcpy(work, sym, (size_t)k * SEAL_SYM_BYTES);
                eset = calloc(k ? k : 1, 1);
                if (!eset) {
                    free(pick); free(work); free(cand_idx);
                    fprintf(stderr, "heal: group %u: out of memory\n",
                            g);
                    rc = -1;
                    goto out;
                }
                memcpy(eset, erased, k);
                for (pi = 0; pi < extra; pi++)
                    eset[cand_idx[pick[pi]]] = 1;
                for (i = 0; i < k + m; i++) {
                    if (i < k) {
                        blks[i] = work + (size_t)i * SEAL_SYM_BYTES;
                        present[i] = eset[i] ? 0 : 1;
                        if (eset[i])
                            memset(blks[i], 0, SEAL_SYM_BYTES);
                    } else {
                        blks[i] = par + (size_t)(i - k) * SEAL_SYM_BYTES;
                        present[i] = 1;
                    }
                }
                /* parity erasures (unreadable stored parity): mark */
                for (i = 0; i < ep && i < m; i++) {
                    blks[k + m - 1 - i] =
                        par + (size_t)(m - 1 - i) * SEAL_SYM_BYTES;
                    present[k + m - 1 - i] = 0;
                }
                ne += ep;
                if (ne <= m &&
                    rs_decode((int)hm->algo, k, m, SEAL_SYM_BYTES, blks,
                              present) == 0) {
                    /* sanity: parity-consistent (necessary, not
                     * sufficient -- hashes decide). Skipped when the
                     * stored parity was unreadable (ep>0): nothing
                     * complete to compare against. */
                    uint8_t **dptr =
                        malloc(sizeof *dptr * (k ? k : 1));
                    uint8_t **pptr =
                        malloc(sizeof *pptr * (m ? m : 1));
                    uint8_t *re = malloc((m ? m : 1) *
                                         (size_t)SEAL_SYM_BYTES);
                    int sane = 0;
                    if (dptr && pptr && re) {
                        unsigned j;
                        for (j = 0; j < k; j++)
                            dptr[j] = work + (size_t)j * SEAL_SYM_BYTES;
                        for (j = 0; j < m; j++)
                            pptr[j] = re + (size_t)j * SEAL_SYM_BYTES;
                        sane = (rs_encode((int)hm->algo, k, m,
                                         SEAL_SYM_BYTES, dptr, pptr) == 0 &&
                                (ep > 0 || memcmp(re, par, (size_t)m *
                                                 SEAL_SYM_BYTES) == 0));
                    }
                    free(dptr); free(pptr); free(re);
                    memset(matched, 0, hm->fi.nfiles ? hm->fi.nfiles : 1);
                    if (sane && heal_validate_candidate(v, hm, g, work,
                                                        eset, matched)) {
                        memcpy(sym, work, (size_t)k * SEAL_SYM_BYTES);
                        e0 = ne - ep;
                        found = 1;
                        free(eset);
                        break;
                    }
                }
                free(eset);
                if (more == 2) break;
                /* next lexicographic subset */
                more = 0;
                for (pi = extra; pi-- > 0;) {
                    if (pick[pi] + 1 < ncand - (extra - 1 - pi)) {
                        unsigned q;
                        pick[pi]++;
                        for (q = pi + 1; q < extra; q++)
                            pick[q] = pick[q - 1] + 1;
                        more = 1;
                        break;
                    }
                }
            }
            free(pick);
            if (found) break;
            }
        }
        free(work); free(cand_idx);
        if (!found) {
            /* Beyond capacity (or parity lies too): nothing verified,
             * nothing written. N names the smallest count beyond cover. */
            unsigned need = e0 > m ? e0 : m + 1;
            fprintf(stderr, "heal: group %u: cannot reconstruct: needs "
                    "%u, has %u\n", g, need, m);
            rc = 0;
            goto out;
        }
    }
    /* write back: every matched file gets its full manifest span.
     * Matched means the accepted candidate assembles to bytes the
     * manifest hash proves -- every byte written is proven, including
     * sizes (truncate/extend ride the full rewrite). Intact files are
     * proven identical already: no churn. Unmatched files (live bytes
     * some unhealed group still owns) are left live: writing unproven
     * bytes is exactly what the contract forbids. Afterwards every
     * touched file is re-hashed: audit state is earned, and the reverify
     * below reports the truth. */
    {
        int wfail = 0;
        for (e = 0; e < hm->fi.nfiles && !wfail; e++) {
            seal_entry *en = &hm->ents[e];
            heal_file *f = &hm->files[e];
            uint64_t es, ee;
            uint8_t *span;
            blake3_hasher hb;
            uint8_t h[32];
            if (en->ftype != SEAL_FT_REG) continue;
            if (!f->len_ok || f->estart == HEAL_UNK_OFF) continue;
            es = f->estart; ee = es + f->elen;
            if (es >= goff + gdb || ee <= goff) continue;
            if (f->st == HF_INTACT) continue;
            if (!matched[e]) continue;
            span = malloc(en->size ? (size_t)en->size : 1);
            if (!span) { wfail = 1; break; }
            /* Same inputs validation used; a miss now is read
             * instability -- fail loudly, write nothing. */
            if (!heal_file_span(v, hm, healed, hm->fi.ngroups, g, sym,
                               e, span)) {
                free(span);
                wfail = 1;
                break;
            }
            blake3_hasher_init(&hb);
            if (en->size)
                blake3_hasher_update(&hb, span, (size_t)en->size);
            blake3_hasher_finalize(&hb, h, sizeof h);
            if (memcmp(h, en->hash, 32) != 0) {
                free(span);
                wfail = 1;
                break;
            }
            if (heal_write_file(v, en->name, span, en->size,
                                &rep->bytes_rewritten) != 0) {
                fprintf(stderr, "heal: group %u: write-back of %s "
                        "failed\n", g, en->name);
                free(span);
                wfail = 1;
                break;
            }
            free(span);
            if (heal_refresh_reg(v, hm, e) != 0) {
                wfail = 1;
                break;
            }
        }
        if (wfail) { rc = -1; goto out; }
        /* parity was unreadable: republish all of it from healed data */
        if (ep) {
            uint8_t **dptr = malloc(sizeof *dptr * (k ? k : 1));
            uint8_t **pptr = malloc(sizeof *pptr * (m ? m : 1));
            invfs_wsession *ws = NULL;
            if (!dptr || !pptr) {
                free(dptr); free(pptr);
                rc = -1;
                goto out;
            }
            for (i = 0; i < k; i++)
                dptr[i] = sym + (size_t)i * SEAL_SYM_BYTES;
            for (i = 0; i < m; i++)
                pptr[i] = par + (size_t)i * SEAL_SYM_BYTES;
            if (rs_encode((int)hm->algo, k, m, SEAL_SYM_BYTES, dptr,
                          pptr) != 0) {
                free(dptr); free(pptr);
                rc = -1;
                goto out;
            }
            free(dptr); free(pptr);
            if (vol_write_begin(v, SEAL_PARITY_NAME, 0, &ws) == 0 || !ws) {
                rc = -1;
                goto out;
            }
            for (i = 0; i < m; i++)
                if (seal_write_full(ws, poff + (uint64_t)i * SEAL_SYM_BYTES,
                                    par + (size_t)i * SEAL_SYM_BYTES,
                                    SEAL_SYM_BYTES) != 0) {
                    vol_write_abort(ws);
                    rc = -1;
                    goto out;
                }
            if (vol_write_commit(ws) != 0) { rc = -1; goto out; }
            rep->bytes_rewritten += (uint64_t)m * SEAL_SYM_BYTES;
        }
        printf("[heal] group %u: reconstructed %u erasures, verified "
               "(BLAKE3), written back\n", g, e0);
        healed[g] = 1;
        rc = 2;
    }
out:
    free(sym); free(par); free(blks); free(present); free(erased);
    free(matched);
    return rc;
}

/* The WP402 entry point. gids == NULL heals every group detection names;
 * explicit gids heal exactly those. dry == 1 detects + plans, writes
 * nothing. 0 = every targeted group healed-or-clean and reverified,
 * -1 = any refusal, failure, or reverify miss (message printed). */
int vol_seal_heal(invfs_volume *v, const uint32_t *gids, size_t ngids,
                  int dry, invfs_seal_heal_report *rep)
{
    heal_manifest hm;
    uint8_t *is_target = NULL, *healed = NULL;
    uint32_t g, e, ntarget = 0;
    char reason[48];
    int mrc, ret = -1;
    uint32_t n_healed = 0, n_parity = 0, n_clean = 0, n_fail = 0;
    if (rep) memset(rep, 0, sizeof *rep);
    if (!v) return -1;
    if (v->sb.vol_flags & VOLF_READONLY) {
        fprintf(stderr, "heal: volume is read-only; heal refuses "
                "(healing rewrites data + parity)\n");
        return -1;
    }
    memset(&hm, 0, sizeof hm);
    mrc = heal_manifest_load(v, &hm);
    if (mrc != 0) return -1;
    is_target = calloc(hm.fi.ngroups ? hm.fi.ngroups : 1, 1);
    healed = calloc(hm.fi.ngroups ? hm.fi.ngroups : 1, 1);
    if (!is_target || !healed) {
        fprintf(stderr, "heal: out of memory\n");
        goto out;
    }
    if (!gids) {
        /* detect: the same recompute the scrub uses, group by group */
        for (g = 0; g < hm.fi.ngroups; g++) {
            if (heal_detect_group(v, &hm, g, reason, 1) != 0) {
                printf("seal-heal-needed %u %s\n", g,
                       reason[0] ? reason : "drift");
                is_target[g] = 1;
                ntarget++;
            }
        }
    } else {
        for (g = 0; g < (uint32_t)ngids; g++) {
            uint32_t t = gids[g];
            if (t >= hm.fi.ngroups) {
                fprintf(stderr, "heal: no such group %u (%u groups "
                        "sealed)\n", t, hm.fi.ngroups);
                goto out;
            }
            if (!is_target[t]) { is_target[t] = 1; ntarget++; }
        }
    }
    rep->groups_scanned = hm.fi.ngroups;
    rep->groups_targeted = ntarget;
    if (dry) {
        uint64_t bytes = 0;
        for (g = 0; g < hm.fi.ngroups; g++)
            if (is_target[g])
                bytes += hm.grps[g].databytes;
        printf("[heal] plan: %u groups, %llu data bytes, %llu parity "
               "bytes; no changes (dry-run)\n", ntarget,
               (unsigned long long)bytes,
               (unsigned long long)ntarget * hm.fi.m * SEAL_SYM_BYTES);
        ret = 0;
        goto out;
    }
    if (!ntarget) {
        fprintf(stderr, "heal: all %u groups verify; nothing to do\n",
                hm.fi.ngroups);
        ret = 0;
        goto out;
    }
    /* fixpoint: a group whose files span other bad groups heals once
     * those do; a pass with no progress refuses the rest. A fault
     * abort breaks out and fails below -- never a silent success. */
    {
        uint32_t remaining = ntarget, passes = 0, progress;
        int aborted = 0;
        do {
            progress = 0;
            passes++;
            for (g = 0; g < hm.fi.ngroups; g++) {
                int hr;
                if (!is_target[g] || healed[g]) continue;
                if (heal_detect_group(v, &hm, g, reason, 1) == 0) {
                    /* converged since detection (shared file healed via
                     * another group): clean, nothing to write */
                    printf("[heal] group %u: clean (nothing to do)\n", g);
                    healed[g] = 1;
                    n_clean++;
                    remaining--;
                    progress = 1;
                    continue;
                }
                hr = heal_one_group(v, &hm, healed, g, rep);
                if (hr == 1) {
                    n_parity++;
                    remaining--;
                    progress = 1;
                } else if (hr == 2) {
                    n_healed++;
                    remaining--;
                    progress = 1;
                } else if (hr < 0) {
                    remaining--;
                    progress = 1;
                }
                /* hr == 0: refused this pass; retry after others heal */
                heal_slow_gate();
                if (invfs_vol_fault(SEAL_HEAL_FAULT_CRASH)) {
                    fprintf(stderr, "heal: fault " SEAL_HEAL_FAULT_CRASH
                            " fired (simulated crash mid-heal)\n");
                    aborted = 1;
                    break;
                }
            }
            if (aborted) break;
        } while (remaining && progress && passes <= ntarget + 1);
        for (g = 0; g < hm.fi.ngroups; g++)
            if (is_target[g] && !healed[g]) {
                if (heal_detect_group(v, &hm, g, reason, 1) != 0)
                    fprintf(stderr, "heal: group %u: still bad (%s); "
                            "left untouched\n", g,
                            reason[0] ? reason : "drift");
                n_fail++;
            }
        if (aborted) n_fail++;
    }
    /* reverify: every healed-or-clean targeted group must recompute
     * (parity-only: drift living in another, unhealed group must not
     * fail this group's verdict). A miss here is a bug below, not
     * drift -- fail loudly either way. */
    for (g = 0; g < hm.fi.ngroups; g++) {
        if (!is_target[g] || !healed[g]) continue;
        if (heal_detect_group(v, &hm, g, reason, 0) != 0) {
            fprintf(stderr, "heal: group %u FAILED reverify (%s)\n", g,
                    reason[0] ? reason : "drift");
            n_fail++;
        }
    }
    /* end-to-end: every non-intact file fully inside healed groups must
     * now hash-match live. This catches write-skips the per-group
     * recompute cannot see (a matched file the write-back never wrote
     * stays drifted while its groups recompute). */
    for (e = 0; e < hm.fi.nfiles; e++) {
        seal_entry *en = &hm.ents[e];
        heal_file *f = &hm.files[e];
        uint64_t es, ee, pos;
        uint8_t *span;
        blake3_hasher hb;
        uint8_t h[32];
        int covered = 1;
        if (en->ftype != SEAL_FT_REG) continue;
        if (!f->len_ok || f->estart == HEAL_UNK_OFF) continue;
        if (f->st == HF_INTACT) continue;
        es = f->estart; ee = es + f->elen;
        for (pos = es; pos < ee;) {
            uint32_t gi = 0;
            while (gi + 1 < hm.fi.ngroups && hm.goff[gi + 1] <= pos)
                gi++;
            if (!healed[gi]) { covered = 0; break; }
            pos = hm.goff[gi] + hm.grps[gi].databytes;
            if (pos <= es) break;   /* cannot advance: no infinite loop */
        }
        if (!covered) continue;
        span = malloc(en->size ? (size_t)en->size : 1);
        if (!span) { n_fail++; break; }
        /* g == ngroups selects no candidate: pure live assembly */
        if (!heal_file_span(v, &hm, healed, hm.fi.ngroups, hm.fi.ngroups,
                           NULL, e, span)) {
            free(span);
            n_fail++;
            continue;
        }
        blake3_hasher_init(&hb);
        if (en->size)
            blake3_hasher_update(&hb, span, (size_t)en->size);
        blake3_hasher_finalize(&hb, h, sizeof h);
        free(span);
        if (memcmp(h, en->hash, 32) != 0) {
            fprintf(stderr, "heal: file %s FAILED reverify (hash)\n",
                    en->name);
            n_fail++;
        }
    }
    rep->groups_healed = n_healed;
    rep->groups_parity = n_parity;
    rep->groups_clean = n_clean;
    rep->groups_failed = n_fail;
    if (!n_fail)
        fprintf(stderr, "heal: %u healed, %u parity-rewritten, %u already "
                "clean over %u targeted\n", n_healed, n_parity, n_clean,
                ntarget);
    ret = n_fail ? -1 : 0;
out:
    free(is_target);
    free(healed);
    heal_manifest_free(&hm);
    return ret;
}
