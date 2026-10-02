/*
 * crc32c.c — CRC32C (Castagnoli, polynomial 0x1EDC6F41)
 * Table-driven, little-endian variant used by BTRFS/EXT4/CRC32C-SSE.
 *
 * A sweep spends about a THIRD of its CPU cycles in here (perf, fresh
 * 8 GiB volume, both QCOW2 bases): every delta record, base page, bitmap
 * span, segment frame and seal stripe is checksummed, and the original
 * byte-at-a-time table loop is a strictly serial dependency chain -- one
 * byte per iteration, each iteration waiting on the previous CRC.
 *
 * Two implementations, same polynomial and same results:
 *   1. SSE4.2  — the hardware carry-less-multiply instruction, 8 bytes at
 *      a time. Selected at runtime via CPUID, so a binary built for a
 *      baseline x86-64 still runs on an old CPU.
 *   2. slice-by-8 — eight tables, eight parallel chains. ~3-4x over the
 *      byte loop, portable, used when SSE4.2 is absent, and the byte loop
 *      remains its tail.
 *
 * "same results" is ASSERTED, not assumed: src/cli/crc32c_test.c pins the
 * two against an independent textbook CRC32C and against each other at every
 * length class, with the fallback FORCED so the check runs on an SSE4.2 host
 * too. Before that test existed there was no comparison anywhere in the tree,
 * and the two paths did not agree.
 */
#include "invarifs.h"
#include <stdlib.h>   /* getenv, for INVFS_CRC32C_FORCE_FALLBACK */

static uint32_t crc32c_table[256];
static uint32_t crc32c_table8[8][256];
static int crc32c_table_ready = 0;
/* -1 = not probed yet, 0 = no SSE4.2, 1 = hardware path available. */
static int crc32c_have_sse42 = -1;
/* -1 = not consulted yet, 0 = CPUID decides, 1 = software path pinned. */
static int crc32c_pinned_fallback = -1;

/* The fallback pin. crc32c_slice8 is static and unreachable from a test
 * binary, so on an SSE4.2 host there was NO WAY to exercise it -- which is how
 * a fallback that disagreed with the hardware for every n >= 8 shipped (see
 * src/cli/crc32c_test.c). Declared here and defined next to the dispatch it
 * steers, so the state and both dispatchers that read it are on one screen. */
static int crc32c_fallback_pinned(void);

static void crc32c_init(void)
{
    uint32_t i, j, k;

    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        crc32c_table[i] = c;
    }
    /* slice-by-8: table n folds the previous slice's tail in, so eight
     * bytes can be consumed per iteration across independent chains */
    for (i = 0; i < 256; i++) {
        uint32_t c = crc32c_table[i];
        /* crc32c_table8[0] IS crc32c_table: slice-by-8's T[0] is the plain
         * byte table, and the slicing loop reads it as the contribution of the
         * eighth byte of every 8-byte group (crc32c_slice8's last term). The
         * k loop starts at 1, so without this store T[0] kept the static
         * zero-fill and every eighth byte was checksummed as 0x00 -- correct
         * for n < 8, wrong for every n >= 8, on any CPU without SSE4.2.
         * src/cli/crc32c_test.c asserts this equality directly. */
        crc32c_table8[0][i] = c;
        for (k = 1; k < 8; k++) {
            c = crc32c_table[c & 0xFF] ^ (c >> 8);
            crc32c_table8[k][i] = c;
        }
    }
    crc32c_table_ready = 1;
}

static int crc32c_probe_sse42(void);

static int crc32c_probe_sse42(void)
{
#if defined(__x86_64__) || defined(__i386__)
    /* raw CPUID leaf 1: SSE4.2 is ECX bit 20. cpuid.h is a C++-era header
     * on some glibc, and the FS builds C11 without it, so ask the
     * instruction directly. */
    unsigned int eax, ebx, ecx, edx;

    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1), "c"(0));
    (void)eax; (void)ebx; (void)edx;
    return (ecx & (1u << 20)) != 0;
#else
    return 0;
#endif
}

#if defined(__x86_64__) || defined(__i386__)
#include <nmmintrin.h>

/* target("sse4.2") on the function, not -msse4.2 on the whole build: the
 * intrinsic is always_inline, so the attribute is what lets it compile,
 * and the CPUID probe still decides at runtime whether we call it. */
__attribute__((target("sse4.2")))
static uint32_t crc32c_hw(uint32_t crc, const uint8_t *p, size_t len)
{
    uint32_t c = ~crc;
    uint64_t c64;

    while (len && ((uintptr_t)p & 7)) {
        c = _mm_crc32_u8(c, *p++);
        len--;
    }
    c64 = c;
    while (len >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        c64 = _mm_crc32_u64(c64, v);
        p += 8;
        len -= 8;
    }
    c = (uint32_t)c64;
    while (len--)
        c = _mm_crc32_u8(c, *p++);
    return ~c;
}
#endif

static uint32_t crc32c_slice8(uint32_t crc, const uint8_t *p, size_t len)
{
    uint32_t c = ~crc;

    while (len && ((uintptr_t)p & 7)) {
        c = (c >> 8) ^ crc32c_table[(c ^ *p++) & 0xFF];
        len--;
    }
    while (len >= 8) {
        uint32_t lo, hi;
        memcpy(&lo, p, 4);
        memcpy(&hi, p + 4, 4);
        lo ^= c;
        c = crc32c_table8[7][lo & 0xFF] ^
            crc32c_table8[6][(lo >> 8) & 0xFF] ^
            crc32c_table8[5][(lo >> 16) & 0xFF] ^
            crc32c_table8[4][(lo >> 24) & 0xFF] ^
            crc32c_table8[3][hi & 0xFF] ^
            crc32c_table8[2][(hi >> 8) & 0xFF] ^
            crc32c_table8[1][(hi >> 16) & 0xFF] ^
            crc32c_table8[0][(hi >> 24) & 0xFF];
        p += 8;
        len -= 8;
    }
    while (len--)
        c = (c >> 8) ^ crc32c_table[(c ^ *p++) & 0xFF];
    return ~c;
}

/* ---- test hooks -------------------------------------------------------
 * These live here, beside the dispatch they steer, rather than at the top of
 * the file where they would have to forward-declare crc32c_init(). */

/* Force the software path. The environment is not consulted once this has
 * been called, so a test can toggle it between vol_close() and vol_open() to
 * model "written on one machine, read on another". */
void invfs_crc32c_force_fallback(int on)
{
    crc32c_pinned_fallback = on ? 1 : 0;
}

static int crc32c_fallback_pinned(void)
{
    if (crc32c_pinned_fallback < 0) {
        const char *e = getenv("INVFS_CRC32C_FORCE_FALLBACK");
        crc32c_pinned_fallback = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return crc32c_pinned_fallback;
}

/* Which path invfs_crc32c()/invfs_crc32c_update() would take RIGHT NOW. A
 * test asserts this rather than assuming: "I asked for the fallback" is not
 * the same claim as "the fallback ran", and only the second one is a control. */
int invfs_crc32c_using_fallback(void)
{
    if (!crc32c_table_ready)
        crc32c_init();
    if (crc32c_have_sse42 < 0)
        crc32c_have_sse42 = crc32c_probe_sse42();
    return crc32c_fallback_pinned() || !crc32c_have_sse42;
}

/* Test-only view of the slice-by-8 table. It is static, so a test cannot
 * otherwise assert that T[0] -- the eighth byte's term, and the row the
 * shipped fallback left zero-filled -- was built at all. Returns 0 for an
 * out-of-range (k, i) so a typo in a test cannot read past the end. */
uint32_t invfs_crc32c_slice8_table(unsigned k, unsigned i)
{
    if (!crc32c_table_ready)
        crc32c_init();
    if (k >= 8 || i >= 256)
        return 0;
    return crc32c_table8[k][i];
}

uint32_t invfs_crc32c(const void *data, size_t len)
{
    if (!crc32c_table_ready)
        crc32c_init();
    if (crc32c_have_sse42 < 0)
        crc32c_have_sse42 = crc32c_probe_sse42();
#if defined(__x86_64__) || defined(__i386__)
    if (crc32c_have_sse42 && !crc32c_fallback_pinned())
        return crc32c_hw(0, (const uint8_t *)data, len);
#endif
    return crc32c_slice8(0, (const uint8_t *)data, len);
}

uint32_t invfs_crc32c_update(uint32_t crc, const void *data, size_t len)
{
    if (!crc32c_table_ready)
        crc32c_init();
    if (crc32c_have_sse42 < 0)
        crc32c_have_sse42 = crc32c_probe_sse42();
#if defined(__x86_64__) || defined(__i386__)
    if (crc32c_have_sse42 && !crc32c_fallback_pinned())
        return crc32c_hw(crc, (const uint8_t *)data, len);
#endif
    return crc32c_slice8(crc, (const uint8_t *)data, len);
}
