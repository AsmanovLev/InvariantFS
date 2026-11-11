/* vol_format.c — the single format gate. See vol_format.h. */
#include "vol_format.h"

#include <stdio.h>

int invfs_format_accept(const invfs_superblock *sb, const char *path,
                        char *why, size_t why_sz)
{
    uint32_t flags = sb->vol_flags;
    uint8_t  gen   = sb->format_version;

    if (why && why_sz)
        why[0] = '\0';

    /* 1. VOLF_META is the authoritative marker: without it the image is not
     * a metadata store volume, whatever its generation says. */
    if (!(flags & VOLF_META)) {
        if (why && why_sz)
            snprintf(why, why_sz,
                     "%s: not a metadata store volume (vol_flags=0x%08x: "
                     "VOLF_META unset); this build reads generation %d "
                     "(build %s) only. The image is untouched.",
                     path, flags, (int)INVFS_FORMAT_VERSION,
                     INVFS_VERSION_STRING);
        return INVFS_FMT_REFUSE_FLAG;
    }

    /* 2. Generation gate: only INVFS_FORMAT_VERSION opens. A volume stamped
     * otherwise (including pre-flip images stamped 3, byte-wise readable but
     * renumbered out from under themselves) is refused by number, loudly,
     * with the image untouched. */
    if (gen != INVFS_FORMAT_VERSION) {
        if (why && why_sz)
            snprintf(why, why_sz,
                     "%s: generation %u refused; this build reads generation "
                     "%d (build %s) only. The image is untouched.",
                     path, (unsigned)gen, (int)INVFS_FORMAT_VERSION,
                     INVFS_VERSION_STRING);
        return INVFS_FMT_REFUSE_GENERATION;
    }

    /* 3. Retired/forbidden flags: none defined today. When a vol_flags bit
     * is retired (as VOLF_META2 / VOLF_ASTV2 were) a reader must refuse it,
     * not read it as current -- that check lands here. */

    return 0;
}
