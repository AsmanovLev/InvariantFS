/* invfs PPMd8 text codec -- thin wrapper over vendored 7-Zip public-domain
 * C sources (Ppmd8*.c/h, Igor Pavlov / Dmitry Shkarin).
 *
 * Wire format mirrors 7-Zip's own .ppmd container byte-for-byte:
 *   [props lo][props hi][range-coded stream incl. END marker]
 * props = ((order-1) | ((memMB-1)<<4) | (restor<<12)) little-endian u16.
 *
 * Stream model: whole-file, logical size lives in the AST (external),
 * but we still encode/expect the explicit END marker exactly like 7-Zip
 * so the codec pair stays bit-comparable with the reference tooling. */
#include "Ppmd8.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define INVFS_PPMD_MEM_MB (64u)
#define INVFS_PPMD_ORDER  8u
#define INVFS_PPDM_RESTORE PPMD8_RESTORE_METHOD_CUT_OFF

static void *SzAlloc(const ISzAlloc *p, size_t size) { (void)p; return calloc(1, size); }
static void SzFree(const ISzAlloc *p, void *a) { (void)p; free(a); }
static ISzAlloc g_alloc = { SzAlloc, SzFree };

typedef struct { IByteIn  base; const Byte *cur, *end; unsigned extra; } CInvIn;
typedef struct { IByteOut base; Byte *cur, *end; size_t overflow; } CInvOut;

static Byte Inv_InRead(IByteInPtr pp)
{
    CInvIn *b = (CInvIn *)pp;
    if (b->cur < b->end) return *(b->cur++);
    b->extra++;                       /* past EOF: 7-Zip counts Extra too */
    return 0;
}

static void Inv_OutWrite(IByteOutPtr pp, Byte b)
{
    CInvOut *o = (CInvOut *)pp;
    if (o->cur < o->end) *(o->cur++) = b;
    else o->overflow++;
}

static void PutProps(Byte *p, unsigned order, unsigned memMb, unsigned restor)
{
    unsigned val = ((order - 1) & 0xF)
                 | (((memMb - 1) & 0xFF) << 4)
                 | ((restor & 3) << 12);
    /* 7-Zip writes two bytes; high bits beyond u16 unused for our params */
    p[0] = (Byte)(val & 0xFF);
    p[1] = (Byte)((val >> 8) & 0xFF);
}

int invfs_ppmd_encode(const uint8_t *in, size_t inlen,
                      uint8_t *out, size_t outcap, size_t *outlen)
{
    CPpmd8 p;
    CInvIn ii; CInvOut oo;
    size_t i;

    if (!in || !out || !outlen || inlen == 0 || inlen > 0x7FFFFFFFull ||
        outcap < 2)
        return -1;

    memset(&p, 0, sizeof p);
    Ppmd8_Construct(&p);
    if (!Ppmd8_Alloc(&p, INVFS_PPMD_MEM_MB << 20, &g_alloc)) return -1;

    ii.base.Read = Inv_InRead; ii.cur = in; ii.end = in + inlen; ii.extra = 0;
    oo.base.Write = Inv_OutWrite; oo.cur = out; oo.end = out + outcap; oo.overflow = 0;

    /* CPpmd8.Stream is a UNION of .In/.Out -- assign ONLY the member the
     * operation actually uses; assigning both clobbers the first one. */
    p.Stream.Out = &oo.base;

    PutProps(oo.cur, INVFS_PPMD_ORDER, INVFS_PPMD_MEM_MB,
             (unsigned)INVFS_PPDM_RESTORE);
    oo.cur += 2;

    Ppmd8_Init_RangeEnc(&p);
    Ppmd8_Init(&p, INVFS_PPMD_ORDER, INVFS_PPDM_RESTORE);

    for (i = 0; i < inlen; i++)
        Ppmd8_EncodeSymbol(&p, in[i]);
    Ppmd8_EncodeSymbol(&p, -1);           /* END marker, like 7-Zip */
    Ppmd8_Flush_RangeEnc(&p);

    Ppmd8_Free(&p, &g_alloc);
    if (oo.overflow) return -1;
    *outlen = (size_t)(oo.cur - out);
    return 0;
}

int invfs_ppmd_decode(const uint8_t *in, size_t inlen,
                      uint8_t *out, size_t outlen)
{
    CPpmd8 p;
    CInvIn ii; CInvOut oo;
    size_t i;
    unsigned order, memMb, restor, props;

    if (!in || !out || !outlen || inlen < 7) return -1;

    memset(&p, 0, sizeof p);
    Ppmd8_Construct(&p);
    if (!Ppmd8_Alloc(&p, INVFS_PPMD_MEM_MB << 20, &g_alloc)) return -1;

    ii.base.Read = Inv_InRead; ii.cur = in; ii.end = in + inlen; ii.extra = 0;
    oo.base.Write = Inv_OutWrite; oo.cur = out; oo.end = out + outlen; oo.overflow = 0;

    /* decode: only .In used (output is written directly via oo.cur) */
    p.Stream.In = &ii.base;

    props = ii.cur[0] | ((unsigned)ii.cur[1] << 8);
    ii.cur += 2;
    order   = (props & 0xF) + 1;
    memMb   = ((props >> 4) & 0xFF) + 1;
    restor  = (props >> 12) & 3;
    if (order != INVFS_PPMD_ORDER || memMb != INVFS_PPMD_MEM_MB ||
        restor != (unsigned)INVFS_PPDM_RESTORE) {
        Ppmd8_Free(&p, &g_alloc);
        return -1;                        /* foreign params */
    }

    if (!Ppmd8_Init_RangeDec(&p)) { Ppmd8_Free(&p, &g_alloc); return -1; }
    Ppmd8_Init(&p, INVFS_PPMD_ORDER, INVFS_PPDM_RESTORE);

    for (i = 0; i < outlen; i++) {
        int sym = Ppmd8_DecodeSymbol(&p);
        if (sym < 0) { Ppmd8_Free(&p, &g_alloc); return -1; }  /* END early / err */
        *(oo.cur++) = (uint8_t)sym;
    }

    Ppmd8_Free(&p, &g_alloc);
    return 0;
}
