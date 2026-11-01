/* ivpack_loader.h — ADR-007 direct ZIP-0 .ivpack loader & runner.
 *
 * Allows zero-extraction loading and execution of plugins and binaries
 * directly from uncompressed .ivpack (ZIP-0) archives using memfd_create,
 * fexecve, and dlmopen via /proc/self/fd/.
 */
#ifndef INVF_IVPACK_LOADER_H
#define INVF_IVPACK_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Check if path or buffer is an .ivpack archive */
int ivpack_is_archive_file(const char *path);
int ivpack_is_archive_mem(const uint8_t *buf, size_t len);

/* Locate member data offset and length in a ZIP-0 file on disk.
 * If entry_name is NULL, or contains wildcard '*', matches first appropriate entry
 * (e.g. wildcard lib or bin).
 * entry_name_out (if non-NULL, size >= 256) receives the matched entry name. */
int ivpack_find_entry_file(const char *path, const char *entry_name,
                           off_t *offset_out, size_t *size_out,
                           char *entry_name_out, size_t entry_name_cap);

/* Locate member data slice in a ZIP-0 buffer in memory.
 * Returns pointer to start of slice within buf, and its size. */
int ivpack_find_entry_mem(const uint8_t *buf, size_t len, const char *entry_name,
                          const uint8_t **data_out, size_t *size_out,
                          char *entry_name_out, size_t entry_name_cap);

/* Read entry from disk file into newly allocated buffer (caller frees) */
int ivpack_read_entry_file(const char *path, const char *entry_name,
                           void **buf_out, size_t *size_out);

/* Create an anonymous sealed memfd containing the uncompressed member bytes */
int ivpack_create_entry_memfd(const char *path, const char *entry_name);

/* Load shared library (.so) directly from .ivpack into link-map namespace */
void *ivpack_dlmopen(const char *path, const char *so_entry, int lmid, int flags);

/* Execute binary directly from .ivpack via fexecve (does not return on success) */
int ivpack_fexecve(const char *path, const char *bin_entry,
                   char *const argv[], char *const envp[]);

#ifdef __cplusplus
}
#endif

#endif /* INVF_IVPACK_LOADER_H */
