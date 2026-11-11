/* vol_format.h — the single format gate. One question, one answer.
 *
 * Before this module, "is this volume readable by this build" was answered
 * in six places, each with its own hand-written message and its own subset
 * of the checks (vnode.c forgot the generation; verify.c forgot the
 * generation; the degraded leg forgot the generation; ...). They had already
 * drifted apart, and a drifted gate is how a volume gets opened into a build
 * that cannot read it.
 *
 * invfs_format_accept is the ONLY place that decides. Every opener goes
 * through it; every refusal is one canonical message.
 */
#ifndef INVFS_VOL_FORMAT_H
#define INVFS_VOL_FORMAT_H

#include "invarifs.h"

/* Refusal codes. These are also the vol_open err codes, so a caller can pass
 * the return value straight through to its own err_out unchanged. */
#define INVFS_FMT_REFUSE_FLAG       (-12)  /* VOLF_META not set */
#define INVFS_FMT_REFUSE_GENERATION (-13)  /* format_version mismatch */
#define INVFS_FMT_REFUSE_FORBIDDEN  (-14)  /* a retired flag is set (reserved) */

/* Decide accept/refuse once, and fill *why (never empty on refusal) with one
 * canonical message. path is what the caller is trying to open; it only
 * prefixes the message. Returns 0 on accept, or one of INVFS_FMT_REFUSE_*.
 *
 * The checks, in order, mirror the format marker ledger in invarifs.h:
 *   1. VOLF_META is the authoritative "metadata store volume" marker.
 *   2. sb.format_version must equal INVFS_FORMAT_VERSION (generation gate).
 *   3. reserved: retired/forbidden vol_flags bits (none defined today).
 *
 * NOTE: this does NOT check sb.magic or the superblock CRC — those are
 * "is this a valid InvariantFS image at all", and stay where they are.
 */
int invfs_format_accept(const invfs_superblock *sb, const char *path,
                        char *why, size_t why_sz);

#endif /* INVFS_VOL_FORMAT_H */
