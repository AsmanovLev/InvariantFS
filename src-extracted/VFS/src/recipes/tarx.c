/*
 * tarx.c — tar container extraction/rebuild for InvariantFS.
 *
 * A tar archive is a stream of 512-byte records:
 *   [512B header][data padded to 512][...][trailer: N zero blocks]
 *
 * We split it into members WITHOUT interpreting them (no longname/PAX
 * expansion, no checksum rewriting): the exact header bytes are kept in
 * the recipe, payload bytes go to sibling inodes "name!partN" (each
 * compressed by the algorithm best for its content), inter-member padding
 * becomes a zero-slot when it is all zeros. Rebuild is a byte-for-byte
 * reassembly -> invariant 1:1.
 *
 * Recipe format (IVFT v1):
 *   [4B "IVFT"][1B ver=1][8B total_len][4B nparts]
 *   per member:
 *     [512B header_orig][8B data_len][1B pad_kind][2B pad_len]
 *   [8B trailer_len][1B trailer_kind]     (kind: 1=zeros, 0=bytes follow)
 *   [trailer bytes, only when trailer_kind==0]
 *
 * Members: header_orig is copied verbatim; data_len is the payload size
 * parsed from the octal/base-256 size field; pad_len = (512 - data_len%512)
 * % 512; pad_kind=1 when the pad bytes are all zero. Payload (+ pad, when
 * pad_kind==0) lives in "name!partN".
 *
 * trailer_kind=1: rebuild zero-fills trailer_len bytes (standard: 2*512).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _MSC_VER
#include <stdio.h>
#endif

typedef struct {
    uint8_t  header[512];   /* exact original header bytes */
    uint64_t data_off;      /* offset of payload in the original tar */
    uint64_t data_len;      /* payload length (from size field) */
    uint8_t  pad_kind;      /* 1 = pad bytes are all zero */
    uint16_t pad_len;       /* 0..511 */
} tarx_member;

static uint64_t tarx_rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

static void tarx_wr64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; }
}

static uint16_t tarx_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void tarx_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8);
}

/* parse octal (or GNU base-256) size field: 12 bytes at offset 124.
   Old tars (Silesia mozilla/xml are V7-style) pad the field with leading
   SPACES instead of zeros -- skip them, or the size reads as 0 and the
   extractor stops at the first header. */
static uint64_t tarx_parse_size(const uint8_t *h)
{
    const uint8_t *f = h + 124;
    int i = 0;
    if (f[0] & 0x80) /* GNU base-256, 8 significant bytes */
        return tarx_rd64(f + 1);
    while (i < 11 && f[i] == ' ') i++;
    uint64_t v = 0;
    for (; i < 11; i++) {
        if (f[i] == 0 || f[i] == ' ') break;
        if (f[i] < '0' || f[i] > '7') break;
        v = (v << 3) | (uint64_t)(f[i] - '0');
    }
    return v;
}

/* checksum: sum of header bytes with the checksum field treated as
   spaces (POSIX) or NULs (GNU). Header valid if either matches. The field
   itself may be space-padded on the left (old tars) -- skip spaces. */
static int tarx_check_checksum(const uint8_t *h)
{
    unsigned sp = 0, nu = 0, want = 0;
    for (int i = 0; i < 512; i++) {
        if (i >= 148 && i < 156) { sp += ' '; continue; }
        nu += h[i]; sp += h[i];
    }
    for (int i = 0; i < 8; i++) {
        unsigned c = h[148 + i];
        if (c == ' ') continue;
        if (c >= '0' && c <= '7') want = (want << 3) | (unsigned)(c - '0');
        else if (c >= 0x80) want = (want << 3) | (unsigned)(c & 7); /* base-256 */
        else break;
    }
    return (want == sp) || (want == nu);
}

/* split tar into members. Returns 0 on success. Trailer: the zero (or
   not) records at the end; *trailer gets bytes only when non-zero. */
int tarx_extract(const uint8_t *tar, size_t tar_len,
                 tarx_member **members_out, size_t *n_out,
                 uint8_t **trailer_out, size_t *trailer_len_out)
{
    size_t pos = 0;
    size_t cap = 64, n = 0;
    tarx_member *m = (tarx_member *)calloc(cap, sizeof(tarx_member));
    if (!m) return -1;

    while (pos + 512 <= tar_len) {
        const uint8_t *h = tar + pos;
        int all_zero = 1;
        for (int i = 0; i < 512; i++) if (h[i]) { all_zero = 0; break; }
        if (all_zero) break; /* trailer begins */
        if (!tarx_check_checksum(h)) break; /* not a tar record */
        uint64_t dlen = tarx_parse_size(h);
        if (dlen > tar_len - pos - 512) { /* truncated/corrupt: stop */
            if (dlen == 0) break;
            break;
        }
        if (n == cap) {
            cap *= 2;
            tarx_member *nm = (tarx_member *)realloc(m, cap * sizeof(tarx_member));
            if (!nm) { free(m); return -1; }
            m = nm;
        }
        memcpy(m[n].header, h, 512);
        m[n].data_off = (uint64_t)(pos + 512);
        m[n].data_len = dlen;
        uint16_t pad = (uint16_t)((512 - (size_t)(dlen % 512)) % 512);
        m[n].pad_len = pad;
        m[n].pad_kind = 1;
        if (pad) {
            const uint8_t *p = tar + pos + 512 + (size_t)dlen;
            for (uint16_t j = 0; j < pad; j++)
                if (p[j]) { m[n].pad_kind = 0; break; }
        }
        n++;
        pos += 512 + (size_t)dlen + pad;
    }

    /* trailer = bytes from pos to end */
    size_t tlen = tar_len - pos;
    uint8_t *tr = NULL;
    if (tlen) {
        int all_zero = 1;
        for (size_t i = pos; i < tar_len; i++)
            if (tar[i]) { all_zero = 0; break; }
        if (!all_zero) {
            tr = (uint8_t *)malloc(tlen);
            if (!tr) { free(m); return -1; }
            memcpy(tr, tar + pos, tlen);
        }
    }
    *members_out = m;
    *n_out = n;
    *trailer_out = tr;
    *trailer_len_out = tlen;
    return 0;
}

/* build the IVFT recipe from parsed members. trailer==NULL -> zeros. */
int tarx_build_recipe(const tarx_member *m, size_t n,
                      const uint8_t *trailer, size_t trailer_len,
                      uint8_t **recipe_out, size_t *rlen_out)
{
    size_t per = 512 + 8 + 1 + 2; /* header + dlen + pad_kind + pad_len */
    size_t rl = 17 + n * per + 9 + (trailer ? trailer_len : 0);
    uint8_t *r = (uint8_t *)malloc(rl);
    if (!r) return -1;
    size_t o = 0;
    memcpy(r + o, "IVFT", 4); o += 4;
    r[o++] = 1; /* ver */
    /* total_len: original tar size */
    size_t total = 512; /* at least one block */
    if (n) {
        total = (size_t)(m[n - 1].data_off + m[n - 1].data_len + m[n - 1].pad_len + 512);
        /* data_off points at payload; block after member = data_off + dlen + pad */
    }
    tarx_wr64(r + o, (uint64_t)total); o += 8;
    tarx_wr16(r + o, (uint16_t)n); o += 2;
    for (size_t i = 0; i < n; i++) {
        memcpy(r + o, m[i].header, 512); o += 512;
        tarx_wr64(r + o, m[i].data_len); o += 8;
        r[o++] = m[i].pad_kind;
        tarx_wr16(r + o, m[i].pad_len); o += 2;
    }
    tarx_wr64(r + o, (uint64_t)trailer_len); o += 8;
    r[o++] = trailer ? 0 : 1;
    if (trailer) { memcpy(r + o, trailer, trailer_len); o += trailer_len; }
    *recipe_out = r;
    *rlen_out = o;
    return 0;
}

/* parse recipe back into members. Returns 0 on success; trailer==NULL
   when trailer_kind==1 (zeros). */
int tarx_parse_recipe(const uint8_t *r, size_t rlen,
                      tarx_member **members_out, size_t *n_out,
                      uint8_t **trailer_out, size_t *trailer_len_out)
{
    if (rlen < 24 || memcmp(r, "IVFT", 4) != 0 || r[4] != 1) return -1;
    size_t n = (size_t)tarx_rd16(r + 13);
    size_t per = 512 + 8 + 1 + 2;
    size_t need = 15 + n * per;
    if (need + 9 > rlen) return -1;
    tarx_member *m = (tarx_member *)calloc(n ? n : 1, sizeof(tarx_member));
    if (!m) return -1;
    size_t o = 15;
    for (size_t i = 0; i < n; i++) {
        memcpy(m[i].header, r + o, 512); o += 512;
        m[i].data_len = tarx_rd64(r + o); o += 8;
        m[i].pad_kind = r[o++];
        m[i].pad_len = tarx_rd16(r + o); o += 2;
        m[i].data_off = 0;
    }
    uint64_t tlen = tarx_rd64(r + o); o += 8;
    uint8_t tkind = r[o++];
    uint8_t *tr = NULL;
    if (tkind == 0) {
        if ((size_t)tlen > rlen - o) { free(m); return -1; }
        tr = (uint8_t *)malloc((size_t)tlen);
        if (!tr) { free(m); return -1; }
        memcpy(tr, r + o, (size_t)tlen);
        o += (size_t)tlen;
    }
    *members_out = m;
    *n_out = n;
    *trailer_out = tr;
    *trailer_len_out = (size_t)tlen;
    return 0;
}

/* how many "name!partN" inodes a recipe references */
int tarx_recipe_num_parts(const uint8_t *r, size_t rlen)
{
    if (rlen < 27 || memcmp(r, "IVFT", 4) != 0 || r[4] != 1) return -1;
    return (int)tarx_rd16(r + 13);
}

/* rebuild the tar byte-for-byte. parts[i] holds member payload (+pad when
   pad_kind==0), plens[i] its length. trailer==NULL -> zero-fill. */
int tarx_rebuild(const tarx_member *m, size_t n,
                 const uint8_t *trailer, size_t trailer_len,
                 const uint8_t *const *parts, const size_t *plens,
                 uint8_t **out, size_t *olen)
{
    size_t total = 0;
    for (size_t i = 0; i < n; i++) total += 512 + (size_t)m[i].data_len + m[i].pad_len;
    total += trailer_len;
    uint8_t *o = (uint8_t *)malloc(total ? total : 1);
    if (!o) return -1;
    size_t p = 0;
    for (size_t i = 0; i < n; i++) {
        memcpy(o + p, m[i].header, 512); p += 512;
        size_t dl = (size_t)m[i].data_len;
        if (parts[i] && plens[i] >= dl) memcpy(o + p, parts[i], dl);
        else memset(o + p, 0, dl);
        p += dl;
        if (m[i].pad_kind == 1) {
            memset(o + p, 0, m[i].pad_len); p += m[i].pad_len;
        } else {
            if (parts[i] && plens[i] >= dl + m[i].pad_len)
                memcpy(o + p, parts[i] + dl, m[i].pad_len);
            p += m[i].pad_len;
        }
    }
    if (trailer_len) {
        if (trailer) memcpy(o + p, trailer, trailer_len);
        else memset(o + p, 0, trailer_len);
        p += trailer_len;
    }
    *out = o;
    *olen = p;
    return 0;
}
