/* ivpack_api.h — Standard C ABI for InvariantFS dynamically loaded plugins
 * (.ivpack / .so).
 *
 * Plugins loaded via dlmopen(LM_ID_NEWLM) or worker pool daemon export
 * these entry points.
 */
#ifndef IVPACK_API_H
#define IVPACK_API_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>   /* ssize_t: the host-I/O callbacks return it */

/* ABI history:
 *   1 — original: desc + container_cmd(ENUMERATE/STRIP/REBUILD/MAP) + estimate.
 *   2 — appended extract_idx and self_path to ivpack_container_args, so
 *       EXTRACT can name a member and a pack can resolve its own pack dir
 *       (p7z's 7zz sibling). The host fills the v2 fields only for a plugin
 *       whose desc->api_version >= 2; a v1 plugin keeps working unchanged.
 *   3 — appended `io` (ivpack_host_io, ADR-015): the host can hand a pack
 *       ranged, callback-based access to the container's BYTES instead of a
 *       path, so the sweep never materializes the container. The host fills
 *       `io` only for a plugin whose desc->api_version >= 3, and only when it
 *       can actually provide it (in-process dispatch); a v1/v2 plugin, and
 *       any pack called through the CLI helper, keeps reading `in_path`.
 *       A v3 pack MUST keep working when io == NULL: that is what the CLI
 *       (`bin/<pack> <cmd> <in> <out>`) and the out-of-process daemon use. */
#define IVPACK_API_VERSION 3
#define IVPACK_API_VERSION_MIN 1

/* ivpack_desc.flags */
#define IVPACK_F_NO_FORK 0x1u   /* plugin is safe to call in-process (no
                                 * exit()/die() on any path). No pack sets
                                 * this yet: every containerpack declines with
                                 * a bare exit(3), so the glue in
                                 * ivpack_impl.h fork-guards each call. */
#define IVPACK_F_HOST_IO 0x2u   /* container commands accept `io` in place of
                                 * in_path (ABI v3, ADR-015). The host fills
                                 * `io` only for a pack that sets this AND
                                 * reports api_version >= 3; every other pack
                                 * is handed a path exactly as before. A pack
                                 * must set it only if EVERY command that reads
                                 * the container can read through the
                                 * callbacks, because in that mode there is no
                                 * file to fall back to. */

#ifdef __cplusplus
extern "C" {
#endif

/* Ranged host I/O (ABI v3, ADR-015).
 *
 * The host fills this for desc->api_version >= 3 packs when it can provide
 * the calls in the pack's own address space. `size` is the byte length the
 * handle spans (the container's size on the read side); 0 means the handle is
 * unusable and the pack must fall back to the path.
 *
 * pread returns the number of bytes read, 0 at/after `size`, or < 0 on error.
 * A short read is legal ONLY at `size - off`; a pack that asks for more than
 * remains gets what remains. pwrite returns the number of bytes written, or
 * < 0. Either member may be NULL: a host that does not provide one says so by
 * leaving the pointer NULL, never by providing a stub that always fails (a
 * pwrite that fails cannot be told from "the volume is full").
 *
 * Why callbacks and not a device: a range of the volume that looks like a
 * file needs a kernel module, a device node or a mount, and per-job mounts
 * inside the pack's Landlock sandbox need CAP_SYS_ADMIN. These callbacks are
 * the same reads the core already performs. See docs/adr/ADR-015. */
typedef struct ivpack_host_io {
    void    *user;      /* opaque, passed back to every callback */
    uint64_t size;      /* bytes the handle spans; 0 = handle unusable */
    ssize_t (*pread)(void *user, void *buf, size_t n, uint64_t off);
    ssize_t (*pwrite)(void *user, const void *buf, size_t n, uint64_t off);
} ivpack_host_io;

/* Plugin descriptor */
typedef struct ivpack_desc {
    uint32_t api_version;       /* IVPACK_API_VERSION */
    const char *name;           /* e.g. "qcow2" */
    const char *version;        /* e.g. "1.0.0" */
    const char *pack_class;     /* "containerpack", "codecpack", "helperpack" */
    uint32_t flags;
} ivpack_desc;

/* Containerpack command arguments */
typedef struct ivpack_container_args {
    int cmd;                    /* 1=ENUMERATE, 2=EXTRACT, 3=STRIP, 4=REBUILD, 5=MAP */
    const char *in_path;        /* input file path */
    const char *out_path;       /* output file path */
    const char *recipe_path;    /* recipe file path */
    const char *mbr_dir;        /* member dir path */
    
    /* Optional in-memory buffers (when path is NULL) */
    const uint8_t *in_buf;
    size_t in_len;
    uint8_t *out_buf;
    size_t out_cap;
    size_t *out_len;
    
    /* Error reporting */
    char *err_msg;
    size_t err_msg_cap;

    /* --- appended in ABI v2 (host fills these only when desc->api_version
     * >= 2; a v1 plugin never reads them) --- */
    const char *extract_idx;    /* EXTRACT: member index string, CLI argv[3] */
    const char *self_path;      /* absolute path of the loaded .so, so a pack
                                 * can find pack-dir siblings (p7z -> 7zz) */

    /* --- appended in ABI v3 (host fills this only when desc->api_version
     * >= 3 AND it can serve the callbacks in-process; NULL otherwise, which
     * is the normal case for a CLI helper. A pack that reads it must treat
     * NULL exactly like an absent option: use in_path. The OUTPUT operands
     * (out_path/recipe_path/mbr_dir) are paths in every case; only the input
     * side is ranged, because that is the side the sweep had to materialize. */
    const ivpack_host_io *io;
} ivpack_container_args;

/* Containerpack estimate results */
typedef struct ivpack_estimate_res {
    uint64_t orig_size;
    uint64_t mbr_size;
    uint64_t recipe_size;
    int eligible;
} ivpack_estimate_res;

/* Standard plugin export functions:
 * Every containerpack plugin exports:
 *   ivpack_get_desc()
 *   ivpack_container_cmd()
 *   ivpack_container_estimate()
 */
typedef const ivpack_desc *(*ivpack_get_desc_fn)(void);
typedef int (*ivpack_container_cmd_fn)(const ivpack_container_args *args);
typedef int (*ivpack_container_estimate_fn)(const char *in_path, ivpack_estimate_res *res);

#ifdef __cplusplus
}
#endif

#endif /* IVPACK_API_H */
