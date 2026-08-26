#ifndef BCJ_X86_H
#define BCJ_X86_H

/*
 * bcj_x86.h — x86 BCJ (branch/call/jump) prefilter (WP14a).
 *
 * Converts relative x86 call/jmp (0xE8/0xE9) operands between
 * file-relative and absolute form so a general-purpose compressor sees
 * the same target address regardless of where in the file the
 * instruction sits. Bijective per buffer when enc and dec run over the
 * SAME byte window with the same start state (both entry points here
 * use state=0, pc=0): the encoder's value shift is exactly undone by
 * the decoder, and bytes that are not converted operands pass through
 * untouched in both directions.
 *
 * InvariantFS applies it per binary-batch member slice (families
 * ELF-x86 / ELF-x86-64) before the slice joins the shared ZSTD batch,
 * and inverts it per member slice after the batch decode.
 */

#include <stddef.h>
#include <stdint.h>

void invfs_bcj_x86_enc(uint8_t *buf, size_t len);
void invfs_bcj_x86_dec(uint8_t *buf, size_t len);

#endif /* BCJ_X86_H */
