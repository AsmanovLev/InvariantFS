/* rs.h — Reed-Solomon erasure coding over GF(2^8) (WP20b layer 2)
 *
 * Two MDS code shapes over the same field (poly 0x11D, generator 2):
 *
 *   RS_ALGO_VM:      systematic Vandermonde RS: the parity matrix is
 *                    P = V2 * V1^-1 (V1 = k x k Vandermonde of the data
 *                    nodes {1..k}, V2 = m x k of the parity nodes
 *                    {k+1..k+m}); encode multiplies through a full 256x256
 *                    product table (the log/exp tables are its build
 *                    step). Decode = Gaussian elimination over the
 *                    surviving rows of the (k+m)xk systematic matrix
 *                    [I; P].
 *
 *                    (The naive Vandermonde parity matrix A[j][i] =
 *                    (j+1)^i -- and its transpose -- are NOT MDS for these
 *                    parameters: over GF(2^8) the submatrix with exponent
 *                    set {1,2,4} at nodes {1,2,3} is a Moore matrix over
 *                    GF(2)-dependent nodes, determinant zero. P = V2*V1^-1
 *                    is the RS evaluation code itself, so every k x k
 *                    survivor submatrix is nonsingular; verified
 *                    exhaustively for (k+m, k) = (36, 32): 0 of 58905
 *                    four-erasure patterns singular, vs 496 resp. 182 for
 *                    the naive forms.)
 *
 *   RS_ALGO_CAUCHY:  Cauchy parity matrix A[j][i] = 1/(x_i + y_j) with
 *                    disjoint x/y sets; encode runs a jerasure-style
 *                    bitmatrix schedule: each GF element c expands to the
 *                    8 column bytes of its 8x8 multiplication bitmatrix
 *                    (col[t] = c * 2^t) and a block multiply-accumulate
 *                    is the XOR of the columns selected by the source
 *                    byte's set bits. No field tables in the inner loop;
 *                    correctness over cleverness. Decode shares the
 *                    GF Gaussian-elimination engine (the bitmatrix is an
 *                    encode-side representation of the SAME linear map).
 *
 * Block model: one RS stripe is k data blocks + m parity blocks, each
 * block_size bytes (the volume layer uses k=32, m in 2..8, 4 KiB blocks).
 * Any k of the k+m blocks reconstruct the stripe (MDS: every k x k
 * submatrix of [I; P] is nonsingular for both shapes over GF(2^8) with
 * k + m <= 256).
 *
 * Self-contained C11, no dependencies beyond libc.
 */
#ifndef INVFS_RS_H
#define INVFS_RS_H

#include <stddef.h>
#include <stdint.h>

#define RS_ALGO_VM     1
#define RS_ALGO_CAUCHY 2

const char *rs_algo_name(int algo);   /* "rs-vm" / "rs-cauchy" / "?" */

/* Encode m parity blocks over k data blocks, each block_size bytes.
 * data[i] (i < k) are read-only; parity[j] (j < m) are fully overwritten.
 * 0 = ok, -1 = bad arguments / out of memory. */
int rs_encode(int algo, unsigned k, unsigned m, size_t block_size,
              uint8_t *const *data, uint8_t *const *parity);

/* Erasure decode. blocks[0..k) are the data slots, blocks[k..k+m) the
 * parity slots of one stripe; present[i] != 0 marks slot content as
 * trustworthy. Erased slots (present[i] == 0) are reconstructed in place
 * (their buffers must be writable). Needs at least k present slots.
 * 0 = every slot now holds its encoded content, -1 = insufficient
 * survivors (or bad arguments); nothing is modified then. */
int rs_decode(int algo, unsigned k, unsigned m, size_t block_size,
              uint8_t **blocks, const uint8_t *present);

/* Synthetic head-to-head: nstripes stripes of k data blocks of block_size
 * bytes in RAM, encoded once per algorithm. *_mbps receive the DATA
 * throughput of each encoder (parity bytes excluded from the count).
 * Each algorithm also has to pass one erasure round-trip (drop min(m,2)
 * slots of a stripe, decode, memcmp) before its number is reported;
 * a failure there is an honest -1, not a fast number. */
int rs_bench(unsigned k, unsigned m, size_t block_size, unsigned nstripes,
             double *vm_mbps, double *cauchy_mbps);

#endif /* INVFS_RS_H */
