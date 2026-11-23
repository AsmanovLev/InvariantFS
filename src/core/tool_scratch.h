/* tool_scratch.h — WHERE the tool pipeline's intermediate files live.
 *
 * The containerpack sweep extracts every member of a container into scratch
 * (src/core/vol_cpack.c), and -- for a pack that does NOT speak ABI v3 host
 * I/O -- first writes the whole container there as well (the "pin"), so that
 * pack's peak is roughly twice the container. ABOVE one container that is the
 * number this policy exists for: where it lands used to be "the first
 * /dev/shm or /tmp that mkdtemp() accepted", with nothing anywhere asking
 * whether the chosen root can hold the job -- and on a host where both of
 * those are tmpfs, that is RAM.
 *
 * ADR-015 removes the pin for a v3 pack (src/core/vol_cpack_hostio.c), so the
 * demand it declares is the member total, which is what phase 2 of the sweep
 * prices; a v1/v2 pack, a CLI-only pack and an .ivpack archive still ask for
 * the pin and still need a root that can hold the container.
 *
 * This module owns the choice. It is a policy, not a convenience: it is
 * deliberately small and dependency-free so that the codecpack trampolines
 * (src/codecs/codec.c) and the volume's tool plumbing
 * (src/core/vol_cpack.c) can share ONE answer instead of each carrying a
 * copy of the same default.
 */
#ifndef INVFS_TOOL_SCRATCH_H
#define INVFS_TOOL_SCRATCH_H

#include <stddef.h>
#include <stdint.h>

/* One file to carry across a migration. The bytes are already in RAM (the
 * caller holds the buffer it just wrote), so a migration is a re-write, not
 * a re-read. */
typedef struct {
    const char *name;
    const uint8_t *data;
    size_t len;
} scratch_file;

/* Bytes this root can still be given, for a job that needs `need`.
 *
 * For a real filesystem that is statvfs free space. For a tmpfs it is the
 * MINIMUM of the filesystem free space and an allocation-aware ceiling: a
 * tmpfs page is charged to the WRITING cgroup, so a tmpfs inside a cgroup
 * with a finite memory.max fails ENOSPC while `df` still reports gigabytes
 * free. The ceiling is min(cgroup memory.high|max - memory.current,
 * /proc/meminfo MemAvailable), scaled by INVFS_SCRATCH_TMPFS_MAX_FRAC, and
 * statvfs alone is never trusted for a memory-backed root.
 *
 * `fs_avail` and `alloc` (either may be NULL) receive the two components
 * separately so the diagnostic can name which one decided. `alloc` is 0
 * when the root is not memory-backed. */
uint64_t tool_scratch_headroom(const char *root, uint64_t *fs_avail,
                               uint64_t *alloc);

/* Create a fresh scratch directory for a job that needs `need` bytes.
 * 0 = created (dir holds the path), -1 = REFUSED, with the numbers on
 * stderr. The roots searched, in order:
 *   $INVFS_TOOL_SCRATCH           forced; no other root is considered
 *   $INVFS_SCRATCH_ROOTS          ':'-separated override of the list
 *   /dev/shm, /tmp, /var/tmp     the default
 * tmpfs-first is kept as the PREFERENCE (it is the fast path and the
 * intermediate files are written once and read once), but a root is only
 * eligible if it can hold the job, so the default falls through to a real
 * directory rather than into RAM. */
int tool_tmpdir(char *dir, size_t cap, uint64_t need);

/* Re-price the scratch after the job's real size became known (the
 * containerpack member total is only known once `enumerate` has run). If
 * the directory chosen by tool_tmpdir() can no longer hold the job, the
 * scratch MIGRATES to the first eligible root, `files` are re-written
 * there, the old directory is removed, and `dir` is rewritten -- the caller
 * must rebuild its path buffers afterwards.
 * 0 = fine where it is or moved, -1 = refused with the numbers. */
int tool_scratch_grow(char *dir, size_t cap, uint64_t need,
                      const scratch_file *files, int nfiles);

#endif /* INVFS_TOOL_SCRATCH_H */
