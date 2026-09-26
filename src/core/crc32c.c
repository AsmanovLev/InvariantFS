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
 * Three implementations, same polynomial and same results:
 *   1. SSE4.2  — the hardware carry-less-multiply instruction, 8 bytes at
 *      a time. Selected at runtime via CPUID, so a binary built for a
 *      baseline x86-64 still runs on an old CPU.
 *   2. slice-by-8 — eight tables, eight parallel chains. ~3-4x over the
 *      byte loop, portable, used when SSE4.2 is absent.
 *   3. the original byte loop — the floor, and the reference the other two
 *      are checked against.
 */
#include "invarifs.h"

static uint32_t crc32c_table[256];
static uint32_t crc32c_table8[8][256];
static int crc32c_table_ready = 0;
/* -1 = not probed yet, 0 = no SSE4.2, 1 = hardware path available. */
static int crc32c_have_sse42 = -1;

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
        for (k = 1; k < 8; k++) {
            c = crc32c_table[c & 0xFF] ^ (c >> 8);
            crc32c_table8[k][i] = c;
        }
    }
    crc32c_table_ready = 1;
}

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

uint32_t invfs_crc32c(const void *data, size_t len)
{
    if (!crc32c_table_ready)
        crc32c_init();
    if (crc32c_have_sse42 < 0)
        crc32c_have_sse42 = crc32c_probe_sse42();
#if defined(__x86_64__) || defined(__i386__)
    if (crc32c_have_sse42)
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
    if (crc32c_have_sse42)
        return crc32c_hw(crc, (const uint8_t *)data, len);
#endif
    return crc32c_slice8(crc, (const uint8_t *)data, len);
}
