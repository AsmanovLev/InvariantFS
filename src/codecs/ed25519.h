/* ed25519.h — ed25519 sign/verify for InvariantFS pack signatures (WP203).
 *
 * PROVENANCE (vendored crypto, public domain)
 * ------------------------------------------
 * The arithmetic in the companion ed25519.c is TweetNaCl version 20140427
 * by Daniel J. Bernstein, Bernard van Gastel, Wesley Janssen, Tanja Lange,
 * Peter Schwabe and Sjaak Smetsers, placed by its authors into the public
 * domain ("We have placed TweetNaCl into the public domain, and we
 * encourage applications to make use of it" — Bernstein et al.,
 * "TweetNaCl: A crypto library in 100 tweets").
 *
 *   canonical upstream: https://tweetnacl.cr.yp.to/20140427/tweetnacl.c
 *                       https://tweetnacl.cr.yp.to/20140427/tweetnacl.h
 *   pristine sha256 (as fetched 2026-10-10):
 *     tweetnacl.c  02e65bc3013ff2168983365e55906bc783c4c7e0a60d8100f17bb303a17175c4
 *     tweetnacl.h  43f29ad721d9927b747b0100ab4160c119e7bb180c7c98a66e4bf79d31244287
 *   cross-checked: tweetnacl.h is byte-identical to the RIOT-OS mirror
 *     (github.com/RIOT-OS/tweetnacl); tweetnacl.c differs from that
 *     mirror only in loop-variable types (u32/u64 vs int) in three
 *     helpers — cosmetic, so the upstream bytes are vendored as-is.
 *
 * Adaptations (mechanical only, no arithmetic touched):
 *   1. tweetnacl.c line 1 (`#include "tweetnacl.h"`) is dropped. The body
 *      needs nothing from that header (all types/macros are local to the
 *      .c); the three entry points used here are redeclared below.
 *      To re-verify: diff upstream tweetnacl.c lines 2..809 against the
 *      marked region in ed25519.c — it must be empty.
 *   2. `randombytes()` (declared extern by TweetNaCl, defined by the
 *      embedder) is provided in ed25519.c via getrandom(2) with a
 *      /dev/urandom fallback. Keygen is the only consumer.
 *   3. Thin wrappers (invfs_ed25519_*) plus strict hex helpers are
 *      appended after the marked region. They allocate, they do not
 *      reimplement any arithmetic.
 *
 * WHY VENDOR instead of libsodium (evaluation, WP203 requirement):
 * signatures gate pack installation, a durability-adjacent path that must
 * work on a bare build host. libsodium-dev is NOT installed here (no
 * /usr/include/sodium.h) and libssl headers are absent too, so linking
 * either would add a new system dependency for every builder and every
 * target recipe. TweetNaCl is self-contained C11+libc, ~800 lines,
 * public-domain, and its ed25519 is byte-compatible with RFC 8032
 * (pinned by src/cli/pack_sign_test.c against the §7.1 vector).
 */
#ifndef INVFS_ED25519_H
#define INVFS_ED25519_H

#include <stddef.h>

#define INVFS_ED25519_PK_LEN   32
#define INVFS_ED25519_SK_LEN   64
#define INVFS_ED25519_SIG_LEN  64
#define INVFS_ED25519_SEED_LEN 32

/* TweetNaCl entry points used (redeclared; see provenance note 1). */
extern int crypto_sign_keypair(unsigned char *pk, unsigned char *sk);
extern int crypto_sign(unsigned char *sm, unsigned long long *smlen,
                       const unsigned char *m, unsigned long long n,
                       const unsigned char *sk);
extern int crypto_sign_open(unsigned char *m, unsigned long long *mlen,
                            const unsigned char *sm, unsigned long long n,
                            const unsigned char *pk);

/* Generate a keypair. 0 on success, -1 when the OS gives no randomness. */
int invfs_ed25519_keypair(unsigned char pk[INVFS_ED25519_PK_LEN],
                          unsigned char sk[INVFS_ED25519_SK_LEN]);

/* Detached sign: sig = Sign(sk, msg). 0 on success, -1 on alloc failure. */
int invfs_ed25519_sign(unsigned char sig[INVFS_ED25519_SIG_LEN],
                       const unsigned char *msg, unsigned long long msglen,
                       const unsigned char sk[INVFS_ED25519_SK_LEN]);

/* Detached verify: 0 = valid, -1 = BAD SIGNATURE (or alloc failure). */
int invfs_ed25519_verify(const unsigned char sig[INVFS_ED25519_SIG_LEN],
                         const unsigned char *msg, unsigned long long msglen,
                         const unsigned char pk[INVFS_ED25519_PK_LEN]);

/* Lowercase hex. Encoded length is 2*n plus NUL; outcap must exceed 2*n.
 * 0 on success, -1 when outcap is too small. */
int invfs_hex_encode(const unsigned char *bin, size_t n,
                     char *out, size_t outcap);

/* Strict decode: s must be EXACTLY 2*outlen hex chars plus NUL (upper or
 * lower case, no 0x prefix, no whitespace, no short/long). 0 on success,
 * -1 on any deviation. Fixed input shape: no allocation, no lengths from
 * untrusted bytes. */
int invfs_hex_decode(const char *s, unsigned char *out, size_t outlen);

#endif /* INVFS_ED25519_H */
