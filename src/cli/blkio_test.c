/*
 * blkio_test.c -- exercise the alignment layer.
 *
 * The read-modify-write path is the part that can silently corrupt a volume:
 * a bug there does not crash, it writes the right bytes to the wrong offset
 * or clobbers a neighbouring sector. So it gets tested against a plain
 * in-memory model of what the backing store should contain, byte for byte.
 *
 * Runs against an image file with device semantics forced on, so the
 * alignment code is exercised without needing a real device or elevation.
 *
 *   invf-blkio-test [scratch-file]
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blkio.h"

#define IMG_SIZE  (4u * 1024u * 1024u)

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

/* deterministic filler so a wrong-offset write is visible */
static unsigned char pat(uint64_t i) { return (unsigned char)(i * 31u + 7u); }

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "blkio_test.img";
    blkio io;
    unsigned char *model, *tmp;
    int rc;
    uint64_t off;
    size_t len;
    int i;

    for (int j = 1; j < argc; j++) {
        if (strcmp(argv[j], "-h") == 0 || strcmp(argv[j], "--help") == 0) {
            fprintf(stderr, "usage: invf-blkio_test [image]\n");
            return 2;
        }
        if (strcmp(argv[j], "-v") == 0 || strcmp(argv[j], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    printf("blkio alignment tests (%s)\n", path);

    model = (unsigned char *)malloc(IMG_SIZE);
    tmp   = (unsigned char *)malloc(IMG_SIZE);
    if (!model || !tmp) { printf("  out of memory\n"); return 1; }
    memset(model, 0, IMG_SIZE);

    /* ---- path handling (pure string work, no I/O) ---- */
    {
        char buf[64];
#ifdef _WIN32
        ok(strcmp(blkio_normalize("W:", buf, sizeof buf), "\\\\.\\W:") == 0,
           "normalize W: -> \\\\.\\W:");
        ok(strcmp(blkio_normalize("W:\\", buf, sizeof buf), "\\\\.\\W:") == 0,
           "normalize W:\\ -> \\\\.\\W:");
        ok(strcmp(blkio_normalize("D:\\VFS\\a.img", buf, sizeof buf),
                  "D:\\VFS\\a.img") == 0, "normalize leaves a file path alone");
        ok(blkio_looks_like_device("W:") == 1, "W: is a device");
        ok(blkio_looks_like_device("\\\\.\\W:") == 1, "\\\\.\\W: is a device");
        ok(blkio_looks_like_device("\\\\.\\PhysicalDrive0") == 1,
           "PhysicalDrive0 is recognised as a device");
        ok(blkio_looks_like_device("D:\\VFS\\a.img") == 0,
           "a file path is not a device");
        ok(blkio_looks_like_device("a.img") == 0, "a bare name is not a device");
        /* the guard that matters: a whole disk must be refused */
        {
            blkio d;
            rc = blkio_open(&d, "\\\\.\\PhysicalDrive0", 0);
            ok(rc == -BLKIO_E_REFUSED, "PhysicalDrive0 refused before open");
            if (rc == 0) blkio_close(&d);
            rc = blkio_open(&d, "\\\\.\\Harddisk0Partition6", 0);
            ok(rc == -BLKIO_E_REFUSED, "Harddisk0PartitionN refused before open");
            if (rc == 0) blkio_close(&d);
        }
#else
        ok(blkio_looks_like_device("/dev/sdb1") == 1, "/dev/sdb1 is a device");
        ok(blkio_looks_like_device("/tmp/a.img") == 0, "a file path is not a device");
        (void)buf;
#endif
    }

    /* ---- create the scratch image ---- */
    rc = blkio_open(&io, path, BLKIO_CREATE);
    if (rc != 0) { printf("  cannot create %s: %s\n", path, blkio_strerror(rc)); return 1; }
    ok(blkio_is_device(&io) == 0, "scratch file is not a device");
    rc = blkio_chsize(&io, IMG_SIZE);
    ok(rc == 0, "chsize on a file");
    ok(blkio_capacity(&io) == IMG_SIZE, "capacity reflects chsize");

    /* Zero it so the file and the model start identical. */
    ok(blkio_pwrite(&io, 0, model, IMG_SIZE) == 0, "zero-fill");
    blkio_close(&io);

    /* ---- reopen with device semantics forced on ---- *
     * This is the whole point of the test: the same file, driven through the
     * aligned bounce path, must end up byte-identical to the model. */
    rc = blkio_open(&io, path, 0);
    if (rc != 0) { printf("  cannot reopen: %s\n", blkio_strerror(rc)); return 1; }
    io.is_dev = 1;
    io.aligned = 1;   /* what blkio_pread/pwrite actually branch on */
    io.sector = 512;
    io.cap = IMG_SIZE;
    io.bounce_base = malloc(BLKIO_BOUNCE + BLKIO_ALIGN);
    if (!io.bounce_base) { printf("  out of memory\n"); return 1; }
    {
        uintptr_t a = (uintptr_t)io.bounce_base;
        a = (a + (BLKIO_ALIGN - 1)) & ~(uintptr_t)(BLKIO_ALIGN - 1);
        io.bounce = (unsigned char *)a;
    }

    /* ---- the cases InvariantFS actually generates ---- */
    {
        struct { uint64_t off; size_t len; const char *what; } cases[] = {
            {      0,     4, "4-byte write at 0 (record CRC)" },
            {      0,  4096, "one aligned block" },
            {    337,   336, "unaligned offset, unaligned len (inode record)" },
            {   4090,    20, "straddles a 4096 boundary" },
            {   4095,     1, "single byte at the last byte of a block" },
            {   4096,     1, "single byte at the first byte of a block" },
            {   8192, 65536, "aligned multi-block (bitmap-sized)" },
            { 100000, 65537, "unaligned offset, odd length, spans many blocks" },
            {   1234,     0, "zero-length write is a no-op" },
            { 2097152, 1024u*1024u, "exactly one bounce buffer" },
            { 2097153, 1024u*1024u, "one bounce buffer, offset by one" },
            { 1048576, 1024u*1024u + 4096u, "larger than the bounce buffer" }
        };
        size_t nc = sizeof cases / sizeof cases[0];
        size_t c;
        unsigned char *src = (unsigned char *)malloc(2u*1024u*1024u + 8192u);
        if (!src) { printf("  out of memory\n"); return 1; }

        for (c = 0; c < nc; c++) {
            off = cases[c].off;
            len = cases[c].len;
            for (i = 0; i < (int)len; i++) src[i] = pat(off + (uint64_t)i + c);

            ok(blkio_pwrite(&io, off, src, len) == 0, cases[c].what);
            memcpy(model + off, src, len);

            /* the write must be visible through the same layer... */
            memset(tmp, 0xAA, len ? len : 1);
            ok(blkio_pread(&io, off, tmp, len) == 0, "  read back");
            ok(len == 0 || memcmp(tmp, src, len) == 0, "  read back matches");

            /* ...and nothing else in the image may have moved. This is the
               check that catches a read-modify-write clobbering a neighbour. */
            ok(blkio_pread(&io, 0, tmp, IMG_SIZE) == 0, "  full image read");
            {
                int same = memcmp(tmp, model, IMG_SIZE) == 0;
                if (!same) {
                    size_t j;
                    for (j = 0; j < IMG_SIZE; j++)
                        if (tmp[j] != model[j]) {
                            printf("        first divergence at %llu "
                                   "(got %02x want %02x)\n",
                                   (unsigned long long)j, tmp[j], model[j]);
                            break;
                        }
                }
                ok(same, "  rest of the image untouched");
            }
        }
        free(src);
    }

    /* ---- sequential seek/read/write, as volume.c uses it ---- */
    {
        unsigned char a[300], b[300];
        for (i = 0; i < 300; i++) a[i] = (unsigned char)(i ^ 0x5A);
        ok(blkio_seek(&io, 12345) == 0, "seek");
        ok(blkio_write(&io, a, 300) == 0, "sequential write");
        memcpy(model + 12345, a, 300);
        ok(blkio_write(&io, a, 300) == 0, "sequential write advances the cursor");
        memcpy(model + 12645, a, 300);
        ok(blkio_seek(&io, 12345) == 0, "seek back");
        ok(blkio_read(&io, b, 300) == 0, "sequential read");
        ok(memcmp(a, b, 300) == 0, "  first block matches");
        ok(blkio_read(&io, b, 300) == 0, "sequential read advances");
        ok(memcmp(a, b, 300) == 0, "  second block matches");
        ok(blkio_pread(&io, 0, tmp, IMG_SIZE) == 0, "full read");
        ok(memcmp(tmp, model, IMG_SIZE) == 0, "image still matches the model");
    }

    /* ---- bounds ---- */
    {
        unsigned char one[8];
        ok(blkio_pread(&io, IMG_SIZE - 4, one, 4) == 0, "read the last 4 bytes");
        ok(blkio_pwrite(&io, IMG_SIZE - 4, one, 4) == 0, "write the last 4 bytes");
        ok(blkio_pread(&io, IMG_SIZE, one, 4) != 0, "read past the end fails");
        ok(blkio_pwrite(&io, IMG_SIZE, one, 4) != 0, "write past the end fails");
        ok(blkio_pwrite(&io, IMG_SIZE - 2, one, 8) != 0,
           "write crossing the end fails");
        ok(blkio_chsize(&io, IMG_SIZE + 1) == -BLKIO_E_TOOSMALL,
           "chsize larger than a device is refused");
        ok(blkio_chsize(&io, IMG_SIZE) == 0, "chsize equal to a device is fine");
    }

    blkio_close(&io);

    /* ---- and, finally, against the real file contents ---- *
     * Everything above went through the bounce layer, including the reads. If
     * the layer had a symmetric bug -- writing to the wrong place and reading
     * from the same wrong place -- it would have passed. Read the file as an
     * ordinary file and compare. */
    {
        FILE *f = fopen(path, "rb");
        ok(f != NULL, "reopen as a plain file");
        if (f) {
            size_t got = fread(tmp, 1, IMG_SIZE, f);
            fclose(f);
            ok(got == IMG_SIZE, "plain read returns the whole image");
            {
                int same = memcmp(tmp, model, IMG_SIZE) == 0;
                if (!same) {
                    size_t j;
                    for (j = 0; j < IMG_SIZE; j++)
                        if (tmp[j] != model[j]) {
                            printf("        first divergence at %llu "
                                   "(got %02x want %02x)\n",
                                   (unsigned long long)j, tmp[j], model[j]);
                            break;
                        }
                }
                ok(same, "on-disk bytes match the model exactly");
            }
        }
    }

    free(model);
    free(tmp);
    remove(path);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
