/* vol_format.c — the single format gate. See vol_format.h. */
#include "vol_format.h"

#include <stdio.h>

int invfs_format_accept(const invfs_superblock *sb, const char *path,
                        char *why, size_t why_sz)
{
    uint32_t flags = sb->vol_flags;

    (void)sb->format_version;   /* informational only: see check 2 below */

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

    /* 2. The former generation gate lived here: sb.format_version had to
     * equal INVFS_FORMAT_VERSION. It is gone on purpose.
     *
     * While the format is unfrozen, a marker that refuses last week's
     * volume buys nothing and costs a migration every time it moves: the
     * byte says nothing about the layout (the 0 -> 1 bump moved no byte of
     * it), so the only thing it could refuse was a volume that reads
     * perfectly well. Building a reader that distinguishes generations is
     * code with no question behind it.
     *
     * VOLF_META stays as the one authoritative marker: it answers the only
     * question that matters -- "does this image claim to carry the metadata
     * store at all" -- and it is set by the thing that creates the volume,
     * not by a constant in a header.
     *
     * When a change really does move the layout, the correct gate is a
     * structural probe (magic/version of the descriptor that moved),
     * because that is the question a reader can actually answer.
     */

    /* 3. Retired/forbidden flags: none defined today. When a vol_flags bit
     * is retired (as VOLF_META2 / VOLF_ASTV2 were) a reader must refuse it,
     * not read it as current -- that check lands here. */

    return 0;
}
