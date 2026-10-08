/*
 * codec.c — codec registry (WP10 §1, §10).
 *
 * v1 formalizes SELECTION only: NONE/LZ4/ZSTD wrap exactly the calls
 * volume.c already makes, PPMD wraps ppmd_codec.c, and the container /
 * external recipes (zip/tarr/gzr/pngr/flacr/pmp/jxl/ape/wv) register just
 * sniff + probe — their transcode logic stays in volume.c for now.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifndef _WIN32
#include <sys/utsname.h>
#endif
#include <unistd.h>

#include "codec.h"
#include "../core/tool_scratch.h"
#include "lz4.h"
#include "ppmd_codec.h"
#include "zstd.h"

/* ---------------- builtin codecs ---------------- */

static int none_encode(const uint8_t *in, size_t inlen,
                       uint8_t *out, size_t outcap, size_t *outlen)
{
    if (outcap < inlen) return -1;
    if (inlen) memcpy(out, in, inlen);
    *outlen = inlen;
    return 0;
}

static int none_decode(const uint8_t *in, size_t inlen,
                       uint8_t *out, size_t outlen)
{
    if (outlen > inlen) return -1;
    if (outlen) memcpy(out, in, outlen);
    return 0;
}

/* Mirror volume.c exactly: one-shot entry points, 0 = failure (LZ4),
 * isError / != outlen (ZSTD, level 19 like the sweep's generic path). */
static int lz4c_encode(const uint8_t *in, size_t inlen,
                       uint8_t *out, size_t outcap, size_t *outlen)
{
    int cs;
    if (inlen > INT32_MAX || outcap > INT32_MAX) return -1;
    cs = LZ4_compress_default((const char *)in, (char *)out,
                              (int)inlen, (int)outcap);
    if (cs <= 0) return -1;
    *outlen = (size_t)cs;
    return 0;
}

static int lz4c_decode(const uint8_t *in, size_t inlen,
                       uint8_t *out, size_t outlen)
{
    int got;
    if (inlen > INT32_MAX || outlen > INT32_MAX) return -1;
    got = LZ4_decompress_safe((const char *)in, (char *)out,
                              (int)inlen, (int)outlen);
    return got == (int)outlen ? 0 : -1;
}

static int zstdc_encode(const uint8_t *in, size_t inlen,
                        uint8_t *out, size_t outcap, size_t *outlen)
{
    size_t cs = ZSTD_compress(out, outcap, in, inlen, 19);
    if (ZSTD_isError(cs)) return -1;
    *outlen = cs;
    return 0;
}

static int zstdc_decode(const uint8_t *in, size_t inlen,
                        uint8_t *out, size_t outlen)
{
    size_t got = ZSTD_decompress(out, outlen, in, inlen);
    return (ZSTD_isError(got) || got != outlen) ? -1 : 0;
}

/* ---------------- hybrid text classifier (WP10 §4) ---------------- */

#define SNIFF_SAMPLE_MAX 8192

static const struct { const char *ext; int family; } ext_families[] = {
    { "c",    INVFS_TEXT_FAMILY_CODE_C },
    { "h",    INVFS_TEXT_FAMILY_CODE_C },
    { "cpp",  INVFS_TEXT_FAMILY_CODE_C },
    { "cc",   INVFS_TEXT_FAMILY_CODE_C },
    { "hpp",  INVFS_TEXT_FAMILY_CODE_C },
    { "py",   INVFS_TEXT_FAMILY_CODE_PY },
    { "js",   INVFS_TEXT_FAMILY_CODE_JS },
    { "ts",   INVFS_TEXT_FAMILY_CODE_JS },
    { "mjs",  INVFS_TEXT_FAMILY_CODE_JS },
    { "java", INVFS_TEXT_FAMILY_CODE_JAVA },
    { "rs",   INVFS_TEXT_FAMILY_CODE_RS },
    { "go",   INVFS_TEXT_FAMILY_CODE_GO },
    { "json", INVFS_TEXT_FAMILY_DATA },
    { "xml",  INVFS_TEXT_FAMILY_DATA },
    { "yaml", INVFS_TEXT_FAMILY_DATA },
    { "yml",  INVFS_TEXT_FAMILY_DATA },
    { "toml", INVFS_TEXT_FAMILY_DATA },
    { "csv",  INVFS_TEXT_FAMILY_DATA },
    { "txt",  INVFS_TEXT_FAMILY_PROSE },
    { "md",   INVFS_TEXT_FAMILY_PROSE },
    { "log",  INVFS_TEXT_FAMILY_PROSE },
    { "rst",  INVFS_TEXT_FAMILY_PROSE },
    { "html", INVFS_TEXT_FAMILY_WEB },
    { "css",  INVFS_TEXT_FAMILY_WEB },
    { "sh",   INVFS_TEXT_FAMILY_SHELL },
    { "bash", INVFS_TEXT_FAMILY_SHELL },
};

/* ASCII-only, locale-free case-insensitive compare */
static int ext_eq(const char *got, const char *want)
{
    while (*got && *want) {
        unsigned a = (unsigned char)*got++;
        unsigned b = (unsigned char)*want++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return *got == *want;
}

/* text iff ZERO NUL bytes AND >=85% printable ASCII (32..126) / \t \n \r */
static int content_is_text(const uint8_t *head, size_t head_len)
{
    size_t n = head_len < SNIFF_SAMPLE_MAX ? head_len : SNIFF_SAMPLE_MAX;
    size_t i, good = 0;

    if (!head || n == 0) return 0;
    for (i = 0; i < n; i++) {
        uint8_t c = head[i];
        if (c == 0) return 0;
        if ((c >= 32 && c <= 126) || c == '\t' || c == '\n' || c == '\r')
            good++;
    }
    return good * 100 >= 85 * n;
}

int invfs_text_family(const char *name, const uint8_t *head, size_t head_len)
{
    if (name) {
        const char *base = strrchr(name, '/');
        const char *dot;
        size_t i;

        base = base ? base + 1 : name;
        dot = strrchr(base, '.');
        if (dot && dot[1]) {
            const char *ext = dot + 1;
            for (i = 0; i < sizeof ext_families / sizeof ext_families[0]; i++)
                if (ext_eq(ext, ext_families[i].ext))
                    return ext_families[i].family;
        }
    }
    return content_is_text(head, head_len) ? INVFS_TEXT_FAMILY_CONTENT : 0;
}

static int sniff_ppmd(const uint8_t *head, size_t head_len, const char *name)
{
    return invfs_text_family(name, head, head_len) ? 50 : 0;
}

/* ---------------- binary (executable) classifier (WP14a) ----------------
 *
 * The binary analogue of invfs_text_family: recognizes executable
 * container magics so the sweep can batch them cross-file (shared ZSTD
 * frame per <=4MB batch, x86 members BCJ-prefiltered first). Magic-only
 * on purpose: an extension like ".bin"/".so" says nothing about the
 * content, and the sweep calls this with the whole file in hand, so a
 * magic check costs nothing extra.
 *
 * CAFEBABE is also the Java .class magic: classifying one as "Mach-O
 * family" is harmless -- the family is only a sort/BCJ-eligibility key,
 * and family 25 never gets the x86 prefilter. */

#define INVFS_BIN_MIN_SIZE 4096   /* smaller files stay on the generic path */

int invfs_binary_family(const uint8_t *head, size_t head_len, const char *name)
{
    (void)name;
    if (!head || head_len < INVFS_BIN_MIN_SIZE) return 0;

    /* ELF: \x7F "ELF"; e_machine is the u16 LE at offset 18 */
    if (head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' &&
        head[3] == 'F') {
        unsigned em = (unsigned)head[18] | ((unsigned)head[19] << 8);
        switch (em) {
        case 62:  return INVFS_BIN_FAMILY_ELF_X64;    /* EM_X86_64 */
        case 3:   return INVFS_BIN_FAMILY_ELF_X86;    /* EM_386 */
        case 183: return INVFS_BIN_FAMILY_ELF_A64;    /* EM_AARCH64 */
        default:  return INVFS_BIN_FAMILY_ELF_OTHER;
        }
    }

    /* PE: "MZ" DOS stub, e_lfanew (u32 LE @0x3C) -> "PE\0\0" */
    if (head[0] == 'M' && head[1] == 'Z') {
        uint32_t peoff = (uint32_t)head[0x3C]        | ((uint32_t)head[0x3D] << 8) |
                         ((uint32_t)head[0x3E] << 16) | ((uint32_t)head[0x3F] << 24);
        if (peoff >= 0x40 && (uint64_t)peoff + 4 <= head_len &&
            head[peoff] == 'P' && head[peoff + 1] == 'E' &&
            head[peoff + 2] == 0 && head[peoff + 3] == 0)
            return INVFS_BIN_FAMILY_PE;
        return 0;   /* an MZ that cannot confirm PE\0\0 is a DOS exe at
                     * best: leave it generic rather than mis-sort it */
    }

    /* Mach-O: 32/64-bit, both byte orders, and the fat-universal magics */
    if (head[0] == 0xFE && head[1] == 0xED &&
        head[2] == 0xFA && (head[3] == 0xCE || head[3] == 0xCF))
        return INVFS_BIN_FAMILY_MACHO;
    if ((head[0] == 0xCE || head[0] == 0xCF) &&
        head[1] == 0xFA && head[2] == 0xED && head[3] == 0xFE)
        return INVFS_BIN_FAMILY_MACHO;
    if (head[0] == 0xCA && head[1] == 0xFE &&
        head[2] == 0xBA && head[3] == 0xBE)
        return INVFS_BIN_FAMILY_MACHO;
    if (head[0] == 0xBE && head[1] == 0xBA &&
        head[2] == 0xFE && head[3] == 0xCA)
        return INVFS_BIN_FAMILY_MACHO;

    return 0;
}

/* ---------------- magic sniffs ---------------- */

static int magic_at(const uint8_t *head, size_t head_len,
                    const uint8_t *magic, size_t magic_len, size_t off)
{
    if (head_len < off + magic_len) return 0;
    return memcmp(head + off, magic, magic_len) == 0 ? 100 : 0;
}

static int sniff_zip(const uint8_t *head, size_t head_len, const char *name)
{
    static const uint8_t m[] = { 'P', 'K', 0x03, 0x04 };
    (void)name;
    return magic_at(head, head_len, m, sizeof m, 0);
}

static int sniff_tarr(const uint8_t *head, size_t head_len, const char *name)
{
    static const uint8_t m[] = { 'u', 's', 't', 'a', 'r' };
    (void)name;
    return magic_at(head, head_len, m, sizeof m, 257);
}

static int sniff_gzr(const uint8_t *head, size_t head_len, const char *name)
{
    static const uint8_t m[] = { 0x1F, 0x8B };
    (void)name;
    return magic_at(head, head_len, m, sizeof m, 0);
}

static int sniff_pngr(const uint8_t *head, size_t head_len, const char *name)
{
    static const uint8_t m[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    (void)name;
    return magic_at(head, head_len, m, sizeof m, 0);
}

static int sniff_flacr(const uint8_t *head, size_t head_len, const char *name)
{
    static const uint8_t m[] = { 'f', 'L', 'a', 'C' };
    (void)name;
    return magic_at(head, head_len, m, sizeof m, 0);
}

static int sniff_pmp(const uint8_t *head, size_t head_len, const char *name)
{
    (void)name;
    /* ID3v2 tag, or MPEG audio frame sync: 0xFF followed by 0xEx/0xFx */
    if (head_len >= 3 && memcmp(head, "ID3", 3) == 0) return 100;
    if (head_len >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0) return 100;
    return 0;
}

static int sniff_jxl(const uint8_t *head, size_t head_len, const char *name)
{
    static const uint8_t box[] = { 0x00, 0x00, 0x00, 0x0C,
                                   'J', 'X', 'L', ' ' };
    static const uint8_t soi[] = { 0xFF, 0xD8, 0xFF };
    (void)name;
    if (magic_at(head, head_len, box, sizeof box, 0)) return 100;
    /* raw codestream */
    if (head_len >= 2 && head[0] == 0xFF && head[1] == 0x0A) return 100;
    /* JPEG SOI: an INPUT format of this codec (the stored form is JXL) */
    if (magic_at(head, head_len, soi, sizeof soi, 0)) return 100;
    return 0;
}

static int sniff_ape(const uint8_t *head, size_t head_len, const char *name)
{
    static const uint8_t m[] = { 'M', 'A', 'C', ' ' };
    (void)name;
    return magic_at(head, head_len, m, sizeof m, 0);
}

static int sniff_wv(const uint8_t *head, size_t head_len, const char *name)
{
    static const uint8_t m[] = { 'w', 'v', 'p', 'k' };
    (void)name;
    return magic_at(head, head_len, m, sizeof m, 0);
}

/* WP14b M2: exe-as-container prefilter. The carving decision needs a
 * whole-file media scan (embedded JPEG >= 16 KiB), which a head window
 * cannot see -- that runs in the vol_sweep_one branch on full content.
 * Here the binary-family magic (ELF/PE/Mach-O) is only a cheap gate for
 * the registry users (tz_sniff_any's UNCOMPRESSIBLE retry). */
static int sniff_exer(const uint8_t *head, size_t head_len, const char *name)
{
    return invfs_binary_family(head, head_len, name) > 0 ? 40 : 0;
}

/* ---------------- codecpack probe (WP10 §10) ---------------- */

#define PACK_MAX_MAGIC 8   /* sniff.magic rules per pack (repeated lines) */

typedef struct {
    uint8_t bytes[16];
    size_t  len;
    size_t  off;
} pack_magic_rule;

struct pack_manifest {
    char     name[64];
    long     algo;            /* -1 when absent */
    long     pack_version;    /* parsed, not acted on (parser-level version) */
    /* Reserve/bootstrap: os/arch the pack's binaries were built for
     * (lowercase uname sysname/machine, e.g. linux/x86_64). Empty =
     * wildcard: manifests written before this key match every host,
     * so all existing packs keep registering. */
    char     os[32];
    char     arch[32];
    uint32_t caps;
    uint64_t dec_mem;
    unsigned generation;
    pack_magic_rule magic[PACK_MAX_MAGIC];
    size_t   n_magic;
    size_t   pending_off;     /* sniff.offset seen before any sniff.magic */
    char     exts[256];       /* sniff.ext comma list, verbatim */
    char     requires[256];   /* extra tools, comma list */
    char     encode[512];
    char     decode[512];
    char     estimate[512];
    int      has_encode;
    int      has_decode;
    int      has_estimate;
    /* WP16a: `type = container` (default/absent = codec). Container packs
     * carry the four decomposition commands instead of encode/decode. */
    int      is_container;
    char     enumerate[512];
    char     extract[512];
    char     strip[512];
    char     rebuild[512];
    int      has_enumerate;
    int      has_extract;
    int      has_strip;
    int      has_rebuild;
    /* WP16b: optional `map` command (container packs only): {in} {out} ->
     * the FS-owned member map (MRMP); presence makes the pack SEEKABLE. */
    char     map[512];
    int      has_map;
    /* WP140: optional `batch` command (container packs only): {in} {dir}
     * {out} -> every member the enumerate table at {dir} announces, written
     * as "<idx>" files into {out}, in ONE header parse. */
    char     batch[512];
    int      has_batch;
    int      decomp_gen;   /* manifest `decomp_gen = 1` (WP-Q2R3) */
};

static void str_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int hex_nib(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* hex string -> bytes; 0 on empty/invalid (a zero-length rule never matches) */
static size_t parse_hex_bytes(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] && s[1]) {
        int hi = hex_nib((unsigned char)s[0]);
        int lo = hex_nib((unsigned char)s[1]);
        if (hi < 0 || lo < 0 || n >= cap) break;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n;
}

static uint32_t parse_caps(const char *s)
{
    static const struct { const char *tok; uint32_t bit; } ct[] = {
        { "seek",      INVFS_CODEC_CAP_SEEK },
        { "batched",   INVFS_CODEC_CAP_BATCHED },
        { "wholefile", INVFS_CODEC_CAP_WHOLEFILE },
        { "container", INVFS_CODEC_CAP_CONTAINER },
        { "external",  INVFS_CODEC_CAP_EXTERNAL },
    };
    uint32_t caps = 0;

    while (*s) {
        char tok[24];
        size_t tl = 0, i;
        while (*s == '|' || *s == ',' || *s == ' ' || *s == '\t') s++;
        while (s[tl] && s[tl] != '|' && s[tl] != ',' &&
               s[tl] != ' ' && s[tl] != '\t') {
            if (tl + 1 >= sizeof tok) return caps;   /* token garbage: stop */
            tok[tl] = s[tl];
            tl++;
        }
        tok[tl] = '\0';
        for (i = 0; i < sizeof ct / sizeof ct[0]; i++)
            if (strcmp(tok, ct[i].tok) == 0) { caps |= ct[i].bit; break; }
        s += tl;
    }
    return caps;
}

/* Minimal key=value parser: trims whitespace, ignores blank lines and
 * #-comments, skips unknown keys. sniff.offset applies to the most recent
 * sniff.magic rule (or to the first one, when it comes first). */
static int pack_parse_lines(FILE *f, struct pack_manifest *m)
{
    char line[1024];

    memset(m, 0, sizeof *m);
    m->algo = -1;
    m->pack_version = -1;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *eq, *val;
        size_t len = strlen(s);

        while (len && (s[len-1] == '\n' || s[len-1] == '\r' ||
                       s[len-1] == ' '  || s[len-1] == '\t'))
            s[--len] = '\0';
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        eq = strchr(s, '=');
        if (!eq) continue;
        val = eq + 1;
        while (eq > s && (eq[-1] == ' ' || eq[-1] == '\t')) eq--;
        *eq = '\0';
        while (*val == ' ' || *val == '\t') val++;
        if (strcmp(s, "encode") == 0) {
            str_copy(m->encode, sizeof m->encode, val);
            m->has_encode = 1;
        } else if (strcmp(s, "decode") == 0) {
            str_copy(m->decode, sizeof m->decode, val);
            m->has_decode = 1;
        } else if (strcmp(s, "estimate") == 0) {
            str_copy(m->estimate, sizeof m->estimate, val);
            m->has_estimate = 1;
        } else if (strcmp(s, "enumerate") == 0) {
            str_copy(m->enumerate, sizeof m->enumerate, val);
            m->has_enumerate = 1;
        } else if (strcmp(s, "extract") == 0) {
            str_copy(m->extract, sizeof m->extract, val);
            m->has_extract = 1;
        } else if (strcmp(s, "strip") == 0) {
            str_copy(m->strip, sizeof m->strip, val);
            m->has_strip = 1;
        } else if (strcmp(s, "rebuild") == 0) {
            str_copy(m->rebuild, sizeof m->rebuild, val);
            m->has_rebuild = 1;
        } else if (strcmp(s, "map") == 0) {
            str_copy(m->map, sizeof m->map, val);
            m->has_map = 1;
        } else if (strcmp(s, "batch") == 0) {
            str_copy(m->batch, sizeof m->batch, val);
            m->has_batch = 1;
        } else if (strcmp(s, "decomp_gen") == 0) {
            m->decomp_gen = (strtol(val, NULL, 0) != 0);
        } else if (strcmp(s, "type") == 0) {
            /* WP16a: only "container" is special; anything else = codec */
            m->is_container = (strcmp(val, "container") == 0);
        } else if (strcmp(s, "name") == 0) {
            str_copy(m->name, sizeof m->name, val);
        } else if (strcmp(s, "algo") == 0) {
            m->algo = strtol(val, NULL, 0);
        } else if (strcmp(s, "pack_version") == 0) {
            m->pack_version = strtol(val, NULL, 0);
        } else if (strcmp(s, "os") == 0) {
            str_copy(m->os, sizeof m->os, val);
        } else if (strcmp(s, "arch") == 0) {
            str_copy(m->arch, sizeof m->arch, val);
        } else if (strcmp(s, "caps") == 0) {
            m->caps = parse_caps(val);
        } else if (strcmp(s, "dec_mem") == 0) {
            m->dec_mem = strtoull(val, NULL, 0);
        } else if (strcmp(s, "generation") == 0) {
            m->generation = (unsigned)strtoul(val, NULL, 0);
        } else if (strcmp(s, "requires") == 0) {
            str_copy(m->requires, sizeof m->requires, val);
        } else if (strcmp(s, "sniff.ext") == 0) {
            str_copy(m->exts, sizeof m->exts, val);
        } else if (strcmp(s, "sniff.magic") == 0) {
            if (m->n_magic < PACK_MAX_MAGIC) {
                pack_magic_rule *r = &m->magic[m->n_magic];
                r->len = parse_hex_bytes(val, r->bytes, sizeof r->bytes);
                r->off = m->pending_off;
                m->pending_off = 0;
                if (r->len) m->n_magic++;
            }
        } else if (strcmp(s, "sniff.offset") == 0) {
            size_t off = (size_t)strtoul(val, NULL, 0);
            if (m->n_magic) m->magic[m->n_magic - 1].off = off;
            else            m->pending_off = off;
        }
    }
    return 1;
}

static int parse_manifest(const char *path, struct pack_manifest *m)
{
    FILE *f = fopen(path, "r");
    int rc;
    if (!f) return 0;
    rc = pack_parse_lines(f, m);
    fclose(f);
    return rc;
}

static int executable(const char *path)
{
    return access(path, X_OK) == 0;
}

/* "<dir>/<tool>", bounded; 1 if the joined path is executable */
static int dir_has_tool(const char *dir, size_t dlen, const char *tool)
{
    char full[4096];
    size_t tlen = strlen(tool);

    if (dlen + 1 + tlen + 1 > sizeof full) return 0;
    memcpy(full, dir, dlen);
    full[dlen] = '/';
    memcpy(full + dlen + 1, tool, tlen + 1);
    return executable(full);
}

static int list_has_tool(const char *dirs, const char *tool)
{
    const char *p = dirs;

    if (!p) return 0;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen && dir_has_tool(p, dlen, tool)) return 1;
        if (!colon) break;
        p = colon + 1;
    }
    return 0;
}

static int on_path(const char *tool)
{
    const char *path = getenv("PATH");
    if (!path) path = "/usr/bin:/bin";
    return list_has_tool(path, tool);
}

/* first argv token (fixed argv, no shell) must be executable: as a path
 * (relative to the pack dir first), else as a name in the pack's bin/ or on
 * PATH */
static int manifest_tool_ok(const char *packdir, const char *argv)
{
    char tok[512];
    size_t i = 0;

    while (argv[i] && argv[i] != ' ' && argv[i] != '\t') {
        if (i + 1 >= sizeof tok) return 0;
        tok[i] = argv[i];
        i++;
    }
    tok[i] = '\0';
    if (!tok[0]) return 0;
    if (strchr(tok, '/')) {
        char full[4096];
        size_t plen = strlen(packdir), tlen = strlen(tok);
        if (plen + 1 + tlen + 1 <= sizeof full) {
            memcpy(full, packdir, plen);
            full[plen] = '/';
            memcpy(full + plen + 1, tok, tlen + 1);
            if (executable(full)) return 1;
        }
        return executable(tok);
    }
    {
        char full[4096];
        size_t plen = strlen(packdir), tlen = strlen(tok);
        if (plen + 5 + tlen + 1 <= sizeof full) {
            memcpy(full, packdir, plen);
            memcpy(full + plen, "/bin/", 5);
            memcpy(full + plen + 5, tok, tlen + 1);
            if (executable(full)) return 1;
        }
    }
    return on_path(tok);
}

/* <dir>/<name>.codecpack/manifest, usable only if it names encode/decode
 * helpers that are all executable */
static int probe_pack_dir(const char *dir, size_t dlen, const char *name)
{
    char packdir[4096];
    char mpath[4096];
    size_t nlen = strlen(name), plen;
    struct pack_manifest m;

    plen = dlen + 1 + nlen + 10;                    /* "/<name>.codecpack" */
    if (plen + 1 > sizeof packdir) return 0;
    memcpy(packdir, dir, dlen);
    packdir[dlen] = '/';
    memcpy(packdir + dlen + 1, name, nlen);
    memcpy(packdir + dlen + 1 + nlen, ".codecpack", 10);
    packdir[plen] = '\0';

    if (plen + 9 + 1 > sizeof mpath) return 0;      /* "/manifest" */
    memcpy(mpath, packdir, plen);
    memcpy(mpath + plen, "/manifest", 9);
    mpath[plen + 9] = '\0';

    if (!parse_manifest(mpath, &m) || !m.has_encode || !m.has_decode) return 0;
    return manifest_tool_ok(packdir, m.encode) && manifest_tool_ok(packdir, m.decode);
}

/* P0-1 guard state. See codec.h: the flag is set once per session
 * (mount option or setter); the env var is read lazily so the offline
 * tools need no init plumbing. Neither site below is hot (both fork),
 * so a getenv per call is lost in the noise. */
static int g_no_autopack;

void invfs_set_no_autopack(int on)
{
    g_no_autopack = on ? 1 : 0;
}

int invfs_no_autopack(void)
{
    const char *e;
    if (g_no_autopack)
        return 1;
    e = getenv("INVFS_NO_AUTOPACK");
    return e && e[0] && e[0] != '0';
}

/* self-describing binary convention: <tool> --invfs-manifest prints a
 * manifest on stdout and exits 0; fixed argv, no shell */
static int tool_self_describes(const char *tool)
{
    int pfd[2];
    pid_t pid;
    int st = 0;
    size_t total = 0;
    char buf[512];
    ssize_t r;

    /* P0-1 guard: the probe forks a pack-supplied binary with the full
     * unsanitised environment (AUDIT WP210-NEW-2). Under noautopack the
     * tool is reported absent without executing anything; PACKONLY
     * entries then defer instead of admitting. */
    if (invfs_no_autopack())
        return 0;
    if (pipe(pfd) != 0) return 0;
    pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return 0; }
    if (pid == 0) {
        char *argv[3];
        int devnull = open("/dev/null", O_RDWR);

        argv[0] = (char *)tool;
        argv[1] = (char *)"--invfs-manifest";
        argv[2] = NULL;
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    while ((r = read(pfd[0], buf, sizeof buf)) > 0)
        total += (size_t)r;
    close(pfd[0]);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 && total > 0;
}

/* search order: $INVFS_CODECPACKS (colon-separated) ->
 * /usr/lib/invfs/codecpacks -> self-describing binary on PATH -> plain tool
 * on PATH */
static int probe_external(const char *name, const char *tool)
{
    static const char sysdir[] = "/usr/lib/invfs/codecpacks";
    const char *p = getenv("INVFS_CODECPACKS");

    /* P0-1 guard: the entry is not available for use, so it probes
     * absent -- before any fork (self-describe) or access() shortcut
     * (on_path) below. Manifest *reads* still register the pack; the
     * exec/cmd trampolines are the enforcement. */
    if (invfs_no_autopack())
        return 0;

    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen && probe_pack_dir(p, dlen, name)) return 1;
        if (!colon) break;
        p = colon + 1;
    }
    if (probe_pack_dir(sysdir, sizeof sysdir - 1, name)) return 1;
    if (tool_self_describes(tool)) return 1;
    return on_path(tool);
}

/* ---------------- dynamic codecpacks (WP13) ----------------
 *
 * WP10 §10 registered packs only as an AVAILABILITY source for builtin
 * external entries; nothing deserialized a manifest into a registry entry.
 * This does: each <dir>/<name>.codecpack/manifest found under the probe
 * search dirs ($INVFS_CODECPACKS colon-dirs, then /usr/lib/invfs/codecpacks)
 * becomes a real registry entry with manifest-driven sniff/probe and
 * encode/decode trampolines that run the manifest argv as a subprocess via
 * the volume.c exec hooks (codec.h).
 *
 * WP16e precedence rule (builtin -> pack migration): a manifest whose algo
 * names a builtin EXTERNAL placeholder (a sniff+probe-only entry: pmp/jxl/
 * ape/wv) OVERRIDES it -- the placeholder drops out of the materialized
 * view and the pack entry is the algo's only entry. Builtin stream codecs
 * (NONE/LZ4/ZSTD/PPMD) and builtin CONTAINER entries (zip/tarr/gzr/pngr/
 * flacr/exer) can never be claimed: a manifest naming their algo is
 * silently skipped. Pack-vs-pack collisions keep the first-registered.
 *
 * Static slots, cap INVFS_PACK_MAX; all manifest strings are strdup'd.
 * Loaded lazily on the first registry access and memoized; the sweep is
 * single-threaded, same discipline as the probe cache. Packs sit BEFORE
 * the PPMD text heuristic in registry order (specific magics first, text
 * LAST) and after the builtin entries.
 */
/* Owner decision 2026-10-08: roomy headroom, no rationale needed beyond
 * "enough". Slots are NOT algo values: the AST algo field is 6 bits
 * (0..63, shared with the builtins), so at most 64-REGISTRY_N distinct
 * pack algos can ever register -- pack_register refuses algo >= 64 and
 * duplicate algos at runtime. Extra slots simply stay empty; the arrays
 * below are small (manifest metadata + strdup'd strings). */
#define INVFS_PACK_MAX 64

typedef struct {
    invfs_codec      pub;       /* what the registry exposes */
    invfs_pack_def   def;       /* exec record for the volume.c hooks */
    char            *dir;       /* pack directory ({pack} substitution) */
    char            *name, *encode, *decode, *estimate, *requires, *exts;
    /* WP16a container commands (all NULL for a codec pack) */
    int              is_container;
    char            *enumerate, *extract, *strip, *rebuild;
    char            *map;       /* WP16b: container packs only, optional */
    char            *batch;     /* WP140: container packs only, optional */
    pack_magic_rule  magic[PACK_MAX_MAGIC];
    size_t           n_magic;
    int              overrides_builtin;  /* WP16e: this pack replaced a
                                          * builtin EXTERNAL entry (its algo
                                          * is pub.algo); packs_ensure drops
                                          * the builtin from the view */
    /* Reserve/bootstrap: which volume's .invariantfs this pack was
     * scanned from, NULL for host-dir scans. Volume packs never drop a
     * builtin from the shared view (that would lie to other volumes);
     * _vol lookups prefer the volume's own section instead. */
    const void      *origin_vol;
    int              probed;    /* memoized availability probe */
    int              avail;
    /* Reserve 3/3: materialized host dir for a volume pack (pack_host_dir
     * below). mat_ok 1 = mat_dir valid; 0 = never tried. No negative
     * cache: a transient staging failure retries next probe/exec, and a
     * complete staging short-circuits inside the volume hook. Freed with
     * the record; host staging itself is dropped by release (unload). */
    char             mat_dir[4096];
    int              mat_ok;
} pack_entry;

static pack_entry packs[INVFS_PACK_MAX];
static size_t     packs_n;
static int        packs_built;      /* 1 = host dirs scanned once */
static int        packs_dirty = 1;  /* 1 = volume sections changed: rebuild the view */
static void       packs_ensure(void);
static size_t     g_all_n;          /* live entries in g_all (below) */

/* generic manifest-sniff: any magic rule -> 100, else extension list -> 50 */
static int pack_sniff_impl(const pack_entry *p, const uint8_t *head,
                           size_t head_len, const char *name)
{
    size_t i;

    for (i = 0; i < p->n_magic; i++)
        if (magic_at(head, head_len, p->magic[i].bytes, p->magic[i].len,
                     p->magic[i].off))
            return 100;
    if (name && p->exts && p->exts[0]) {
        const char *base = strrchr(name, '/');
        const char *dot;
        const char *x;

        base = base ? base + 1 : name;
        dot = strrchr(base, '.');
        if (!dot || !dot[1]) return 0;
        x = p->exts;
        while (*x) {
            const char *comma = strchr(x, ',');
            size_t tl = comma ? (size_t)(comma - x) : strlen(x);
            char tok[32];

            if (tl && tl < sizeof tok) {
                memcpy(tok, x, tl);
                tok[tl] = '\0';
                if (ext_eq(dot + 1, tok)) return 50;
            }
            if (!comma) break;
            x = comma + 1;
        }
    }
    return 0;
}

/* Reserve 3/3: volume tags with live host staging (pointer values only;
 * release never dereferences, so dangling tags after close are safe).
 * unload drops one volume's staging, probe_reset drops everything. */
#define STAGE_VOL_MAX 16
static const void *stage_vols[STAGE_VOL_MAX];
static size_t stage_n;

static void stage_tag(const void *vol)
{
    size_t i;
    for (i = 0; i < stage_n; i++)
        if (stage_vols[i] == vol) return;
    if (stage_n < STAGE_VOL_MAX)
        stage_vols[stage_n++] = vol;
}

static void stage_untag(const void *vol)
{
    size_t i, n = 0;
    for (i = 0; i < stage_n; i++) {
        if (stage_vols[i] == vol) {
            invfs_vol_pack_release(vol);
            continue;
        }
        stage_vols[n++] = stage_vols[i];
    }
    stage_n = n;
}

/* Reserve 3/3: record/host-dir resolution (defined after the
 * materialized view, which owns packs[]/g_all). */
static pack_entry *pack_record_for(const invfs_codec *c);
static const char *pack_host_dir(pack_entry *p);

/* a `requires` tool must resolve the way the exec layer will look for it:
 * $INVFS_TOOLS/<name> -> /usr/lib/invfs/tools/<name> -> <pack>/bin/<name>
 * (the pack-sibling convention the helpers themselves use) -> PATH */
static int pack_tool_resolvable(const char *pdir, const char *tool)
{
    static const char tooldir[] = "/usr/lib/invfs/tools";
    const char *dir = getenv("INVFS_TOOLS");
    char pbin[4096];
    int n;

    if (dir && *dir && dir_has_tool(dir, strlen(dir), tool)) return 1;
    if (dir_has_tool(tooldir, sizeof tooldir - 1, tool)) return 1;
    if (pdir) {
        n = snprintf(pbin, sizeof pbin, "%s/bin", pdir);
        if (n > 0 && (size_t)n < sizeof pbin &&
            dir_has_tool(pbin, (size_t)n, tool))
            return 1;
    }
    return on_path(tool);
}

/* available iff every argv's tool resolves (the codec set — encode/decode,
 * estimate when present — or the WP16a container set — enumerate/extract/
 * strip/rebuild, plus the WP16b map command and the WP140 batch command when
 * the pack declares them) and every `requires` entry does */
static int pack_probe_impl(pack_entry *p)
{
    const char *r;
    const char *hd;

    if (p->probed) return p->avail;
    p->probed = 1;
    p->avail = 0;
    /* Reserve 3/3: volume packs answer from their staged host dir
     * (materialized on first probe, memoized with the verdict). A
     * staging failure is a decline, exactly like an absent host tool. */
    hd = pack_host_dir(p);
    if (!hd) return 0;
    if (p->is_container) {
        if (!manifest_tool_ok(hd, p->enumerate) ||
            !manifest_tool_ok(hd, p->extract) ||
            !manifest_tool_ok(hd, p->strip) ||
            !manifest_tool_ok(hd, p->rebuild))
            return 0;
        if (p->map && !manifest_tool_ok(hd, p->map))
            return 0;
        /* WP140: a declared batch whose argv0 does not resolve must make the
         * pack UNAVAILABLE, not silently degrade it to per-member extract:
         * the lane prefers batch, and "declared but unrunnable" is a pack
         * that cannot be installed as written. */
        if (p->batch && !manifest_tool_ok(hd, p->batch))
            return 0;
    } else {
        if (!manifest_tool_ok(hd, p->encode) ||
            !manifest_tool_ok(hd, p->decode))
            return 0;
    }
    if (p->estimate && !manifest_tool_ok(hd, p->estimate))
        return 0;
    r = p->requires;
    while (r && *r) {
        const char *comma = strchr(r, ',');
        size_t tl = comma ? (size_t)(comma - r) : strlen(r);
        char tok[64];

        if (!tl || tl >= sizeof tok) return 0;
        memcpy(tok, r, tl);
        tok[tl] = '\0';
        if (!pack_tool_resolvable(hd, tok)) return 0;
        if (!comma) break;
        r = comma + 1;
    }
    p->avail = 1;
    return 1;
}

/* Trampoline plumbing: spool the input buffer to a scratch file, run the
 * manifest argv through the volume.c exec hook, slurp the output file.
 *
 * This used to carry its OWN copy of the tmpfs-first root list, sized by
 * nothing -- the same defect the volume's tool_tmpdir() had, in a second
 * place, so a codecpack trampoline could fill a tmpfs that the sized
 * decision had already ruled out for the lanes around it. It now shares the
 * one decision (src/core/tool_scratch.c), told how much the trampoline will
 * actually put there: the input spool plus the output the caller reserved. */
static int pack_tmpdir(char *dir, size_t cap, uint64_t need)
{
    return tool_tmpdir(dir, cap, need);
}

static int pack_write(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); return -1; }
    return fclose(f);
}

static int pack_slurp(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;

    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    *out = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!*out) { fclose(f); return -1; }
    if (fread(*out, 1, (size_t)sz, f) != (size_t)sz) {
        free(*out); *out = NULL; fclose(f); return -1;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return 0;
}

static int pack_buffer_io(pack_entry *p, int is_encode,
                          const uint8_t *in, size_t inlen,
                          uint8_t *out, size_t outcap, size_t *outlen)
{
    char dir[64], pin[128], pout[128];
    uint8_t *res = NULL;
    size_t res_len = 0;
    int rc = -1;

    if (pack_tmpdir(dir, sizeof dir, (uint64_t)inlen + (uint64_t)outcap
                    + (1u << 20)) != 0) return -1;
    snprintf(pin, sizeof pin, "%s/in", dir);
    snprintf(pout, sizeof pout, "%s/out", dir);
    if (pack_write(pin, in, inlen) != 0) { rmdir(dir); return -1; }
    /* nonzero exit or a missing/empty output = the pack declined */
    if (invfs_codec_pack_exec(&p->pub, is_encode, pin, pout) == 0 &&
        pack_slurp(pout, &res, &res_len) == 0 && res_len > 0 &&
        res_len <= outcap) {
        memcpy(out, res, res_len);
        *outlen = res_len;
        rc = 0;
    }
    free(res);
    unlink(pin);
    unlink(pout);
    rmdir(dir);
    return rc;
}

/* decode is exact-size: a short/long result means the blob is corrupt --
 * bit-exact or nothing (the PMP rule) */
static int pack_decode_io(pack_entry *p, const uint8_t *in, size_t inlen,
                          uint8_t *out, size_t outlen)
{
    size_t got = 0;

    if (pack_buffer_io(p, 0, in, inlen, out, outlen, &got) != 0) return -1;
    return got == outlen ? 0 : -1;
}

/* The registry fn-pointer signature carries no codec identity, so the
 * trampolines are per-slot, generated by one macro. */
#define PACK_TRAMPOLINES(i)                                                     \
static int pack_sniff_##i(const uint8_t *h, size_t hl, const char *n)           \
{ return (size_t)(i) < packs_n ? pack_sniff_impl(&packs[i], h, hl, n) : 0; }    \
static int pack_probe_##i(void)                                                 \
{ return (size_t)(i) < packs_n ? pack_probe_impl(&packs[i]) : 0; }              \
static int pack_encode_##i(const uint8_t *in, size_t il, uint8_t *out,          \
                           size_t cap, size_t *ol)                              \
{ return (size_t)(i) < packs_n ? pack_buffer_io(&packs[i], 1, in, il, out,      \
                                                cap, ol) : -1; }                \
static int pack_decode_##i(const uint8_t *in, size_t il, uint8_t *out,          \
                           size_t ol)                                           \
{ return (size_t)(i) < packs_n ? pack_decode_io(&packs[i], in, il, out, ol)     \
                               : -1; }

PACK_TRAMPOLINES(0)
PACK_TRAMPOLINES(1)
PACK_TRAMPOLINES(2)
PACK_TRAMPOLINES(3)
PACK_TRAMPOLINES(4)
PACK_TRAMPOLINES(5)
PACK_TRAMPOLINES(6)
PACK_TRAMPOLINES(7)
PACK_TRAMPOLINES(8)
PACK_TRAMPOLINES(9)
PACK_TRAMPOLINES(10)
PACK_TRAMPOLINES(11)
PACK_TRAMPOLINES(12)
PACK_TRAMPOLINES(13)
PACK_TRAMPOLINES(14)
PACK_TRAMPOLINES(15)
PACK_TRAMPOLINES(16)
PACK_TRAMPOLINES(17)
PACK_TRAMPOLINES(18)
PACK_TRAMPOLINES(19)
PACK_TRAMPOLINES(20)
PACK_TRAMPOLINES(21)
PACK_TRAMPOLINES(22)
PACK_TRAMPOLINES(23)
PACK_TRAMPOLINES(24)
PACK_TRAMPOLINES(25)
PACK_TRAMPOLINES(26)
PACK_TRAMPOLINES(27)
PACK_TRAMPOLINES(28)
PACK_TRAMPOLINES(29)
PACK_TRAMPOLINES(30)
PACK_TRAMPOLINES(31)
PACK_TRAMPOLINES(32)
PACK_TRAMPOLINES(33)
PACK_TRAMPOLINES(34)
PACK_TRAMPOLINES(35)
PACK_TRAMPOLINES(36)
PACK_TRAMPOLINES(37)
PACK_TRAMPOLINES(38)
PACK_TRAMPOLINES(39)
PACK_TRAMPOLINES(40)
PACK_TRAMPOLINES(41)
PACK_TRAMPOLINES(42)
PACK_TRAMPOLINES(43)
PACK_TRAMPOLINES(44)
PACK_TRAMPOLINES(45)
PACK_TRAMPOLINES(46)
PACK_TRAMPOLINES(47)
PACK_TRAMPOLINES(48)
PACK_TRAMPOLINES(49)
PACK_TRAMPOLINES(50)
PACK_TRAMPOLINES(51)
PACK_TRAMPOLINES(52)
PACK_TRAMPOLINES(53)
PACK_TRAMPOLINES(54)
PACK_TRAMPOLINES(55)
PACK_TRAMPOLINES(56)
PACK_TRAMPOLINES(57)
PACK_TRAMPOLINES(58)
PACK_TRAMPOLINES(59)
PACK_TRAMPOLINES(60)
PACK_TRAMPOLINES(61)
PACK_TRAMPOLINES(62)
PACK_TRAMPOLINES(63)

static const struct pack_slot_fns {
    int (*sniff)(const uint8_t *, size_t, const char *);
    int (*probe)(void);
    int (*encode)(const uint8_t *, size_t, uint8_t *, size_t, size_t *);
    int (*decode)(const uint8_t *, size_t, uint8_t *, size_t);
} pack_fns[INVFS_PACK_MAX] = {
    { pack_sniff_0, pack_probe_0, pack_encode_0, pack_decode_0 },
    { pack_sniff_1, pack_probe_1, pack_encode_1, pack_decode_1 },
    { pack_sniff_2, pack_probe_2, pack_encode_2, pack_decode_2 },
    { pack_sniff_3, pack_probe_3, pack_encode_3, pack_decode_3 },
    { pack_sniff_4, pack_probe_4, pack_encode_4, pack_decode_4 },
    { pack_sniff_5, pack_probe_5, pack_encode_5, pack_decode_5 },
    { pack_sniff_6, pack_probe_6, pack_encode_6, pack_decode_6 },
    { pack_sniff_7, pack_probe_7, pack_encode_7, pack_decode_7 },
    { pack_sniff_8, pack_probe_8, pack_encode_8, pack_decode_8 },
    { pack_sniff_9, pack_probe_9, pack_encode_9, pack_decode_9 },
    { pack_sniff_10, pack_probe_10, pack_encode_10, pack_decode_10 },
    { pack_sniff_11, pack_probe_11, pack_encode_11, pack_decode_11 },
    { pack_sniff_12, pack_probe_12, pack_encode_12, pack_decode_12 },
    { pack_sniff_13, pack_probe_13, pack_encode_13, pack_decode_13 },
    { pack_sniff_14, pack_probe_14, pack_encode_14, pack_decode_14 },
    { pack_sniff_15, pack_probe_15, pack_encode_15, pack_decode_15 },
    { pack_sniff_16, pack_probe_16, pack_encode_16, pack_decode_16 },
    { pack_sniff_17, pack_probe_17, pack_encode_17, pack_decode_17 },
    { pack_sniff_18, pack_probe_18, pack_encode_18, pack_decode_18 },
    { pack_sniff_19, pack_probe_19, pack_encode_19, pack_decode_19 },
    { pack_sniff_20, pack_probe_20, pack_encode_20, pack_decode_20 },
    { pack_sniff_21, pack_probe_21, pack_encode_21, pack_decode_21 },
    { pack_sniff_22, pack_probe_22, pack_encode_22, pack_decode_22 },
    { pack_sniff_23, pack_probe_23, pack_encode_23, pack_decode_23 },
    { pack_sniff_24, pack_probe_24, pack_encode_24, pack_decode_24 },
    { pack_sniff_25, pack_probe_25, pack_encode_25, pack_decode_25 },
    { pack_sniff_26, pack_probe_26, pack_encode_26, pack_decode_26 },
    { pack_sniff_27, pack_probe_27, pack_encode_27, pack_decode_27 },
    { pack_sniff_28, pack_probe_28, pack_encode_28, pack_decode_28 },
    { pack_sniff_29, pack_probe_29, pack_encode_29, pack_decode_29 },
    { pack_sniff_30, pack_probe_30, pack_encode_30, pack_decode_30 },
    { pack_sniff_31, pack_probe_31, pack_encode_31, pack_decode_31 },
    { pack_sniff_32, pack_probe_32, pack_encode_32, pack_decode_32 },
    { pack_sniff_33, pack_probe_33, pack_encode_33, pack_decode_33 },
    { pack_sniff_34, pack_probe_34, pack_encode_34, pack_decode_34 },
    { pack_sniff_35, pack_probe_35, pack_encode_35, pack_decode_35 },
    { pack_sniff_36, pack_probe_36, pack_encode_36, pack_decode_36 },
    { pack_sniff_37, pack_probe_37, pack_encode_37, pack_decode_37 },
    { pack_sniff_38, pack_probe_38, pack_encode_38, pack_decode_38 },
    { pack_sniff_39, pack_probe_39, pack_encode_39, pack_decode_39 },
    { pack_sniff_40, pack_probe_40, pack_encode_40, pack_decode_40 },
    { pack_sniff_41, pack_probe_41, pack_encode_41, pack_decode_41 },
    { pack_sniff_42, pack_probe_42, pack_encode_42, pack_decode_42 },
    { pack_sniff_43, pack_probe_43, pack_encode_43, pack_decode_43 },
    { pack_sniff_44, pack_probe_44, pack_encode_44, pack_decode_44 },
    { pack_sniff_45, pack_probe_45, pack_encode_45, pack_decode_45 },
    { pack_sniff_46, pack_probe_46, pack_encode_46, pack_decode_46 },
    { pack_sniff_47, pack_probe_47, pack_encode_47, pack_decode_47 },
    { pack_sniff_48, pack_probe_48, pack_encode_48, pack_decode_48 },
    { pack_sniff_49, pack_probe_49, pack_encode_49, pack_decode_49 },
    { pack_sniff_50, pack_probe_50, pack_encode_50, pack_decode_50 },
    { pack_sniff_51, pack_probe_51, pack_encode_51, pack_decode_51 },
    { pack_sniff_52, pack_probe_52, pack_encode_52, pack_decode_52 },
    { pack_sniff_53, pack_probe_53, pack_encode_53, pack_decode_53 },
    { pack_sniff_54, pack_probe_54, pack_encode_54, pack_decode_54 },
    { pack_sniff_55, pack_probe_55, pack_encode_55, pack_decode_55 },
    { pack_sniff_56, pack_probe_56, pack_encode_56, pack_decode_56 },
    { pack_sniff_57, pack_probe_57, pack_encode_57, pack_decode_57 },
    { pack_sniff_58, pack_probe_58, pack_encode_58, pack_decode_58 },
    { pack_sniff_59, pack_probe_59, pack_encode_59, pack_decode_59 },
    { pack_sniff_60, pack_probe_60, pack_encode_60, pack_decode_60 },
    { pack_sniff_61, pack_probe_61, pack_encode_61, pack_decode_61 },
    { pack_sniff_62, pack_probe_62, pack_encode_62, pack_decode_62 },
    { pack_sniff_63, pack_probe_63, pack_encode_63, pack_decode_63 },
};

static char *pack_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void pack_entry_free(pack_entry *p)
{
    free(p->dir); free(p->name); free(p->encode); free(p->decode);
    free(p->estimate); free(p->requires); free(p->exts);
    free(p->enumerate); free(p->extract); free(p->strip); free(p->rebuild);
    free(p->map);
    free(p->batch);
    memset(p, 0, sizeof *p);
}

/* WP12(e): probe() results memoized per registry entry, keyed by algo.
 * Filled lazily on the first probe of each codec. The sweep is
 * single-threaded today, so a plain unsynchronized cache is fine --
 * revisit if probing ever goes concurrent. */
#define PROBE_CACHE_SLOTS 16   /* INVFS_ALGO_* registry ids run 0..15; 14
                                * (ZSTD_BCJ) is an AST-only tag (WP14a) with
                                * no registry entry, so it never probes */
static signed char probe_cache[PROBE_CACHE_SLOTS];   /* 0 = not probed yet */

static int probe_cached(uint32_t algo, const char *name, const char *tool)
{
    signed char r;
    if (algo >= PROBE_CACHE_SLOTS) return probe_external(name, tool);
    r = probe_cache[algo];
    if (r == 0) {
        r = (signed char)(probe_external(name, tool) ? 1 : -1);
        probe_cache[algo] = r;
    }
    return r > 0;
}

/* test hook: probe results and pack registrations are memoized; fixtures
 * that mutate INVFS_CODECPACKS/PATH between scenarios must reset first. */
void invfs_codec_probe_reset(void)
{
    size_t i;

    memset(probe_cache, 0, sizeof probe_cache);
    /* Reserve 3/3: the test hook drops every section; staged host dirs
     * go with them (release is tag-based, safe past close). */
    for (i = 0; i < stage_n; i++)
        invfs_vol_pack_release(stage_vols[i]);
    stage_n = 0;
    for (i = 0; i < packs_n; i++) pack_entry_free(&packs[i]);
    packs_n = 0;
    packs_built = 0;
    packs_dirty = 1;   /* the view rebuilds (empty) on next lookup */
    g_all_n = 0;
}

static int probe_pmp(void) { return probe_cached(INVFS_ALGO_PMP, "pmp", "packMP3"); }
static int probe_jxl(void) { return probe_cached(INVFS_ALGO_JXL, "jxl", "cjxl"); }
static int probe_ape(void) { return probe_cached(INVFS_ALGO_APE, "ape", "mac"); }
static int probe_wv(void)  { return probe_cached(INVFS_ALGO_WV,  "wv",  "wavpack"); }

/* ---------------- the registry ----------------
 * Order = sniff priority: specific magics first, text LAST. NONE/LZ4/ZSTD
 * have no sniff (chosen by policy, never self-selected by content). */
static const invfs_codec registry[] = {
    { INVFS_ALGO_NONE, "none",
      INVFS_CODEC_CAP_SEEK, 0, 0,
      NULL, NULL, none_encode, none_decode },
    { INVFS_ALGO_LZ4, "lz4",
      INVFS_CODEC_CAP_SEEK, 64ull << 10, 0,
      NULL, NULL, lz4c_encode, lz4c_decode },
    { INVFS_ALGO_ZSTD, "zstd",
      INVFS_CODEC_CAP_SEEK, (8ull << 20) + (64ull << 10), 0,
      NULL, NULL, zstdc_encode, zstdc_decode },
    { INVFS_ALGO_ZIPR, "zip",
      INVFS_CODEC_CAP_CONTAINER, 0, 1,
      sniff_zip, NULL, NULL, NULL },
    { INVFS_ALGO_TARR, "tarr",
      INVFS_CODEC_CAP_CONTAINER, 0, 1,
      sniff_tarr, NULL, NULL, NULL },
    { INVFS_ALGO_GZR, "gzr",
      INVFS_CODEC_CAP_CONTAINER, 0, 1,
      sniff_gzr, NULL, NULL, NULL },
    { INVFS_ALGO_PNGR, "pngr",
      INVFS_CODEC_CAP_CONTAINER, 0, 1,
      sniff_pngr, NULL, NULL, NULL },
    { INVFS_ALGO_FLACR, "flacr",
      INVFS_CODEC_CAP_CONTAINER, 0, 1,
      sniff_flacr, NULL, NULL, NULL },
    { INVFS_ALGO_PMP, "pmp",
      INVFS_CODEC_CAP_EXTERNAL, 0, 1,
      sniff_pmp, probe_pmp, NULL, NULL },
    /* WP16e: the JPEG->JXL lane migrated to tools/codecpacks/jxl.codecpack.
     * The placeholder keeps sniff + probe so the sweep can DEFER JPEGs when
     * the pack is not installed (PACKONLY -- see codec.h), and a pack whose
     * manifest claims algo 4 OVERRIDES this entry (pack_register). */
    { INVFS_ALGO_JXL, "jxl",
      INVFS_CODEC_CAP_EXTERNAL | INVFS_CODEC_CAP_PACKONLY, 0, 1,
      sniff_jxl, probe_jxl, NULL, NULL },
    { INVFS_ALGO_APE, "ape",
      INVFS_CODEC_CAP_EXTERNAL, 0, 1,
      sniff_ape, probe_ape, NULL, NULL },
    { INVFS_ALGO_WV, "wv",
      INVFS_CODEC_CAP_EXTERNAL, 0, 1,
      sniff_wv, probe_wv, NULL, NULL },
    { INVFS_ALGO_EXER, "exer",
      INVFS_CODEC_CAP_CONTAINER, 0, 1,
      sniff_exer, NULL, NULL, NULL },
    { INVFS_ALGO_PPMD, "ppmd",
      INVFS_CODEC_CAP_BATCHED, 68ull << 20, 0,
      sniff_ppmd, NULL, invfs_ppmd_encode, invfs_ppmd_decode },
};

#define REGISTRY_N (sizeof registry / sizeof registry[0])

/* The 6-bit AST algo field is the hard ceiling both sides share:
 * builtins must leave room for packs. This fires when someone adds a
 * builtin row past the pack space, not when packs fill up (that is a
 * runtime refusal with a log line). */
_Static_assert(REGISTRY_N < 64,
    "builtin registry rows must fit the 6-bit AST algo field");

/* materialized registry: static entries + loaded packs, PPMD last.
 * g_all_vol tags each slot's origin in lockstep (NULL = static/host);
 * the tags are what _vol lookups scope by. The arrays are rebuilt in
 * place when volume sections change, so entry POINTERS stay valid but
 * their TARGETS may move: do not hold them across vol_open/vol_close
 * of any volume. In practice nothing does (sweeps hold their volume
 * open throughout). */
static invfs_codec g_all[REGISTRY_N + INVFS_PACK_MAX];
static const void *g_all_vol[REGISTRY_N + INVFS_PACK_MAX];

/* ---------------- pack registration + materialized view (WP13) ---------- */

/* Reserve/bootstrap host identity: lowercase uname sysname/machine
 * (linux/x86_64). Manifest os/arch match case-insensitively; empty
 * is a wildcard so pre-os/arch manifests keep registering. */
static void pack_host_id(char os[32], char arch[32])
{
#ifdef _WIN32
    str_copy(os, 32, "windows");
    str_copy(arch, 32, "");    /* undetectable here: arch always matches */
#else
    struct utsname u;
    size_t i;
    if (uname(&u) != 0) {
        str_copy(os, 32, "");
        str_copy(arch, 32, "");
        return;
    }
    str_copy(os, 32, u.sysname);
    str_copy(arch, 32, u.machine);
    for (i = 0; os[i]; i++)
        if (os[i] >= 'A' && os[i] <= 'Z') os[i] += (char)('a' - 'A');
    for (i = 0; arch[i]; i++)
        if (arch[i] >= 'A' && arch[i] <= 'Z') arch[i] += (char)('a' - 'A');
#endif
}

static int pack_word_matches(const char *want, const char *have)
{
    size_t i;
    if (!want[0])
        return 1;   /* wildcard */
    for (i = 0; ; i++) {
        unsigned char a = (unsigned char)want[i];
        unsigned char b = (unsigned char)have[i];
        if (a >= 'A' && a <= 'Z') a += (unsigned char)('a' - 'A');
        if (b >= 'A' && b <= 'Z') b += (unsigned char)('a' - 'A');
        if (a != b)
            return 0;
        if (!a)
            return 1;
    }
}

static const char *pack_host_os(void)
{
    static char os[32];
    static int once;
    if (!once) {
        char arch[32];
        pack_host_id(os, arch);
        once = 1;
    }
    return os;
}

static const char *pack_host_arch(void)
{
    static char arch[32];
    static int once;
    if (!once) {
        char os[32];
        pack_host_id(os, arch);
        once = 1;
    }
    return arch;
}

static int pack_host_matches(const struct pack_manifest *m)
{
    /* An arch this host cannot detect (_WIN32 above) matches anything:
     * refusing on unknown hardware would brick every stamped pack. */
    if (m->arch[0] && pack_host_arch()[0] &&
        !pack_word_matches(m->arch, pack_host_arch()))
        return 0;
    if (m->os[0] && pack_host_os()[0] &&
        !pack_word_matches(m->os, pack_host_os()))
        return 0;
    return 1;
}

/* Trampolines close over the SLOT index, not the entry: any structural
 * change (register, unload-compact) must rebind every live entry to
 * its current slot, or an entry silently drives its neighbour's
 * pack (or a dead slot past packs_n, which declines everything). */
static void pack_rebind_all(void)
{
    size_t i;
    for (i = 0; i < packs_n; i++) {
        packs[i].pub.sniff = pack_fns[i].sniff;
        packs[i].pub.probe = pack_fns[i].probe;
        if (packs[i].is_container) {
            packs[i].pub.encode = NULL;
            packs[i].pub.decode = NULL;
        } else {
            packs[i].pub.encode = pack_fns[i].encode;
            packs[i].pub.decode = pack_fns[i].decode;
        }
    }
}

/* vol == NULL for host-dir scans, else the volume whose .invariantfs
 * the manifest was read from. Origin scopes duplicates (two volumes
 * may carry different packs under one algo or name) and volume packs
 * never drop a builtin from the shared view -- _vol lookups prefer the
 * volume's own section instead. */
static void pack_register_inner(const char *dir, const struct pack_manifest *m,
                                const void *vol)
{
    pack_entry *p;
    size_t i;
    int overrides = 0;

    if (!pack_host_matches(m)) {
        /* Reserve/bootstrap: a pack built for another OS/arch is not
         * silently absent -- the volume that needs it names the gap. */
        fprintf(stderr, "[codecpack] %s/%s skipped: built for %s/%s"
                " (this host is %s/%s)\n", dir, m->name,
                m->os[0] ? m->os : "any", m->arch[0] ? m->arch : "any",
                pack_host_os(), pack_host_arch());
        return;
    }

    if (packs_n >= INVFS_PACK_MAX) {
        fprintf(stderr, "[codecpack] WARNING: pack table full (%d), %s/%s dropped\n",
                INVFS_PACK_MAX, dir, m->name);
        return;
    }
    if (!m->name[0] || m->algo < 0)
        return;
    /* WP16a: a container pack (type=container) declares the four
     * decomposition commands INSTEAD of encode/decode; a codec pack
     * declares encode/decode. A pack missing its type's commands is
     * silently skipped (a half-written pack must not register). */
    if (m->is_container) {
        if (!m->has_enumerate || !m->has_extract ||
            !m->has_strip || !m->has_rebuild)
            return;
    } else if (!m->has_encode || !m->has_decode) {
        return;
    }
    if ((unsigned long)m->algo >= 64) return;   /* AST algo field is 6 bits */
    /* WP16e override precedence: a pack manifest may claim the algo of a
     * builtin EXTERNAL placeholder (sniff+probe only, the transcode lives
     * outside the registry -- pmp/jxl/ape/wv); the pack then REPLACES that
     * entry in the materialized registry (sniff, policy fields and the
     * encode/decode trampolines all come from the manifest). Builtin STREAM
     * codecs (NONE/LZ4/ZSTD/PPMD -- they have in-process encode/decode) and
     * builtin CONTAINER entries are never overridden: their algos stay
     * taken. */
    for (i = 0; i < REGISTRY_N; i++)
        if (registry[i].algo == (uint32_t)m->algo) {
            if (!(registry[i].caps & INVFS_CODEC_CAP_EXTERNAL))
                return;                     /* algo taken, not overridable */
            if (!vol)
                overrides = 1;
            break;
        }
    for (i = 0; i < packs_n; i++) {
        if (packs[i].origin_vol != vol)
            continue;   /* another folder's same algo/name is fine */
        if (packs[i].pub.algo == (uint32_t)m->algo) return;
        if (strcmp(packs[i].name, m->name) == 0) return;     /* seen already */
    }

    p = &packs[packs_n];
    memset(p, 0, sizeof *p);
    p->is_container = m->is_container;
    p->dir      = pack_strdup(dir);
    p->name     = pack_strdup(m->name);
    p->encode   = m->has_encode ? pack_strdup(m->encode) : NULL;
    p->decode   = m->has_decode ? pack_strdup(m->decode) : NULL;
    p->estimate = m->has_estimate ? pack_strdup(m->estimate) : NULL;
    p->requires = m->requires[0] ? pack_strdup(m->requires) : NULL;
    p->exts     = m->exts[0] ? pack_strdup(m->exts) : NULL;
    p->enumerate = m->has_enumerate ? pack_strdup(m->enumerate) : NULL;
    p->extract   = m->has_extract ? pack_strdup(m->extract) : NULL;
    p->strip     = m->has_strip ? pack_strdup(m->strip) : NULL;
    p->rebuild   = m->has_rebuild ? pack_strdup(m->rebuild) : NULL;
    /* WP16b: `map` is a container-ABI command; on a codec pack the line is
     * parsed but never wired (def.map stays NULL, no CAP_SEEK). Same for
     * WP140's `batch`. */
    p->map = (m->is_container && m->has_map) ? pack_strdup(m->map) : NULL;
    p->batch = (m->is_container && m->has_batch) ? pack_strdup(m->batch) : NULL;
    p->def.decomp_gen = (m->is_container && m->decomp_gen) ? 1 : 0;
    if (!p->dir || !p->name ||
        (m->has_encode && !p->encode) || (m->has_decode && !p->decode) ||
        (m->has_estimate && !p->estimate) ||
        (m->requires[0] && !p->requires) || (m->exts[0] && !p->exts) ||
        (m->is_container && m->has_map && !p->map) ||
        (m->is_container && m->has_batch && !p->batch) ||
        (m->is_container &&
         (!p->enumerate || !p->extract || !p->strip || !p->rebuild))) {
        pack_entry_free(p);
        return;
    }
    memcpy(p->magic, m->magic, sizeof p->magic);
    p->n_magic = m->n_magic;
    p->overrides_builtin = overrides;

    p->pub.algo          = (uint32_t)m->algo;
    p->pub.name          = p->name;
    p->pub.caps          = m->caps;
    p->pub.dec_mem_bytes = m->dec_mem;
    p->pub.generation    = (uint16_t)m->generation;
    /* sniff/probe/encode/decode come from pack_rebind_all() below. */
    if (p->is_container) {
        /* a container pack never whole-file transcodes: the WP13 sweep
         * loop skips NULL encode/decode, and the WP16a container branch
         * picks it up by the CONTAINER cap + def->is_container. CONTAINER/
         * EXTERNAL/WHOLEFILE are forced on regardless of the manifest:
         * packs are external by definition and the rebuild is a
         * whole-file read unit (the ARC divert and the policy compliance
         * check both key off WHOLEFILE). WP16b: a `map` command makes the
         * container SEEKABLE (local splice reads through the !mbrmap
         * sibling, no pack exec); it is the ONLY source of CAP_SEEK for a
         * container pack -- a caps line claiming "seek" without the map
         * command would divert reads to a map that cannot exist. */
        p->pub.caps  |= INVFS_CODEC_CAP_CONTAINER | INVFS_CODEC_CAP_EXTERNAL |
                        INVFS_CODEC_CAP_WHOLEFILE;
        if (p->map)
            p->pub.caps |= INVFS_CODEC_CAP_SEEK;
        else
            p->pub.caps &= ~INVFS_CODEC_CAP_SEEK;
        p->pub.encode = NULL;
        p->pub.decode = NULL;
    }
    /* codec packs: encode/decode bound by pack_rebind_all() below. */

    p->def.dir      = p->dir;
    p->def.encode   = p->encode;
    p->def.decode   = p->decode;
    p->def.estimate = p->estimate;
    p->def.requires = p->requires;
    p->def.is_container = p->is_container;
    p->def.enumerate = p->enumerate;
    p->def.extract   = p->extract;
    p->def.strip     = p->strip;
    p->def.rebuild   = p->rebuild;
    p->def.map       = p->map;
    p->def.batch     = p->batch;
    p->origin_vol = vol;
    packs_n++;
    pack_rebind_all();
    packs_dirty = 1;
    memset(probe_cache, 0, sizeof probe_cache);
}

/* Reserve/bootstrap: register one manifest from a memory buffer (the
 * volume scan reads manifests with vol_read_file, not stdio). Returns
 * 0 on registration, -1 when skipped or unparseable. */
int invfs_codec_register_pack_mem(const char *dir, const uint8_t *buf,
                                  size_t blen, const void *vol)
{
#ifdef _WIN32
    (void)dir; (void)buf; (void)blen; (void)vol;
    return -1;   /* volume packs need fmemopen; Windows is parked */
#else
    struct pack_manifest m;
    FILE *f;
    size_t before = packs_n;
    if (!dir || !buf || !blen || !vol)
        return -1;
    f = fmemopen((void *)buf, blen, "r");
    if (!f)
        return -1;
    if (!pack_parse_lines(f, &m)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    pack_register_inner(dir, &m, vol);
    return packs_n > before ? 0 : -1;
#endif
}

/* Reserve/bootstrap: drop every pack a volume brought. Called at
 * vol_close (before teardown) and by the probe-reset hook. Entries
 * compact by swap-with-last plus full rebind (trampolines close over
 * the slot); the materialized view rebuilds lazily on next lookup. */
void invfs_codec_unload_volume(const void *vol)
{
    size_t i, n;
    if (!vol)
        return;
    n = 0;
    for (i = 0; i < packs_n; i++) {
        if (packs[i].origin_vol == vol) {
            pack_entry_free(&packs[i]);
            continue;
        }
        if (n != i)
            packs[n] = packs[i];
        n++;
    }
    if (n != packs_n) {
        packs_n = n;
        pack_rebind_all();
        packs_dirty = 1;
        memset(probe_cache, 0, sizeof probe_cache);
    }
    /* Reserve 3/3: drop this volume's host staging with its section. */
    stage_untag(vol);
}

/* Reserve/bootstrap: the volume's own section first, then the shared
 * view. A volume pack never shadows another volume's entry -- only
 * its own volume's lookups see it preferred. */
const invfs_codec *invfs_codec_by_algo_vol(const void *vol, uint32_t algo)
{
    size_t i;
    packs_ensure();
    if (vol) {
        for (i = 0; i < packs_n; i++)
            if (packs[i].origin_vol == vol && packs[i].pub.algo == algo)
                return &packs[i].pub;
    }
    return invfs_codec_by_algo(algo);
}

static void pack_scan_dir(const char *dir, size_t dlen)
{
    char path[4096];
    DIR *d;
    struct dirent *de;

    while (dlen && dir[dlen - 1] == '/') dlen--;   /* tolerate "dir/" */
    if (!dlen || dlen + 1 >= sizeof path) return;
    memcpy(path, dir, dlen);
    path[dlen] = '\0';
    d = opendir(path);
    if (!d) return;
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        size_t plen = dlen + 1 + nl;
        struct pack_manifest m;

        if (nl < 11 || strcmp(de->d_name + nl - 10, ".codecpack") != 0)
            continue;
        if (plen + 9 + 1 > sizeof path) continue;    /* "/manifest" */
        path[dlen] = '/';
        memcpy(path + dlen + 1, de->d_name, nl + 1); /* path = pack dir */
        memcpy(path + plen, "/manifest", 10);
        if (!parse_manifest(path, &m)) continue;
        path[plen] = '\0';
        pack_register_inner(path, &m, NULL);
    }
    closedir(d);
}

/* Resolve the codecpack registry.
 *
 * INVFS_CODECPACKS, when set and non-empty, IS the registry: its colon-dirs
 * are scanned and nothing else is. That is the only way a caller can say
 * "use exactly these packs" -- and "point discovery at an empty dir and get
 * no codecpacks" is what a control arm is. It used to be scanned and THEN
 * the system dir, so the empty dir disabled only the FIRST search dir and
 * /usr/lib/invfs/codecpacks still contributed whatever the host had
 * installed (on this box: ext4fs, qcow2, rawdisk). Every "pack absent"
 * control that set only INVFS_CODECPACKS was measuring the host, not the
 * product; test-ext4fs.sh's ratio demo compared the pack lane against a
 * control arm that ran the same lane.
 *
 * INVFS_CODECPACKS unset or empty keeps the deployed default: the system
 * dir /usr/lib/invfs/codecpacks, which the packaging installs root-owned.
 * INVFS_CODECPACKS_SYS=0 skips it in that case, which is what a hermetic
 * unit run wants (WP101) and what it now gets without also having to blank
 * INVFS_CODECPACKS.
 *
 * PATH probing of tool names is probe()-time business, not registration. */
static void pack_scan_all(void)
{
    static const char sysdir[] = "/usr/lib/invfs/codecpacks";
    const char *p = getenv("INVFS_CODECPACKS");
    const char *nosys = getenv("INVFS_CODECPACKS_SYS");

    if (p && *p) {
        while (*p) {
            const char *colon = strchr(p, ':');
            size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
            if (dlen) pack_scan_dir(p, dlen);
            if (!colon) break;
            p = colon + 1;
        }
        return;                      /* explicit registry: authoritative */
    }
    if (nosys && strcmp(nosys, "0") == 0)
        return;
    pack_scan_dir(sysdir, sizeof sysdir - 1);
}

/* lazy init: scan once, then build the materialized registry view --
 * static entries, packs, PPMD last (registry order = sniff priority:
 * specific magics first, text LAST, packs sit between). WP16e: a builtin
 * EXTERNAL entry a loaded pack overrides drops out of the view -- the
 * pack's entry (in the packs section) is the ONLY entry for that algo. */
static void packs_ensure(void)
{
    size_t i, j, n;

    if (!packs_built) {
        packs_built = 1;
        pack_scan_all();
    }
    if (!packs_dirty)
        return;
    packs_dirty = 0;
    n = 0;
    for (i = 0; i < REGISTRY_N; i++) {
        int taken = 0;
        if (registry[i].algo == INVFS_ALGO_PPMD) continue;
        for (j = 0; j < packs_n; j++)
            if (!packs[j].origin_vol && packs[j].overrides_builtin &&
                packs[j].pub.algo == registry[i].algo) {
                taken = 1;
                break;
            }
        if (taken) continue;
        g_all[n] = registry[i];
        g_all_vol[n] = NULL;
        n++;
    }
    /* Host packs first (registration order), then volume sections: the
     * shared by_algo() answer stays host-deterministic no matter when
     * a volume was opened. */
    for (i = 0; i < packs_n; i++) {
        if (packs[i].origin_vol) continue;
        g_all[n] = packs[i].pub;
        g_all_vol[n] = NULL;
        n++;
    }
    for (i = 0; i < packs_n; i++) {
        if (!packs[i].origin_vol) continue;
        g_all[n] = packs[i].pub;
        g_all_vol[n] = packs[i].origin_vol;
        n++;
    }
    for (i = 0; i < REGISTRY_N; i++)
        if (registry[i].algo == INVFS_ALGO_PPMD) {
            g_all[n] = registry[i];
            g_all_vol[n] = NULL;
            n++;
        }
    g_all_n = n;
}

/* Reserve 3/3 bodies (see forwards above). */
static pack_entry *pack_record_for(const invfs_codec *c)
{
    size_t i;
    if (!c) return NULL;
    packs_ensure();
    if (c >= g_all && c < g_all + g_all_n) {
        const void *ov = g_all_vol[c - g_all];
        for (i = 0; i < packs_n; i++)
            if (packs[i].pub.algo == c->algo &&
                packs[i].origin_vol == ov)
                return &packs[i];
        return NULL;
    }
    for (i = 0; i < packs_n; i++)
        if (&packs[i].pub == c) return &packs[i];
    return NULL;
}

static const char *pack_host_dir(pack_entry *p)
{
    char staged[4096];
    if (!p || !p->dir) return NULL;
    if (!p->origin_vol) return p->dir;
    if (p->mat_ok == 1) return p->mat_dir;
    if (invfs_no_autopack()) return NULL;
    if (invfs_vol_pack_materialize(p->origin_vol, p->dir,
                                   staged, sizeof staged) != 0)
        return NULL;
    if (snprintf(p->mat_dir, sizeof p->mat_dir, "%s", staged) >=
        (int)sizeof p->mat_dir)
        return NULL;
    p->mat_ok = 1;
    stage_tag(p->origin_vol);
    return p->mat_dir;
}

const char *invfs_codec_pack_host_dir(const invfs_codec *c)
{
    pack_entry *p = pack_record_for(c);
    if (!p) return NULL;
    return pack_host_dir(p);
}

const invfs_pack_def *invfs_codec_pack_def(const invfs_codec *c)
{
    pack_entry *p = pack_record_for(c);
    return p ? &p->def : NULL;
}

/* Does this pack carry any claim rule -- a sniff.magic or a sniff.ext?
 *
 * The distinction matters to the sweep's try-last pass (WP103, vol_sweep.c),
 * documented as a SECOND chance for a pack that "scored 0 ... (no
 * sniff.magic AND no sniff.ext -- a `family = code` general codec)". A pack
 * that HAS claim rules and scored 0 did not fail to recognise the file; it
 * recognised it and said no. Offering that pack a trial run anyway lets a
 * non-claim reach a whole-file encode, and -- when it declines -- lets its
 * GENERIC_GUARD stamp overwrite the one a lane that DID claim the file
 * already wrote. That stamp is terminal for the file until the stamped
 * codec's generation grows (the sweep's class policy), so the overwrite
 * does not merely mislabel: it freezes the file out of every later sweep.
 *
 * Returns 1 for a pack with at least one magic rule or a non-empty
 * extension list; 0 for a claim-free general codec, and 0 for a builtin
 * entry (there is no pack record to hold rules). */
int invfs_codec_pack_claims(const invfs_codec *c)
{
    pack_entry *p = pack_record_for(c);
    if (!p) return 0;
    if (p->n_magic > 0) return 1;
    if (p->exts && p->exts[0]) return 1;
    return 0;
}

const invfs_codec *invfs_codec_by_algo(uint32_t algo)
{
    size_t i;
    packs_ensure();
    for (i = 0; i < g_all_n; i++)
        if (g_all[i].algo == algo) return &g_all[i];
    return NULL;
}

const invfs_codec *invfs_codec_all(size_t *count)
{
    packs_ensure();
    if (count) *count = g_all_n;
    return g_all;
}

uint16_t invfs_registry_generation(void)
{
    uint16_t g = 0;
    size_t i;
    packs_ensure();
    for (i = 0; i < g_all_n; i++)
        if (g_all[i].generation > g) g = g_all[i].generation;
    return g;
}

/* ---------------- WP16b/WP19: codec profiles ---------------- */

static const char *const profile_names[7] = {
    "fast", "balanced", "dense", "archive", "faster", "fastest", "turbo"
};

int invfs_profile_parse(const char *s)
{
    int i;
    if (!s) return -1;
    for (i = 0; i < 7; i++)
        if (strcmp(s, profile_names[i]) == 0) return i;
    return -1;
}

const char *invfs_profile_name(int p)
{
    if (p < 0 || p > 6) return "balanced";
    return profile_names[p];
}

/* Generic-sweep ZSTD level per profile. balanced = 19 is the historical
 * default: an unset INVFS_PROFILE must reproduce the bytes existing volumes
 * were swept with. dense/archive share 19 since WP19 (22 is dropped);
 * archive reserves the slot for a future LZMA2 backend swap, not a higher
 * zstd level. Meaningless for fastest/turbo (no ZSTD involved) -- they get
 * the default so an out-of-place call still answers sanely. */
int invfs_profile_zstd_level(int p)
{
    switch (p) {
    case INVFS_PROFILE_FAST:   return 6;
    case INVFS_PROFILE_FASTER: return 3;
    default:                   return 19;
    }
}

/* Admission-time codec substitution for the meta-profiles (WP19): the
 * generic sweep and the RAW write path ask "which floor codec" here; the
 * registry itself never learns the meta-profiles exist. */
int invfs_profile_generic_algo(int p)
{
    switch (p) {
    case INVFS_PROFILE_FASTEST: return INVFS_ALGO_LZ4;
    case INVFS_PROFILE_TURBO:   return INVFS_ALGO_NONE;
    default:                    return INVFS_ALGO_ZSTD;
    }
}
