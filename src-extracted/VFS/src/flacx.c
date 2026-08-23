/*
 * flacx.c — InvariantFS FLAC recipe tool (byte-exact lossless transcode)
 *
 *   flacx extract <in.flac> <recipe.bin>        (parse frames -> binary recipe)
 *   flacx rebuild <in.wav> <recipe.bin> <out.flac>  (PCM + recipe -> exact FLAC)
 *
 * Invariant: rebuild(ffmpeg FLAC->WAV, extract(FLAC)) == FLAC byte-for-byte.
 * The recipe keeps every frame header bit verbatim (predictor type/order/
 * coefficients, shift, Rice partition params, channel modes, wasted bits);
 * residual samples are recomputed from PCM, which is deterministic.
 *
 * Binary recipe format (little-endian, version 1):
 *   [4B magic "IVFR"][1B version][4B hlen][hlen raw bytes: "fLaC"..first frame]
 *   [8B total_samples][2B min_block][2B max_block][4B sample_rate]
 *   [1B channels][1B bps][4B frame_count]
 *   per frame:
 *     [1B bs_code][1B sr_code][1B ch_assign][1B ss_code][8B sample_num]
 *     [2B block_size][2B bs_header][4B sample_rate]
 *     per subframe (channels from ch_assign):
 *       [1B type][1B wasted][1B bps_sub]
 *       type 0 (CONSTANT): [4B val]
 *       type 1 (VERBATIM): nothing (samples come from PCM)
 *       type 8..12 (FIXED): [1B order][order x 4B warmup]
 *       type 32..63 (LPC):  [1B order][1B prec][1B shift]
 *                           [order x 4B coeff][order x 4B warmup]
 *       residual (FIXED/LPC): [1B rmethod][1B porder][nparts x 1B ks]
 * All sample/coeff values are the FULL values (already shifted by wasted).
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------- bit I/O ---------------- */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;   /* bit position */
} br_t;

static int br_get(br_t *b, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (b->pos >= b->len * 8) return 0;  /* stream end: zeros */
        v = (v << 1) | ((b->data[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

/* bw_t: dynamic bit writer. Buffer lives on the HEAP (a 1 MiB stack array
   here caused STATUS_STACK_OVERFLOW 0xC00000FD at function entry: MSVC's
   default stack is exactly 1 MiB, so `bw_t bw;` as a local blew it before
   the first statement ran). */
typedef struct {
    uint8_t *buf;
    size_t bits;
    size_t cap;   /* allocated bytes */
} bw_t;

static void bw_init(bw_t *b) { b->buf = NULL; b->bits = 0; b->cap = 0; }
static void bw_free(bw_t *b) { free(b->buf); b->buf = NULL; b->bits = 0; b->cap = 0; }
static void bw_grow(bw_t *b, size_t need)
{
    if (need <= b->cap) return;
    size_t nc = b->cap ? b->cap : 65536;
    while (nc < need) nc *= 2;
    uint8_t *nb = (uint8_t *)realloc(b->buf, nc);
    if (!nb) { fprintf(stderr, "[bw] OOM need=%zu\n", need); exit(1); }
    b->buf = nb;
    b->cap = nc;
}

static void bw_put(bw_t *b, uint32_t v, int n)
{
    bw_grow(b, (b->bits + (size_t)n + 7) >> 3);
    size_t pos = b->bits >> 3;
    int sh = (int)(b->bits & 7);
    for (int i = n - 1; i >= 0; i--) {
        if ((v >> i) & 1) {
            b->buf[pos] |= (uint8_t)(0x80 >> sh);
        } else {
            b->buf[pos] &= (uint8_t)~(0x80 >> sh);
        }
        if (++sh == 8) { sh = 0; pos++; }
    }
    b->bits += (size_t)n;
}

/* bulk zero-write (Rice unary runs) — memset fast path */
static void bw_zeros(bw_t *b, size_t n)
{
    if (!n) return;
    bw_grow(b, (b->bits + n + 7) >> 3);
    size_t pos = b->bits >> 3;
    int sh = (int)(b->bits & 7);
    while (sh != 0 && n) {
        b->buf[pos] &= (uint8_t)~(0x80 >> sh);
        sh++; n--;
        if (sh == 8) { sh = 0; pos++; }
    }
    if (n >= 8) {
        memset(b->buf + pos, 0, n >> 3);
        pos += n >> 3;
        n &= 7;
    }
    while (n--) {
        b->buf[pos] &= (uint8_t)~(0x80 >> sh);
        if (++sh == 8) { sh = 0; pos++; }
    }
    b->bits = (pos << 3) + sh;
}
static void bw_align(bw_t *b)
{
    while (b->bits & 7) bw_put(b, 0, 1);
}
static size_t bw_size(const bw_t *b) { return b->bits / 8; }

/* ---------------- CRCs (RFC 9639: CRC-8 poly 0x07, CRC-16 poly 0x8005) ------- */
static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
    }
    return c;
}
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x8005) : (uint16_t)(c << 1);
    }
    return c;
}

/* ---------------- binary recipe helpers ---------------- */
static void wr8(uint8_t **p, uint8_t v)  { *(*p)++ = v; }
static void wr16(uint8_t **p, uint16_t v){ *(*p)++ = v & 0xFF; *(*p)++ = v >> 8; }
static void wr32(uint8_t **p, uint32_t v){ *(*p)++ = v & 0xFF; *(*p)++ = (v>>8)&0xFF; *(*p)++ = (v>>16)&0xFF; *(*p)++ = v >> 24; }
static void wr64(uint8_t **p, uint64_t v){ for (int i = 0; i < 8; i++) *(*p)++ = (v >> (8*i)) & 0xFF; }
static uint8_t  rd8(const uint8_t **p)  { return *(*p)++; }
static uint16_t rd16(const uint8_t **p) { uint16_t v = (uint16_t)((*p)[0] | ((*p)[1] << 8)); *p += 2; return v; }
static uint32_t rd32(const uint8_t **p) { uint32_t v = (uint32_t)((*p)[0] | ((*p)[1]<<8) | ((*p)[2]<<16) | ((uint32_t)(*p)[3]<<24)); *p += 4; return v; }
static uint64_t rd64(const uint8_t **p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)(*p)[i] << (8*i); *p += 8; return v; }

/* frame/channel mode */
static int frame_channels(int ch_assign)
{
    if (ch_assign <= 7) return ch_assign + 1;
    if (ch_assign <= 10) return 2;
    if (ch_assign == 11) return 8;
    return -1;
}
/* subframe bps: mid/side side channel (1) uses bps+1; right/side ch0 too */
static int sub_bps(int ch_assign, int ch, int bps)
{
    /* side channel carries one extra bit (RFC 9639 / ffmpeg):
       8=left/side  -> ch1=side
       9=right/side -> ch0=side  (!!)
       10=mid/side  -> ch1=side */
    if (ch == 1 && (ch_assign == 8 || ch_assign == 10)) return bps + 1;
    if (ch == 0 && ch_assign == 9) return bps + 1;
    return bps;
}

/* ---------------- recipe struct (parsed) ---------------- */
typedef struct {
    int32_t *warmup;   /* order */
    int32_t *coeff;    /* order (LPC) */
    uint8_t *ks;       /* nparts */
    uint8_t type, wasted, bps_sub;
    uint8_t order, prec, rmethod, porder;
    int8_t  shift;
    int32_t val;       /* CONSTANT */
} subframe_t;

typedef struct {
    uint64_t sample_num;
    uint32_t sample_rate;
    uint16_t block_size, bs_header;
    uint8_t bs_code, sr_code, ch_assign, ss_code;
    subframe_t *sf;
    int nsub;
} frame_t;

typedef struct {
    uint8_t *header;          /* metadata blocks WITHOUT cover slots, all last=0 */
    size_t hlen;              /* FULL header size (fLaC..first frame) */
    size_t hlen_wo;           /* header size without cover-slot bytes */
    uint64_t total_samples;
    uint16_t min_block, max_block;
    uint32_t sample_rate;
    uint8_t channels, bps;
    frame_t *frames;
    int nframes;
    struct flacx_cover { uint32_t offset, len; uint8_t kind; uint8_t *data; } *covers;
    uint32_t n_covers;        /* number of slots (PICTURE + non-zero PADDING) */
} recipe_t;

typedef struct flacx_cover flacx_cover;

/* ---------------- WAV reader (8/16/24-bit) ---------------- */
/* WAV parser over an in-memory buffer (8/16/24-bit PCM) */
static uint8_t *read_file(const char *path, size_t *len);
static int32_t *read_wav_mem(const uint8_t *buf, size_t sz, int *out_n, int *out_ch, int *out_bits)
{
    if (sz < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return NULL;

    int ch = 0, bits = 0;
    size_t pos = 12;
    while (pos + 8 <= sz) {
        uint32_t clen = (uint32_t)(buf[pos+4] | (buf[pos+5]<<8) | (buf[pos+6]<<16) | ((uint32_t)buf[pos+7]<<24));
        if (!memcmp(buf + pos, "fmt ", 4) && clen >= 16) {
            ch = (int)(buf[pos+10] | (buf[pos+11]<<8));
            bits = (int)(buf[pos+22] | (buf[pos+23]<<8));
        }
        pos += 8 + clen + (clen & 1);
    }
    if (!ch || !bits) return NULL;

    int32_t *samples = NULL;
    int n = 0;
    pos = 12;
    while (pos + 8 <= sz) {
        uint32_t clen = (uint32_t)(buf[pos+4] | (buf[pos+5]<<8) | (buf[pos+6]<<16) | ((uint32_t)buf[pos+7]<<24));
        if (!memcmp(buf + pos, "data", 4)) {
            const uint8_t *d = buf + pos + 8;
            size_t dn = clen;
            if (pos + 8 + dn > sz) dn = sz - pos - 8;
            if (bits == 16) {
                n = (int)(dn / 2);
                samples = (int32_t *)malloc((size_t)n * 4);
                for (int i = 0; i < n; i++) samples[i] = (int16_t)(d[2*i] | (d[2*i+1] << 8));
            } else if (bits == 24) {
                n = (int)(dn / 3);
                samples = (int32_t *)malloc((size_t)n * 4);
                for (int i = 0; i < n; i++) {
                    int32_t v = (int32_t)(d[3*i] | (d[3*i+1] << 8) | (d[3*i+2] << 16));
                    if (v >= (1 << 23)) v -= (1 << 24);
                    samples[i] = v;
                }
            } else if (bits == 8) {
                n = (int)dn;
                samples = (int32_t *)malloc((size_t)n * 4);
                for (int i = 0; i < n; i++) samples[i] = (int8_t)d[i];
            }
        }
        pos += 8 + clen + (clen & 1);
    }
    *out_n = n; *out_ch = ch; *out_bits = bits;
    return samples;
}

static int32_t *read_wav(const char *path, int *out_n, int *out_ch, int *out_bits)
{
    size_t sz = 0;
    uint8_t *b = read_file(path, &sz);
    if (!b) return NULL;
    int32_t *s = read_wav_mem(b, sz, out_n, out_ch, out_bits);
    free(b);
    return s;
}

/* ---------------- RECIPE EXTRACT (parse FLAC) ---------------- */
static int read_utf8(br_t *b, uint64_t *out)
{
    int bb = br_get(b, 8);
    if (!(bb & 0x80)) { *out = (uint64_t)bb; return 0; }
    int n = 0, mask = 0x80;
    while (bb & mask) { n++; mask >>= 1; }
    uint64_t v = (uint64_t)(bb & (mask - 1));
    for (int i = 0; i < n - 1; i++) v = (v << 6) | (uint64_t)(br_get(b, 8) & 0x3F);
    *out = v;
    return 0;
}

static uint32_t bs_table[16] = { 0, 192, 576, 1152, 2304, 4608, 0, 0,
                                 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
static uint32_t sr_table[16] = { 0, 88200, 176400, 192000, 8000, 16000, 22050,
                                 24000, 32000, 44100, 48000, 96000, 0, 0, 0, 0 };
static int ss_table[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };

/* read raw bytes from file into memory */
static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = (uint8_t *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)sz;
    return b;
}

/* parse one frame; returns byte offset of next frame or -1 */
static long parse_frame(const uint8_t *d, size_t n, long off,
                        const recipe_t *si, frame_t *fr)
{
    br_t br;
    br.data = d + off;
    br.len = n - (size_t)off;
    br.pos = 16;
    if (br.len < 8) return -1;
    if (br.data[0] != 0xFF || (br.data[1] & 0xFE) != 0xF8) return -1;

    fr->bs_code = (uint8_t)br_get(&br, 4);
    fr->sr_code = (uint8_t)br_get(&br, 4);
    fr->ch_assign = (uint8_t)br_get(&br, 4);
    fr->ss_code = (uint8_t)br_get(&br, 3);
    br_get(&br, 1);
    if (read_utf8(&br, &fr->sample_num) != 0) return -1;

    uint16_t bs_header;
    uint32_t block_size;
    if (fr->bs_code == 6) { bs_header = (uint16_t)br_get(&br, 8); block_size = bs_header + 1; }
    else if (fr->bs_code == 7) { bs_header = (uint16_t)br_get(&br, 16); block_size = bs_header + 1; }
    else { block_size = bs_table[fr->bs_code]; bs_header = (uint16_t)block_size; }
    fr->block_size = (uint16_t)block_size;
    fr->bs_header = bs_header;

    if (fr->sr_code == 12) fr->sample_rate = (uint32_t)br_get(&br, 8) * 1000;
    else if (fr->sr_code == 13) fr->sample_rate = (uint32_t)br_get(&br, 16) * 10;
    else if (fr->sr_code == 14) fr->sample_rate = (uint32_t)br_get(&br, 16);
    else fr->sample_rate = sr_table[fr->sr_code] ? sr_table[fr->sr_code] : si->sample_rate;

    int bps = ss_table[fr->ss_code] ? ss_table[fr->ss_code] : si->bps;

    /* verify CRC-8 over header bits */
    size_t hdr_bits = br.pos;
    uint8_t crc_stored = (uint8_t)br_get(&br, 8);
    uint8_t crc_calc = crc8(d + off, hdr_bits / 8);
#ifndef INVFS_EMBED_FLACX
    if (crc_calc != crc_stored)
        fprintf(stderr, "  [parse] crc8 fail @%ld stored=%02x calc=%02x\n",
                off, crc_stored, crc_calc);
#endif
    if (crc_calc != crc_stored) return -1;

    int channels = frame_channels(fr->ch_assign);
    if (channels <= 0) return -1;

    fr->nsub = channels;
    fr->sf = (subframe_t *)calloc((size_t)channels, sizeof(subframe_t));
    if (!fr->sf) return -1;

    for (int c = 0; c < channels; c++) {
        subframe_t *sf = &fr->sf[c];
        int hb = br_get(&br, 8);
        sf->wasted = (uint8_t)(hb & 1);
        sf->type = (uint8_t)((hb >> 1) & 0x3F);
        if (sf->wasted) {
            int cnt = 0;
            while (br_get(&br, 1) == 0) cnt++;
            sf->wasted = (uint8_t)(cnt + 1);
        }
        int bps_sub = sub_bps(fr->ch_assign, c, bps);
        sf->bps_sub = (uint8_t)bps_sub;
        int bps_eff = bps_sub - sf->wasted;
        if (sf->type == 0) {           /* CONSTANT */
            int32_t v = (int32_t)br_get(&br, bps_eff);
            v = (int32_t)((uint32_t)v << sf->wasted);
            if (v >= (1 << (bps_sub - 1))) v -= (1 << bps_sub);
            sf->val = v;
        } else if (sf->type == 1) {    /* VERBATIM: skip samples */
            br_get(&br, bps_eff * block_size);
        } else if (sf->type >= 8 && sf->type <= 12) {  /* FIXED */
            sf->order = (uint8_t)(sf->type - 8);
            sf->warmup = (int32_t *)calloc(sf->order ? sf->order : 1, 4);
            for (int i = 0; i < sf->order; i++) {
                int32_t v = (int32_t)br_get(&br, bps_eff);
                v = (int32_t)((uint32_t)v << sf->wasted);
                if (v >= (1 << (bps_sub - 1))) v -= (1 << bps_sub);
                sf->warmup[i] = v;
            }
        } else if (sf->type >= 32 && sf->type <= 63) {  /* LPC */
            sf->order = (uint8_t)(sf->type - 31);
            sf->warmup = (int32_t *)calloc(sf->order ? sf->order : 1, 4);
            sf->coeff = (int32_t *)calloc(sf->order ? sf->order : 1, 4);
            for (int i = 0; i < sf->order; i++) {
                int32_t v = (int32_t)br_get(&br, bps_eff);
                v = (int32_t)((uint32_t)v << sf->wasted);
                if (v >= (1 << (bps_sub - 1))) v -= (1 << bps_sub);
                sf->warmup[i] = v;
            }
            sf->prec = (uint8_t)(br_get(&br, 4) + 1);
            sf->shift = (int8_t)br_get(&br, 5);
            if (sf->shift >= 16) sf->shift = (int8_t)(sf->shift - 32);
            for (int i = 0; i < sf->order; i++) {
                int32_t raw = (int32_t)br_get(&br, sf->prec);
                if (raw >= (1 << (sf->prec - 1))) raw -= (1 << sf->prec);
                sf->coeff[i] = raw;   /* libFLAC: stream order = coeff[0] for x[i-1] */
            }
        } else {
#ifndef INVFS_EMBED_FLACX
            fprintf(stderr, "  [parse] invalid subframe type %u (hb=%02x) ch=%d @%ld\n",
                    sf->type, hb, c, off);
#endif
            return -1;
        }

        /* residual (FIXED / LPC) */
        if (sf->type >= 8) {
            sf->rmethod = (uint8_t)br_get(&br, 2);
            /* partition order is ALWAYS 4 bits (ks widen to 5 for Rice2) */
            sf->porder = (uint8_t)br_get(&br, 4);
            int nparts = 1 << sf->porder;
            sf->ks = (uint8_t *)calloc((size_t)nparts, 1);
            int part = (int)(block_size >> sf->porder);
            /* partition i covers samples [i*part, (i+1)*part); the first
               `order` samples of the block are warmups, so residuals in
               partition i = part - max(order - i*part, 0). When order >
               part, the leftover warmups spill into partition 1 (its
               residual count shrinks). Empty partitions carry NO k. */
#ifndef INVFS_EMBED_FLACX
            fprintf(stderr, "  [parse] frame@%ld ch=%d type=%u order=%u bps_sub=%d wasted=%u prec=%u shift=%d rmethod=%u porder=%u part=%d\n",
                    off, c, sf->type, sf->order, bps_sub, sf->wasted, sf->prec, sf->shift, sf->rmethod, sf->porder, part);
#endif
            for (int p = 0; p < nparts; p++) {
                int warm = (sf->order > p * part) ? sf->order - p * part : 0;
                int nsamp = part - warm;
                /* k is ALWAYS present, even for empty partitions (some
                   encoders write it unconditionally — ffmpeg skips it,
                   which silently corrupts these streams) */
                int kb = sf->rmethod == 0 ? 4 : 5;
                int k = (int)br_get(&br, kb);
                sf->ks[p] = (uint8_t)k;
                if (nsamp <= 0) continue;
                if (k == ((sf->rmethod == 0) ? 15 : 31)) {
                    int bits = br_get(&br, 5);
                    br_get(&br, bits * nsamp);  /* escape: raw residuals */
                } else {
                    for (int s = 0; s < nsamp; s++) {
                        int q = 0;
                        while (br_get(&br, 1) == 0) q++;
                        br_get(&br, k);
                    }
                }
            }
        }
    }

    br.pos = (br.pos + 7) & ~(size_t)7;   /* byte align */
    size_t fb = br.pos / 8;
    (void)fb;
    uint16_t crc_stored16 = (uint16_t)(br_get(&br, 16));
    uint16_t crc_calc16 = crc16(d + off, fb);
#ifndef INVFS_EMBED_FLACX
    if (crc_calc16 != crc_stored16)
        fprintf(stderr, "  [parse] crc16 fail @%ld stored=%04x calc=%04x fb=%zu\n",
                off, crc_stored16, crc_calc16, fb);
#endif
    if (crc_calc16 != crc_stored16) return -1;
    return off + (long)fb + 2;
}

int flacx_extract(const uint8_t *d, size_t n, uint8_t **recipe_out, size_t *rlen_out,
                  flacx_cover **covers_out, uint32_t *n_covers_out)
{
    if (!d || n < 8 || memcmp(d, "fLaC", 4)) { fprintf(stderr, "not flac\n"); return 1; }
    if (covers_out) *covers_out = NULL;
    if (n_covers_out) *n_covers_out = 0;

    recipe_t si;
    memset(&si, 0, sizeof si);
    size_t pos = 4;
    while (pos < n) {
        uint8_t lb = d[pos];
        uint32_t blen = (uint32_t)((d[pos+1] << 16) | (d[pos+2] << 8) | d[pos+3]);
        if (pos + 4 + blen > n) break;
        if ((lb & 0x7F) == 0 && blen >= 34) {
            const uint8_t *b = d + pos + 4;
            si.min_block = (uint16_t)((b[0] << 8) | b[1]);
            si.max_block = (uint16_t)((b[2] << 8) | b[3]);
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) v = (v << 8) | b[10 + i];
            si.sample_rate = (uint32_t)(v >> 44);
            si.channels = (uint8_t)(((v >> 41) & 7) + 1);
            si.bps = (uint8_t)(((v >> 36) & 0x1F) + 1);
            si.total_samples = v & 0xFFFFFFFFF;
        }
        pos += 4 + blen;
        if (lb & 0x80) break;
    }
    si.hlen = pos;

    /* Split metadata: PICTURE blocks (type 6) and PADDING blocks (type 1)
       are extracted into cover slots — dedup-friendly and shrinks the
       recipe: all-zero PADDING (the usual case — freed tag space) becomes
       kind=1 (len only, zero-filled on rebuild), everything else keeps its
       bytes in a coverN payload. Remaining header blocks keep last=0
       (re-set on rebuild to the final block). */
    {
        size_t p = 4;
        size_t wo = 4;   /* keep the "fLaC" marker */
        uint8_t *hdr = (uint8_t *)malloc(pos);
        if (!hdr) return 1;
        memcpy(hdr, d, 4);
        while (p < pos) {
            uint8_t lb = d[p];
            uint32_t blen = (uint32_t)((d[p+1] << 16) | (d[p+2] << 8) | d[p+3]);
            size_t blk = p + 4 + blen;
            if (((lb & 0x7F) == 6) || ((lb & 0x7F) == 1)) {  /* PICTURE / PADDING */
                /* kind=1 (zero-fill) is only legal for PADDING: a zeroed
                   PICTURE must keep its block type for bit-exact rebuild */
                int all_zero = 0;
                if ((lb & 0x7F) == 1) {
                    all_zero = 1;
                    for (size_t z = p + 4; z < blk; z++)
                        if (d[z]) { all_zero = 0; break; }
                }
                si.covers = (flacx_cover *)realloc(si.covers,
                            (si.n_covers + 1) * sizeof(flacx_cover));
                if (!si.covers) { free(hdr); return 1; }
                flacx_cover *cv = &si.covers[si.n_covers];
                cv->offset = (uint32_t)p;
                cv->len = (uint32_t)(4 + blen);
                cv->kind = all_zero ? 1 : 0;
                cv->data = NULL;
                if (!all_zero) {
                    cv->data = (uint8_t *)malloc(cv->len);
                    if (!cv->data) { free(hdr); return 1; }
                    memcpy(cv->data, d + p, cv->len);
                    cv->data[0] = lb & 0x7F;   /* last=0 (dedup-friendly) */
                }
                si.n_covers++;
            } else {
                memcpy(hdr + wo, d + p, 4 + blen);
                hdr[wo] = lb & 0x7F;       /* last=0 */
                wo += 4 + blen;
            }
            p = blk;
        }
        si.header = hdr;
        si.hlen_wo = wo;
    }

    si.frames = NULL;
    si.nframes = 0;
    long off = (long)pos;
    int cap = 0;
    while (off + 8 <= (long)n) {
        if (d[off] != 0xFF || (d[off+1] & 0xFE) != 0xF8) {
#ifndef INVFS_EMBED_FLACX
            fprintf(stderr, "  [extract] no sync @%ld (after %d frames), next: %02x %02x %02x %02x\n",
                    off, si.nframes, d[off], d[off+1], d[off+2], d[off+3]);
#endif
            break;
        }
        if (si.nframes == cap) {
            cap = cap ? cap * 2 : 256;
            si.frames = (frame_t *)realloc(si.frames, (size_t)cap * sizeof(frame_t));
        }
        frame_t *fr = &si.frames[si.nframes];
        memset(fr, 0, sizeof *fr);
        long nxt = parse_frame(d, n, off, &si, fr);
        if (nxt < 0) {
            /* corrupted frame (CRC fail / invalid structure): the file can
               NOT be reproduced bit-exactly from PCM — refuse to transcode
               (the invariant says: keep the original in that case). */
            fprintf(stderr, "frame %d parse fail @%ld — file has corrupt frames, "
                            "keeping original\n", si.nframes, off);
            for (int i = 0; i < si.nframes; i++) {
                for (int c = 0; c < si.frames[i].nsub; c++) {
                    subframe_t *sf = &si.frames[i].sf[c];
                    free(sf->warmup); free(sf->coeff); free(sf->ks);
                }
                free(si.frames[i].sf);
            }
            free(si.frames);
            free(si.header);
            for (uint32_t c = 0; c < si.n_covers; c++) free(si.covers[c].data);
            free(si.covers);
            return 1;
        }
        si.nframes++;
        off = nxt;
    }

    /* serialize (v3: cover slots [offset][len][kind] after frame count) */
    size_t total = 4 + 1 + 4 + 8 + 2 + 2 + 4 + 1 + 1 + 4 + 2 +
                   si.n_covers * 9 + si.hlen_wo;
    for (int i = 0; i < si.nframes; i++) {
        frame_t *fr = &si.frames[i];
        total += 1 + 1 + 1 + 1 + 8 + 2 + 2 + 4;
        for (int c = 0; c < fr->nsub; c++) {
            subframe_t *sf = &fr->sf[c];
            total += 3;
            if (sf->type == 0) total += 4;
            if (sf->type >= 8) {
                if (sf->type >= 32) total += 1 + 1 + 1 + (size_t)sf->order * 8;
                else total += 1 + (size_t)sf->order * 4;
                total += 1 + 1 + (size_t)(1 << sf->porder);
            }
        }
    }
    uint8_t *r = (uint8_t *)calloc(1, total);
    uint8_t *p = r;
    memcpy(p, "IVFR", 4); p += 4;
    *p++ = 3;
    wr32(&p, (uint32_t)si.hlen);          /* full header size */
    wr64(&p, si.total_samples);
    wr16(&p, si.min_block); wr16(&p, si.max_block);
    wr32(&p, si.sample_rate);
    *p++ = si.channels; *p++ = si.bps;
    wr32(&p, (uint32_t)si.nframes);
    wr16(&p, (uint16_t)si.n_covers);
    for (uint32_t c = 0; c < si.n_covers; c++) {
        wr32(&p, si.covers[c].offset);
        wr32(&p, si.covers[c].len);
        *p++ = si.covers[c].kind;         /* 0=data in coverN, 1=zeros */
    }
    memcpy(p, si.header, si.hlen_wo); p += si.hlen_wo;
    for (int i = 0; i < si.nframes; i++) {
        frame_t *fr = &si.frames[i];
        *p++ = fr->bs_code; *p++ = fr->sr_code; *p++ = fr->ch_assign; *p++ = fr->ss_code;
        wr64(&p, fr->sample_num);
        wr16(&p, fr->block_size); wr16(&p, fr->bs_header);
        wr32(&p, fr->sample_rate);
        for (int c = 0; c < fr->nsub; c++) {
            subframe_t *sf = &fr->sf[c];
            *p++ = sf->type; *p++ = sf->wasted; *p++ = sf->bps_sub;
            if (sf->type == 0) wr32(&p, (uint32_t)sf->val);
            if (sf->type >= 8) {
                if (sf->type >= 32) {
                    *p++ = sf->order; *p++ = sf->prec; *p++ = (uint8_t)sf->shift;
                    for (int j = 0; j < sf->order; j++) wr32(&p, (uint32_t)sf->coeff[j]);
                    for (int j = 0; j < sf->order; j++) wr32(&p, (uint32_t)sf->warmup[j]);
                } else {
                    *p++ = sf->order;
                    for (int j = 0; j < sf->order; j++) wr32(&p, (uint32_t)sf->warmup[j]);
                }
                *p++ = sf->rmethod; *p++ = sf->porder;
                int nparts = 1 << sf->porder;
                for (int j = 0; j < nparts; j++) *p++ = sf->ks[j];
            }
        }
    }
    *recipe_out = r;
    *rlen_out = (size_t)(p - r);
#ifndef INVFS_EMBED_FLACX
    fprintf(stderr, "recipe: %d frames, %d bytes (%d%% of source), %u cover(s)\n",
            si.nframes, (int)(p - r), (int)((p - r) * 100 / (n ? n : 1)),
            (unsigned)si.n_covers);
#endif
    if (covers_out) {
        *covers_out = si.covers;
        if (n_covers_out) *n_covers_out = si.n_covers;
    } else {
        for (uint32_t c = 0; c < si.n_covers; c++) free(si.covers[c].data);
        free(si.covers);
    }
    /* cleanup */
    for (int i = 0; i < si.nframes; i++) {
        for (int c = 0; c < si.frames[i].nsub; c++) {
            subframe_t *sf = &si.frames[i].sf[c];
            free(sf->warmup); free(sf->coeff); free(sf->ks);
        }
        free(si.frames[i].sf);
    }
    free(si.frames);
    free(si.header);
    return 0;
}

/* number of cover payloads referenced by a recipe (0 for v1 recipes) */
/* number of DATA cover payloads (kind=0) referenced by a recipe.
   v1: 0; v2: all slots carry data; v3: only kind=0 slots. */
int flacx_recipe_num_covers(const uint8_t *r, size_t rn)
{
    if (!r || rn < 33 || memcmp(r, "IVFR", 4)) return 0;
    if (r[4] < 2) return 0;
    int nslots = (int)(r[31] | (r[32] << 8));
    if (r[4] == 2) return nslots;
    if (nslots < 0 || (size_t)(33 + nslots * 9) > rn) return 0;
    int n = 0;
    for (int i = 0; i < nslots; i++)
        if (r[33 + i * 9 + 8] == 0) n++;   /* kind byte */
    return n;
}
/* flacx_rebuild_part.c — rebuild + main (appended to flacx.c) */

static int32_t unzigzag(int32_t m) { return (m & 1) ? -(m / 2) - 1 : m / 2; }
static int64_t zigzag64(int64_t v) { return v < 0 ? -2 * v - 1 : 2 * v; }

static void put_utf8(bw_t *b, uint64_t v)
{
    if (v < 0x80) bw_put(b, (uint32_t)v, 8);
    else if (v < 0x800) { bw_put(b, 0xC0 | (uint32_t)(v >> 6), 8); bw_put(b, 0x80 | (uint32_t)(v & 0x3F), 8); }
    else if (v < 0x10000) { bw_put(b, 0xE0 | (uint32_t)(v >> 12), 8); bw_put(b, 0x80 | (uint32_t)((v >> 6) & 0x3F), 8); bw_put(b, 0x80 | (uint32_t)(v & 0x3F), 8); }
    else if (v < 0x200000) { bw_put(b, 0xF0 | (uint32_t)(v >> 18), 8); bw_put(b, 0x80 | (uint32_t)((v >> 12) & 0x3F), 8); bw_put(b, 0x80 | (uint32_t)((v >> 6) & 0x3F), 8); bw_put(b, 0x80 | (uint32_t)(v & 0x3F), 8); }
}

static const int32_t fixed_coeffs[5][4] = {
    {0}, {1}, {2, -1}, {3, -3, 1}, {4, -6, 4, -1}
};

static int write_residuals(bw_t *bw, const subframe_t *sf, const int64_t *res, int nres)
{
    bw_put(bw, sf->rmethod, 2);
    bw_put(bw, sf->porder, 4);   /* partition order: always 4 bits */
    int nparts = 1 << sf->porder;
    int kbit = sf->rmethod == 0 ? 4 : 5;
    int esc = sf->rmethod == 0 ? 15 : 31;
    int bs = nres + sf->order;
    int part = bs >> sf->porder;
    int first = (sf->porder > 0) ? part - sf->order : nres;
    for (int p = 0; p < nparts; p++) {
        int warm = (sf->order > p * part) ? sf->order - p * part : 0;
        int n = part - warm;
        /* k is written even for empty partitions (mirror of the parse
           side: some encoders write it unconditionally) */
        int k = sf->ks[p];
        bw_put(bw, (uint32_t)k, kbit);
        if (n <= 0) continue;
        int lo = (p * part > sf->order) ? p * part - sf->order : 0;
        if (k == esc) {
            int bits = 1;
            for (int i = 0; i < n; i++) {
                int64_t a = res[lo + i] < 0 ? -res[lo + i] : res[lo + i];
                int bl = 1;
                while ((a >> bl) != 0) bl++;
                if (bl + 1 > bits) bits = bl + 1;  /* sign bit */
            }
            bw_put(bw, (uint32_t)bits, 5);
            for (int i = 0; i < n; i++)
                bw_put(bw, (uint32_t)(res[lo + i] & ((1LL << bits) - 1)), bits);
            continue;
        }
        for (int i = 0; i < n; i++) {
            int64_t m = zigzag64(res[lo + i]);
            int64_t q = m >> k;
            /* sanity: absurd unary runs mean corrupt recipe/PCM */
            if (q > 100000000LL) { fprintf(stderr, "write_residuals: bad q=%lld\n", (long long)q); return -1; }
            bw_zeros(bw, (size_t)q);
            bw_put(bw, 1, 1);
            bw_put(bw, (uint32_t)(m & ((1LL << k) - 1)), k);
        }
    }
    return 0;
}

/* rebuild exact FLAC from PCM-WAV buffer + recipe buffer */
int flacx_rebuild(const uint8_t *wav, size_t wlen, const uint8_t *r, size_t rn,
                  const flacx_cover *covers, uint32_t ncovers,
                  uint8_t **out_buf, size_t *out_len)
{
#ifndef INVFS_EMBED_FLACX
    fprintf(stderr, "[rebuild] enter rn=%zu\n", rn);
#endif
    if (!r || rn < 32 || memcmp(r, "IVFR", 4)) { fprintf(stderr, "bad recipe\n"); return 1; }
    int ver = r[4];
    const uint8_t *p = r + 5;
    uint32_t hlen = rd32(&p);   /* full header size (with covers) */
    recipe_t si;
    memset(&si, 0, sizeof si);
    si.hlen = hlen;
    uint32_t ncv = 0;
    uint32_t cover_off[16], cover_len[16];
    uint8_t  cover_kind[16];
    int nframes = 0;
    if (ver >= 2) {
        si.total_samples = rd64(&p);
        si.min_block = rd16(&p); si.max_block = rd16(&p);
        si.sample_rate = rd32(&p);
        si.channels = rd8(&p); si.bps = rd8(&p);
        nframes = (int)rd32(&p);
        ncv = rd16(&p);
        if (ncv > 16) { fprintf(stderr, "too many covers %u\n", ncv); return 1; }
        size_t cover_bytes = 0, zero_bytes = 0;
        for (uint32_t i = 0; i < ncv; i++) {
            cover_off[i] = rd32(&p);
            cover_len[i] = rd32(&p);
            cover_kind[i] = (ver >= 3) ? (uint8_t)rd8(&p) : 0;
            if (cover_kind[i] == 0) cover_bytes += cover_len[i];
            else zero_bytes += cover_len[i];
        }
        size_t hdr_wo_len = hlen - cover_bytes - zero_bytes;
        if (cover_bytes + zero_bytes > hlen || hdr_wo_len > rn - (size_t)(p - r))
            return 1;
        const uint8_t *header_wo = p;
        p += hdr_wo_len;
        /* rebuild full header: splice covers back into their offsets */
        {
            uint8_t *header = (uint8_t *)malloc(hlen);
            if (!header) return 1;
            size_t out = 0, src = 0;
            uint32_t di = 0;   /* data-slot index into covers[] */
            for (uint32_t i = 0; i < ncv; i++) {
                size_t gap = (size_t)cover_off[i] - out;
                if (src + gap > hdr_wo_len) { free(header); return 1; }
                memcpy(header + out, header_wo + src, gap);
                out += gap; src += gap;
                if (cover_kind[i] == 1) {
                    /* zero PADDING slot: regenerate the block header
                       (type=1, last re-set later by the block walk) */
                    if (cover_len[i] < 4) { free(header); return 1; }
                    uint32_t blen = cover_len[i] - 4;
                    header[out + 0] = 0x01;
                    header[out + 1] = (uint8_t)(blen >> 16);
                    header[out + 2] = (uint8_t)(blen >> 8);
                    header[out + 3] = (uint8_t)blen;
                    memset(header + out + 4, 0, blen);
                } else {
                    if (di >= (uint32_t)ncovers || covers[di].len != cover_len[i]) {
                        fprintf(stderr, "cover %u size mismatch\n", i);
                        free(header); return 1;
                    }
                    memcpy(header + out, covers[di].data, covers[di].len);
                    di++;
                }
                out += cover_len[i];
            }
            if (out < hlen)
                memcpy(header + out, header_wo + src, hlen - out);
            /* re-set the last-metadata-block flag on the final block */
            {
                uint64_t hp = 4;
                while (hp + 4 <= hlen) {
                    uint32_t blen = (uint32_t)((header[hp+1] << 16) |
                                               (header[hp+2] << 8) | header[hp+3]);
                    uint64_t nx = hp + 4 + blen;
                    if (nx > hlen || nx <= hp) break;  /* corrupt */
                    if (nx == hlen) { header[hp] |= 0x80; break; }
                    hp = nx;
                }
            }
            si.header = header;   /* owned */
        }
    } else {
        /* v1: header bytes follow the size field verbatim */
        if (hlen + 26 > rn) return 1;
        si.total_samples = rd64(&p);
        si.min_block = rd16(&p); si.max_block = rd16(&p);
        si.sample_rate = rd32(&p);
        si.channels = rd8(&p); si.bps = rd8(&p);
        nframes = (int)rd32(&p);
        (void)cover_off; (void)cover_len; (void)cover_kind;
        si.header = (uint8_t *)malloc(hlen);
        if (!si.header) return 1;
        memcpy(si.header, p, hlen);
        p += hlen;
    }
#ifndef INVFS_EMBED_FLACX
    fprintf(stderr, "[rebuild] nframes=%d hlen=%u covers=%u\n", nframes, hlen, ncv);
#endif

    int wn = 0, wch = 0, wbits = 0;
    int32_t *samples = read_wav_mem(wav, wlen, &wn, &wch, &wbits);
#ifndef INVFS_EMBED_FLACX
    fprintf(stderr, "[rebuild] wav n=%d ch=%d bits=%d nframes=%d\n", wn, wch, wbits, nframes);
#endif
    if (!samples) { fprintf(stderr, "bad wav\n"); return 1; }
    int nch = wch;

    size_t cap = si.hlen + (size_t)nframes * 16384 + 65536;
    uint8_t *outbuf = (uint8_t *)malloc(cap);
    if (!outbuf) return 1;
    size_t outlen = 0;
    memcpy(outbuf + outlen, si.header, hlen); outlen += hlen;

    for (int fi = 0; fi < nframes; fi++) {
#ifndef INVFS_EMBED_FLACX
        if ((fi % 200) == 0) fprintf(stderr, "[rebuild] frame %d/%d\n", fi, nframes);
#endif
        uint8_t bs_code = rd8(&p), sr_code = rd8(&p), ch_assign = rd8(&p), ss_code = rd8(&p);
        uint64_t sample_num = rd64(&p);
        uint16_t block_size = rd16(&p), bs_header = rd16(&p);
        uint32_t frame_rate = rd32(&p);

        uint64_t start;
        if (si.min_block == si.max_block) start = sample_num * si.min_block;
        else start = sample_num;
        if (start + block_size > (uint64_t)wn / nch) block_size = (uint16_t)((uint64_t)wn / nch - start);

        bw_t bw;
        bw_init(&bw);
        bw_put(&bw, 0xFF, 8);
        bw_put(&bw, 0xF8, 8);
        bw_put(&bw, bs_code, 4);
        bw_put(&bw, sr_code, 4);
        bw_put(&bw, ch_assign, 4);
        bw_put(&bw, ss_code, 3);
        bw_put(&bw, 0, 1);
        put_utf8(&bw, sample_num);
        if (bs_code == 6) bw_put(&bw, bs_header, 8);
        else if (bs_code == 7) bw_put(&bw, bs_header, 16);
        if (sr_code == 12) bw_put(&bw, frame_rate / 1000, 8);
        else if (sr_code == 13) bw_put(&bw, frame_rate / 10, 16);
        else if (sr_code == 14) bw_put(&bw, frame_rate, 16);
        size_t pre_bits = bw.bits;
        bw_put(&bw, 0, 8);  /* CRC-8 placeholder */
        {
            uint8_t hb[64];
            size_t hlen2 = pre_bits / 8;
            for (size_t i = 0; i < hlen2; i++) {
                hb[i] = 0;
                for (int k = 0; k < 8; k++)
                    if (bw.buf[(i * 8 + k) >> 3] & (1 << (7 - ((i * 8 + k) & 7)))) hb[i] |= (uint8_t)(1 << (7 - k));
            }
            uint8_t c8 = crc8(hb, hlen2);
            for (int k = 0; k < 8; k++) {
                size_t bit = pre_bits + k;
                if ((c8 >> (7 - k)) & 1) bw.buf[bit >> 3] |= (uint8_t)(1 << (7 - (bit & 7)));
                else bw.buf[bit >> 3] &= (uint8_t)~(1 << (7 - (bit & 7)));
            }
        }

        int64_t *chs = (int64_t *)malloc(2 * (size_t)block_size * sizeof(int64_t));
        if (!chs) { bw_free(&bw); free(samples); free(si.header); return 1; }
        const int32_t *L = samples + (size_t)start * nch;
        const int32_t *R = (nch > 1) ? L + 1 : NULL;
        /* channel assignment (RFC 9639): 8=left/side, 9=right/side,
           10=mid/side — channel 1 is ALWAYS the side channel (bps+1) */
        if (ch_assign <= 7) {
            for (int i = 0; i < block_size; i++) chs[0*block_size+i] = L[i * nch];
            if (nch > 1) for (int i = 0; i < block_size; i++) chs[1*block_size+i] = L[i * nch + 1];
        } else if (ch_assign == 8) {  /* left + side */
            for (int i = 0; i < block_size; i++) {
                chs[0*block_size+i] = L[i * nch];
                chs[1*block_size+i] = L[i * nch] - R[i * nch];
            }
        } else if (ch_assign == 9) {  /* right + side: ch0=SIDE, ch1=right */
            for (int i = 0; i < block_size; i++) {
                chs[0*block_size+i] = L[i * nch] - R[i * nch];
                chs[1*block_size+i] = R[i * nch];
            }
        } else if (ch_assign == 10) {  /* mid + side: mid=(L+R)>>1 (floor) */
            for (int i = 0; i < block_size; i++) {
                int64_t l = L[i * nch], rr = R[i * nch];
                chs[0*block_size+i] = (l + rr) >> 1;
                chs[1*block_size+i] = l - rr;
            }
        } else {
            for (int i = 0; i < block_size; i++) chs[0*block_size+i] = L[i * nch];
        }

        int nsub = (nch > 1) ? 2 : 1;
        for (int c = 0; c < nsub; c++) {
            subframe_t sf;
            memset(&sf, 0, sizeof sf);
            sf.type = rd8(&p);
            sf.wasted = rd8(&p);
            sf.bps_sub = rd8(&p);
            int beff = sf.bps_sub - sf.wasted;
            bw_put(&bw, 0, 1);
            bw_put(&bw, sf.type, 6);
            bw_put(&bw, sf.wasted ? 1 : 0, 1);
            if (sf.wasted) {
                for (int i = 0; i < sf.wasted - 1; i++) bw_put(&bw, 0, 1);
                bw_put(&bw, 1, 1);
            }
            if (sf.type == 0) {
                int32_t val = (int32_t)rd32(&p);
                bw_put(&bw, (uint32_t)((val >> sf.wasted) & ((1 << beff) - 1)), beff);
            } else if (sf.type == 1) {
                for (int i = 0; i < block_size; i++)
                    bw_put(&bw, (uint32_t)((chs[c*block_size+i] >> sf.wasted) & ((1LL << beff) - 1)), beff);
            } else if (sf.type >= 8 && sf.type <= 12) {
                sf.order = rd8(&p);
                int32_t warm[32];
                for (int i = 0; i < sf.order; i++) warm[i] = (int32_t)rd32(&p);
                for (int i = 0; i < sf.order; i++)
                    bw_put(&bw, (uint32_t)((warm[i] >> sf.wasted) & ((1 << beff) - 1)), beff);
                sf.rmethod = rd8(&p); sf.porder = rd8(&p);
                int nparts = 1 << sf.porder;
                uint8_t *ks = (uint8_t *)malloc((size_t)nparts);
                for (int i = 0; i < nparts; i++) ks[i] = rd8(&p);
                sf.ks = ks;
                int nres = block_size - sf.order;
                int64_t *res = (int64_t *)malloc((size_t)nres * 8);
                for (int i = sf.order; i < block_size; i++) {
                    int64_t pred = 0;
                    for (int j = 0; j < sf.order; j++)
                        pred += chs[c*block_size+i - 1 - j] * fixed_coeffs[sf.type - 8][j];
                    res[i - sf.order] = chs[c*block_size+i] - pred;
                }
                if (write_residuals(&bw, &sf, res, nres)) { bw_free(&bw); free(chs); free(res); free(samples); free(si.header); return 1; }
                free(res);
            } else if (sf.type >= 32 && sf.type <= 63) {
                sf.order = rd8(&p);
                sf.prec = rd8(&p);
                sf.shift = (int8_t)rd8(&p);
                int32_t coeff[32], warm[32];
                for (int i = 0; i < sf.order; i++) coeff[i] = (int32_t)rd32(&p);
                for (int i = 0; i < sf.order; i++) warm[i] = (int32_t)rd32(&p);
                for (int i = 0; i < sf.order; i++)
                    bw_put(&bw, (uint32_t)((warm[i] >> sf.wasted) & ((1 << beff) - 1)), beff);
                bw_put(&bw, sf.prec - 1, 4);
                bw_put(&bw, (uint32_t)(sf.shift & 0x1F), 5);
                for (int i = 0; i < sf.order; i++)
                    bw_put(&bw, (uint32_t)(coeff[i] & ((1 << sf.prec) - 1)), sf.prec);
                sf.rmethod = rd8(&p); sf.porder = rd8(&p);
                int nparts = 1 << sf.porder;
                uint8_t *ks = (uint8_t *)malloc((size_t)nparts);
                for (int i = 0; i < nparts; i++) ks[i] = rd8(&p);
                sf.ks = ks;
                int nres = block_size - sf.order;
                int64_t *res = (int64_t *)malloc((size_t)nres * 8);
                for (int i = sf.order; i < block_size; i++) {
                    int64_t pred = 0;
                    for (int j = 0; j < sf.order; j++)
                        pred += chs[c*block_size+i - 1 - j] * coeff[j];
                    pred >>= sf.shift;
                    res[i - sf.order] = chs[c*block_size+i] - pred;
                }
                if (write_residuals(&bw, &sf, res, nres)) { bw_free(&bw); free(chs); free(res); free(samples); free(si.header); return 1; }
                free(res);
            }
            free(sf.warmup);
            free(sf.coeff);
            free(sf.ks);
        }

        bw_align(&bw);
        size_t frame_bytes = bw.bits / 8;
        uint16_t c16 = crc16(bw.buf, frame_bytes);
        bw_put(&bw, c16, 16);
        if (outlen + bw.bits / 8 > cap) {
            cap = outlen + bw.bits / 8 + 65536;
            outbuf = (uint8_t *)realloc(outbuf, cap);
        }
        memcpy(outbuf + outlen, bw.buf, bw.bits / 8);
        outlen += bw.bits / 8;
        bw_free(&bw);
    }

    *out_buf = outbuf;
    *out_len = outlen;
    free(samples);
    free(si.header);
    return 0;
}

#ifndef INVFS_EMBED_FLACX
int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: flacx extract <in.flac> <recipe.bin>\n"
                        "       flacx rebuild <in.wav> <recipe.bin> <out.flac>\n");
        return 2;
    }
    if (!strcmp(argv[1], "extract")) {
        size_t n = 0, rlen = 0;
        uint8_t *d = read_file(argv[2], &n);
        uint8_t *r = NULL;
        flacx_cover *covers = NULL;
        uint32_t ncv = 0;
        if (!d) return 1;
        if (flacx_extract(d, n, &r, &rlen, &covers, &ncv) != 0) { free(d); return 1; }
        FILE *f = fopen(argv[3], "wb");
        if (!f) return 1;
        fwrite(r, 1, rlen, f);
        fclose(f);
        /* write data cover payloads next to the recipe: recipe.bin.cN,
           one per kind=0 slot (kind=1 zero-slots have no payload) */
        uint32_t di = 0;
        for (uint32_t i = 0; i < ncv; i++) {
            if (!covers[i].data) { free(covers[i].data); continue; }
            char cn[300];
            snprintf(cn, sizeof cn, "%s.c%u", argv[3], di++);
            FILE *cf = fopen(cn, "wb");
            if (!cf) { free(covers[i].data); free(covers); free(r); free(d); return 1; }
            fwrite(covers[i].data, 1, covers[i].len, cf);
            fclose(cf);
            free(covers[i].data);
        }
        free(covers);
        free(r);
        free(d);
        return 0;
    }
    if (!strcmp(argv[1], "rebuild") && argc >= 5) {
        fprintf(stderr, "[main] rebuild %s %s %s\n", argv[2], argv[3], argv[4]);
        size_t wlen = 0, rlen = 0, olen = 0;
        uint8_t *w = read_file(argv[2], &wlen);
        uint8_t *r = read_file(argv[3], &rlen);
        fprintf(stderr, "[main] w=%p r=%p wlen=%zu rlen=%zu\n", (void*)w, (void*)r, wlen, rlen);
        uint8_t *o = NULL;
        if (!w || !r) return 1;
        /* load cover payloads from recipe.bin.cN */
        int ncv = flacx_recipe_num_covers(r, rlen);
        flacx_cover covers[16];
        uint8_t *cdata[16];
        for (int i = 0; i < ncv && i < 16; i++) {
            char cn[300];
            snprintf(cn, sizeof cn, "%s.c%d", argv[3], i);
            size_t clen = 0;
            cdata[i] = read_file(cn, &clen);
            if (!cdata[i]) { free(w); free(r); return 1; }
            covers[i].len = (uint32_t)clen;
            covers[i].data = cdata[i];
            covers[i].offset = 0;
        }
        if (flacx_rebuild(w, wlen, r, rlen, covers, (uint32_t)ncv, &o, &olen) != 0) {
            for (int i = 0; i < ncv && i < 16; i++) free(cdata[i]);
            free(w); free(r); return 1;
        }
        for (int i = 0; i < ncv && i < 16; i++) free(cdata[i]);
        FILE *f = fopen(argv[4], "wb");
        if (!f) return 1;
        fwrite(o, 1, olen, f);
        fclose(f);
        free(o);
        free(w);
        free(r);
        return 0;
    }
    return 2;
}
#endif /* INVFS_EMBED_FLACX */
