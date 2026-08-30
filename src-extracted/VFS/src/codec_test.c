/*
 * codec_test.c — exercise the codec registry (WP10).
 *
 * Standalone, NOT wired into the Makefile; build by hand:
 *   gcc -std=gnu11 -O2 -I src-extracted/VFS/src -o /tmp/codec_test \
 *       src-extracted/VFS/src/codec_test.c build/obj/codec.o \
 *       build/obj/ppmd8.o build/obj/ppmd8enc.o build/obj/ppmd8dec.o \
 *       build/obj/ppmd_codec.o build/obj/lz4.o \
 *       -Wl,-l:libzstd.so.1 -lz -lpthread
 */
#define _CRT_SECURE_NO_WARNINGS

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "codec.h"
#include "invarifs.h"
#include "bcj_x86.h"

/* ---- test-local implementations of the pack exec hooks (WP13) ----
 * The production hooks live in volume.c (which this test does not link);
 * codec.c's trampolines call them, so the fixture pack round-trips through
 * the REAL dynamic-registration code and a stubbed subprocess runner. */

static char *subst_token(const char *tok, const char *in, const char *out)
{
    /* whole-token substitution is enough for the fixture manifests */
    if (!strcmp(tok, "{in}"))  return strdup(in);
    if (!strcmp(tok, "{out}")) return strdup(out);
    return strdup(tok);
}

int invfs_codec_pack_exec(const invfs_codec *c, int is_encode,
                          const char *in_path, const char *out_path)
{
    const invfs_pack_def *def = invfs_codec_pack_def(c);
    const char *tmpl;
    char *argv[16];
    int argc = 0, i, rc;
    char buf[1024];
    char *save = NULL, *t;
    int st = 0;
    pid_t pid;

    if (!def) return -1;
    tmpl = is_encode ? def->encode : def->decode;
    if (!tmpl || strlen(tmpl) >= sizeof buf) return -1;
    strcpy(buf, tmpl);
    for (t = strtok_r(buf, " \t", &save); t && argc < 15;
         t = strtok_r(NULL, " \t", &save))
        argv[argc++] = subst_token(t, in_path, out_path);
    argv[argc] = NULL;
    if (!argc) return -1;
    pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        execvp(argv[0], argv);
        _exit(127);
    }
    for (i = 0; i < argc; i++) free(argv[i]);
    if (pid < 0) return -1;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return rc;
}

int invfs_codec_pack_estimate(const invfs_codec *c, const char *in_path,
                              uint64_t *out_bytes)
{
    const invfs_pack_def *def = invfs_codec_pack_def(c);
    (void)in_path; (void)out_bytes;
    if (!def || !def->estimate) return -1;   /* the fixture has none */
    return -1;
}


static int failures = 0;
static int checks = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* ---------------- registry shape ---------------- */

static void test_registry(void)
{
    size_t n = 0, i;
    const invfs_codec *all = invfs_codec_all(&n);
    const invfs_codec *c;
    int roundtrip;

    ok(all != NULL && n == 14, "registry holds the 14 static entries");
    ok(invfs_registry_generation() >= 1, "registry generation >= 1");

    c = invfs_codec_by_algo(INVFS_ALGO_NONE);
    ok(c && strcmp(c->name, "none") == 0, "lookup NONE");
    ok(c && (c->caps & INVFS_CODEC_CAP_SEEK) && c->dec_mem_bytes == 0,
       "NONE: seekable, zero decode memory");
    c = invfs_codec_by_algo(INVFS_ALGO_LZ4);
    ok(c && (c->caps & INVFS_CODEC_CAP_SEEK) &&
       c->dec_mem_bytes == (64ull << 10), "LZ4: seekable, 64 KiB decode window");
    c = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
    ok(c && (c->caps & INVFS_CODEC_CAP_SEEK) &&
       c->dec_mem_bytes == (8ull << 20) + (64ull << 10),
       "ZSTD: seekable, 8 MiB window + unit");
    c = invfs_codec_by_algo(INVFS_ALGO_PPMD);
    ok(c && (c->caps & INVFS_CODEC_CAP_BATCHED) &&
       !(c->caps & INVFS_CODEC_CAP_SEEK), "PPMD: batched, NOT seekable");
    ok(c && c->dec_mem_bytes == (68ull << 20), "PPMD: 64 MiB model + 4 MiB unit");

    c = invfs_codec_by_algo(INVFS_ALGO_ZIPR);
    ok(c && (c->caps & INVFS_CODEC_CAP_CONTAINER) && c->encode == NULL &&
       c->decode == NULL && c->generation == 1 && c->probe == NULL,
       "ZIP: container, selection-only, generation 1");
    c = invfs_codec_by_algo(INVFS_ALGO_PMP);
    ok(c && (c->caps & INVFS_CODEC_CAP_EXTERNAL) && c->probe != NULL &&
       c->encode == NULL && c->generation == 1,
       "PMP: external, probed, generation 1");
    ok(invfs_codec_by_algo(63) == NULL, "unknown algo -> NULL");

    /* order = sniff priority; by_algo must return the table entries */
    ok(all[n - 1].algo == INVFS_ALGO_PPMD, "text heuristic is LAST");
    roundtrip = 1;
    for (i = 0; i < n; i++)
        if (invfs_codec_by_algo(all[i].algo) != &all[i]) roundtrip = 0;
    ok(roundtrip, "by_algo round-trips every table entry");
}

/* ---------------- sniff ---------------- */

static void test_sniff(void)
{
    const invfs_codec *pngr  = invfs_codec_by_algo(INVFS_ALGO_PNGR);
    const invfs_codec *pmp   = invfs_codec_by_algo(INVFS_ALGO_PMP);
    const invfs_codec *ppmd  = invfs_codec_by_algo(INVFS_ALGO_PPMD);
    const invfs_codec *zip   = invfs_codec_by_algo(INVFS_ALGO_ZIPR);
    const invfs_codec *tarr  = invfs_codec_by_algo(INVFS_ALGO_TARR);
    const invfs_codec *gzr   = invfs_codec_by_algo(INVFS_ALGO_GZR);
    const invfs_codec *flacr = invfs_codec_by_algo(INVFS_ALGO_FLACR);
    const invfs_codec *jxl   = invfs_codec_by_algo(INVFS_ALGO_JXL);
    const invfs_codec *ape   = invfs_codec_by_algo(INVFS_ALGO_APE);
    const invfs_codec *wv    = invfs_codec_by_algo(INVFS_ALGO_WV);

    static const uint8_t png[8]  = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    static const uint8_t zipm[4] = { 'P', 'K', 0x03, 0x04 };
    static const uint8_t gzm[2]  = { 0x1F, 0x8B };
    static const uint8_t mp3id3[6] = { 'I', 'D', '3', 4, 0, 0 };
    static const uint8_t mp3frame[2] = { 0xFF, 0xFB };
    static const uint8_t jpeg[2]   = { 0xFF, 0xD8 };
    static const uint8_t jpegsoi[3] = { 0xFF, 0xD8, 0xFF };
    static const uint8_t jxlc[2]   = { 0xFF, 0x0A };
    static const uint8_t jxlb[8]   = { 0, 0, 0, 0x0C, 'J', 'X', 'L', ' ' };
    uint8_t tar[512];
    uint8_t bin[256];
    size_t i;
    static const char text[] =
        "the quick brown fox jumps over the lazy dog\n"
        "pack my box with five dozen liquor jugs\n";

    memset(tar, 0, sizeof tar);
    memcpy(tar + 257, "ustar", 5);
    for (i = 0; i < sizeof bin; i++) bin[i] = (uint8_t)i;   /* has NULs */

    ok(pngr->sniff(png, sizeof png, "x.png") == 100, "PNG magic -> 100");
    ok(pngr->sniff(png, 4, "x.png") == 0, "short buffer cannot match PNG magic");
    ok(pmp->sniff(png, sizeof png, "x.png") == 0, "PNG is not MP3");
    ok(zip->sniff(zipm, sizeof zipm, "x.zip") == 100, "ZIP magic -> 100");
    ok(zip->sniff(png, sizeof png, "x.png") == 0, "PNG is not ZIP");
    ok(tarr->sniff(tar, sizeof tar, "x.tar") == 100, "ustar@257 -> 100");
    ok(tarr->sniff(tar, 260, "x.tar") == 0, "ustar needs 262 bytes of head");
    ok(gzr->sniff(gzm, sizeof gzm, "x.gz") == 100, "GZIP magic -> 100");
    ok(pmp->sniff(mp3id3, sizeof mp3id3, "x.mp3") == 100, "ID3 -> MP3");
    ok(pmp->sniff(mp3frame, sizeof mp3frame, "x.mp3") == 100, "frame sync -> MP3");
    ok(pmp->sniff(jpeg, sizeof jpeg, "x.jpg") == 0, "JPEG is not MP3");
    ok(jxl->sniff(jxlc, sizeof jxlc, "x.jxl") == 100, "JXL codestream -> 100");
    ok(jxl->sniff(jxlb, sizeof jxlb, "x.jxl") == 100, "JXL box -> 100");
    ok(jxl->sniff(jpegsoi, sizeof jpegsoi, "x.jpg") == 100,
       "JPEG SOI -> JXL (JPEG is the codec's input format)");
    ok(jxl->sniff(jpeg, sizeof jpeg, "x.jpg") == 0,
       "2-byte SOI prefix is not enough");
    ok(jxl->encode == NULL && jxl->decode == NULL &&
       (jxl->caps & INVFS_CODEC_CAP_EXTERNAL) &&
       (jxl->caps & INVFS_CODEC_CAP_PACKONLY),
       "jxl placeholder (WP16e): EXTERNAL|PACKONLY, no transcode of its own");
    ok(ape->sniff((const uint8_t *)"MAC ", 4, "x.ape") == 100, "APE magic -> 100");
    ok(wv->sniff((const uint8_t *)"wvpk", 4, "x.wv") == 100, "WavPack magic -> 100");
    ok(flacr->sniff((const uint8_t *)"fLaC", 4, "x.flac") == 100, "FLAC magic -> 100");

    ok(ppmd->sniff(NULL, 0, "main.c") == 50, ".c filename -> text (50)");
    ok(ppmd->sniff((const uint8_t *)text, sizeof text - 1, "noext") == 50,
       "ascii content, no extension -> text (50)");
    ok(ppmd->sniff(bin, sizeof bin, "noext") == 0, "binary with NULs -> not text");
    ok(ppmd->sniff(bin, sizeof bin, "weird.c") == 50, "known extension beats binary content");
    ok(ppmd->sniff(png, sizeof png, "x.png") == 0, "PNG is not text");
}

static void test_text_family(void)
{
    uint8_t bin[64];
    size_t i;
    static const char text[] = "plain ascii prose, nothing fancy\n";

    for (i = 0; i < sizeof bin; i++) bin[i] = (uint8_t)(i * 4);  /* NULs inside */

    ok(invfs_text_family("a.c", NULL, 0) == INVFS_TEXT_FAMILY_CODE_C, "fam .c");
    ok(invfs_text_family("a.H", NULL, 0) == INVFS_TEXT_FAMILY_CODE_C, "fam .H (case)");
    ok(invfs_text_family("a.cpp", NULL, 0) == INVFS_TEXT_FAMILY_CODE_C, "fam .cpp");
    ok(invfs_text_family("a.py", NULL, 0) == INVFS_TEXT_FAMILY_CODE_PY, "fam .py");
    ok(invfs_text_family("a.JS", NULL, 0) == INVFS_TEXT_FAMILY_CODE_JS, "fam .JS (case)");
    ok(invfs_text_family("a.mjs", NULL, 0) == INVFS_TEXT_FAMILY_CODE_JS, "fam .mjs");
    ok(invfs_text_family("a.java", NULL, 0) == INVFS_TEXT_FAMILY_CODE_JAVA, "fam .java");
    ok(invfs_text_family("a.rs", NULL, 0) == INVFS_TEXT_FAMILY_CODE_RS, "fam .rs");
    ok(invfs_text_family("a.go", NULL, 0) == INVFS_TEXT_FAMILY_CODE_GO, "fam .go");
    ok(invfs_text_family("a.yaml", NULL, 0) == INVFS_TEXT_FAMILY_DATA, "fam .yaml");
    ok(invfs_text_family("a.csv", NULL, 0) == INVFS_TEXT_FAMILY_DATA, "fam .csv");
    ok(invfs_text_family("a.txt", NULL, 0) == INVFS_TEXT_FAMILY_PROSE, "fam .txt");
    ok(invfs_text_family("a.md", NULL, 0) == INVFS_TEXT_FAMILY_PROSE, "fam .md");
    ok(invfs_text_family("a.html", NULL, 0) == INVFS_TEXT_FAMILY_WEB, "fam .html");
    ok(invfs_text_family("a.sh", NULL, 0) == INVFS_TEXT_FAMILY_SHELL, "fam .sh");
    ok(invfs_text_family("dir/sub/x.hpp", NULL, 0) == INVFS_TEXT_FAMILY_CODE_C,
       "family taken from basename");
    ok(invfs_text_family("noext", (const uint8_t *)text, sizeof text - 1) ==
       INVFS_TEXT_FAMILY_CONTENT, "content-sniffed text -> CONTENT family");
    ok(invfs_text_family(NULL, (const uint8_t *)text, sizeof text - 1) ==
       INVFS_TEXT_FAMILY_CONTENT, "NULL name + text head -> CONTENT family");
    ok(invfs_text_family("noext", bin, sizeof bin) == 0, "binary -> not text");
    ok(invfs_text_family("a.unknownext", bin, sizeof bin) == 0,
       "unknown ext + binary -> not text");
}

/* ---------------- WP14a: binary classifier + BCJ round-trip ---------------- */

static void test_binary_family(void)
{
    uint8_t elf[4096], pe[4096], bin[4096];
    size_t i;
    static const char text[] =
        "the quick brown fox jumps over the lazy dog\n";

    memset(elf, 0, sizeof elf);
    elf[0] = 0x7F; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';

    elf[18] = 62;  elf[19] = 0;
    ok(invfs_binary_family(elf, sizeof elf, "a.out") == INVFS_BIN_FAMILY_ELF_X64,
       "ELF e_machine=62 (x86-64) -> family 20");
    elf[18] = 3;
    ok(invfs_binary_family(elf, sizeof elf, "a.out") == INVFS_BIN_FAMILY_ELF_X86,
       "ELF e_machine=3 (i386) -> family 21");
    elf[18] = 183;
    ok(invfs_binary_family(elf, sizeof elf, "a.out") == INVFS_BIN_FAMILY_ELF_A64,
       "ELF e_machine=183 (aarch64) -> family 22");
    elf[18] = 40;
    ok(invfs_binary_family(elf, sizeof elf, "a.out") == INVFS_BIN_FAMILY_ELF_OTHER,
       "ELF e_machine=40 (arm) -> family 23");
    elf[18] = 0xF3; elf[19] = 0;   /* 243 = EM_RISCV */
    ok(invfs_binary_family(elf, sizeof elf, "a.out") == INVFS_BIN_FAMILY_ELF_OTHER,
       "ELF e_machine=243 (riscv) -> family 23");

    /* the >=4KB gate: same magic, smaller file -> not batchable */
    elf[18] = 62;
    ok(invfs_binary_family(elf, 4095, "a.out") == 0, "ELF below 4KB -> 0");
    ok(invfs_binary_family(elf, 20, "a.out") == 0, "ELF 20-byte head -> 0");
    ok(invfs_binary_family(elf, 4096, "a.out") == INVFS_BIN_FAMILY_ELF_X64,
       "ELF exactly 4KB -> family 20");

    /* PE: MZ + e_lfanew -> "PE\0\0" */
    memset(pe, 0, sizeof pe);
    pe[0] = 'M'; pe[1] = 'Z';
    pe[0x3C] = 0x80;
    pe[0x80] = 'P'; pe[0x81] = 'E';
    ok(invfs_binary_family(pe, sizeof pe, "x.exe") == INVFS_BIN_FAMILY_PE,
       "MZ + PE\\0\\0 @0x80 -> family 24");
    pe[0x80] = 'X';
    ok(invfs_binary_family(pe, sizeof pe, "x.exe") == 0,
       "MZ without PE signature -> 0");
    pe[0x80] = 'P';
    pe[0x3C] = 0xFF; pe[0x3D] = 0xFF; pe[0x3E] = 0xFF; pe[0x3F] = 0x7F;
    ok(invfs_binary_family(pe, sizeof pe, "x.exe") == 0,
       "e_lfanew past the head window -> 0 (no guessing)");

    /* Mach-O magics */
    memset(bin, 0, sizeof bin);
    bin[0] = 0xFE; bin[1] = 0xED; bin[2] = 0xFA; bin[3] = 0xCF;
    ok(invfs_binary_family(bin, sizeof bin, "x") == INVFS_BIN_FAMILY_MACHO,
       "Mach-O 64-bit -> family 25");
    bin[0] = 0xFE; bin[1] = 0xED; bin[2] = 0xFA; bin[3] = 0xCE;
    ok(invfs_binary_family(bin, sizeof bin, "x") == INVFS_BIN_FAMILY_MACHO,
       "Mach-O 32-bit -> family 25");
    bin[0] = 0xCE; bin[1] = 0xFA; bin[2] = 0xED; bin[3] = 0xFE;
    ok(invfs_binary_family(bin, sizeof bin, "x") == INVFS_BIN_FAMILY_MACHO,
       "Mach-O CIGAM -> family 25");
    bin[0] = 0xCA; bin[1] = 0xFE; bin[2] = 0xBA; bin[3] = 0xBE;
    ok(invfs_binary_family(bin, sizeof bin, "x") == INVFS_BIN_FAMILY_MACHO,
       "Mach-O fat (CAFEBABE) -> family 25");

    /* negatives */
    for (i = 0; i < sizeof bin; i++) bin[i] = (uint8_t)(i * 31u + 7u);
    ok(invfs_binary_family(bin, sizeof bin, "x.bin") == 0,
       "random binary -> 0");
    {
        uint8_t big[4096];
        memset(big, 'x', sizeof big);
        memcpy(big, text, sizeof text - 1);
        ok(invfs_binary_family(big, sizeof big, "notes.txt") == 0,
           "4KB of prose -> 0");
    }
    {
        /* a >=4KB shebang script: binary classifier must refuse it (the
         * text classifier claims it first on the real pipeline) */
        uint8_t sh[4096];
        memset(sh, '\n', sizeof sh);
        memcpy(sh, "#!/bin/sh\nexit 0", 16);
        ok(invfs_binary_family(sh, sizeof sh, "x") == 0,
           "shebang script -> 0 (stays on the text path)");
        ok(invfs_text_family("x.sh", sh, sizeof sh) == INVFS_TEXT_FAMILY_SHELL,
           "shebang script IS text (.sh)");
    }
    {
        /* real ELF from the build host, if readable */
        FILE *f = fopen("/bin/true", "rb");
        if (f) {
            size_t got = fread(bin, 1, sizeof bin, f);
            fclose(f);
            if (got == sizeof bin)
                ok(invfs_binary_family(bin, got, "true") ==
                   INVFS_BIN_FAMILY_ELF_X64, "/bin/true -> ELF x86-64");
        }
    }
}

static void test_bcj(void)
{
    /* synthetic x86-ish stream: E8/E9 call+jmp sites with small rel32
     * targets (the convertible shape) inside filler */
    uint8_t code[8192], work[8192];
    size_t i;

    for (i = 0; i < sizeof code; i++)
        code[i] = (uint8_t)(i * 37u + 11u);
    for (i = 64; i + 5 < sizeof code; i += 256) {
        uint32_t rel = (uint32_t)(i * 3 + 7);
        code[i] = (i & 512) ? 0xE8 : 0xE9;   /* call / jmp */
        code[i + 1] = (uint8_t)(rel & 0xFF);
        code[i + 2] = (uint8_t)((rel >> 8) & 0xFF);
        code[i + 3] = (uint8_t)((rel >> 16) & 0xFF);
        code[i + 4] = 0;                     /* top byte 0x00: convertible */
    }

    memcpy(work, code, sizeof work);
    invfs_bcj_x86_enc(work, sizeof work);
    ok(memcmp(work, code, sizeof code) != 0,
       "BCJ enc transforms convertible call/jmp operands");
    invfs_bcj_x86_dec(work, sizeof work);
    ok(memcmp(work, code, sizeof code) == 0,
       "BCJ dec inverts enc byte-exactly (whole buffer, pc=0)");

    /* slice-local bijectivity (the WP14a read-path shape): enc a buffer,
     * dec a sub-window of it starting at a 0 mod anything offset is NOT
     * the contract -- enc and dec windows must coincide, so exercise the
     * actual contract: several independent slices, each enc+dec at pc=0 */
    {
        uint8_t a[3000], b[3000];
        for (i = 0; i < sizeof a; i++) a[i] = (uint8_t)(i * 13u + 5u);
        a[100] = 0xE8; a[101] = 0x34; a[102] = 0x12; a[103] = 0; a[104] = 0;
        memcpy(b, a, sizeof a);
        invfs_bcj_x86_enc(b, sizeof b);
        invfs_bcj_x86_dec(b, sizeof b);
        ok(memcmp(a, b, sizeof a) == 0, "BCJ slice round-trip (3KB)");
    }

    /* <5 byte buffers pass through untouched in both directions */
    {
        uint8_t tiny[4] = { 0xE8, 1, 2, 3 };
        uint8_t ref[4];
        memcpy(ref, tiny, 4);
        invfs_bcj_x86_enc(tiny, 4);
        invfs_bcj_x86_dec(tiny, 4);
        ok(memcmp(tiny, ref, 4) == 0, "BCJ: <5B buffer untouched");
    }

    /* no convertible bytes: enc is a no-op */
    {
        uint8_t plain[4096];
        for (i = 0; i < sizeof plain; i++) plain[i] = (uint8_t)(i * 7u + 3u);
        memcpy(work, plain, sizeof plain);
        invfs_bcj_x86_enc(work, sizeof plain);
        ok(memcmp(work, plain, sizeof plain) == 0,
           "BCJ: buffer without call sites unchanged");
    }
}

/* ---------------- round trips ---------------- */

static void fill_text(uint8_t *buf, size_t len)
{
    static const char *lines[] = {
        "#include <stdio.h>",
        "int main(void) { return 0; }",
        "the quick brown fox jumps over the lazy dog",
    };
    size_t pos = 0;
    unsigned lno = 0;

    while (len - pos > 128) {
        int w = snprintf((char *)buf + pos, len - pos, "%s  /* %u */\n",
                         lines[lno % 3], lno);
        if (w <= 0) break;
        pos += (size_t)w;
        lno++;
    }
    while (pos < len) buf[pos++] = '.';
}

static void test_roundtrips(void)
{
    const invfs_codec *none = invfs_codec_by_algo(INVFS_ALGO_NONE);
    const invfs_codec *lz4  = invfs_codec_by_algo(INVFS_ALGO_LZ4);
    const invfs_codec *zstd = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
    const invfs_codec *ppmd = invfs_codec_by_algo(INVFS_ALGO_PPMD);

    uint8_t small[128], enc[128], dec[128];
    size_t olen = 0, i;

    size_t raw_len = 100 * 1024;
    uint8_t *raw  = (uint8_t *)malloc(raw_len);
    uint8_t *comp = (uint8_t *)malloc(raw_len + raw_len / 2 + 4096);
    uint8_t *back = (uint8_t *)malloc(raw_len);
    size_t clen = 0, cap = raw_len + raw_len / 2 + 4096;
    int done = 0;

    if (!raw || !comp || !back) { ok(0, "out of memory"); return; }

    for (i = 0; i < sizeof small; i++) small[i] = (uint8_t)(i * 31u + 7u);

    ok(none->encode(small, sizeof small, enc, sizeof enc, &olen) == 0 &&
       olen == sizeof small && memcmp(enc, small, sizeof small) == 0,
       "NONE encode = memcpy");
    ok(none->encode(small, sizeof small, enc, sizeof small - 1, &olen) == -1,
       "NONE encode: outcap < inlen -> -1");
    ok(none->decode(enc, sizeof enc, dec, sizeof dec) == 0 &&
       memcmp(dec, small, sizeof small) == 0, "NONE decode = memcpy");
    ok(none->decode(enc, sizeof enc, dec, sizeof enc + 1) == -1,
       "NONE decode: outlen > inlen -> -1");

    fill_text(raw, raw_len);

    olen = 0;
    ok(lz4->encode(raw, raw_len, comp, cap, &olen) == 0 && olen < raw_len,
       "LZ4 compresses repetitive text");
    ok(olen > 0 && lz4->decode(comp, olen, back, raw_len) == 0 &&
       memcmp(back, raw, raw_len) == 0, "LZ4 round-trip");

    olen = 0;
    ok(zstd->encode(raw, raw_len, comp, cap, &olen) == 0 && olen < raw_len,
       "ZSTD compresses repetitive text");
    ok(olen > 0 && zstd->decode(comp, olen, back, raw_len) == 0 &&
       memcmp(back, raw, raw_len) == 0, "ZSTD round-trip");

    clen = 0;
    ok(ppmd->encode(raw, raw_len, comp, cap, &clen) == 0 && clen < raw_len,
       "PPMD compresses ~100KB repetitive text");
    if (clen > 0) {
        ok(ppmd->decode(comp, clen, back, raw_len) == 0 &&
           memcmp(back, raw, raw_len) == 0, "PPMD round-trip through registry");
        ok(ppmd->decode(comp, clen, back, raw_len + 1) == -1,
           "PPMD decode past the END marker fails");
        done = 1;
    }
    (void)done;

    free(raw);
    free(comp);
    free(back);
}

/* ---------------- probe / codecpack fixtures ---------------- */

static int write_file(const char *path, const char *data, int exec)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, strlen(data), f) != strlen(data)) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    if (exec && chmod(path, 0755) != 0) return -1;
    return 0;
}

static void test_probe(void)
{
    const invfs_codec *pmp = invfs_codec_by_algo(INVFS_ALGO_PMP);
    const invfs_codec *jxl = invfs_codec_by_algo(INVFS_ALGO_JXL);
    const invfs_codec *ape = invfs_codec_by_algo(INVFS_ALGO_APE);
    const invfs_codec *wv  = invfs_codec_by_algo(INVFS_ALGO_WV);
    char dir[256], packs[320], pack[384], bin[384], empty[384], path[448];
    char *saved_path;
    int r;

    /* availability of real tools is environment-dependent: just exercise */
    r = pmp->probe(); ok(r == 0 || r == 1, "probe(pmp) is well-defined");
    printf("  info  probe: pmp=%d jxl=%d ape=%d wv=%d\n",
           r, jxl->probe(), ape->probe(), wv->probe());

    saved_path = getenv("PATH");
    saved_path = saved_path ? strdup(saved_path) : NULL;

    snprintf(dir, sizeof dir, "/tmp/invfs_codec_test_%d", (int)getpid());
    snprintf(packs, sizeof packs, "%s/packs", dir);
    snprintf(pack, sizeof pack, "%s/packs/pmp.codecpack", dir);
    snprintf(bin, sizeof bin, "%s/bin", dir);
    snprintf(empty, sizeof empty, "%s/empty", dir);
    mkdir(dir, 0755);
    mkdir(packs, 0755);
    mkdir(pack, 0755);
    mkdir(bin, 0755);
    mkdir(empty, 0755);

    /* 1. codecpack with executable helpers -> available */
    snprintf(path, sizeof path, "%s/bin", pack);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/manifest", pack);
    r = write_file(path,
                   "# test pack\n"
                   "  name = pmp\n"
                   "algo=11\n"
                   "caps=external\n"
                   "dec_mem=1048576\n"
                   "generation=1\n"
                   "sniff.magic=494433\n"
                   "encode = bin/enc {in} {out}\n"
                   "decode=bin/dec {in} {out}\n"
                   "bogus key line\n"
                   "unknown.key = ignored\n", 0);
    snprintf(path, sizeof path, "%s/bin/enc", pack);
    r |= write_file(path, "#!/bin/sh\nexit 0\n", 1);
    snprintf(path, sizeof path, "%s/bin/dec", pack);
    r |= write_file(path, "#!/bin/sh\nexit 0\n", 1);
    ok(r == 0, "fixture: codecpack written");
    setenv("INVFS_CODECPACKS", packs, 1);
    invfs_codec_probe_reset();
    ok(pmp->probe() == 1, "codecpack manifest makes the codec available");

    /* 2. no pack, but a self-describing binary on PATH -> available */
    setenv("INVFS_CODECPACKS", "", 1);
    snprintf(path, sizeof path, "%s/packMP3", bin);
    r = write_file(path,
                   "#!/bin/sh\n"
                   "if [ \"$1\" = \"--invfs-manifest\" ]; then echo name=pmp; exit 0; fi\n"
                   "exit 1\n", 1);
    ok(r == 0, "fixture: self-describing tool written");
    setenv("PATH", bin, 1);
    invfs_codec_probe_reset();
    ok(pmp->probe() == 1, "self-describing binary makes the codec available");

    /* 3. broken pack (helpers missing) + tool absent -> unavailable */
    snprintf(path, sizeof path, "%s/enc", pack);   /* remove the good helpers */
    snprintf(path, sizeof path, "%s/bin/enc", pack);
    unlink(path);
    snprintf(path, sizeof path, "%s/bin/dec", pack);
    unlink(path);
    setenv("INVFS_CODECPACKS", packs, 1);
    setenv("PATH", empty, 1);
    invfs_codec_probe_reset();
    ok(pmp->probe() == 0, "broken pack + absent tool -> unavailable");

    /* restore the environment, clean up the fixture */
    if (saved_path) { setenv("PATH", saved_path, 1); free(saved_path); }
    unsetenv("INVFS_CODECPACKS");
    snprintf(path, sizeof path, "%s/manifest", pack);
    unlink(path);
    snprintf(path, sizeof path, "%s/packMP3", bin);
    unlink(path);
    snprintf(path, sizeof path, "%s/bin", pack);
    rmdir(path);
    rmdir(pack);
    rmdir(packs);
    rmdir(bin);
    rmdir(empty);
    rmdir(dir);
}

/* ---------------- dynamic pack registration (WP13) ---------------- */

static void test_packs(void)
{
    char dir[256], packs[320], pack[384], dupe[384], cpack[384], bpack[384],
         mpack[384], opack[384], path[448];
    const invfs_codec *c, *all;
    const invfs_pack_def *def;
    size_t n = 0, i, olen = 0;
    uint8_t data[64], enc[128], dec[64];
    uint64_t est = 0;
    static const uint8_t mz4[6] = { 0, 0, 0, 0, 'M', 'Z' };   /* MZ at off 4 */
    static const uint8_t mz0[6] = { 'M', 'Z', 0, 0, 0, 0 };   /* MZ at off 0 */
    static const uint8_t splt[4] = { 'S', 'P', 'L', 'T' };
    int r;

    snprintf(dir, sizeof dir, "/tmp/invfs_pack_test_%d", (int)getpid());
    snprintf(packs, sizeof packs, "%s/packs", dir);
    snprintf(pack, sizeof pack, "%s/fakeimg.codecpack", packs);
    snprintf(dupe, sizeof dupe, "%s/dupe.codecpack", packs);
    snprintf(cpack, sizeof cpack, "%s/spltmini.codecpack", packs);
    snprintf(bpack, sizeof bpack, "%s/badcont.codecpack", packs);
    snprintf(mpack, sizeof mpack, "%s/spltmap.codecpack", packs);
    snprintf(opack, sizeof opack, "%s/jxlpack.codecpack", packs);
    r = mkdir(dir, 0755) | mkdir(packs, 0755) | mkdir(pack, 0755) |
        mkdir(dupe, 0755) | mkdir(cpack, 0755) | mkdir(bpack, 0755) |
        mkdir(mpack, 0755) | mkdir(opack, 0755);

    /* one valid pack (sniff.offset BEFORE sniff.magic: pending-offset path)
     * plus a second pack whose algo collides with builtin ZSTD (skipped) */
    snprintf(path, sizeof path, "%s/manifest", pack);
    r |= write_file(path,
                    "# fake pack for codec_test\n"
                    "name = fakeimg\n"
                    "algo = 42\n"
                    "pack_version = 1\n"
                    "caps = external|wholefile\n"
                    "dec_mem = 4096\n"
                    "generation = 3\n"
                    "sniff.offset = 4\n"
                    "sniff.magic = 4D5A\n"
                    "sniff.ext = fak,fake\n"
                    "requires = cp\n"
                    "encode = cp {in} {out}\n"
                    "decode = cp {in} {out}\n"
                    "map = cp {in} {out}\n"   /* container-ABI key on a codec
                                               * pack: parsed, never wired */
                    "unknown.key = skipped\n", 0);
    snprintf(path, sizeof path, "%s/manifest", dupe);
    r |= write_file(path,
                    "name = dupe\n"
                    "algo = 1\n"
                    "caps = external\n"
                    "encode = cp {in} {out}\n"
                    "decode = cp {in} {out}\n", 0);
    /* WP16a: a container pack (type=container): the four decomposition
     * commands instead of encode/decode; plus a broken one (no rebuild)
     * that must NOT register */
    snprintf(path, sizeof path, "%s/manifest", cpack);
    r |= write_file(path,
                    "# container pack fixture (WP16a)\n"
                    "name = spltmini\n"
                    "type = container\n"
                    "algo = 43\n"
                    "pack_version = 1\n"
                    "generation = 2\n"
                    "dec_mem = 0\n"
                    "sniff.magic = 53504C54\n"
                    "sniff.ext = splt\n"
                    "enumerate = cp {in} {out}\n"
                    "extract = cp {in} {idx} {out}\n"
                    "strip = cp {in} {out}\n"
                    "rebuild = cp {recipe} {dir} {out}\n", 0);
    snprintf(path, sizeof path, "%s/manifest", bpack);
    r |= write_file(path,
                    "name = badcont\n"
                    "type = container\n"
                    "algo = 44\n"
                    "sniff.magic = 42414443\n"
                    "enumerate = cp {in} {out}\n"
                    "extract = cp {in} {idx} {out}\n"
                    "strip = cp {in} {out}\n", 0);
    /* WP16b: a container pack WITH a map command -> CAP_SEEK on the entry */
    snprintf(path, sizeof path, "%s/manifest", mpack);
    r |= write_file(path,
                    "# seekable container pack fixture (WP16b)\n"
                    "name = spltmap\n"
                    "type = container\n"
                    "algo = 45\n"
                    "pack_version = 1\n"
                    "generation = 1\n"
                    "sniff.magic = 53504C54\n"
                    "enumerate = cp {in} {out}\n"
                    "extract = cp {in} {idx} {out}\n"
                    "strip = cp {in} {out}\n"
                    "rebuild = cp {recipe} {dir} {out}\n"
                    "map = cp {in} {out}\n", 0);
    /* WP16e: a codec pack claiming algo 4 (JXL) -- held by a builtin
     * EXTERNAL placeholder -- OVERRIDES the builtin entry (pack wins);
     * the dupe pack above (algo 1, ZSTD) proves stream codecs cannot be
     * claimed. */
    snprintf(path, sizeof path, "%s/manifest", opack);
    r |= write_file(path,
                    "# override fixture (WP16e)\n"
                    "name = jxlpack\n"
                    "algo = 4\n"
                    "pack_version = 1\n"
                    "caps = external|wholefile\n"
                    "dec_mem = 4096\n"
                    "generation = 7\n"
                    "sniff.magic = FFD8FF\n"
                    "encode = cp {in} {out}\n"
                    "decode = cp {in} {out}\n", 0);
    ok(r == 0, "fixture: pack dirs written");

    setenv("INVFS_CODECPACKS", packs, 1);
    invfs_codec_probe_reset();

    all = invfs_codec_all(&n);
    ok(all != NULL && n == 17,
       "packs registered: 13 static + codec + override + 2 containers "
       "(the override REPLACES the builtin jxl placeholder)");
    ok(all[n - 1].algo == INVFS_ALGO_PPMD,
       "text heuristic still LAST with packs loaded");
    c = invfs_codec_by_algo(INVFS_ALGO_ZSTD);
    ok(c && strcmp(c->name, "zstd") == 0,
       "pack whose algo collides with a builtin is skipped");
    ok(invfs_codec_by_algo(44) == NULL,
       "container pack missing a command (rebuild) is not registered");

    c = invfs_codec_by_algo(42);
    ok(c != NULL, "pack found by algo");
    ok(c && strcmp(c->name, "fakeimg") == 0, "pack name from manifest");
    ok(c && (c->caps & INVFS_CODEC_CAP_EXTERNAL) &&
       (c->caps & INVFS_CODEC_CAP_WHOLEFILE), "pack caps parsed");
    ok(c && c->dec_mem_bytes == 4096 && c->generation == 3,
       "pack dec_mem/generation parsed");
    ok(c && c->sniff && c->probe && c->encode && c->decode,
       "pack fn pointers wired (trampolines)");

    ok(c && c->sniff(mz4, sizeof mz4, "x.bin") == 100,
       "manifest magic@offset -> 100");
    ok(c && c->sniff(mz0, sizeof mz0, "x.bin") == 0,
       "magic at the wrong offset -> 0");
    ok(c && c->sniff(mz0, sizeof mz0, "x.fak") == 50,
       "extension list -> 50");
    ok(c && c->sniff(mz0, sizeof mz0, "x.png") == 0, "no match -> 0");

    ok(c && c->probe() == 1, "pack probe: cp resolvable (requires)");

    for (i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 29 + 5);
    olen = 0;
    ok(c && c->encode(data, sizeof data, enc, sizeof enc, &olen) == 0 &&
       olen == sizeof data && memcmp(enc, data, sizeof data) == 0,
       "pack encode trampoline (cp = identity)");
    ok(c && c->decode(enc, olen, dec, sizeof dec) == 0 &&
       memcmp(dec, data, sizeof data) == 0,
       "pack decode trampoline (cp = identity)");
    ok(c && c->decode(enc, olen, dec, sizeof dec - 1) == -1,
       "pack decode: wrong output size -> -1 (bit-exact or nothing)");

    ok(invfs_codec_pack_estimate(c, "whatever", &est) == -1,
       "pack without estimate command -> -1");
    ok(invfs_registry_generation() >= 3,
       "registry generation includes packs");
    def = invfs_codec_pack_def(c);
    ok(def && def->dir && strstr(def->dir, "fakeimg.codecpack") != NULL,
       "pack def exposes the pack dir");
    ok(invfs_codec_pack_def(invfs_codec_by_algo(INVFS_ALGO_ZSTD)) == NULL,
       "builtin codec has no pack def");

    /* ---- WP16a: container pack registration + precedence ---- */
    c = invfs_codec_by_algo(43);
    ok(c != NULL, "container pack found by algo");
    ok(c && strcmp(c->name, "spltmini") == 0, "container pack name parsed");
    ok(c && (c->caps & INVFS_CODEC_CAP_CONTAINER) &&
       (c->caps & INVFS_CODEC_CAP_EXTERNAL) &&
       (c->caps & INVFS_CODEC_CAP_WHOLEFILE),
       "container pack caps forced: CONTAINER|EXTERNAL|WHOLEFILE");
    ok(c && c->generation == 2, "container pack generation parsed");
    ok(c && c->sniff && c->probe, "container pack sniff/probe wired");
    ok(c && c->encode == NULL && c->decode == NULL,
       "container pack has NO encode/decode (the WP13 codec loop skips it)");
    ok(c && c->sniff(splt, sizeof splt, "x.splt") == 100,
       "container pack magic -> 100");
    ok(c && c->sniff(mz0, sizeof mz0, "x.splt") == 50,
       "container pack extension list -> 50");
    ok(c && c->sniff(mz0, sizeof mz0, "x.bin") == 0,
       "container pack: no magic/ext match -> 0");
    ok(c && c->probe() == 1, "container pack probe: all four argv tools resolve");
    def = invfs_codec_pack_def(c);
    ok(def && def->is_container == 1, "pack def: is_container");
    ok(def && def->enumerate && def->extract && def->strip && def->rebuild,
       "pack def exposes the four container commands");
    ok(def && def->map == NULL && !(c->caps & INVFS_CODEC_CAP_SEEK),
       "map-less container pack: no map cmd, NO CAP_SEEK");
    ok(def && strstr(def->rebuild, "{recipe}") && strstr(def->rebuild, "{dir}"),
       "rebuild argv carries {recipe} {dir} placeholders");

    /* WP16b: the same container pack + a `map` command gains CAP_SEEK */
    c = invfs_codec_by_algo(45);
    ok(c != NULL && strcmp(c->name, "spltmap") == 0,
       "seekable container pack found by algo");
    ok(c && (c->caps & INVFS_CODEC_CAP_CONTAINER) &&
       (c->caps & INVFS_CODEC_CAP_EXTERNAL) &&
       (c->caps & INVFS_CODEC_CAP_WHOLEFILE) &&
       (c->caps & INVFS_CODEC_CAP_SEEK),
       "container pack with a map command: CONTAINER|EXTERNAL|WHOLEFILE|SEEK");
    ok(c && c->probe && c->probe() == 1,
       "seekable container pack probe covers the map tool");
    def = c ? invfs_codec_pack_def(c) : NULL;
    ok(def && def->map && strstr(def->map, "{in}") && strstr(def->map, "{out}"),
       "pack def exposes the map command argv");
    def = invfs_codec_pack_def(invfs_codec_by_algo(42));
    ok(def && def->map == NULL &&
       !(invfs_codec_by_algo(42)->caps & INVFS_CODEC_CAP_SEEK),
       "map line on a CODEC pack is ignored (no def.map, no CAP_SEEK)");
    /* precedence: builtin entries first, packs after, text LAST */
    {
        size_t exer_at = 0, pack_at = 0, ppmd_at = 0;
        for (i = 0; i < n; i++) {
            if (all[i].algo == INVFS_ALGO_EXER) exer_at = i;
            if (all[i].algo == 43) pack_at = i;
            if (all[i].algo == INVFS_ALGO_PPMD) ppmd_at = i;
        }
        ok(exer_at < pack_at && pack_at < ppmd_at,
           "order: builtin magics first, packs after, text LAST");
    }
    /* WP16e: the pack claiming the builtin JXL algo REPLACED the
     * placeholder: one entry for algo 4, and it is the pack's. */
    c = invfs_codec_by_algo(INVFS_ALGO_JXL);
    ok(c && strcmp(c->name, "jxlpack") == 0,
       "override: pack claims the builtin EXTERNAL algo (name from manifest)");
    ok(c && c->encode && c->decode && c->sniff && c->probe,
       "override: the pack entry carries the trampolines");
    ok(c && c->generation == 7 && c->dec_mem_bytes == 4096,
       "override: generation/dec_mem come from the manifest");
    ok(c && !(c->caps & INVFS_CODEC_CAP_PACKONLY),
       "override: the PACKONLY bit is the placeholder's, not the pack's");
    def = c ? invfs_codec_pack_def(c) : NULL;
    ok(def && def->dir && strstr(def->dir, "jxlpack.codecpack") != NULL,
       "override: the algo-4 entry has a pack def");
    {
        size_t jxl_at = 0, exer_at = 0, ppmd_at = 0;
        for (i = 0; i < n; i++) {
            if (all[i].algo == INVFS_ALGO_JXL) jxl_at = i;
            if (all[i].algo == INVFS_ALGO_EXER) exer_at = i;
            if (all[i].algo == INVFS_ALGO_PPMD) ppmd_at = i;
        }
        ok(exer_at < jxl_at && jxl_at < ppmd_at,
           "override: the winning entry sits in the pack section");
    }
    ok(invfs_codec_pack_def(invfs_codec_by_algo(INVFS_ALGO_TARR)) == NULL ||
       !invfs_codec_pack_def(invfs_codec_by_algo(INVFS_ALGO_TARR))->is_container,
       "builtin TARR is not a container pack");

    /* reset must unload the packs (the probe cache AND the registration);
     * the env goes first or the re-scan legitimately finds them again */
    unsetenv("INVFS_CODECPACKS");
    invfs_codec_probe_reset();
    ok(invfs_codec_by_algo(42) == NULL, "reset unloads packs");
    ok(invfs_codec_by_algo(43) == NULL, "reset unloads container packs");
    ok(invfs_codec_by_algo(45) == NULL, "reset unloads seekable packs");
    c = invfs_codec_by_algo(INVFS_ALGO_JXL);
    ok(c && strcmp(c->name, "jxl") == 0 && c->encode == NULL &&
       c->decode == NULL && (c->caps & INVFS_CODEC_CAP_PACKONLY),
       "reset restores the overridden builtin placeholder (sniff+probe only)");
    all = invfs_codec_all(&n);
    ok(n == 14, "reset restores the static registry");
    invfs_codec_probe_reset();   /* a second reset is harmless */

    snprintf(path, sizeof path, "%s/manifest", pack);
    unlink(path);
    snprintf(path, sizeof path, "%s/manifest", dupe);
    unlink(path);
    snprintf(path, sizeof path, "%s/manifest", cpack);
    unlink(path);
    snprintf(path, sizeof path, "%s/manifest", bpack);
    unlink(path);
    snprintf(path, sizeof path, "%s/manifest", mpack);
    unlink(path);
    snprintf(path, sizeof path, "%s/manifest", opack);
    unlink(path);
    rmdir(pack);
    rmdir(dupe);
    rmdir(cpack);
    rmdir(bpack);
    rmdir(mpack);
    rmdir(opack);
    rmdir(packs);
    rmdir(dir);
}

/* ---------------- WP16b: codec profiles ---------------- */

static void test_profiles(void)
{
    ok(invfs_profile_parse("fast") == INVFS_PROFILE_FAST, "parse fast");
    ok(invfs_profile_parse("balanced") == INVFS_PROFILE_BALANCED,
       "parse balanced");
    ok(invfs_profile_parse("dense") == INVFS_PROFILE_DENSE, "parse dense");
    ok(invfs_profile_parse("archive") == INVFS_PROFILE_ARCHIVE,
       "parse archive");
    ok(invfs_profile_parse("faster") == INVFS_PROFILE_FASTER, "parse faster");
    ok(invfs_profile_parse("fastest") == INVFS_PROFILE_FASTEST,
       "parse fastest");
    ok(invfs_profile_parse("turbo") == INVFS_PROFILE_TURBO, "parse turbo");
    ok(invfs_profile_parse(NULL) == -1, "NULL is not a profile");
    ok(invfs_profile_parse("") == -1, "empty is not a profile");
    ok(invfs_profile_parse("FAST") == -1, "profile names are exact/lowercase");
    ok(invfs_profile_parse("ludicrous") == -1, "unknown name -> -1");
    ok(strcmp(invfs_profile_name(INVFS_PROFILE_FAST), "fast") == 0 &&
       strcmp(invfs_profile_name(INVFS_PROFILE_BALANCED), "balanced") == 0 &&
       strcmp(invfs_profile_name(INVFS_PROFILE_DENSE), "dense") == 0 &&
       strcmp(invfs_profile_name(INVFS_PROFILE_ARCHIVE), "archive") == 0 &&
       strcmp(invfs_profile_name(INVFS_PROFILE_FASTER), "faster") == 0 &&
       strcmp(invfs_profile_name(INVFS_PROFILE_FASTEST), "fastest") == 0 &&
       strcmp(invfs_profile_name(INVFS_PROFILE_TURBO), "turbo") == 0,
       "name round-trip (7 rungs)");
    ok(invfs_profile_zstd_level(INVFS_PROFILE_FAST) == 6 &&
       invfs_profile_zstd_level(INVFS_PROFILE_BALANCED) == 19 &&
       invfs_profile_zstd_level(INVFS_PROFILE_DENSE) == 19 &&
       invfs_profile_zstd_level(INVFS_PROFILE_ARCHIVE) == 19 &&
       invfs_profile_zstd_level(INVFS_PROFILE_FASTER) == 3,
       "generic sweep levels: 6/19/19/19/3 (19 = the historical default, "
       "22 dropped in WP19)");
    ok(invfs_profile_zstd_level(-1) == 19 &&
       invfs_profile_zstd_level(99) == 19,
       "out-of-range profile -> the default level");
    ok(invfs_profile_generic_algo(INVFS_PROFILE_FAST) == INVFS_ALGO_ZSTD &&
       invfs_profile_generic_algo(INVFS_PROFILE_BALANCED) == INVFS_ALGO_ZSTD &&
       invfs_profile_generic_algo(INVFS_PROFILE_DENSE) == INVFS_ALGO_ZSTD &&
       invfs_profile_generic_algo(INVFS_PROFILE_ARCHIVE) == INVFS_ALGO_ZSTD &&
       invfs_profile_generic_algo(INVFS_PROFILE_FASTER) == INVFS_ALGO_ZSTD &&
       invfs_profile_generic_algo(INVFS_PROFILE_FASTEST) == INVFS_ALGO_LZ4 &&
       invfs_profile_generic_algo(INVFS_PROFILE_TURBO) == INVFS_ALGO_NONE,
       "meta-profiles substitute the floor codec: fastest=LZ4, turbo=NONE");
    ok(invfs_profile_generic_algo(-1) == INVFS_ALGO_ZSTD &&
       invfs_profile_generic_algo(99) == INVFS_ALGO_ZSTD,
       "out-of-range profile -> ZSTD (the default shape)");
}

int main(void)
{
    printf("codec registry tests\n");

    test_registry();
    test_sniff();
    test_text_family();
    test_binary_family();
    test_bcj();
    test_roundtrips();
    test_probe();
    test_packs();
    test_profiles();

    printf("%d checks, %d failure(s)\n", checks, failures);
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
