/* ivpack_loader.c — ADR-007 direct ZIP-0 .ivpack loader & runner.
 *
 * Implements direct memory-based execution and dynamic loading
 * from uncompressed .ivpack archives without disk extraction.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#include <dlfcn.h>

#include "ivpack_loader.h"

#define ZIP_EOCD_MAGIC   0x06054b50u
#define ZIP_CEN_MAGIC    0x02014b50u
#define ZIP_LOCAL_MAGIC  0x04034b50u

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

int ivpack_is_archive_mem(const uint8_t *buf, size_t len)
{
    if (!buf || len < 22) return 0;
    return (buf[0] == 0x50 && buf[1] == 0x4b && buf[2] == 0x03 && buf[3] == 0x04);
}

int ivpack_is_archive_file(const char *path)
{
    if (!path) return 0;
    size_t l = strlen(path);
    if (l >= 7 && strcmp(path + l - 7, ".ivpack") == 0) return 1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    uint8_t sig[4];
    ssize_t n = read(fd, sig, sizeof(sig));
    close(fd);
    return (n == 4 && sig[0] == 0x50 && sig[1] == 0x4b && sig[2] == 0x03 && sig[3] == 0x04);
}

static int entry_matches(const char *entry, const char *pattern)
{
    if (!pattern || !pattern[0]) return 1;
    if (strcmp(entry, pattern) == 0) return 1;

    /* Wildcard support: lib .so or bin tool */
    if (strcmp(pattern, "lib/*.so") == 0 || strcmp(pattern, "*.so") == 0) {
        size_t el = strlen(entry);
        if (el > 3 && strcmp(entry + el - 3, ".so") == 0) {
            if (strncmp(entry, "lib/", 4) == 0 || strcmp(pattern, "*.so") == 0)
                return 1;
        }
        return 0;
    }
    if (strcmp(pattern, "bin/*") == 0) {
        if (strncmp(entry, "bin/", 4) == 0 && entry[4] != '\0' && !strchr(entry + 4, '/'))
            return 1;
        return 0;
    }
    return 0;
}

int ivpack_find_entry_mem(const uint8_t *buf, size_t len, const char *entry_name,
                          const uint8_t **data_out, size_t *size_out,
                          char *entry_name_out, size_t entry_name_cap)
{
    if (!buf || len < 22) return -1;

    /* Locate EOCD: search backwards from end */
    size_t max_search = len > 65557 ? 65557 : len;
    size_t min_eocd = len - max_search;
    size_t eocd_pos = 0;
    bool found_eocd = false;

    for (size_t i = len - 22; i >= min_eocd; i--) {
        if (rd32(buf + i) == ZIP_EOCD_MAGIC) {
            eocd_pos = i;
            found_eocd = true;
            break;
        }
        if (i == 0) break;
    }

    if (!found_eocd) return -1;

    uint16_t num_entries = rd16(buf + eocd_pos + 10);
    uint32_t cen_off = rd32(buf + eocd_pos + 16);
    if (cen_off >= len) return -1;

    uint32_t p = cen_off;
    for (uint16_t i = 0; i < num_entries; i++) {
        if (p + 46 > len || rd32(buf + p) != ZIP_CEN_MAGIC) break;

        uint16_t method = rd16(buf + p + 10);
        uint32_t usize = rd32(buf + p + 24);
        uint16_t nlen = rd16(buf + p + 28);
        uint16_t elen = rd16(buf + p + 30);
        uint16_t clen = rd16(buf + p + 32);
        uint32_t local_off = rd32(buf + p + 42);

        if (p + 46 + nlen > len) break;

        char cur_name[256];
        size_t cplen = nlen < sizeof(cur_name) - 1 ? nlen : sizeof(cur_name) - 1;
        memcpy(cur_name, buf + p + 46, cplen);
        cur_name[cplen] = '\0';

        if (entry_matches(cur_name, entry_name)) {
            /* Matched! Verify method is stored (0) */
            if (method != 0) {
                fprintf(stderr, "ivpack: entry %s is compressed (method %u), requires ZIP-0 Stored\n",
                        cur_name, (unsigned)method);
                return -2;
            }
            if (local_off + 30 > len || rd32(buf + local_off) != ZIP_LOCAL_MAGIC) return -1;

            uint16_t loc_nlen = rd16(buf + local_off + 26);
            uint16_t loc_elen = rd16(buf + local_off + 28);
            size_t payload_off = local_off + 30 + loc_nlen + loc_elen;

            if (payload_off + usize > len) return -1;

            if (data_out) *data_out = buf + payload_off;
            if (size_out) *size_out = (size_t)usize;
            if (entry_name_out && entry_name_cap > 0) {
                strncpy(entry_name_out, cur_name, entry_name_cap - 1);
                entry_name_out[entry_name_cap - 1] = '\0';
            }
            return 0;
        }

        p += 46 + nlen + elen + clen;
    }

    return -1;
}

int ivpack_find_entry_file(const char *path, const char *entry_name,
                           off_t *offset_out, size_t *size_out,
                           char *entry_name_out, size_t entry_name_cap)
{
    if (!path) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 22) {
        close(fd);
        return -1;
    }

    size_t fsize = (size_t)st.st_size;
    /* Map the file read-only to quickly parse ZIP directory */
    const uint8_t *buf = (const uint8_t *)mmap(NULL, fsize, PROT_READ, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        close(fd);
        return -1;
    }

    const uint8_t *data_ptr = NULL;
    size_t sz = 0;
    int rc = ivpack_find_entry_mem(buf, fsize, entry_name, &data_ptr, &sz,
                                   entry_name_out, entry_name_cap);
    if (rc == 0) {
        if (offset_out) *offset_out = (off_t)(data_ptr - buf);
        if (size_out) *size_out = sz;
    }

    munmap((void *)buf, fsize);
    close(fd);
    return rc;
}

int ivpack_read_entry_file(const char *path, const char *entry_name,
                           void **buf_out, size_t *size_out)
{
    off_t offset = 0;
    size_t sz = 0;
    if (ivpack_find_entry_file(path, entry_name, &offset, &sz, NULL, 0) != 0)
        return -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    void *mem = malloc(sz + 1);
    if (!mem) {
        close(fd);
        return -1;
    }

    ssize_t n = pread(fd, mem, sz, offset);
    close(fd);
    if (n != (ssize_t)sz) {
        free(mem);
        return -1;
    }
    ((char *)mem)[sz] = '\0';

    if (buf_out) *buf_out = mem;
    if (size_out) *size_out = sz;
    return 0;
}

int ivpack_create_entry_memfd(const char *path, const char *entry_name)
{
    off_t offset = 0;
    size_t sz = 0;
    char matched_name[256];
    matched_name[0] = '\0';

    if (ivpack_find_entry_file(path, entry_name, &offset, &sz,
                               matched_name, sizeof(matched_name)) != 0) {
        return -1;
    }

    int in_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (in_fd < 0) return -1;

    const char *tag = matched_name[0] ? matched_name : (entry_name ? entry_name : "ivpack_exec");
    const char *base = strrchr(tag, '/');
    if (base) tag = base + 1;

    int mfd = -1;
#if defined(__NR_memfd_create) || defined(SYS_memfd_create)
    mfd = memfd_create(tag, MFD_CLOEXEC);
#endif
    if (mfd < 0) {
        /* Fallback if memfd_create not directly available */
#ifdef SYS_memfd_create
        mfd = (int)syscall(SYS_memfd_create, tag, 1);
#endif
    }

    if (mfd < 0) {
        close(in_fd);
        return -1;
    }

    if (ftruncate(mfd, (off_t)sz) != 0) {
        close(mfd);
        close(in_fd);
        return -1;
    }

    off_t cur_off = offset;
    size_t remaining = sz;
    while (remaining > 0) {
        ssize_t s = sendfile(mfd, in_fd, &cur_off, remaining);
        if (s <= 0) {
            if (s < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            close(mfd);
            close(in_fd);
            return -1;
        }
        remaining -= (size_t)s;
    }

    close(in_fd);
    fchmod(mfd, 0755);
    return mfd;
}

void *ivpack_dlmopen(const char *path, const char *so_entry, int lmid, int flags)
{
    const char *target = so_entry ? so_entry : "lib/*.so";
    int mfd = ivpack_create_entry_memfd(path, target);
    if (mfd < 0) return NULL;

    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", mfd);

    void *h = NULL;
#ifdef LM_ID_NEWLM
    if (lmid == LM_ID_NEWLM) {
        h = dlmopen(LM_ID_NEWLM, proc_path, flags);
    }
#endif
    if (!h) {
        h = dlopen(proc_path, flags);
    }

    close(mfd);
    return h;
}

int ivpack_fexecve(const char *path, const char *bin_entry,
                   char *const argv[], char *const envp[])
{
    const char *target = bin_entry ? bin_entry : "bin/*";
    int mfd = ivpack_create_entry_memfd(path, target);
    if (mfd < 0) return -1;

    fexecve(mfd, argv, envp);

    /* Fallback via /proc/self/fd */
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", mfd);
    execve(proc_path, argv, envp);

    close(mfd);
    return -1;
}
