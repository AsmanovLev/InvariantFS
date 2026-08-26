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
#include <unistd.h>

#include "codec.h"
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
static int parse_manifest(const char *path, struct pack_manifest *m)
{
    FILE *f = fopen(path, "r");
    char line[1024];

    if (!f) return 0;
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
        } else if (strcmp(s, "name") == 0) {
            str_copy(m->name, sizeof m->name, val);
        } else if (strcmp(s, "algo") == 0) {
            m->algo = strtol(val, NULL, 0);
        } else if (strcmp(s, "pack_version") == 0) {
            m->pack_version = strtol(val, NULL, 0);
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
    fclose(f);
    return 1;
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
 * Static slots, cap INVFS_PACK_MAX; all manifest strings are strdup'd.
 * Loaded lazily on the first registry access and memoized; the sweep is
 * single-threaded, same discipline as the probe cache. Packs sit BEFORE
 * the PPMD text heuristic in registry order (specific magics first, text
 * LAST) and after the builtin entries.
 */
#define INVFS_PACK_MAX 8

typedef struct {
    invfs_codec      pub;       /* what the registry exposes */
    invfs_pack_def   def;       /* exec record for the volume.c hooks */
    char            *dir;       /* pack directory ({pack} substitution) */
    char            *name, *encode, *decode, *estimate, *requires, *exts;
    pack_magic_rule  magic[PACK_MAX_MAGIC];
    size_t           n_magic;
    int              probed;    /* memoized availability probe */
    int              avail;
} pack_entry;

static pack_entry packs[INVFS_PACK_MAX];
static size_t     packs_n;
static int        packs_built;      /* 1 = dirs scanned + registry materialized */
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

/* a `requires` tool must resolve the way volume.c's tool_resolve will look
 * for it: $INVFS_TOOLS/<name> -> /usr/lib/invfs/tools/<name> -> PATH */
static int pack_tool_resolvable(const char *tool)
{
    static const char tooldir[] = "/usr/lib/invfs/tools";
    const char *dir = getenv("INVFS_TOOLS");

    if (dir && *dir && dir_has_tool(dir, strlen(dir), tool)) return 1;
    if (dir_has_tool(tooldir, sizeof tooldir - 1, tool)) return 1;
    return on_path(tool);
}

/* available iff every argv's tool resolves (encode/decode, estimate when
 * present) and every `requires` entry does */
static int pack_probe_impl(pack_entry *p)
{
    const char *r;

    if (p->probed) return p->avail;
    p->probed = 1;
    p->avail = 0;
    if (!manifest_tool_ok(p->dir, p->encode) ||
        !manifest_tool_ok(p->dir, p->decode))
        return 0;
    if (p->estimate && !manifest_tool_ok(p->dir, p->estimate))
        return 0;
    r = p->requires;
    while (r && *r) {
        const char *comma = strchr(r, ',');
        size_t tl = comma ? (size_t)(comma - r) : strlen(r);
        char tok[64];

        if (!tl || tl >= sizeof tok) return 0;
        memcpy(tok, r, tl);
        tok[tl] = '\0';
        if (!pack_tool_resolvable(tok)) return 0;
        if (!comma) break;
        r = comma + 1;
    }
    p->avail = 1;
    return 1;
}

/* Trampoline plumbing: spool the input buffer to a scratch file, run the
 * manifest argv through the volume.c exec hook, slurp the output file.
 * Same tmpfs-first roots as volume.c's tool_tmpdir. */
static int pack_tmpdir(char *dir, size_t cap)
{
    static const char *roots[] = { "/dev/shm", "/tmp" };
    size_t r;

    for (r = 0; r < sizeof roots / sizeof roots[0]; r++) {
        int n = snprintf(dir, cap, "%s/invfs-pack-XXXXXX", roots[r]);
        if (n > 0 && (size_t)n < cap && mkdtemp(dir) != NULL)
            return 0;
    }
    return -1;
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

    if (pack_tmpdir(dir, sizeof dir) != 0) return -1;
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
    memset(p, 0, sizeof *p);
}

/* WP12(e): probe() results memoized per registry entry, keyed by algo.
 * Filled lazily on the first probe of each codec. The sweep is
 * single-threaded today, so a plain unsynchronized cache is fine --
 * revisit if probing ever goes concurrent. */
#define PROBE_CACHE_SLOTS 16   /* INVFS_ALGO_* registry ids run 0..13; 14
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
    for (i = 0; i < packs_n; i++) pack_entry_free(&packs[i]);
    packs_n = 0;
    packs_built = 0;
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
    { INVFS_ALGO_JXL, "jxl",
      INVFS_CODEC_CAP_EXTERNAL, 0, 1,
      sniff_jxl, probe_jxl, NULL, NULL },
    { INVFS_ALGO_APE, "ape",
      INVFS_CODEC_CAP_EXTERNAL, 0, 1,
      sniff_ape, probe_ape, NULL, NULL },
    { INVFS_ALGO_WV, "wv",
      INVFS_CODEC_CAP_EXTERNAL, 0, 1,
      sniff_wv, probe_wv, NULL, NULL },
    { INVFS_ALGO_PPMD, "ppmd",
      INVFS_CODEC_CAP_BATCHED, 68ull << 20, 0,
      sniff_ppmd, NULL, invfs_ppmd_encode, invfs_ppmd_decode },
};

#define REGISTRY_N (sizeof registry / sizeof registry[0])

/* materialized registry: static entries + loaded packs, PPMD last */
static invfs_codec g_all[REGISTRY_N + INVFS_PACK_MAX];

/* ---------------- pack registration + materialized view (WP13) ---------- */

static void pack_register(const char *dir, const struct pack_manifest *m)
{
    pack_entry *p;
    size_t i;

    if (packs_n >= INVFS_PACK_MAX) return;
    if (!m->name[0] || m->algo < 0 || !m->has_encode || !m->has_decode)
        return;
    if ((unsigned long)m->algo >= 64) return;   /* AST algo field is 6 bits */
    for (i = 0; i < REGISTRY_N; i++)
        if (registry[i].algo == (uint32_t)m->algo) return;   /* algo taken */
    for (i = 0; i < packs_n; i++) {
        if (packs[i].pub.algo == (uint32_t)m->algo) return;
        if (strcmp(packs[i].name, m->name) == 0) return;     /* seen already */
    }

    p = &packs[packs_n];
    memset(p, 0, sizeof *p);
    p->dir      = pack_strdup(dir);
    p->name     = pack_strdup(m->name);
    p->encode   = pack_strdup(m->encode);
    p->decode   = pack_strdup(m->decode);
    p->estimate = m->has_estimate ? pack_strdup(m->estimate) : NULL;
    p->requires = m->requires[0] ? pack_strdup(m->requires) : NULL;
    p->exts     = m->exts[0] ? pack_strdup(m->exts) : NULL;
    if (!p->dir || !p->name || !p->encode || !p->decode ||
        (m->has_estimate && !p->estimate) ||
        (m->requires[0] && !p->requires) || (m->exts[0] && !p->exts)) {
        pack_entry_free(p);
        return;
    }
    memcpy(p->magic, m->magic, sizeof p->magic);
    p->n_magic = m->n_magic;

    p->pub.algo          = (uint32_t)m->algo;
    p->pub.name          = p->name;
    p->pub.caps          = m->caps;
    p->pub.dec_mem_bytes = m->dec_mem;
    p->pub.generation    = (uint16_t)m->generation;
    p->pub.sniff         = pack_fns[packs_n].sniff;
    p->pub.probe         = pack_fns[packs_n].probe;
    p->pub.encode        = pack_fns[packs_n].encode;
    p->pub.decode        = pack_fns[packs_n].decode;

    p->def.dir      = p->dir;
    p->def.encode   = p->encode;
    p->def.decode   = p->decode;
    p->def.estimate = p->estimate;
    p->def.requires = p->requires;
    packs_n++;
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
        pack_register(path, &m);
    }
    closedir(d);
}

/* scan $INVFS_CODECPACKS colon-dirs, then the system dir (PATH probing of
 * tool names is probe()-time business, not registration) */
static void pack_scan_all(void)
{
    static const char sysdir[] = "/usr/lib/invfs/codecpacks";
    const char *p = getenv("INVFS_CODECPACKS");

    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        if (dlen) pack_scan_dir(p, dlen);
        if (!colon) break;
        p = colon + 1;
    }
    pack_scan_dir(sysdir, sizeof sysdir - 1);
}

/* lazy init: scan once, then build the materialized registry view --
 * static entries, packs, PPMD last (registry order = sniff priority:
 * specific magics first, text LAST, packs sit between) */
static void packs_ensure(void)
{
    size_t i, n;

    if (packs_built) return;
    packs_built = 1;
    pack_scan_all();
    n = 0;
    for (i = 0; i < REGISTRY_N; i++)
        if (registry[i].algo != INVFS_ALGO_PPMD) g_all[n++] = registry[i];
    for (i = 0; i < packs_n; i++) g_all[n++] = packs[i].pub;
    for (i = 0; i < REGISTRY_N; i++)
        if (registry[i].algo == INVFS_ALGO_PPMD) g_all[n++] = registry[i];
    g_all_n = n;
}

const invfs_pack_def *invfs_codec_pack_def(const invfs_codec *c)
{
    size_t i;

    if (!c) return NULL;
    packs_ensure();
    for (i = 0; i < packs_n; i++)
        if (packs[i].pub.algo == c->algo) return &packs[i].def;
    return NULL;
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
