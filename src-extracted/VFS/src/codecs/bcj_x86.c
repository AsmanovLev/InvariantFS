/*
 * bcj_x86.c — x86 BCJ (branch converter) prefilter (WP14a).
 *
 * Vendored from 7-Zip's C/Bra86.c (z7_BranchConvSt_X86_Enc/Dec),
 * reduced to the single-converter core:
 *
 *   Bra86.c -- Branch converter for X86 code (BCJ)
 *   2023-04-02 : Igor Pavlov : Public domain
 *
 * The z7 plumbing (7zTypes.h/CpuArch.h types, the MY_CPU_LE_UNALIGN and
 * BR_CONV_USE_OPT_PC_PTR fast paths, the streaming return value) is cut
 * down to plain stdint types and memcpy-based little-endian 32-bit
 * access (the rest of InvariantFS already assumes an LE host, see
 * invarifs.h invfs_le*). The conversion logic is byte-for-byte the
 * upstream algorithm, including the mask state machine.
 */
#include <string.h>

#include "bcj_x86.h"

static uint32_t bcj_get32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void     bcj_put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

#define BCJ_NEED_CONV_FOR_MS_BYTE(b) ((((b) + 1) & 0xfe) == 0)

/* One pass over [p, p+size). pc is the virtual address of byte 0 (we
 * always pass 0: members are filtered standalone); *state carries the
 * cross-call scan mask (0 for a self-contained buffer, our only use).
 * encoding=1 maps rel->abs, 0 maps abs->rel. Returns where it stopped
 * (unused here: we always convert whole buffers). */
static uint8_t *bcj_x86_conv(uint8_t *p, size_t size, uint32_t pc,
                             uint32_t *state, int encoding)
{
    if (size < 5)
        return p;
    {
        const uint8_t *lim = p + size - 4;
        unsigned mask = (unsigned)*state;
        /* upstream BR_PC_GET without the pointer-arithmetic variant:
         * pc += size up front, then (pc - (lim - p)) == (p - data) + 4
         * at every conversion site (lim = data + size - 4) */
        pc += (uint32_t)size;
        goto start;

        for (;; mask |= 4) {
        start:
            if (p >= lim)
                goto fin;
            {
                const uint32_t sv = bcj_get32(p) ^ 0xe8e8e8e8u;
                p += 4;
                if ((sv & 0xfeu) == 0)        { goto m0; } mask >>= 1;
                if ((sv & 0xfe00u) == 0)      { goto m1; } mask >>= 1;
                if ((sv & 0xfe0000u) == 0)    { goto m2; } mask = 0;
                if ((sv & 0xfe000000u) == 0)  { goto a3; }
            }
            goto main_loop;

        m0: p--;
        m1: p--;
        m2: p--;
            if (mask == 0)
                goto a3;
            if (p > lim)
                goto fin_p;
            if (mask > 4 || mask == 3) {
                mask >>= 1;
                continue;
            }
            mask >>= 1;
            if (BCJ_NEED_CONV_FOR_MS_BYTE(p[mask]))
                continue;
            {
                uint32_t v = bcj_get32(p);
                uint32_t c;
                v += (1u << 24);
                if (v & 0xfe000000u) continue;
                c = pc - (uint32_t)(lim - p);
                if (encoding) v += c; else v -= c;
                {
                    /* mask here is in {0,1,2} (pre-shift), so the shift
                     * stays well inside u32 */
                    mask <<= 3;
                    if (BCJ_NEED_CONV_FOR_MS_BYTE(v >> mask)) {
                        v ^= ((0x100u << mask) - 1);
                        if (encoding) v += c; else v -= c;
                    }
                    mask = 0;
                }
                v &= (1u << 25) - 1;
                v -= (1u << 24);
                bcj_put32(p, v);
                p += 4;
                goto main_loop;
            }

        main_loop:
            if (p >= lim)
                goto fin;
            for (;;) {
                const uint32_t sv = bcj_get32(p) ^ 0xe8e8e8e8u;
                p += 4;
                if ((sv & 0xfeu) == 0)        { goto a0; }
                if ((sv & 0xfe00u) == 0)      { goto a1; }
                if ((sv & 0xfe0000u) == 0)    { goto a2; }
                if ((sv & 0xfe000000u) == 0)  { goto a3; }
                if (p >= lim)
                    goto fin;
            }

        a0: p--;
        a1: p--;
        a2: p--;
        a3:
            if (p > lim)
                goto fin_p;
            {
                uint32_t v = bcj_get32(p);
                uint32_t c;
                v += (1u << 24);
                if (v & 0xfe000000u) continue;
                c = pc - (uint32_t)(lim - p);
                if (encoding) v += c; else v -= c;
                v &= (1u << 25) - 1;
                v -= (1u << 24);
                bcj_put32(p, v);
                p += 4;
                goto main_loop;
            }
        }

    fin_p:
        p--;
    fin:
        *state = (uint32_t)mask;
        return p;
    }
}

void invfs_bcj_x86_enc(uint8_t *buf, size_t len)
{
    uint32_t state = 0;
    bcj_x86_conv(buf, len, 0, &state, 1);
}

void invfs_bcj_x86_dec(uint8_t *buf, size_t len)
{
    uint32_t state = 0;
    bcj_x86_conv(buf, len, 0, &state, 0);
}
