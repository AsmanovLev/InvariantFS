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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codec.h"
#include "invarifs.h"

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

    ok(all != NULL && n == 13, "registry holds the 13 v1 entries");
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

int main(void)
{
    printf("codec registry tests\n");

    test_registry();
    test_sniff();
    test_text_family();
    test_roundtrips();
    test_probe();

    printf("%d checks, %d failure(s)\n", checks, failures);
    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
