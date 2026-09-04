/*
 * nametest.c -- the 255-byte name limit, driven through vol_create_file.
 *
 *   invf-nametest <image>
 *
 * Host paths cannot reach this boundary: MAX_PATH stops CreateFile long before
 * a single component gets to 255 bytes, so a copy-based test silently never
 * exercises it. This calls the volume API directly.
 *
 * Two defects motivated it. The record stores name_len beside a 256-byte name
 * field, and the zero-length-file path used to write the full strlen into
 * name_len while strncpy capped the bytes actually stored -- a record whose
 * length disagreed with its contents. Separately the decomposition paths
 * append "!part<N>" to the parent name, which can cross 255 even when the
 * parent is legal, and the child creation would then fail after the parent was
 * already committed.
 *
 * What must hold: at the limit a name works and round-trips exactly; past it
 * creation is refused; and a name close enough to the limit that its part
 * suffixes would not fit is either kept whole or fails cleanly, never left
 * half-decomposed.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"

static int checks, fails;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { fails++; printf("  FAIL %s\n", what); }
}

/* name of exactly n bytes: "<pad...>.bin" so the extension probe still runs */
static char *mkname(size_t n)
{
    char *s = (char *)malloc(n + 1);
    memset(s, 'a', n);
    memcpy(s + n - 4, ".bin", 4);
    s[n] = 0;
    return s;
}

int main(int argc, char **argv)
{
    invfs_volume *v;
    int err = 0;
    char *n255, *n256, *n300, *n250;
    uint64_t id;
    uint8_t small[64], big[300 * 1024];
    size_t i;

    if (argc < 2) { fprintf(stderr, "usage: invf-nametest <image>\n"); return 2; }

    v = vol_open(argv[1], &err);
    if (!v) { fprintf(stderr, "vol_open %s failed: %d\n", argv[1], err); return 1; }

    for (i = 0; i < sizeof small; i++) small[i] = (uint8_t)i;
    /* incompressible, so the big cases actually allocate many segments */
    for (i = 0; i < sizeof big; i++) big[i] = (uint8_t)((i * 2654435761u) >> 13);

    printf("name limit is %d bytes\n", INVFS_MAX_NAME);

    /* --- exactly at the limit: must work, and read back byte-identical --- */
    n255 = mkname(INVFS_MAX_NAME);
    id = vol_create_file(v, n255, small, sizeof small);
    ok(id != 0, "255-byte name accepted");
    if (id) {
        uint64_t sid = 0, ssz = 0, sct = 0;
        ok(vol_stat_full(v, n255, &sid, &ssz, &sct) == 0, "255-byte name stats");
        ok(ssz == sizeof small, "255-byte name size correct");
        if (sid) {
            uint8_t back[sizeof small];
            memset(back, 0, sizeof back);
            /* vol_read_range returns bytes read, not a status */
            ok(vol_read_range(v, sid, 0, sizeof small, back) == (int)sizeof small,
               "255-byte name reads back");
            ok(memcmp(back, small, sizeof small) == 0,
               "255-byte name content identical");
        }
    }

    /* --- one past the limit: must refuse, not truncate --- */
    n256 = mkname(INVFS_MAX_NAME + 1);
    ok(vol_create_file(v, n256, small, sizeof small) == 0,
       "256-byte name refused");
    ok(vol_stat_full(v, n256, NULL, NULL, NULL) != 0,
       "256-byte name left nothing behind");
    /* the refused name must not have landed as a truncated 255-byte record
       either -- that was the original bug's signature */
    {
        char *trunc = mkname(INVFS_MAX_NAME + 1);
        trunc[INVFS_MAX_NAME] = 0;
        ok(vol_stat_full(v, trunc, NULL, NULL, NULL) != 0,
           "256-byte name did not appear truncated to 255");
        free(trunc);
    }

    n300 = mkname(300);
    ok(vol_create_file(v, n300, small, sizeof small) == 0,
       "300-byte name refused");

    /* --- large file whose part names would overflow: must not half-build ---
       250 bytes leaves 5 for a suffix; "!part10" needs 7. Whatever the volume
       decides, it must be consistent: either the file is there and readable in
       full, or it is not there at all. */
    n250 = mkname(250);
    id = vol_create_file(v, n250, big, sizeof big);
    if (id) {
        uint64_t sid = 0, ssz = 0, sct = 0;
        ok(vol_stat_full(v, n250, &sid, &ssz, &sct) == 0, "250-byte name stats");
        ok(ssz == sizeof big, "250-byte big file full size");
        if (sid) {
            uint8_t *back = (uint8_t *)malloc(sizeof big);
            int got = vol_read_range(v, sid, 0, sizeof big, back);
            if (got != (int)sizeof big)
                printf("  (250-byte big file: read %d of %d)\n",
                       got, (int)sizeof big);
            ok(got == (int)sizeof big, "250-byte big file reads back");
            ok(memcmp(back, big, sizeof big) == 0,
               "250-byte big file content identical");
            free(back);
        }
    } else {
        ok(vol_stat_full(v, n250, NULL, NULL, NULL) != 0,
           "250-byte big file refused cleanly, nothing left behind");
    }

    /* --- the zero-length path, which had its own name_len bug --- */
    {
        char *z = mkname(INVFS_MAX_NAME);
        z[0] = 'z';
        id = vol_create_file(v, z, NULL, 0);
        ok(id != 0, "255-byte name, zero-length file accepted");
        if (id) {
            uint64_t ssz = 1;
            ok(vol_stat_full(v, z, NULL, &ssz, NULL) == 0,
               "255-byte zero-length file stats");
            ok(ssz == 0, "255-byte zero-length file size is 0");
        }
        z[0] = 'y';
        {
            char *z2 = (char *)malloc(INVFS_MAX_NAME + 2);
            memset(z2, 'y', INVFS_MAX_NAME + 1);
            memcpy(z2 + INVFS_MAX_NAME - 3, ".bin", 4);
            z2[INVFS_MAX_NAME + 1] = 0;
            ok(vol_create_file(v, z2, NULL, 0) == 0,
               "256-byte name, zero-length file refused");
            free(z2);
        }
        free(z);
    }

    ok(vol_flush(v) == 0, "flush after name cases");
    vol_close(v);

    free(n255); free(n256); free(n300); free(n250);

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
