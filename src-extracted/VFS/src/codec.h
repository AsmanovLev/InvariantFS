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

#endif /* CODEC_H */
