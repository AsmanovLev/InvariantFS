/* vol_seal.h — native v3 seal backend (WP201): par2-inspired, no compat.
 *
 * WHAT THIS IS. The v3 content-seal: live REG/LNK file bytes are streamed in
 * sorted-name order, sliced into 64 KiB symbols (== INVFS_SEGMENT_SIZE, so a
 * symbol lines up with one RAW segment), packed into groups of k data
 * symbols, and each group gains m Reed-Solomon parity symbols (rs.c, over
 * GF(2^8)). Parity lives in the hidden file "\x01seal-parity"; the manifest
 * (per-file name/type/size/BLAKE3 + per-group shape + CRC32C) lives in the
 * hidden file "\x01seal-footer", written LAST: no valid footer = no seal.
 * Hidden ("\x01...") names are ordinary files the listings filter out
 * (src/core/vol_dirs.c:366) and vol_find still resolves, so no allocator,
 * block-0, or transform change was needed for storage.
 *
 * WHAT THIS IS NOT. Not a byte-compat par2 (our hashes, our footer, our
 * sizes). Not whole-disk stripes (k+m <= 256 forbids it; repair cost scales
 * with the group). Not healing: v1 is detect-only — vol_seal_verify reports,
 * never rewrites (scrub auto-heal is a later WP). Not incremental: re-seal
 * is a full recompute; the allocator's dirty-stripe bitmap is still
 * maintained (seal_dirty_mark) but v1 does not consult it.
 *
 * MENU (fixed — no arbitrary matrix shapes, audit surface):
 *   pct  5 -> (k=20,m=1)    ~4.8% overhead
 *   pct 10 -> (k=9, m=1)    10%   overhead   (default)
 *   pct 20 -> (k=8, m=2)    20%   overhead
 *   pct 25 -> (k=6, m=2)    25%   overhead
 * Overhead here is parity/data symbols; the manifest adds ~43 B + name per
 * file on top.
 *
 * This header holds the parts a unit test can reach without a volume: the
 * menu map, the capacity plan, and the footer/parity-header codecs (pure
 * buffer in/out, strict on the way in). The volume engine in vol_seal.c is
 * built out of these, so the tested code IS the shipped code.
 */
#ifndef INVFS_VOL_SEAL_H
#define INVFS_VOL_SEAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Symbol size: one RAW segment (src/core/volume.h: INVFS_SEGMENT_SIZE). */
#define SEAL_SYM_BYTES 65536u

/* Hidden seal files (ordinary files; listings skip "\x01*" names). */
#define SEAL_PARITY_NAME "\x01seal-parity"
#define SEAL_FOOTER_NAME "\x01seal-footer"

/* Footer/parity-header magic (exactly 8 bytes each, no NUL — the 9-letter
 * "INVFSSEAL" does NOT fit and silently truncated to its prefix). */
#define SEAL_FOOTER_MAGIC "INVFSEAL"
#define SEAL_PARITY_MAGIC "INVFSEAP"
#define SEAL_FORMAT_VERSION 1u

/* INVFS_FAULT site armed between the parity commit and the footer commit:
 * INVFS_FAULT="vol_seal_crash:1" makes vol_seal fail exactly as a kill -9
 * there would (parity new, footer prior-or-absent), for the crash-mid-seal
 * probe. Production with the variable unset never fires it. */
#define SEAL_FAULT_CRASH "vol_seal_crash"

/* ---- menu ---------------------------------------------------------- */
/* Map a --seal percent to (k,m). pct==10 is the default; any other value
 * than {5,10,20,25} is coerced to 10. Always returns 0. */
int seal_menu(int pct, unsigned *k_out, unsigned *m_out);

/* Capacity plan from covered data bytes: groups = ceil(bytes/(k*sym)),
 * parity = groups*m*sym. Pure arithmetic (no volume), used by --dry-run. */
void seal_plan(unsigned k, unsigned m, uint64_t data_bytes,
               uint64_t *groups_out, uint64_t *parity_bytes_out);

/* ---- footer codec (strict in, exact out) ---------------------------- */
/* Prelude (fixed 28 bytes, LE): magic[8] ver u16 k u16 m u16 sym u32
 *   algo u16 seq u64. The algo travels in the footer so a re-opened volume
 *   keeps the operator's code choice, not the default.
 * Entry (per file): namelen u16 name[type u8] size u64 [hash[32] iff REG/LNK].
 *   namelen 1..255; type is INVFS_ITYP_* (only REG/LNK carry a hash).
 * Group (per group): datasyms u16 databytes u64.
 * Trailer (fixed 12 bytes, LE): nfiles u32 ngroups u32 crc32c u32, the CRC
 *   covering every footer byte before it.
 * Parity header (fixed 34 bytes, LE): magic[8] ver u16 k u16 m u16 sym u32
 *   ngroups u32 seq u64 hcrc u32, the CRC covering the 30 bytes before it.
 */
#define SEAL_PRELUDE_LEN 28u
#define SEAL_TRAILER_LEN 12u
#define SEAL_PARHDR_LEN  34u

/* File types the footer can carry a content hash for (match invarifs.h). */
#define SEAL_FT_REG 0u
#define SEAL_FT_LNK 2u

typedef struct {
    uint16_t k, m;
    uint32_t sym;
    uint64_t seq;
    uint32_t nfiles;
    uint32_t ngroups;
} seal_footinfo;

/* Encoders: write one piece at out (caller holds space), return bytes
 * written, 0 on bad input (namelen 0/overlong, NULL with nonzero len).
 * entry: hash must be 32 bytes for REG/LNK, ignored otherwise. */
size_t seal_prelude_enc(uint8_t out[SEAL_PRELUDE_LEN],
                        unsigned k, unsigned m, unsigned algo, uint64_t seq);
size_t seal_entry_enc(uint8_t *out, size_t cap, const char *name,
                      unsigned ftype, uint64_t size, const uint8_t hash[32]);
size_t seal_group_enc(uint8_t out[10], unsigned datasyms, uint64_t databytes);
size_t seal_trailer_enc(uint8_t out[SEAL_TRAILER_LEN], const uint8_t *body,
                        size_t body_len, uint32_t nfiles, uint32_t ngroups);
size_t seal_parhdr_enc(uint8_t out[SEAL_PARHDR_LEN],
                       unsigned k, unsigned m, uint32_t ngroups, uint64_t seq);

/* Strict parser over an untrusted footer buffer (torn reads!). Returns
 *   0 = valid (info filled),
 *   1 = not sealed (bad magic/version/shape/CRC/truncation/trailing bytes —
 *       NEVER trusted, never a partial answer),
 *  -1 = out of memory (only when info_out arrays are requested — see below).
 * With entries_out/groups_out NULL, no allocation happens and -1 is
 * impossible; the engine verifies by re-streaming, never by trusting this.
 * With non-NULL out params the arrays are malloc'd (caller frees); any
 * refusal frees what it holds and returns 1 (or -1 on OOM only). */
typedef struct {
    uint16_t namelen;
    char name[256];
    uint8_t ftype;
    uint64_t size;
    uint8_t hash[32];
    int has_hash;
} seal_entry;
typedef struct {
    uint16_t datasyms;
    uint64_t databytes;
} seal_group;
int seal_footer_parse(const uint8_t *buf, size_t len, seal_footinfo *info_out,
                      seal_entry **entries_out, seal_group **groups_out);

/* Strict parser over an untrusted 34-byte parity header. Same contract:
 * 0 = valid (fields filled), 1 = not sealed, never -1. */
int seal_parhdr_parse(const uint8_t *buf, size_t len,
                      unsigned *k_out, unsigned *m_out, uint32_t *ngroups_out,
                      uint64_t *seq_out);

#ifdef __cplusplus
}
#endif

#endif /* INVFS_VOL_SEAL_H */
