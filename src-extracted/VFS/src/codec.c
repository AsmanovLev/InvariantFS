/*
 * codec.c — codec registry (WP10 §1, §10).
 *
 * v1 formalizes SELECTION only: NONE/LZ4/ZSTD wrap exactly the calls
 * volume.c already makes, PPMD wraps ppmd_codec.c, and the container /
 * external recipes (zip/tarr/gzr/pngr/flacr/pmp/jxl/ape/wv) register just
 * sniff + probe — their transcode logic stays in volume.c for now.
 */
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

struct pack_manifest {
    char encode[512];
    char decode[512];
    int  has_encode;
    int  has_decode;
};

static void str_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Minimal key=value parser: trims whitespace, ignores blank lines and
 * #-comments, skips keys probe does not need (name/algo/caps/dec_mem/
 * generation/sniff.* matter when REGISTERING a pack, not when probing). */
static int parse_manifest(const char *path, struct pack_manifest *m)
{
    FILE *f = fopen(path, "r");
    char line[1024];

    if (!f) return 0;
    memset(m, 0, sizeof *m);
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

/* WP12(e): probe() results memoized per registry entry, keyed by algo.
 * Filled lazily on the first probe of each codec. The sweep is
 * single-threaded today, so a plain unsynchronized cache is fine --
 * revisit if probing ever goes concurrent. */
#define PROBE_CACHE_SLOTS 16   /* INVFS_ALGO_* run 0..12 (invarifs.h, codec.h) */
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

/* test hook: probe results are memoized; fixtures that mutate
 * INVFS_CODECPACKS/PATH between scenarios must reset first. */
void invfs_codec_probe_reset(void)
{
    memset(probe_cache, 0, sizeof probe_cache);
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

const invfs_codec *invfs_codec_by_algo(uint32_t algo)
{
    size_t i;
    for (i = 0; i < REGISTRY_N; i++)
        if (registry[i].algo == algo) return &registry[i];
    return NULL;
}

const invfs_codec *invfs_codec_all(size_t *count)
{
    if (count) *count = REGISTRY_N;
    return registry;
}

uint16_t invfs_registry_generation(void)
{
    uint16_t g = 0;
    size_t i;
    for (i = 0; i < REGISTRY_N; i++)
        if (registry[i].generation > g) g = registry[i].generation;
    return g;
}
