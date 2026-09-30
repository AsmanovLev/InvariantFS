/* sibling_retire_v3_test.c — a v3 overwrite must retire the superseded
 * content's "name!..." siblings, exactly as its v2 twin does.
 *
 * The divergence this exists for (WP meta-write-dup-audit, finding A): the
 * v2 write commit ends with
 *
 *     if (s->owns_siblings) vol_delete_siblings(v, s->name);
 *
 * (src/core/vol_write.c:1131) and the v3 commit (vol_write_commit_v3) had
 * no such leg. On v3 a decomposed container keeps its payload in sibling
 * INODES -- vol_create_tar_file calls vol_create_blob_file("%s!part%u"),
 * which on v3 lands as a real dirent -- so overwriting a swept a.tar left
 * a.tar!part0..N live for ever:
 *
 *   - the sweep walks an internal '!' name but never retires it,
 *   - spn_reclaim cannot free a block a live recipe names, and those parts
 *     have live recipes,
 *   - so their segments are unreachable AND unreclaimable: measured, the
 *     free-block count stayed flat across two further sweeps.
 *
 * The fix makes both formats ask ONE rule (ast_owns_siblings,
 * src/core/vol_records.c) and adds the retire to the v3 commit, so the two
 * cannot disagree again.
 *
 * Legs:
 *   A  a plain file has no siblings and the replace does not walk for them
 *   B  a TAR decomposed into name!partN, then overwritten -> the parts are
 *      GONE (this is the red control: it fails on the unfixed tree)
 *   C  the overwritten name still reads back the NEW bytes, bit-exact
 *      (the retire must not disturb what `name` resolves to)
 *   D  it survives a remount -- the retire is durable, not just in RAM
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "volume_internal.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  OK    %s\n", what);
    }
}

static invfs_volume *g_v;
static char g_img[512];

/* A small, highly compressible ustar archive: the lane only fires when the
 * parts+recipe come out SMALLER than the original, so the payloads have to
 * be compressible or the test would be measuring the lane's refusal. */
static size_t build_tar(uint8_t **out)
{
    const char *names[4] = { "m0", "m1", "m2", "m3" };
    size_t pos = 0, cap = 8192;
    uint8_t *buf = malloc(cap);
    int i;

    for (i = 0; i < 4; i++) {
        uint8_t hdr[512];
        uint8_t payload[400];
        size_t plen = sizeof payload;
        size_t j;
        unsigned sum = 0;

        memset(hdr, 0, sizeof hdr);
        memset(payload, 'a' + i, plen);
        memcpy(hdr, names[i], strlen(names[i]));
        memcpy(hdr + 100, "0000644\0", 8);
        memcpy(hdr + 108, "0000000\0", 8);
        memcpy(hdr + 116, "0000000\0", 8);
        memcpy(hdr + 124, "0001000\0", 8);       /* size, octal */
        memcpy(hdr + 136, "00000000000\0", 12);  /* mtime */
        memcpy(hdr + 148, "        ", 8);        /* checksum placeholder */
        hdr[156] = '0';                          /* regular file */
        memcpy(hdr + 257, "ustar", 5);
        memcpy(hdr + 263, "00", 2);
        for (j = 0; j < sizeof hdr; j++)
            sum += hdr[j];
        snprintf((char *)hdr + 148, 8, "%06o", sum);
        hdr[154] = '\0';

        for (j = 0; pos + 512 + plen + 512 > cap; j++) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + pos, hdr, 512);
        memcpy(buf + pos + 512, payload, plen);
        pos += 512 + plen;
        while (pos % 512) { buf[pos++] = 0; }
    }
    memset(buf + pos, 0, 512 * 2);               /* two trailer blocks */
    pos += 512 * 2;
    *out = buf;
    return pos;
}

static int count_siblings(const char *name)
{
    char pn[320];
    int n = 0, i;
    for (i = 0; i < 64; i++) {
        snprintf(pn, sizeof pn, "%s!part%d", name, i);
        if (vol_find(g_v, pn))
            n++;
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    uint8_t *tar = NULL, *back = NULL;
    size_t tar_len, back_len = 0;
    static const char payload[] = "NEW CONTENT, NOT A TAR AT ALL";
    int err = 0, i;

    printf("sibling_retire_v3_test: v3 overwrite retires name! siblings\n");

    snprintf(g_img, sizeof g_img, "%s/invf-sibling-retire-test.img", dir);
    unlink(g_img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 root, g_img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        fprintf(stderr, "sibling_retire_v3_test: vol_open failed: err=%d\n", err);
        return 2;
    }

    /* ---- leg A: a plain file owns no siblings ------------------------ */
    ok(vol_replace_file(g_v, "plain.txt", (const uint8_t *)"hello", 5) != 0,
       "leg A: plain.txt written");
    ok(count_siblings("plain.txt") == 0,
       "leg A: a plain file owns no name! siblings");
    ok(vol_replace_file(g_v, "plain.txt", (const uint8_t *)"hello2", 6) != 0,
       "leg A: plain.txt overwritten");
    ok(vol_read_file(g_v, vol_find(g_v, "plain.txt"), &back, &back_len) == 0 &&
       back_len == 6 && memcmp(back, "hello2", 6) == 0,
       "leg A: the overwrite reads back byte-exact");
    free(back);
    back = NULL;

    /* ---- leg B: a decomposed container, then an overwrite ------------- */
    tar_len = build_tar(&tar);
    ok(vol_replace_file(g_v, "a.tar", tar, tar_len) != 0,
       "leg B: a.tar stored verbatim");
    ok(vol_create_tar_file(g_v, "a.tar", tar, tar_len) != 0,
       "leg B: the TARR lane decomposed a.tar");
    ok(count_siblings("a.tar") > 0,
       "leg B: the decomposition created name!partN siblings");
    printf("        (a.tar!partN live before the overwrite: %d)\n",
           count_siblings("a.tar"));

    ok(vol_replace_file(g_v, "a.tar", (const uint8_t *)payload,
                        sizeof payload - 1) != 0,
       "leg B: a.tar overwritten with unrelated bytes");
    ok(count_siblings("a.tar") == 0,
       "leg B: the overwrite RETIRED the superseded siblings");
    printf("        (a.tar!partN live after the overwrite: %d)\n",
           count_siblings("a.tar"));

    /* ---- leg C: the name still reads back the NEW bytes ---------------- */
    back = NULL;
    ok(vol_read_file(g_v, vol_find(g_v, "a.tar"), &back, &back_len) == 0 &&
       back_len == sizeof payload - 1 &&
       back && memcmp(back, payload, back_len) == 0,
       "leg C: a.tar reads back the new bytes, bit-exact");
    free(back);
    back = NULL;
    /* the parts must be gone by NAME, not merely unreachable: a stale
     * dirent would still be listed by readdir. */
    for (i = 0; i < 64; i++) {
        char pn[320];
        snprintf(pn, sizeof pn, "a.tar!part%d", i);
        if (vol_find(g_v, pn))
            break;
    }
    ok(i == 64, "leg C: no a.tar!partN name resolves");

    /* ---- leg D: the retire is durable --------------------------------- */
    vol_close(g_v);
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        fprintf(stderr, "sibling_retire_v3_test: reopen failed: err=%d\n", err);
        return 2;
    }
    ok(count_siblings("a.tar") == 0,
       "leg D: still no siblings after a remount");
    back = NULL;
    ok(vol_read_file(g_v, vol_find(g_v, "a.tar"), &back, &back_len) == 0 &&
       back_len == sizeof payload - 1 &&
       back && memcmp(back, payload, back_len) == 0,
       "leg D: a.tar still reads back the new bytes after a remount");
    free(back);

    vol_close(g_v);
    free(tar);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
