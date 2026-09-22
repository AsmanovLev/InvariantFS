/* invf-zip: recursive AST reconstruction — read ZIP members from a
 * container file inside an InvariantFS image via byte-range remapping.
 *
 *   invf-zip list <image> <zipname>
 *   invf-zip get  <image> <zipname> <member> <out>
 *
 * The container itself stays compressed on disk (LZ4/ZSTD/JXL/...);
 * each member is decoded on demand through vol_read_range (AST recipe).
 * Decompression: method 0 = stored, 8 = deflate (zlib).
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "volume.h"
#define MINIZ_HEADER_FILE_ONLY
#define MINIZ_NO_ARCHIVE_APIS
#include "miniz.c"  /* amalgamated: full impl in one TU */

#define ZIP_EOCD   0x06054b50u
#define ZIP_CEN    0x02014b50u
#define ZIP_LOCAL  0x04034b50u

typedef struct {
    char     name[256];
    uint32_t usize;
    uint32_t csize;
    uint16_t method;
    uint32_t local_off;
    uint32_t crc;
} zip_member;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* parse EOCD + central directory from a full ZIP buffer */
static int zip_parse(const uint8_t *z, size_t zlen, zip_member *m, int maxm, int *nm)
{
    if (zlen < 22) { fprintf(stderr, "zip: buffer too small for EOCD\n"); return -1; }
    size_t eocd = zlen >= 22 ? zlen - 22 : 0;
    while (eocd > 0 && rd32(z + eocd) != ZIP_EOCD) eocd--;
    if (rd32(z + eocd) != ZIP_EOCD) { fprintf(stderr, "zip: EOCD not found\n"); return -1; }
    uint16_t ncen = rd16(z + eocd + 10);
    uint32_t cen_off = rd32(z + eocd + 16);
    if (cen_off >= zlen) { fprintf(stderr, "zip: bad central dir offset\n"); return -1; }

    int n = 0;
    uint32_t p = cen_off;
    for (int i = 0; i < ncen; i++) {
        if (p + 46 > zlen || rd32(z + p) != ZIP_CEN) { fprintf(stderr, "zip: bad CEN entry %d\n", i); break; }
        uint16_t method = rd16(z + p + 10);
        uint32_t crc = rd32(z + p + 16);
        uint32_t csize = rd32(z + p + 20);
        uint32_t usize = rd32(z + p + 24);
        uint16_t nlen = rd16(z + p + 28);
        uint16_t elen = rd16(z + p + 30);
        uint16_t clen = rd16(z + p + 32);
        uint32_t local_off = rd32(z + p + 42);
        if (n < maxm && nlen < sizeof(m[n].name)) {
            if (p + 46 + nlen > zlen) { fprintf(stderr, "zip: CEN name extends past buffer\n"); return -1; }
            memcpy(m[n].name, z + p + 46, nlen);
            m[n].name[nlen] = 0;
            m[n].method = method;
            m[n].crc = crc;
            m[n].csize = csize;
            m[n].usize = usize;
            m[n].local_off = local_off;
            n++;
        }
        p += 46 + nlen + elen + clen;
    }
    *nm = n;
    return 0;
}

static int member_data(const uint8_t *z, size_t zlen, const zip_member *m,
                       uint8_t **out, size_t *outlen)
{
    if (m->local_off + 30 > zlen || rd32(z + m->local_off) != ZIP_LOCAL) {
        fprintf(stderr, "zip: bad local header for %s\n", m->name);
        return -1;
    }
    uint16_t nlen = rd16(z + m->local_off + 26);
    uint16_t elen = rd16(z + m->local_off + 28);
    if (m->local_off + 30 + (size_t)nlen + (size_t)elen > zlen) {
        fprintf(stderr, "zip: local header extends past buffer "
                "(off=%zu nlen=%u elen=%u zlen=%zu)\n",
                (size_t)m->local_off, nlen, elen, zlen);
        return -1;
    }
    const uint8_t *data = z + m->local_off + 30 + nlen + elen;
    if (m->method == 0) {  /* stored */
        if ((size_t)m->usize > zlen - (size_t)(data - z)) {
            fprintf(stderr, "zip: stored member out of bounds\n");
            return -1;
        }
        *out = (uint8_t *)malloc(m->usize ? m->usize : 1);
        memcpy(*out, data, m->usize);
        *outlen = m->usize;
        return 0;
    }
    if (m->method == 8) {  /* deflate — raw stream in ZIP */
        if ((size_t)m->csize > zlen - (size_t)(data - z)) {
            fprintf(stderr, "zip: deflate stream out of bounds\n");
            return -1;
        }
        size_t want = m->usize ? m->usize : (size_t)m->csize * 4 + 64;
        *out = (uint8_t *)malloc(want ? want : 1);
        size_t got = tinfl_decompress_mem_to_mem(*out, want, data, m->csize,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (got != want) {
            fprintf(stderr, "zip: inflate failed (got %zu want %zu)\n", got, want);
            free(*out);
            return -1;
        }
        *outlen = got;
        return 0;
    }
    fprintf(stderr, "zip: unsupported method %u\n", m->method);
    return -1;
}

#ifdef ZTEST_STANDALONE
/* stubs so zip.c compiles without volume.c */
invfs_volume *vol_open(const char *p, int *e) { (void)p; (void)e; return 0; }
int vol_read_file(invfs_volume *vol, uint64_t ino, uint8_t **out, size_t *outlen) { (void)vol;(void)ino;(void)out;(void)outlen; return -1; }
int vol_find2_dummy;
uint64_t vol_find(invfs_volume *vol, const char *name) { (void)vol; (void)name; return 0; }
int vol_stat(invfs_volume *vol, const char *name, uint64_t *sz) { (void)vol;(void)name;(void)sz; return -1; }
void vol_close(invfs_volume *vol) { (void)vol; }
#endif

static uint64_t find_inode(invfs_volume *vol, const char *name)
{
    uint64_t ino = vol_find(vol, name);
    if (!ino) {
        uint64_t sz;
        if (vol_stat(vol, name, &sz) != 0) return 0;
        ino = vol_find(vol, name);
    }
    return ino;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "usage: invf-zip list <image> <zipname>\n"
                            "       invf-zip get  <image> <zipname> <member> <out>\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc < 4) {
        fprintf(stderr, "usage: invf-zip list <image> <zipname>\n"
                        "       invf-zip get  <image> <zipname> <member> <out>\n");
        return 2;
    }
    const char *cmd = argv[1];
    const char *img = argv[2];
    const char *zname = argv[3];

    int err = 0;
    invfs_volume *vol = vol_open(img, &err);
    if (!vol) { fprintf(stderr, "cannot open %s\n", img); return 1; }

    uint8_t *z = NULL;
    size_t zlen = 0;
    if (vol_read_file(vol, find_inode(vol, zname), &z, &zlen) != 0) {
        fprintf(stderr, "cannot read %s from image\n", zname);
        vol_close(vol);
        return 1;
    }

    static zip_member mem[4096];  /* 1.1MB — heap, not stack */
    int nm = 0;
    if (zip_parse(z, zlen, mem, 4096, &nm) != 0) {
        free(z); vol_close(vol);
        return 1;
    }

    if (strcmp(cmd, "list") == 0) {
        invfs_ast_child_entry *ch = NULL;
        size_t nch = 0;
        printf("members of %s (%zu bytes):\n", zname, zlen);
        /* prefer AST children (fast, already reconstructed);
         * fall back to parsing the ZIP stream */
        if (vol_get_children(vol, find_inode(vol, zname), &ch, &nch) == 0 && nch > 0) {
            for (size_t i = 0; i < nch; i++)
                printf("  %-48s %10u bytes  %s (crc %08x)\n",
                       ch[i].name, ch[i].usize,
                       ch[i].method == 0 ? "stored" : ch[i].method == 8 ? "deflate" : "?",
                       ch[i].crc);
            printf("%zu member(s) from AST\n", nch);
            free(ch);
        } else {
            for (int i = 0; i < nm; i++)
                printf("  %-48s %10u bytes  %s\n", mem[i].name, mem[i].usize,
                       mem[i].method == 0 ? "stored" : mem[i].method == 8 ? "deflate" : "?");
            printf("%d member(s)\n", nm);
        }
    } else if (strcmp(cmd, "get") == 0 && argc >= 6) {
        const char *member = argv[4];
        const char *outf = argv[5];
        int found = -1;
        for (int i = 0; i < nm; i++)
            if (strcmp(mem[i].name, member) == 0) { found = i; break; }
        if (found < 0) { fprintf(stderr, "zip: member '%s' not found\n", member); free(z); vol_close(vol); return 1; }
        uint8_t *data = NULL;
        size_t dlen = 0;
        if (member_data(z, zlen, &mem[found], &data, &dlen) != 0) {
            free(z); vol_close(vol);
            return 1;
        }
        FILE *f = fopen(outf, "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", outf); free(data); free(z); vol_close(vol); return 1; }
        if (fwrite(data, 1, dlen, f) != dlen || fclose(f) != 0) {
            fprintf(stderr, "write failed: %s\n", outf);
            free(data); free(z); vol_close(vol); return 1;
        }
        printf("extracted '%s' -> %s (%zu bytes)\n", member, outf, dlen);
        free(data);
    } else {
        fprintf(stderr, "bad command\n");
        free(z); vol_close(vol);
        return 2;
    }

    free(z);
#ifndef ZTEST_STANDALONE
    vol_close(vol);
#endif
    return 0;
}
