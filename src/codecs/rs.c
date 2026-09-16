/* rs.c — Reed-Solomon erasure coding over GF(2^8) (WP20b layer 2)
 *
 * Implements the rs.h contract: rs-vm and rs-cauchy encode/decode/bench.
 *
 * GF(2^8) with the reducing polynomial 0x11D and generator 2; log/exp
 * tables are built once (lazily, idempotently) and a full 256x256 product
 * table is derived from them for the rs-vm inner loops.
 *
 * Parity matrices (both shapes are systematic: data slots are the cargo
 * blocks verbatim, parity slots are extra blocks):
 *
 *   RS_ALGO_VM:     P = V2 * V1^-1 where V1 is the k x k Vandermonde of
 *                   the data nodes {1..k} and V2 the m x k Vandermonde of
 *                   the parity nodes {k+1..k+m} -- i.e. the systematic
 *                   form of the Reed-Solomon evaluation code. Encode is a
 *                   matrix multiply through the 256x256 product table.
 *
 *                   NOTE: this is deliberately NOT the naive "Vandermonde
 *                   parity matrix A[j][i] = (j+1)^i". That raw form (and
 *                   its transpose P[j][i] = (i+1)^j) is NOT MDS for k=32:
 *                   over GF(2^8) the generalized-Vandermonde submatrix with
 *                   exponent set {1,2,4} at nodes {1,2,3} is a Moore matrix
 *                   over GF(2)-dependent nodes (3 = 1+2), so its
 *                   determinant vanishes (exhaustively: 496 of the 58905
 *                   four-erasure patterns of a (36,32) stripe are
 *                   singular; the transpose loses 182). P = V2*V1^-1 is
 *                   the RS code itself, so EVERY k x k survivor submatrix
 *                   is nonsingular (0/58905 singular, checked
 *                   exhaustively); the Cauchy shape below is MDS by its
 *                   own theorem (also 0/58905).
 *
 *   RS_ALGO_CAUCHY: C[j][i] = 1/(x_i + y_j) over GF(2^8) with the disjoint
 *                   node sets x = {0..k-1}, y = {k..k+m-1} ('+' is XOR).
 *                   Encode runs the jerasure-style bitmatrix schedule:
 *                   each coefficient c expands to the 8 column bytes of
 *                   its 8x8 multiplication bitmatrix (col[t] = c * 2^t)
 *                   and a block multiply-accumulate is the XOR of the
 *                   columns selected by the source byte's set bits. No
 *                   field tables in the inner loop; correctness over
 *                   cleverness.
 *
 * Decode (both shapes) shares one engine: pick any k surviving slots of
 * the (k+m) x k systematic matrix [I; P], invert that k x k matrix over
 * GF (Gauss-Jordan), rebuild the erased data slots as combinations of the
 * survivors, then recompute any erased parity slots from the rebuilt
 * data. Nothing is modified unless every erased slot could be rebuilt.
 *
 * Self-contained C11, no dependencies beyond libc.
 */

#include "rs.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#define RS_MAX_K  255u   /* vm nodes are 1..k+m, all nonzero GF elements */
#define RS_MAX_PM 255u

/* ---- GF(2^8), poly 0x11D, generator 2 ---- */

static uint8_t rs_gf_exp[512];
static uint8_t rs_gf_log[256];
static uint8_t rs_gf_mul[256][256];   /* full product table (rs-vm inner loop) */
static int     rs_gf_ready;

static void rs_gf_init(void)
{
    unsigned x = 1;
    int i, j;
    if (rs_gf_ready) return;
    for (i = 0; i < 255; i++) {
        rs_gf_exp[i] = (uint8_t)x;
        rs_gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (i = 255; i < 512; i++)
        rs_gf_exp[i] = rs_gf_exp[i - 255];
    for (i = 0; i < 256; i++)
        for (j = 0; j < 256; j++)
            rs_gf_mul[i][j] = (i && j)
                ? rs_gf_exp[rs_gf_log[i] + rs_gf_log[j]] : 0;
    rs_gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
    return (a && b) ? rs_gf_exp[rs_gf_log[a] + rs_gf_log[b]] : 0;
}

static uint8_t gf_inv(uint8_t a)
{
    return rs_gf_exp[255 - rs_gf_log[a]];   /* a != 0 */
}

static uint8_t gf_pow(uint8_t a, unsigned e)
{
    if (!e) return 1;
    if (!a) return 0;
    return rs_gf_exp[(unsigned)rs_gf_log[a] * (e % 255) % 255];
}

/* ---- parity matrices ---- */

/* Fill P (m x k, row-major) for the algorithm. 0 = ok, -1 = bad shape. */
static int rs_matrix(int algo, unsigned k, unsigned m, uint8_t *P)
{
    unsigned i, j;
    if (!k || !m || k > RS_MAX_K || m > RS_MAX_PM || k + m > 256)
        return -1;
    if (algo == RS_ALGO_CAUCHY) {
        /* x_i = i, y_j = k + j: disjoint node sets, '+' is XOR */
        for (j = 0; j < m; j++)
            for (i = 0; i < k; i++)
                P[j * k + i] = gf_inv((uint8_t)(i ^ (k + j)));
        return 0;
    }
    if (algo == RS_ALGO_VM) {
        /* P = V2 * V1^-1 (see the file header for why not the raw
         * Vandermonde). V1[a][b] = (a+1)^b (data nodes 1..k);
         * V2[j][b] = (k+1+j)^b (parity nodes k+1..k+m). */
        uint8_t *V1 = (uint8_t *)malloc((size_t)k * k);
        uint8_t *V1i = (uint8_t *)malloc((size_t)k * k);
        unsigned a, b;
        if (!V1 || !V1i) { free(V1); free(V1i); return -1; }
        for (a = 0; a < k; a++)
            for (b = 0; b < k; b++)
                V1[a * k + b] = gf_pow((uint8_t)(a + 1), b);
        /* invert V1 (Gauss-Jordan over GF) */
        {
            uint8_t *aug = (uint8_t *)malloc((size_t)k * 2 * k);
            if (!aug) { free(V1); free(V1i); return -1; }
            for (a = 0; a < k; a++) {
                memcpy(aug + a * 2 * k, V1 + a * k, k);
                memset(aug + a * 2 * k + k, 0, k);
                aug[a * 2 * k + k + a] = 1;
            }
            for (b = 0; b < k; b++) {
                unsigned p = b;
                while (p < k && !aug[p * 2 * k + b]) p++;
                if (p == k) {   /* V1 is Vandermonde: never happens */
                    free(aug); free(V1); free(V1i);
                    return -1;
                }
                if (p != b) {
                    uint8_t *t = (uint8_t *)malloc(2 * k);
                    if (!t) { free(aug); free(V1); free(V1i); return -1; }
                    memcpy(t, aug + p * 2 * k, 2 * k);
                    memcpy(aug + p * 2 * k, aug + b * 2 * k, 2 * k);
                    memcpy(aug + b * 2 * k, t, 2 * k);
                    free(t);
                }
                {
                    uint8_t iv = gf_inv(aug[b * 2 * k + b]);
                    unsigned c;
                    for (c = 0; c < 2 * k; c++)
                        aug[b * 2 * k + c] = gf_mul(aug[b * 2 * k + c], iv);
                }
                for (a = 0; a < k; a++) {
                    uint8_t f = aug[a * 2 * k + b];
                    unsigned c;
                    if (a == b || !f) continue;
                    for (c = 0; c < 2 * k; c++)
                        aug[a * 2 * k + c] ^= gf_mul(f, aug[b * 2 * k + c]);
                }
            }
            for (a = 0; a < k; a++)
                memcpy(V1i + a * k, aug + a * 2 * k + k, k);
            free(aug);
        }
        for (j = 0; j < m; j++)
            for (b = 0; b < k; b++) {
                uint8_t acc = 0;
                for (a = 0; a < k; a++)
                    acc ^= gf_mul(gf_pow((uint8_t)(k + 1 + j), a),
                                  V1i[a * k + b]);
                P[j * k + b] = acc;
            }
        free(V1);
        free(V1i);
        return 0;
    }
    return -1;
}

/* ---- block multiply-accumulate: dst ^= c * src (block_size bytes) ---- */

/* rs-vm: one table lookup per byte. */
static void rs_mac_vm(uint8_t *dst, const uint8_t *src, uint8_t c, size_t n)
{
    const uint8_t *T = rs_gf_mul[c];
    size_t i;
    if (!c) return;
    if (c == 1 && n % sizeof(uint64_t) == 0) {
        uint64_t *d = (uint64_t *)dst;
        const uint64_t *s = (const uint64_t *)src;
        for (i = 0; i < n / sizeof(uint64_t); i++)
            d[i] ^= s[i];
        return;
    }
    if (c == 1) {   /* odd tail: c==1 is a plain XOR either way */
        for (i = 0; i < n; i++)
            dst[i] ^= src[i];
        return;
    }
    for (i = 0; i < n; i++)
        dst[i] ^= T[src[i]];
}

/* rs-cauchy: jerasure bitmatrix schedule -- the source byte's set bits
 * select which of the 8 column bytes (col[t] = c * 2^t) are XORed in. */
static void rs_mac_cauchy(uint8_t *dst, const uint8_t *src, uint8_t c,
                          size_t n)
{
    uint8_t col[8];
    size_t i;
    unsigned t;
    if (!c) return;
    for (t = 0; t < 8; t++)
        col[t] = gf_mul(c, (uint8_t)(1u << t));
    for (i = 0; i < n; i++) {
        uint8_t b = src[i], r = 0;
        if (b & 0x01) r ^= col[0];
        if (b & 0x02) r ^= col[1];
        if (b & 0x04) r ^= col[2];
        if (b & 0x08) r ^= col[3];
        if (b & 0x10) r ^= col[4];
        if (b & 0x20) r ^= col[5];
        if (b & 0x40) r ^= col[6];
        if (b & 0x80) r ^= col[7];
        dst[i] ^= r;
    }
}

static void rs_mac(int algo, uint8_t *dst, const uint8_t *src, uint8_t c,
                   size_t n)
{
    if (algo == RS_ALGO_CAUCHY)
        rs_mac_cauchy(dst, src, c, n);
    else
        rs_mac_vm(dst, src, c, n);
}

const char *rs_algo_name(int algo)
{
    switch (algo) {
        case RS_ALGO_VM:     return "rs-vm";
        case RS_ALGO_CAUCHY: return "rs-cauchy";
        default:             return "?";
    }
}

int rs_encode(int algo, unsigned k, unsigned m, size_t block_size,
              uint8_t *const *data, uint8_t *const *parity)
{
    uint8_t *P;
    unsigned i, j;

    rs_gf_init();
    if (!data || !parity || !block_size)
        return -1;
    P = (uint8_t *)malloc((size_t)m * k);
    if (!P) return -1;
    if (rs_matrix(algo, k, m, P) != 0) { free(P); return -1; }
    for (j = 0; j < m; j++) {
        memset(parity[j], 0, block_size);
        for (i = 0; i < k; i++)
            rs_mac(algo, parity[j], data[i], P[j * k + i], block_size);
    }
    free(P);
    return 0;
}

/* Invert the n x n GF matrix m in place (Gauss-Jordan). 0 = ok. */
static int rs_gf_mat_inv(uint8_t *m, unsigned n)
{
    uint8_t *aug;
    unsigned a, b, c;

    aug = (uint8_t *)malloc((size_t)n * 2 * n);
    if (!aug) return -1;
    for (a = 0; a < n; a++) {
        memcpy(aug + a * 2 * n, m + a * n, n);
        memset(aug + a * 2 * n + n, 0, n);
        aug[a * 2 * n + n + a] = 1;
    }
    for (b = 0; b < n; b++) {
        unsigned p = b;
        while (p < n && !aug[p * 2 * n + b]) p++;
        if (p == n) { free(aug); return -1; }   /* singular */
        if (p != b) {
            uint8_t *t = (uint8_t *)malloc(2 * n);
            if (!t) { free(aug); return -1; }
            memcpy(t, aug + p * 2 * n, 2 * n);
            memcpy(aug + p * 2 * n, aug + b * 2 * n, 2 * n);
            memcpy(aug + b * 2 * n, t, 2 * n);
            free(t);
        }
        {
            uint8_t iv = gf_inv(aug[b * 2 * n + b]);
            for (c = 0; c < 2 * n; c++)
                aug[b * 2 * n + c] = gf_mul(aug[b * 2 * n + c], iv);
        }
        for (a = 0; a < n; a++) {
            uint8_t f = aug[a * 2 * n + b];
            if (a == b || !f) continue;
            for (c = 0; c < 2 * n; c++)
                aug[a * 2 * n + c] ^= gf_mul(f, aug[b * 2 * n + c]);
        }
    }
    for (a = 0; a < n; a++)
        memcpy(m + a * n, aug + a * 2 * n + n, n);
    free(aug);
    return 0;
}

int rs_decode(int algo, unsigned k, unsigned m, size_t block_size,
              uint8_t **blocks, const uint8_t *present)
{
    uint8_t *P = NULL, *B = NULL;
    unsigned *surv = NULL;      /* surviving slot indices, k of them */
    unsigned i, j, nsurv = 0, nerased = 0;
    int rc = -1;

    rs_gf_init();
    if (!blocks || !present || !block_size)
        return -1;
    if (!k || !m || k > RS_MAX_K || m > RS_MAX_PM || k + m > 256)
        return -1;
    for (i = 0; i < k + m; i++) {
        if (!blocks[i]) return -1;
        if (present[i]) nsurv++;
        else nerased++;
    }
    if (nsurv < k) return -1;         /* insufficient survivors: touch nothing */
    if (!nerased) return 0;           /* nothing to rebuild */

    P = (uint8_t *)malloc((size_t)m * k);
    B = (uint8_t *)malloc((size_t)k * k);
    surv = (unsigned *)malloc(k * sizeof *surv);
    if (!P || !B || !surv) goto out;
    if (rs_matrix(algo, k, m, P) != 0) goto out;

    /* B = the k survivor rows of the (k+m) x k systematic matrix [I; P] */
    nsurv = 0;
    for (i = 0; i < k + m && nsurv < k; i++) {
        if (!present[i]) continue;
        if (i < k) {
            memset(B + nsurv * k, 0, k);
            B[nsurv * k + i] = 1;
        } else {
            memcpy(B + nsurv * k, P + (i - k) * k, k);
        }
        surv[nsurv++] = i;
    }
    if (rs_gf_mat_inv(B, k) != 0) goto out;   /* MDS: never singular */

    /* erased DATA slots: d_e = (row e of B^-1) . survivors */
    for (i = 0; i < k; i++) {
        if (present[i]) continue;
        memset(blocks[i], 0, block_size);
        for (j = 0; j < k; j++)
            rs_mac(algo, blocks[i], blocks[surv[j]], B[i * k + j],
                   block_size);
    }
    /* erased PARITY slots: recompute from the (now complete) data */
    for (i = 0; i < m; i++) {
        if (present[k + i]) continue;
        memset(blocks[k + i], 0, block_size);
        for (j = 0; j < k; j++)
            rs_mac(algo, blocks[k + i], blocks[j], P[i * k + j],
                   block_size);
    }
    rc = 0;
out:
    free(P);
    free(B);
    free(surv);
    return rc;
}

/* ---- bench ---- */

static double rs_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

/* deterministic content (xorshift64): the same bytes for both algos */
static uint64_t rs_rng_state;
static uint64_t rs_rng(void)
{
    uint64_t x = rs_rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rs_rng_state = x;
    return x;
}

/* One encode timing pass over nstripes stripes. Returns DATA MB/s. */
static double rs_bench_one(int algo, unsigned k, unsigned m,
                           size_t block_size, unsigned nstripes,
                           uint8_t *databuf)
{
    uint8_t **data = (uint8_t **)malloc(k * sizeof *data);
    uint8_t **par = (uint8_t **)malloc(m * sizeof *par);
    uint8_t *parbuf;
    unsigned s, i, j;
    double t0, t1;

    parbuf = (uint8_t *)malloc((size_t)m * block_size);
    if (!data || !par || !parbuf) {
        free(data); free(par); free(parbuf);
        return -1.0;
    }
    for (j = 0; j < m; j++)
        par[j] = parbuf + (size_t)j * block_size;

    t0 = rs_now();
    for (s = 0; s < nstripes; s++) {
        for (i = 0; i < k; i++)
            data[i] = databuf + ((size_t)s * k + i) * block_size;
        if (rs_encode(algo, k, m, block_size, data, par) != 0) {
            free(data); free(par); free(parbuf);
            return -1.0;
        }
    }
    t1 = rs_now();
    free(data);
    free(par);
    free(parbuf);
    if (t1 <= t0) return -1.0;
    return (double)k * (double)block_size * (double)nstripes
           / (t1 - t0) / (1024.0 * 1024.0);
}

/* Erasure round-trip: encode one stripe, drop `drop` slots, decode,
 * memcmp against the originals. 0 = the codec is honest. */
static int rs_roundtrip(int algo, unsigned k, unsigned m, size_t block_size,
                        unsigned drop, uint8_t *databuf)
{
    uint8_t **blocks = (uint8_t **)malloc((k + m) * sizeof *blocks);
    uint8_t *present = (uint8_t *)malloc(k + m);
    uint8_t *work = NULL, *keep = NULL;
    unsigned i;
    int rc = -1;

    work = (uint8_t *)malloc((size_t)(k + m) * block_size);
    keep = (uint8_t *)malloc((size_t)(k + m) * block_size);
    if (!blocks || !present || !work || !keep) goto out;
    {
        uint8_t **dd = (uint8_t **)malloc(k * sizeof *dd);
        uint8_t **pp = (uint8_t **)malloc(m * sizeof *pp);
        if (!dd || !pp) { free(dd); free(pp); goto out; }
        for (i = 0; i < k; i++) dd[i] = databuf + (size_t)i * block_size;
        for (i = 0; i < m; i++) pp[i] = work + (size_t)(k + i) * block_size;
        memcpy(work, databuf, (size_t)k * block_size);
        if (rs_encode(algo, k, m, block_size, dd, pp) != 0) {
            free(dd); free(pp);
            goto out;
        }
        free(dd); free(pp);
    }
    memcpy(keep, work, (size_t)(k + m) * block_size);
    /* drop the first `drop` slots (mixed data/parity by index wrap) */
    for (i = 0; i < k + m; i++) {
        blocks[i] = work + (size_t)i * block_size;
        present[i] = 1;
    }
    for (i = 0; i < drop && i < k + m; i++) {
        unsigned slot = (i * (k + 1)) % (k + m);   /* spread over data+parity */
        while (!present[slot]) slot = (slot + 1) % (k + m);
        present[slot] = 0;
        memset(blocks[slot], 0xA5, block_size);
    }
    if (rs_decode(algo, k, m, block_size, blocks, present) != 0) goto out;
    if (memcmp(work, keep, (size_t)(k + m) * block_size) != 0) goto out;
    rc = 0;
out:
    free(blocks);
    free(present);
    free(work);
    free(keep);
    return rc;
}

int rs_bench(unsigned k, unsigned m, size_t block_size, unsigned nstripes,
             double *vm_mbps, double *cauchy_mbps)
{
    uint8_t *databuf;
    size_t total = (size_t)k * block_size * nstripes;
    size_t i;

    rs_gf_init();
    if (!vm_mbps || !cauchy_mbps || !k || !m || !block_size || !nstripes)
        return -1;
    if (k > RS_MAX_K || k + m > 256) return -1;

    databuf = (uint8_t *)malloc(total);
    if (!databuf) return -1;
    rs_rng_state = 0x9E3779B97F4A7C15ull;
    for (i = 0; i < total / 8; i++)
        ((uint64_t *)databuf)[i] = rs_rng();

    /* honesty gate first: a broken codec gets no number */
    if (rs_roundtrip(RS_ALGO_VM, k, m, block_size,
                     m < 2 ? m : 2, databuf) != 0) { free(databuf); return -1; }
    if (rs_roundtrip(RS_ALGO_CAUCHY, k, m, block_size,
                     m < 2 ? m : 2, databuf) != 0) { free(databuf); return -1; }

    *vm_mbps = rs_bench_one(RS_ALGO_VM, k, m, block_size, nstripes, databuf);
    *cauchy_mbps = rs_bench_one(RS_ALGO_CAUCHY, k, m, block_size, nstripes,
                                databuf);
    free(databuf);
    if (*vm_mbps <= 0 || *cauchy_mbps <= 0) return -1;
    return 0;
}
