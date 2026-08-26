#ifndef CODEC_H
#define CODEC_H

/*
 * codec.h — codec registry (WP10 §1).
 *
 * One static table describing every compression recipe the sweep can pick:
 * what it recognises (sniff), what decoding costs (dec_mem_bytes), whether
 * an external tool has to exist for it (probe), and the actual transform
 * for builtin codecs. Registry order IS sniff priority: specific magics
 * first, the generic text heuristic (PPMD) last.
 */
#include <stddef.h>
#include <stdint.h>

#include "invarifs.h"

#define INVFS_CODEC_CAP_SEEK      0x01
#define INVFS_CODEC_CAP_BATCHED   0x02
#define INVFS_CODEC_CAP_WHOLEFILE 0x04
#define INVFS_CODEC_CAP_CONTAINER 0x08
#define INVFS_CODEC_CAP_EXTERNAL  0x10

/* Registry-local algo id: a ZIP container keeps its original archive bytes
 * on disk (members are windows into them, see zip.c), so no AST entry ever
 * carries this value -- it exists for sweep selection / invfs.class stamping
 * only. Fits the 6-bit AST algo field if that ever changes. */
#define INVFS_ALGO_ZIPR 12

typedef struct invfs_codec {
    uint32_t     algo;           /* INVFS_ALGO_* from invarifs.h */
    const char  *name;
    uint32_t     caps;
    uint64_t     dec_mem_bytes;  /* peak decoder memory per unit */
    uint16_t     generation;     /* bumps when encoder gains a sub-encoder */
    int       (*sniff)(const uint8_t *head, size_t head_len, const char *name);
    int       (*probe)(void);    /* EXTERNAL only; NULL = builtin */
    int       (*encode)(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap, size_t *outlen);
    int       (*decode)(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);
} invfs_codec;

const invfs_codec *invfs_codec_by_algo(uint32_t algo);
const invfs_codec *invfs_codec_all(size_t *count);   /* order = sniff priority: specific magics first, text LAST */
uint16_t           invfs_registry_generation(void);  /* max generation over all codecs */
void               invfs_codec_probe_reset(void);    /* test hook: clear memoized probe() results + unload packs */

/* ---- codecpack execution boundary (WP13) ----
 *
 * Dynamically registered packs (invfs_codec_load_packs, lazy on first
 * registry access) carry a manifest argv executed as a subprocess. The
 * registry lives in codec.c but process execution is volume.c's tool-layer
 * business, so the two run-time hooks below are DECLARED here and
 * IMPLEMENTED in volume.c (codec.c's encode/decode trampolines call them;
 * codec_test stubs them). */
typedef struct invfs_pack_def {
    const char *dir;       /* pack directory — the {pack} substitution */
    const char *encode;    /* argv template, {in}/{out}/{pack} placeholders */
    const char *decode;
    const char *estimate;  /* argv template with {in}; NULL when absent */
    const char *requires;  /* comma list of extra tools, NULL when none */
} invfs_pack_def;

/* The pack record behind a registry entry, or NULL for builtin codecs.
 * Matches by algo (the materialized registry holds COPIES of the pack
 * records, so callers cannot rely on pointer identity). */
const invfs_pack_def *invfs_codec_pack_def(const invfs_codec *c);

/* Run the pack's encode (is_encode=1) or decode argv with {in}/{out}
 * substituted by the given paths. Returns the child's exit code, -1 on
 * failure to launch. */
int invfs_codec_pack_exec(const invfs_codec *c, int is_encode,
                          const char *in_path, const char *out_path);

/* Run the pack's estimate argv ({in} substituted); parses a u64 byte count
 * from its stdout. Returns 0 on success, -1 when the pack has no estimate
 * command or it failed. */
int invfs_codec_pack_estimate(const invfs_codec *c, const char *in_path,
                              uint64_t *out_bytes);

/* Text families — the batching sort key (WP10 §4). 1..10 are the extension
 * families; INVFS_TEXT_FAMILY_CONTENT is content-sniffed text with no known
 * extension; 0 = not text. */
#define INVFS_TEXT_FAMILY_CODE_C    1
#define INVFS_TEXT_FAMILY_CODE_PY   2
#define INVFS_TEXT_FAMILY_CODE_JS   3
#define INVFS_TEXT_FAMILY_CODE_JAVA 4
#define INVFS_TEXT_FAMILY_CODE_RS   5
#define INVFS_TEXT_FAMILY_CODE_GO   6
#define INVFS_TEXT_FAMILY_DATA      7
#define INVFS_TEXT_FAMILY_PROSE     8
#define INVFS_TEXT_FAMILY_WEB       9
#define INVFS_TEXT_FAMILY_SHELL     10
#define INVFS_TEXT_FAMILY_CONTENT   11

int invfs_text_family(const char *name, const uint8_t *head, size_t head_len);

/* Binary (executable) families — the WP14a binary-batch sort key. Kept
 * well clear of the INVFS_TEXT_FAMILY_* range (1..11) so one sort key
 * column can never mix the two batching domains. */
#define INVFS_BIN_FAMILY_ELF_X64   20   /* e_machine 62: BCJ-prefiltered */
#define INVFS_BIN_FAMILY_ELF_X86   21   /* e_machine 3:  BCJ-prefiltered */
#define INVFS_BIN_FAMILY_ELF_A64   22   /* e_machine 183 */
#define INVFS_BIN_FAMILY_ELF_OTHER 23   /* any other e_machine */
#define INVFS_BIN_FAMILY_PE        24   /* MZ + PE\0\0 at e_lfanew */
#define INVFS_BIN_FAMILY_MACHO     25   /* FEEDFACE/CAFEBABE magic family */
/* 26 ("other executable") is reserved: NOT assigned in v1. In particular
 * a "#!" shebang script is TEXT (the text classifier claims it first) and
 * must never be binary-batched. */

/* 0 = not binary-batchable. Magic-only: ELF (arch split by e_machine @18),
 * PE (MZ + PE\0\0 at e_lfanew), Mach-O. Files smaller than 4096 bytes are
 * never classified (head_len carries the FILE size at the sweep call site,
 * which passes the whole file). A PE whose e_lfanew points past the head
 * window reads as 0 rather than guessing. */
int invfs_binary_family(const uint8_t *head, size_t head_len, const char *name);

#endif /* CODEC_H */
