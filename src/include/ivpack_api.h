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

/* ABI history:
 *   1 — original: desc + container_cmd(ENUMERATE/STRIP/REBUILD/MAP) + estimate.
 *   2 — appended extract_idx and self_path to ivpack_container_args, so
 *       EXTRACT can name a member and a pack can resolve its own pack dir
 *       (p7z's 7zz sibling). The host fills the v2 fields only for a plugin
 *       whose desc->api_version >= 2; a v1 plugin keeps working unchanged. */
#define IVPACK_API_VERSION 2
#define IVPACK_API_VERSION_MIN 1

/* ivpack_desc.flags */
#define IVPACK_F_NO_FORK 0x1u   /* plugin is safe to call in-process (no
                                 * exit()/die() on any path). No pack sets
                                 * this yet: every containerpack declines with
                                 * a bare exit(3), so the glue in
                                 * ivpack_impl.h fork-guards each call. */

#ifdef __cplusplus
extern "C" {
#endif

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
