/*
 * dokan_fs.c — InvariantFS filesystem for Windows via Dokan
 *
 *   invf-dokan <image> <mountpoint>
 *   mountpoint: "Y:" or "Z:" (free letter)
 *
 * Build: cl dokan_fs.c volume.c crc32c.c lz4.c dokan2.lib (v2.3.1 SDK)
 * Data path: read -> AST -> L2P -> segment decode (LZ4/ZSTD/raw)
 *            write -> buffered write-back -> RAW zone (LZ4) on flush
 */
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <dokan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "invarifs.h"
#include "volume.h"

/* ---- record copied out of the volume's name index ---- */
typedef struct {
    char name[256];
    uint64_t inode_id;
    uint64_t size;
    uint64_t ctime;
} fs_entry;

static invfs_volume *g_vol;
static CRITICAL_SECTION g_lock;
/* per-op tracing: unconditional stderr writes on every callback cost more
   than the filesystem work on small I/O. Opt in with INVFS_DEBUG=1. */
static int g_debug = 0;
#define DBG(...) do { if (g_debug) fprintf(stderr, __VA_ARGS__); } while (0)
/* open-handle counter: background sweep runs only when no handle is open
   (its tombstones/frees are then safe — nothing reads old versions) */
static volatile LONG g_open_handles = 0;

/* ---- write-path profiler (INVFS_PROF=1) --------------------------------
   Per-phase timings, reported every PROF_BUCKET commits and then reset, so
   a term that grows with the number of files on the volume shows up as a
   rising column instead of being averaged away. `wall` is the elapsed time
   for the whole bucket: the gap between it and the sum of the phases is
   time spent outside our callbacks (Dokan, the kernel, the caller).
   Dokan runs callbacks on several threads at once, so the accumulators are
   interlocked microsecond counters -- plain doubles produced torn values
   and NaNs in the report. */
/* INVFS_PROF_N overrides the bucket size at runtime (e.g. "5" for a
   short test run; default 200 for a long copy). */
static int g_prof_bucket = 200;
#define PROF_BUCKET g_prof_bucket
enum { PF_LOOKUP, PF_READBACK, PF_FIND, PF_DELETE, PF_CREATE, PF_FLUSH, PF_N };
static const char *g_pf_name[PF_N] = {
    "lookup", "readback", "find", "delete", "create", "flush"
};
static int g_prof = 0;
static volatile LONG64 g_pf[PF_N];
static double g_pf_wall0;
static volatile LONG g_pf_n;
static long g_pf_total;

static double prof_now(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
#define PROF_T0() (g_prof ? prof_now() : 0.0)
#define PROF_ADD(slot, t0) \
    do { if (g_prof) InterlockedExchangeAdd64(&g_pf[slot], \
             (LONG64)((prof_now() - (t0)) * 1000.0)); } while (0)

/* Whole-callback accounting. The phase counters above only cover the commit
   path; these say how often Windows enters each callback per committed file
   and how long it stays. A term that lives in a callback nobody instrumented
   (a directory enumeration on every create, say) shows up here and nowhere
   else. */
enum { CB_CREATE, CB_GETINFO, CB_FIND, CB_READ, CB_WRITE, CB_CLOSE,
       CB_CLEANUP, CB_N };
static const char *g_cb_name[CB_N] = {
    "create", "getinfo", "find", "read", "write", "close", "cleanup"
};
static volatile LONG64 g_cb_us[CB_N];
static volatile LONG g_cb_n[CB_N];
#define CB_ENTER() double cb_t0 = PROF_T0()
#define CB_LEAVE(slot) \
    do { if (g_prof) { \
        InterlockedIncrement(&g_cb_n[slot]); \
        InterlockedExchangeAdd64(&g_cb_us[slot], \
            (LONG64)((prof_now() - cb_t0) * 1000.0)); \
    } } while (0)

/* call once per committed file, with g_lock held */
static void prof_tick(void)
{
    int i;
    if (!g_prof) return;
    if (g_pf_n == 0) g_pf_wall0 = prof_now();
    if (++g_pf_n < PROF_BUCKET) return;
    fprintf(stderr, "[prof] files %ld..%ld:",
            g_pf_total, g_pf_total + g_pf_n - 1);
    for (i = 0; i < PF_N; i++)
        fprintf(stderr, " %s %.3f", g_pf_name[i],
                (double)g_pf[i] / 1000.0 / g_pf_n);
    fprintf(stderr, "  wall %.3f  (ms/file)\n",
            (prof_now() - g_pf_wall0) / g_pf_n);
    fprintf(stderr, "[prof]   callbacks per file:");
    for (i = 0; i < CB_N; i++)
        fprintf(stderr, " %s %.2fx/%.3f", g_cb_name[i],
                (double)g_cb_n[i] / g_pf_n,
                (double)g_cb_us[i] / 1000.0 / g_pf_n);
    fprintf(stderr, "  (calls/file, ms/file)\n");
    g_pf_total += g_pf_n;
    memset((void *)g_pf, 0, sizeof g_pf);
    memset((void *)g_cb_n, 0, sizeof g_cb_n);
    memset((void *)g_cb_us, 0, sizeof g_cb_us);
    g_pf_n = 0;
}

/* 0 on success, -1 if the name does not fit (or is not convertible).

   WideCharToMultiByte writes NOTHING and returns 0 when the output does not
   fit -- it does not truncate. Both return values used to be discarded, so a
   path over 255 UTF-8 bytes left the caller's stack buffer exactly as it was
   found: uninitialised. path_to_utf8 then walked it looking for backslashes,
   past the end of the buffer if no NUL happened to be in it, and handed the
   garbage to the volume as a file name. Callers must check now. */
static int utf16_to_utf8(const wchar_t *u, char *c, int max)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, u, -1, c, max, NULL, NULL);
    if (n <= 0) {
        if (max > 0) c[0] = 0;
        return -1;
    }
    return 0;
}

static int path_to_utf8(const wchar_t *path, char *out, int max)
{
    if (path[0] == L'\\' && path[1] != 0)
        path++;
    if (utf16_to_utf8(path, out, max) != 0)
        return -1;
    /* normalize to forward slashes: volume logic (vol_is_dir/vol_list_dir/
       vol_ensure_path) is slash-based */
    for (char *p = out; *p; p++)
        if (*p == '\\') *p = '/';
    return 0;
}

/* The shadow file table is gone. It mirrored the volume's inode area in RAM
   and was rebuilt from disk after every create/delete/rename, so each file
   operation cost a full re-read of the area — the dominant term once a
   volume held more than a few hundred files. volume.c now keeps an O(1)
   name index, so the callbacks query it directly. */
static void rebuild_table_locked(void) { }

/* Copy out the record for `name`: inode id, size, ctime. NULL if absent. */
static fs_entry *lookup_entry(const char *name, fs_entry *out)
{
    int ok;
    EnterCriticalSection(&g_lock);
    ok = vol_stat_full(g_vol, name, &out->inode_id, &out->size,
                       &out->ctime) == 0;
    LeaveCriticalSection(&g_lock);
    if (!ok) return NULL;
    strncpy(out->name, name, sizeof out->name - 1);
    out->name[sizeof out->name - 1] = 0;
    return out;
}

/* ---- write context (buffered write-back) ---- */
typedef struct {
    char name[256];
    uint8_t *buf;
    size_t len, cap;
    BOOLEAN is_dir;
    /* Cleanup ran a delete for this handle. Close must then NOT write the
       buffer back: it holds the content the handle read at open, and writing
       it recreates the file the caller just deleted. */
    BOOLEAN deleted;
    /* The volume record existed at open, so a partial write has to be merged
       into the current content before it can be committed. loaded says that
       merge already happened.

       Reading it at open instead cost the whole file on every open, including
       the opens that never write: Explorer opens each file in a folder for
       tags and thumbnails, so browsing a directory decoded every track in it
       (49 MB took 5 s), and deleting cost a full read of the file being
       deleted. Both are pure-read opens whose buffer was thrown away. */
    BOOLEAN existed, loaded;
    uint64_t inode_id;
    uint64_t vsize;        /* size of the volume record at open */
} wctx;

static wctx *ctx_alloc(const wchar_t *path)
{
    wctx *c = (wctx *)calloc(1, sizeof(wctx));
    if (!c) return NULL;
    /* An unrepresentable path fails the allocation: every caller already has
       to handle NULL, so the bad name never reaches the volume. */
    if (path_to_utf8(path, c->name, sizeof c->name) != 0) {
        free(c);
        return NULL;
    }
    return c;
}

static void ctx_free(wctx *c)
{
    if (!c) return;
    free(c->buf);
    free(c);
}

/* ---- Dokan callbacks ---- */
static NTSTATUS create_file_impl(LPCWSTR path,
    PDOKAN_IO_SECURITY_CONTEXT sec, ACCESS_MASK access, ULONG attrs,
    ULONG share, ULONG disp, ULONG opts, PDOKAN_FILE_INFO info);
static NTSTATUS DOKAN_CALLBACK DkZwCreateFile(LPCWSTR path,
    PDOKAN_IO_SECURITY_CONTEXT sec, ACCESS_MASK access, ULONG attrs,
    ULONG share, ULONG disp, ULONG opts, PDOKAN_FILE_INFO info)
{
    CB_ENTER();
    NTSTATUS st = create_file_impl(path, sec, access, attrs, share, disp,
                                   opts, info);
    CB_LEAVE(CB_CREATE);
    return st;
}

static NTSTATUS create_file_impl(LPCWSTR path,
    PDOKAN_IO_SECURITY_CONTEXT sec, ACCESS_MASK access, ULONG attrs,
    ULONG share, ULONG disp, ULONG opts, PDOKAN_FILE_INFO info)
{
    (void)access; (void)attrs; (void)share;
    if (opts & FILE_DELETE_ON_CLOSE)
        info->DeletePending = TRUE;   /* .NET delete-on-close */
    DBG("[ZwCreateFile] %ls\n", path);
    char name[256];
    if (path_to_utf8(path, name, sizeof name) != 0)
        return STATUS_NAME_TOO_LONG;
    BOOLEAN root = (path[0] == L'\\' && path[1] == 0);

    if (root) {
        info->IsDirectory = TRUE;
        return STATUS_SUCCESS;
    }

    fs_entry ebuf;
    fs_entry *e;
    double pt = PROF_T0();
    e = lookup_entry(name, &ebuf);
    PROF_ADD(PF_LOOKUP, pt);

    /* directory open/create: signal is CreateOptions FILE_DIRECTORY_FILE
       or attrs FILE_ATTRIBUTE_DIRECTORY (Dokan versions differ); must be
       checked BEFORE the plain-file FILE_OPEN probe (dirs have no fs_entry
       under their own name — the anchor is "name/") */
    if ((opts & FILE_DIRECTORY_FILE) || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        /* open existing directory (FILE_OPEN / FILE_OPEN_IF) */
        if (disp == FILE_OPEN || disp == FILE_OPEN_IF) {
            if (vol_is_dir(g_vol, name)) {
                info->IsDirectory = TRUE;
                info->Context = 0;
                return STATUS_SUCCESS;
            }
            if (disp == FILE_OPEN)
                return STATUS_OBJECT_NAME_NOT_FOUND;
        }
        /* create directory: ONLY on FILE_CREATE — Set-Content/editors open
           FILES with FILE_DIRECTORY_FILE too (OPEN_IF), and treating that
           as mkdir silently swallows their writes */
        if (disp == FILE_CREATE) {
            EnterCriticalSection(&g_lock);
            uint64_t d = vol_mkdir(g_vol, name);
            vol_flush(g_vol);
            rebuild_table_locked();
            LeaveCriticalSection(&g_lock);
            if (!d) return STATUS_OBJECT_NAME_COLLISION;
            info->IsDirectory = TRUE;
            info->Context = 0;
            return STATUS_SUCCESS;
        }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    if (disp == FILE_OPEN && !e) {
        /* path may be an existing directory (opened without directory
           flags by the shell/explorer) */
        if (vol_is_dir(g_vol, name)) {
            info->IsDirectory = TRUE;
            info->Context = 0;
            return STATUS_SUCCESS;
        }
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (disp == FILE_CREATE && e)
        return STATUS_OBJECT_NAME_COLLISION;
    if ((disp == FILE_OPEN_IF || disp == FILE_OVERWRITE_IF) && !e &&
        vol_is_dir(g_vol, name)) {
        info->IsDirectory = TRUE;
        info->Context = 0;
        return STATUS_SUCCESS;
    }

    info->IsDirectory = FALSE;

    /* read-write open: buffered write-back context. We ALWAYS grant
       write (even for plain reads) — Windows apps (Set-Content, editors)
       open once and write into the same handle; a read-only answer
       (context=0) makes Dokan skip WriteFile entirely and the write is
       silently lost. CloseFile flushes only when len>0, so pure reads
       stay untouched. */
    wctx *c = ctx_alloc(path);
    if (!c) return STATUS_INSUFFICIENT_RESOURCES;
    if (e && disp != FILE_CREATE && disp != FILE_OVERWRITE && disp != FILE_OVERWRITE_IF) {
        /* Existing content is NOT loaded here. A handle open for plain reads
           would otherwise pay for decoding the whole file before reading a
           byte: Explorer opens every file in a folder for tags and
           thumbnails, so browsing a directory decoded every track in it
           (49 MB took 5 s), and deleting cost a full read of the file being
           deleted. The record is loaded lazily, at the first WriteFile whose
           new bytes do not provably cover the whole old file.

           The truncating dispositions are excluded on purpose: FILE_CREATE,
           FILE_OVERWRITE and FILE_OVERWRITE_IF all mean the old content is
           gone, so leaving existed clear keeps the lazy load from resurrecting
           it. */
        c->existed = TRUE;
        c->inode_id = e->inode_id;
        c->vsize = e->size;
    }
    info->Context = (ULONG64)(uintptr_t)c;
    InterlockedIncrement(&g_open_handles);
    return STATUS_SUCCESS;
}

static NTSTATUS getinfo_impl(LPCWSTR path,
    LPBY_HANDLE_FILE_INFORMATION st_out, PDOKAN_FILE_INFO info);
static NTSTATUS DOKAN_CALLBACK DkGetFileInformation(LPCWSTR path,
    LPBY_HANDLE_FILE_INFORMATION st_out, PDOKAN_FILE_INFO info)
{
    CB_ENTER();
    NTSTATUS st = getinfo_impl(path, st_out, info);
    CB_LEAVE(CB_GETINFO);
    return st;
}

static NTSTATUS getinfo_impl(LPCWSTR path,
    LPBY_HANDLE_FILE_INFORMATION buf, PDOKAN_FILE_INFO info)
{
    memset(buf, 0, sizeof *buf);
    if (path[0] == L'\\' && path[1] == 0) {
        buf->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        return STATUS_SUCCESS;
    }
    char name[256];
    if (path_to_utf8(path, name, sizeof name) != 0)
        return STATUS_NAME_TOO_LONG;
    fs_entry ebuf;
    fs_entry *e = lookup_entry(name, &ebuf);
    int is_dir = 0;
    if (!e) {
        EnterCriticalSection(&g_lock);
        is_dir = vol_is_dir(g_vol, name);
        LeaveCriticalSection(&g_lock);
    }

    /* An open handle's buffer is authoritative, exactly as it is for ReadFile
       above, and for a stronger reason here: the volume record does not exist
       until Close. Between a successful FILE_CREATE and that Close, asking the
       volume about the name gives OBJECT_NAME_NOT_FOUND -- which contradicts
       the SUCCESS this same callback pair just returned for the create. The
       driver resolves the contradiction as STATUS_INVALID_PARAMETER, which
       surfaces as 0x80070057 and made Explorer put up "Retry" for every file
       in a copy. (CopyFileEx and robocopy write without querying the fresh
       handle first, which is why they always went through cleanly.)

       Directories never get a context -- create_file_impl leaves Context 0 for
       them -- so a context here always means a file handle, and c->len is the
       current size whether the handle just created the file, truncated it, or
       loaded it for read-modify-write. Reporting e->size instead would also be
       stale for read-after-write within one handle. */
    wctx *c = (wctx *)(uintptr_t)info->Context;
    if (c && !is_dir && (c->buf || !c->existed)) {
        /* Same rule as ReadFile: an untouched buffer on a handle to an
           existing file is not a zero-byte file, it is a file whose bytes are
           still only on the volume -- and e->size below is the right answer
           for it. Reporting 0 here told Explorer every file it opened was
           empty. */
        EnterCriticalSection(&g_lock);
        uint64_t sz = (uint64_t)c->len;
        LeaveCriticalSection(&g_lock);
        buf->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        buf->nFileSizeHigh = (DWORD)(sz >> 32);
        buf->nFileSizeLow = (DWORD)(sz & 0xFFFFFFFF);
        buf->nNumberOfLinks = 1;
        if (e) {
            uint64_t ft = ((uint64_t)e->ctime + 11644473600ULL) * 10000000ULL;
            buf->ftCreationTime.dwLowDateTime = (DWORD)ft;
            buf->ftCreationTime.dwHighDateTime = (DWORD)(ft >> 32);
            buf->nFileIndexHigh = (DWORD)(e->inode_id >> 32);
            buf->nFileIndexLow = (DWORD)(e->inode_id & 0xFFFFFFFF);
        } else {
            /* Not committed yet, so there is no stored ctime to report. The
               record will be stamped at Close, a moment from now; saying "now"
               is closer than the 1601 epoch a zeroed FILETIME would show. */
            GetSystemTimeAsFileTime(&buf->ftCreationTime);
        }
        buf->ftLastWriteTime = buf->ftCreationTime;
        buf->ftLastAccessTime = buf->ftCreationTime;
        return STATUS_SUCCESS;
    }

    if (!e && !is_dir)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    buf->dwFileAttributes = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    buf->nFileSizeHigh = is_dir ? 0 : (DWORD)(e->size >> 32);
    buf->nFileSizeLow = is_dir ? 0 : (DWORD)(e->size & 0xFFFFFFFF);
    if (is_dir) {
        /* a directory's record is its anchor, "name/"; without this lookup
           every directory reports 01.01.1601 to Explorer and to Get-Item */
        char anchor[258];
        snprintf(anchor, sizeof anchor, "%s/", name);
        e = lookup_entry(anchor, &ebuf);
    }
    /* ctime is stored as unix seconds; Windows wants 100 ns ticks since 1601 */
    if (e) {
        uint64_t ft = ((uint64_t)e->ctime + 11644473600ULL) * 10000000ULL;
        buf->ftCreationTime.dwLowDateTime = (DWORD)ft;
        buf->ftCreationTime.dwHighDateTime = (DWORD)(ft >> 32);
        buf->ftLastWriteTime = buf->ftCreationTime;
        buf->ftLastAccessTime = buf->ftCreationTime;
    }
    buf->nNumberOfLinks = 1;
    if (e) {
        buf->nFileIndexHigh = (DWORD)(e->inode_id >> 32);
        buf->nFileIndexLow = (DWORD)(e->inode_id & 0xFFFFFFFF);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK DkFindFiles(LPCWSTR path, PFillFindData fill,
    PDOKAN_FILE_INFO info);

/* Thin timing wrappers: the callbacks have many return paths, so the clock
   is stopped in one place instead of at each of them. */
static NTSTATUS find_files_impl(LPCWSTR path, PFillFindData fill,
    PDOKAN_FILE_INFO info);
static NTSTATUS DOKAN_CALLBACK DkFindFiles(LPCWSTR path, PFillFindData fill,
    PDOKAN_FILE_INFO info)
{
    CB_ENTER();
    NTSTATUS st = find_files_impl(path, fill, info);
    CB_LEAVE(CB_FIND);
    return st;
}

static NTSTATUS find_files_impl(LPCWSTR path, PFillFindData fill,
    PDOKAN_FILE_INFO info)
{
    char dir[256];
    int cap = 4096, n;
    invfs_dirent *ents;
    if (path_to_utf8(path, dir, sizeof dir) != 0)
        return STATUS_NAME_TOO_LONG;
    /* path_to_utf8 has already turned separators into '/', so the root
       arrives here as "/" and the old backslash tests never matched:
       vol_list_dir got the prefix "//" and every root listing came back
       empty ("dir Y:\" -> File Not Found) while files opened by name fine. */
    while (dir[0] == '/' || dir[0] == '\\')
        memmove(dir, dir + 1, strlen(dir));

    /* vol_list_dir truncates at `max` with no way to signal it — a full
       buffer is indistinguishable from an exactly-full directory, so grow
       and retry until the result comes back short. Silently stopping at
       4096 hid entries from every listing. */
    for (;;) {
        ents = (invfs_dirent *)malloc((size_t)cap * sizeof(invfs_dirent));
        if (!ents) return STATUS_INSUFFICIENT_RESOURCES;
        EnterCriticalSection(&g_lock);
        n = vol_list_dir(g_vol, dir, ents, cap);
        LeaveCriticalSection(&g_lock);
        if (n < cap) break;
        free(ents);
        if (cap > (1 << 20)) return STATUS_INSUFFICIENT_RESOURCES;
        cap *= 2;
    }
    if (n < 0) { free(ents); return STATUS_INTERNAL_ERROR; }

    int i;
    for (i = 0; i < n; i++) {
        WIN32_FIND_DATAW fd;
        memset(&fd, 0, sizeof fd);
        MultiByteToWideChar(CP_UTF8, 0, ents[i].name, -1, fd.cFileName,
                            (int)(sizeof fd.cFileName / sizeof(WCHAR)));
        fd.dwFileAttributes = ents[i].is_dir ? FILE_ATTRIBUTE_DIRECTORY
                                             : FILE_ATTRIBUTE_NORMAL;
        {
            /* Explorer sorts and renders from these; leaving them zero
               makes every file look 0 bytes and every directory look like
               01.01.1601. vol_list_dir carries both out of the name index,
               so this used to be a stat per entry -- one seek+read each,
               and Windows enumerates after every create. */
            uint64_t ft = ((uint64_t)ents[i].ctime + 11644473600ULL) * 10000000ULL;
            if (!ents[i].is_dir) {
                fd.nFileSizeHigh = (DWORD)(ents[i].size >> 32);
                fd.nFileSizeLow = (DWORD)(ents[i].size & 0xFFFFFFFF);
            }
            fd.ftCreationTime.dwLowDateTime = (DWORD)ft;
            fd.ftCreationTime.dwHighDateTime = (DWORD)(ft >> 32);
            fd.ftLastWriteTime = fd.ftCreationTime;
            fd.ftLastAccessTime = fd.ftCreationTime;
        }
        fill(&fd, info);
    }
    free(ents);
    return STATUS_SUCCESS;
}

static NTSTATUS read_file_impl(LPCWSTR path, LPVOID buf,
    DWORD len, LPDWORD read, LONGLONG off, PDOKAN_FILE_INFO info);
static NTSTATUS DOKAN_CALLBACK DkReadFile(LPCWSTR path, LPVOID buf,
    DWORD len, LPDWORD read, LONGLONG off, PDOKAN_FILE_INFO info)
{
    CB_ENTER();
    NTSTATUS st = read_file_impl(path, buf, len, read, off, info);
    CB_LEAVE(CB_READ);
    return st;
}

static NTSTATUS read_file_impl(LPCWSTR path, LPVOID buf,
    DWORD len, LPDWORD read, LONGLONG off, PDOKAN_FILE_INFO info)
{
    char name[256];
    if (off < 0) return STATUS_INVALID_PARAMETER;
    if (path_to_utf8(path, name, sizeof name) != 0)
        return STATUS_NAME_TOO_LONG;

    /* ZwCreateFile loads existing content into the write-back buffer, and
       WriteFile only touches that buffer — so once a handle is open the
       buffer, not the volume, is authoritative. Reading through to the
       volume here would return pre-write bytes for read-after-write in
       the same handle.

       The test is on the context, not on c->buf: a file created in this
       handle has no buffer until something is written, and falling through
       to the volume for it returned OBJECT_NAME_NOT_FOUND for a handle whose
       create had just succeeded — the same contradiction GetFileInformation
       produced, and the driver turns this one into a bare "file not found"
       at a moment when the caller is holding an open handle to that file.
       An empty buffer is a legitimate zero-byte read, not a missing file. */
    wctx *c = (wctx *)(uintptr_t)info->Context;
    if (c && (c->buf || !c->existed)) {
        /* The buffer answers only once it holds something. A handle opened on
           an existing file no longer loads that file at open, so an untouched
           buffer here means "nothing written yet" and the volume still has the
           bytes -- serving 0 from the buffer would make every read of every
           existing file return EOF. c->existed distinguishes that from a file
           created in this handle, which genuinely has no volume record and for
           which an empty buffer IS the whole content. */
        EnterCriticalSection(&g_lock);
        size_t avail = (c->buf && (ULONGLONG)off < c->len)
                       ? c->len - (size_t)off : 0;
        size_t n = avail < len ? avail : len;
        if (n) memcpy(buf, c->buf + off, n);
        LeaveCriticalSection(&g_lock);
        *read = (DWORD)n;
        return STATUS_SUCCESS;
    }

    fs_entry ebuf;
    fs_entry *e = lookup_entry(name, &ebuf);
    if (!e)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if ((ULONGLONG)off >= e->size) {
        *read = 0;
        return STATUS_SUCCESS;
    }
    int got;
    EnterCriticalSection(&g_lock);
    got = vol_read_range(g_vol, e->inode_id, (uint64_t)off, len, buf);
    LeaveCriticalSection(&g_lock);
    if (got < 0)
        return STATUS_INTERNAL_ERROR;
    *read = (DWORD)got;
    return STATUS_SUCCESS;
}

static NTSTATUS write_file_impl(LPCWSTR path, LPCVOID buf,
    DWORD len, LPDWORD written, LONGLONG off, PDOKAN_FILE_INFO info);
static NTSTATUS DOKAN_CALLBACK DkWriteFile(LPCWSTR path, LPCVOID buf,
    DWORD len, LPDWORD written, LONGLONG off, PDOKAN_FILE_INFO info)
{
    CB_ENTER();
    NTSTATUS st = write_file_impl(path, buf, len, written, off, info);
    CB_LEAVE(CB_WRITE);
    return st;
}

static NTSTATUS write_file_impl(LPCWSTR path, LPCVOID buf,
    DWORD len, LPDWORD written, LONGLONG off, PDOKAN_FILE_INFO info)
{
    (void)path;
    wctx *c = (wctx *)(uintptr_t)info->Context;
    if (!c)
        return STATUS_INVALID_HANDLE;
    if (!vol_write_enabled(g_vol))
        return STATUS_DISK_FULL;   /* volume hit hard-min floor */
    /* ENOSPC early-out: refuse before buffering when the file's projected
     * size would breach the reserve+floor (apps see the error on
     * WriteFile, not silently at Close) */
    if (vol_free_blocks_cached(g_vol) <
        vol_write_guard(g_vol) +
        ((uint64_t)c->len + len + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE)
        return STATUS_DISK_FULL;
    /* The inode area is a separate, append-only budget and runs out long
       before the data zones do: it used to be a flat 2 MB, so a volume of any
       size stopped accepting new files at ~6200. When it is exhausted
       vol_create_file returns 0 at Close -- which is void and cannot report
       anything -- so the write was accepted and then silently dropped.
       Refuse here instead, where the application still sees the error. */
    if (vol_inode_area_free(g_vol) < INVFS_INODE_REC_MAX)
        return STATUS_DISK_FULL;
    DBG("[Write] off=%lld len=%u\n", (long long)off, len);
    EnterCriticalSection(&g_lock);
    /* Pull the old content in now, unless this write makes it irrelevant.
       Only a write starting at 0 that reaches at least the old end replaces
       the record outright; anything shorter leaves a tail that has to survive,
       and anything starting past c->len leaves a hole. Getting this wrong
       truncates a file on a partial rewrite, so the test is the conservative
       one: load unless the new bytes provably cover the old.
       This is the load that used to happen on every open. */
    if (c->existed && !c->loaded && !c->deleted &&
        !(off == 0 && (uint64_t)len >= c->vsize)) {
        uint8_t *data = NULL; size_t dlen = 0;
        double pt2 = PROF_T0();
        if (vol_read_file(g_vol, c->inode_id, &data, &dlen) == 0 && dlen > 0) {
            if (dlen > c->cap) {
                uint8_t *nb = (uint8_t *)realloc(c->buf, dlen);
                if (!nb) {
                    free(data);
                    LeaveCriticalSection(&g_lock);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                c->buf = nb;
                c->cap = dlen;
            }
            memcpy(c->buf, data, dlen);
            if (dlen > c->len) c->len = dlen;
        }
        free(data);
        PROF_ADD(PF_READBACK, pt2);
        c->loaded = TRUE;
    }
    size_t need = (size_t)off + len;
    if (need > c->cap) {
        size_t ncap = c->cap ? c->cap : 4096;
        while (ncap < need) ncap *= 2;
        uint8_t *nb = (uint8_t *)realloc(c->buf, ncap);
        if (!nb) {
            LeaveCriticalSection(&g_lock);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        c->buf = nb;
        c->cap = ncap;
    }
    if (need > c->len)
        memset(c->buf + c->len, 0, need - c->len);
    memcpy(c->buf + off, buf, len);
    if (need > c->len) c->len = need;
    LeaveCriticalSection(&g_lock);
    *written = len;
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK DkFlushFileBuffers(LPCWSTR path, PDOKAN_FILE_INFO info)
{
    DBG("[Flush] %ls\n", path);
    wctx *c = (wctx *)(uintptr_t)info->Context;
    if (!c || (c->buf && c->len == 0)) return STATUS_SUCCESS;
    if (!c->buf || c->len == 0) return STATUS_SUCCESS;
    EnterCriticalSection(&g_lock);
    if (!vol_write_enabled(g_vol)) {
        LeaveCriticalSection(&g_lock);
        return STATUS_DISK_FULL;
    }
    vol_ensure_path(g_vol, c->name);   /* auto-create parent dirs */
    /* Replace, do not delete-then-create: the old order tombstoned the user's
       file before it knew the new one would fit, so an ENOSPC overwrite
       destroyed both. It also stranded the old file's !recipe siblings. */
    uint64_t nid = vol_replace_file(g_vol, c->name, c->buf, c->len);
    if (nid == 0) {
        fprintf(stderr, "invf-dokan: vol_replace_file failed (%s)\n", c->name);
        LeaveCriticalSection(&g_lock);
        return STATUS_DISK_FULL;
    }
    vol_mark_pending(g_vol, nid);   /* on-demand sweep */
    vol_flush(g_vol);
    rebuild_table_locked();
    LeaveCriticalSection(&g_lock);
    return STATUS_SUCCESS;
}

/* SetEndOfFile / allocation: resize the buffered write context (no real
   preallocation on our side — allocation is on flush). */
static NTSTATUS DOKAN_CALLBACK DkSetAllocationSize(LPCWSTR path,
    LONGLONG alloc_size, PDOKAN_FILE_INFO info)
{
    (void)path;
    wctx *c = (wctx *)(uintptr_t)info->Context;
    if (alloc_size < 0) return STATUS_INVALID_PARAMETER;
    if (!c) return STATUS_SUCCESS;   /* read-only handle: nothing to do */
    EnterCriticalSection(&g_lock);
    size_t need = (size_t)alloc_size;
    if (need > c->cap) {
        size_t ncap = c->cap ? c->cap : 4096;
        while (ncap < need) ncap *= 2;
        uint8_t *nb = (uint8_t *)realloc(c->buf, ncap);
        if (!nb) { LeaveCriticalSection(&g_lock); return STATUS_INSUFFICIENT_RESOURCES; }
        c->buf = nb;
        c->cap = ncap;
    }
    if (need < c->len) c->len = need;   /* truncate */
    else if (need > c->len) {
        memset(c->buf + c->len, 0, need - c->len);
        c->len = need;
    }
    LeaveCriticalSection(&g_lock);
    return STATUS_SUCCESS;
}

/* SetEndOfFile: truncate/extend the buffered write context. Set-Content
   calls this BEFORE writing (size 0) — without the callback Dokan
   answers STATUS_INVALID_PARAMETER and the app never reaches WriteFile. */
static NTSTATUS DOKAN_CALLBACK DkSetEndOfFile(LPCWSTR path,
    LONGLONG byte_offset, PDOKAN_FILE_INFO info)
{
    return DkSetAllocationSize(path, byte_offset, info);
}

/* SetFileAttributes: attributes are virtual — just acknowledge. */
static NTSTATUS DOKAN_CALLBACK DkSetFileAttributes(LPCWSTR path,
    DWORD attrs, PDOKAN_FILE_INFO info)
{
    (void)path; (void)attrs; (void)info;
    return STATUS_SUCCESS;
}

/* MoveFile: without this, saving from Notepad, VS Code or anything else that
   writes to a temp file and renames over the target fails outright. */
static NTSTATUS DOKAN_CALLBACK DkMoveFile(LPCWSTR path, LPCWSTR new_path,
    BOOL replace_existing, PDOKAN_FILE_INFO info)
{
    char from[256], to[256];
    int rc;
    if (path_to_utf8(path, from, sizeof from) != 0 ||
        path_to_utf8(new_path, to, sizeof to) != 0)
        return STATUS_NAME_TOO_LONG;
    DBG("[MoveFile] %s -> %s (replace=%d)\n", from, to, (int)replace_existing);

    /* the source may still have buffered writes in this handle; get them on
       disk first, otherwise the rename moves the pre-write version and the
       flush at Cleanup then recreates the file under the OLD name */
    wctx *c = (wctx *)(uintptr_t)info->Context;
    if (c && c->buf && c->len > 0) {
        NTSTATUS st = DkFlushFileBuffers(path, info);
        if (st != STATUS_SUCCESS) return st;
        c->len = 0;   /* flushed; do not write it again at Close */
    }

    EnterCriticalSection(&g_lock);
    if (!vol_write_enabled(g_vol)) {
        LeaveCriticalSection(&g_lock);
        return STATUS_DISK_FULL;
    }
    if (vol_find(g_vol, to) != 0 || vol_is_dir(g_vol, to)) {
        if (!replace_existing) {
            LeaveCriticalSection(&g_lock);
            return STATUS_OBJECT_NAME_COLLISION;
        }
        if (vol_is_dir(g_vol, to)) {
            LeaveCriticalSection(&g_lock);
            return STATUS_ACCESS_DENIED;   /* never clobber a directory */
        }
        /* clobbering the destination drops its transcode siblings with it;
           vol_delete_file left them behind as unreachable live records */
        vol_unlink(g_vol, to);
    }
    rc = vol_rename(g_vol, from, to);
    if (rc == 0) {
        vol_flush(g_vol);
        rebuild_table_locked();
    }
    LeaveCriticalSection(&g_lock);

    if (c) strncpy(c->name, to, sizeof(c->name) - 1);   /* follow the move */
    switch (rc) {
    case 0:  return STATUS_SUCCESS;
    case -2: return STATUS_OBJECT_NAME_COLLISION;
    case -3: return STATUS_DISK_FULL;
    default: return STATUS_OBJECT_NAME_NOT_FOUND;
    }
}

/* DeleteFile/DeleteDirectory are Windows' "can this be deleted?" probe. They
   must NOT delete — Cleanup does that once DeletePending is set. Answering
   here is what makes the error surface at DeleteFile() instead of silently
   at close. */
static NTSTATUS DOKAN_CALLBACK DkDeleteFile(LPCWSTR path, PDOKAN_FILE_INFO info)
{
    char name[256];
    uint64_t id;
    if (path_to_utf8(path, name, sizeof name) != 0)
        return STATUS_NAME_TOO_LONG;
    if (!info->DeletePending) return STATUS_SUCCESS;   /* cancel notification */
    EnterCriticalSection(&g_lock);
    id = vol_find(g_vol, name);
    int isdir = vol_is_dir(g_vol, name);
    int ro = !vol_write_enabled(g_vol);
    LeaveCriticalSection(&g_lock);
    if (ro) return STATUS_MEDIA_WRITE_PROTECTED;
    if (isdir) return STATUS_FILE_IS_A_DIRECTORY;
    /* A handle open on an uncommitted file has no volume record to find yet,
       so vol_find gives 0 for a file the caller demonstrably has open. This
       is how a copy tool cleans up after a failed transfer: create, write,
       fail, then delete the partial destination. Refusing that with
       "not found" leaves the partial file to be committed at Close. */
    if (id == 0 && info->Context == 0) return STATUS_OBJECT_NAME_NOT_FOUND;
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK DkDeleteDirectory(LPCWSTR path, PDOKAN_FILE_INFO info)
{
    char name[256];
    int n;
    invfs_dirent probe[2];
    if (path_to_utf8(path, name, sizeof name) != 0)
        return STATUS_NAME_TOO_LONG;
    if (!info->DeletePending) return STATUS_SUCCESS;
    EnterCriticalSection(&g_lock);
    int ro = !vol_write_enabled(g_vol);
    int isdir = vol_is_dir(g_vol, name);
    n = isdir ? vol_list_dir(g_vol, name, probe, 2) : 0;
    LeaveCriticalSection(&g_lock);
    if (ro) return STATUS_MEDIA_WRITE_PROTECTED;
    if (!isdir) return STATUS_OBJECT_NAME_NOT_FOUND;
    if (n > 0) return STATUS_DIRECTORY_NOT_EMPTY;
    return STATUS_SUCCESS;
}

/* SetFileTime: timestamps are not stored per-field (the inode record has a
   single ctime, written at create). Accept and drop rather than fail — a
   STATUS_NOT_IMPLEMENTED here makes copy and archive tools abort the whole
   operation after the data was already written correctly. */
static NTSTATUS DOKAN_CALLBACK DkSetFileTime(LPCWSTR path, CONST FILETIME *creation,
    CONST FILETIME *access, CONST FILETIME *write, PDOKAN_FILE_INFO info)
{
    (void)path; (void)creation; (void)access; (void)write; (void)info;
    return STATUS_SUCCESS;
}

static void cleanup_impl(LPCWSTR path, PDOKAN_FILE_INFO info);
static void DOKAN_CALLBACK DkCleanup(LPCWSTR path, PDOKAN_FILE_INFO info)
{
    CB_ENTER();
    cleanup_impl(path, info);
    CB_LEAVE(CB_CLEANUP);
}

static void cleanup_impl(LPCWSTR path, PDOKAN_FILE_INFO info)
{
    (void)path;
    if (info->DeletePending) {
        char name[256];
        /* void callback: a name the volume cannot store also cannot name
           anything already on it, so there is nothing here to delete. */
        if (path_to_utf8(path, name, sizeof name) != 0)
            return;
        EnterCriticalSection(&g_lock);
        if (info->IsDirectory) {
            if (vol_rmdir(g_vol, name) == 0) {
                vol_flush(g_vol);
                rebuild_table_locked();
            }
        } else if (vol_unlink(g_vol, name) == 0) {
            vol_flush(g_vol);
            rebuild_table_locked();
        }
        /* Tell Close the file is gone. Opening for delete still loads the
           existing content into the write-back buffer, and Close writes any
           non-empty buffer back unconditionally -- so without this the record
           removed a line above is recreated a moment later and the delete
           reports success while the file stays on the volume. */
        wctx *c = (wctx *)(uintptr_t)info->Context;
        if (c) c->deleted = TRUE;
        LeaveCriticalSection(&g_lock);
    }
}

static void close_file_impl(LPCWSTR path, PDOKAN_FILE_INFO info);
static void DOKAN_CALLBACK DkCloseFile(LPCWSTR path, PDOKAN_FILE_INFO info)
{
    CB_ENTER();
    close_file_impl(path, info);
    CB_LEAVE(CB_CLOSE);
}

static void close_file_impl(LPCWSTR path, PDOKAN_FILE_INFO info)
{
    DBG("[Close] %ls\n", path);
    wctx *c = (wctx *)(uintptr_t)info->Context;
    if (c) {
        /* flush pending writes if not flushed yet. CloseFile is void — a
           failure here cannot be reported to the caller, so it is logged
           and the context is released either way; leaking it (and the
           handle count) would wedge the background sweep forever. */
        if (c->buf && c->len > 0 && !c->deleted) {
            double pt;
            EnterCriticalSection(&g_lock);
            pt = PROF_T0();
            uint64_t nid = vol_replace_file(g_vol, c->name, c->buf, c->len);
            PROF_ADD(PF_CREATE, pt);
            if (nid == 0) {
                /* the previous version is still on the volume: replace appends
                   before it tombstones, so a failure here loses only the write
                   we could not fit, not the file it would have overwritten */
                fprintf(stderr, "invf-dokan: replace fail (%s)\n", c->name);
            } else {
                vol_mark_pending(g_vol, nid);   /* on-demand sweep */
                pt = PROF_T0();
                vol_flush(g_vol);
                PROF_ADD(PF_FLUSH, pt);
                rebuild_table_locked();
            }
            prof_tick();                 /* under g_lock: the report reads
                                            and resets shared counters */
            LeaveCriticalSection(&g_lock);
        }
        ctx_free(c);
        info->Context = 0;
        InterlockedDecrement(&g_open_handles);
    }
}

/* background on-demand sweep: drain pending list while the volume is
   idle (no open handles).

   OFF by default. Sweeping is a foreground operation (invf-sweep) by design:
   the transcode holds g_lock, which also serializes every write callback, so
   a batch of audio files keeps the lock for minutes. Dokan gives a callback
   opt.Timeout to answer and aborts it otherwise, so a copy landing during a
   sweep does not merely wait -- the driver kills it and the application sees
   a hard error (Explorer 0x80070057, PowerShell 0x800705AA) on an arbitrary
   file. Nothing is lost by deferring: alloc_raw_or_shadow() spills into the
   Shadow zone once RAW is full, so an unswept volume still uses its whole
   capacity, just without the space savings until invf-sweep runs.

   Set INVFS_SWEEP_INTERVAL (seconds) to re-enable it. */
static volatile LONG g_sweep_stop = 0;
static HANDLE g_sweep_thread = NULL;

static DWORD WINAPI sweep_thread(LPVOID arg)
{
    (void)arg;
    const char *iv = getenv("INVFS_SWEEP_INTERVAL");
    DWORD interval;
    if (!iv || atoi(iv) <= 0) return 0;   /* disabled unless asked for */
    interval = (DWORD)atoi(iv) * 1000;
    if (interval < 1000) interval = 1000;
    while (!g_sweep_stop) {
        /* wake in 250 ms slices so unmount is not blocked for a full
           interval waiting on a sleeping thread */
        DWORD waited = 0;
        while (waited < interval && !g_sweep_stop) {
            Sleep(250);
            waited += 250;
        }
        if (g_sweep_stop) break;
        if (g_open_handles != 0) continue;
        EnterCriticalSection(&g_lock);
        /* re-check under the lock: the test above raced with a create that
           had not yet bumped the counter, and the sweep then held the lock
           for the whole batch while that write sat behind it */
        if (g_open_handles == 0 && vol_pending_count(g_vol) > 0) {
            int n = vol_sweep_pending(g_vol);
            if (n > 0) {
                vol_flush(g_vol);
                rebuild_table_locked();
                fprintf(stderr, "[sweep] on-demand: processed %d pending\n", n);
            }
        }
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

static void stop_sweep_thread(void)
{
    HANDLE t = g_sweep_thread;
    if (!t) return;
    g_sweep_thread = NULL;
    InterlockedExchange(&g_sweep_stop, 1);
    /* the sweep holds g_lock while working; give it time to finish a
       sweep_pending pass rather than killing it mid-journal-write */
    if (WaitForSingleObject(t, 30000) == WAIT_TIMEOUT)
        fprintf(stderr, "invf-dokan: sweep thread did not stop in 30s\n");
    CloseHandle(t);
}

/* Dokan 2.x passes the resolved mount point here. The old 1.x signature
   (PDOKAN_FILE_INFO only) still compiles as a warning and then corrupts
   the argument — C4113. */
static NTSTATUS DOKAN_CALLBACK DkMounted(LPCWSTR mount_point, PDOKAN_FILE_INFO info)
{
    (void)info;
    DBG("[Mounted] %ls\n", mount_point);
    g_sweep_stop = 0;
    g_sweep_thread = CreateThread(NULL, 0, sweep_thread, NULL, 0, NULL);
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK DkUnmounted(PDOKAN_FILE_INFO info)
{
    (void)info;
    DBG("[Unmounted]\n");
    stop_sweep_thread();
    /* last chance to get the journal on disk before the process exits */
    EnterCriticalSection(&g_lock);
    vol_flush(g_vol);
    LeaveCriticalSection(&g_lock);
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK DkGetDiskFreeSpace(PULONGLONG free_avail,
    PULONGLONG total, PULONGLONG total_free, PDOKAN_FILE_INFO info)
{
    (void)info;
    const invfs_superblock *sb = vol_sb(g_vol);
    uint64_t free_blocks;
    EnterCriticalSection(&g_lock);
    free_blocks = vol_count_free(g_vol);
    LeaveCriticalSection(&g_lock);
    *total = sb->total_blocks * (uint64_t)INVFS_BLOCK_SIZE;
    *free_avail = free_blocks * (uint64_t)INVFS_BLOCK_SIZE;
    *total_free = free_blocks * (uint64_t)INVFS_BLOCK_SIZE;
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK DkGetVolumeInformation(LPWSTR vol_name,
    DWORD vol_name_size, LPDWORD serial, LPDWORD max_comp,
    LPDWORD flags, LPWSTR fs_name, DWORD fs_name_size, PDOKAN_FILE_INFO info)
{
    (void)info;
    /* Dokan passes these sizes in CHARACTERS, not bytes — dividing by
       sizeof(WCHAR) halved the usable buffer. */
    wcscpy_s(vol_name, vol_name_size, L"InvariantFS");
    *serial = 0x494E5646;
    *max_comp = 255;
    /* no FILE_SUPPORTS_REPARSE_POINTS: reparse points are not implemented,
       and advertising them makes the shell issue calls we answer wrongly */
    *flags = FILE_CASE_SENSITIVE_SEARCH | FILE_CASE_PRESERVED_NAMES |
             FILE_UNICODE_ON_DISK;
    /* This is the string Explorer, `fsutil fsinfo volumeinfo` and the "File
       system" column show for a mounted volume. It is not the on-disk magic
       (still "InvariFS", 8 bytes, and changing that would invalidate every
       existing volume) -- it is only what we tell Windows we are, so it can
       be spelled out in full. */
    wcscpy_s(fs_name, fs_name_size, L"InvariantFS");
    return STATUS_SUCCESS;
}

/* Windows probes for one specific name far more often than it enumerates:
   every create asks the directory whether the file is already there, and the
   query carries the exact file name as the search pattern. With only
   FindFiles implemented, Dokan answers that by enumerating the whole
   directory and filtering the result itself -- O(names in the directory) per
   created file, which was the last term growing with volume size (1.25 ms of
   a 1.45 ms write at 1600 files).
   Wildcard patterns still need the full listing, so those return
   STATUS_NOT_IMPLEMENTED and Dokan falls back to FindFiles. */
static NTSTATUS DOKAN_CALLBACK DkFindFilesWithPattern(LPCWSTR path,
    LPCWSTR pattern, PFillFindData fill, PDOKAN_FILE_INFO info)
{
    char dir[256], pat[256], full[520];
    WIN32_FIND_DATAW fd;
    const wchar_t *p;
    uint64_t id, size, ctime, ft;
    int is_dir;
    NTSTATUS st;

    if (!pattern) return STATUS_NOT_IMPLEMENTED;
    for (p = pattern; *p; p++)
        if (*p == L'*' || *p == L'?' || *p == L'<' || *p == L'>' || *p == L'"')
            return STATUS_NOT_IMPLEMENTED;
    if (p == pattern) return STATUS_NOT_IMPLEMENTED;   /* empty */

    CB_ENTER();
    if (path_to_utf8(path, dir, sizeof dir) != 0) {
        CB_LEAVE(CB_FIND);
        return STATUS_NAME_TOO_LONG;
    }
    while (dir[0] == '/' || dir[0] == '\\')
        memmove(dir, dir + 1, strlen(dir));
    if (WideCharToMultiByte(CP_UTF8, 0, pattern, -1, pat, sizeof pat,
                            NULL, NULL) == 0) {
        CB_LEAVE(CB_FIND);
        return STATUS_NOT_IMPLEMENTED;
    }
    /* "." and ".." are Dokan's business, not ours */
    if (strcmp(pat, ".") == 0 || strcmp(pat, "..") == 0 || strchr(pat, '/')) {
        CB_LEAVE(CB_FIND);
        return STATUS_NOT_IMPLEMENTED;
    }

    if (dir[0]) snprintf(full, sizeof full, "%s/%s", dir, pat);
    else        snprintf(full, sizeof full, "%s", pat);

    EnterCriticalSection(&g_lock);
    is_dir = vol_is_dir(g_vol, full);
    if (is_dir) {
        char anchor[522];
        snprintf(anchor, sizeof anchor, "%s/", full);
        if (vol_stat_full(g_vol, anchor, &id, &size, &ctime) != 0)
            ctime = 0;
        size = 0;
        st = STATUS_SUCCESS;
    } else {
        st = vol_stat_full(g_vol, full, &id, &size, &ctime) == 0
           ? STATUS_SUCCESS : STATUS_NO_SUCH_FILE;
    }
    LeaveCriticalSection(&g_lock);

    /* An empty result is a legitimate answer -- the file does not exist yet.
       Reporting it costs the caller nothing and saves the full enumeration. */
    if (st != STATUS_SUCCESS) {
        CB_LEAVE(CB_FIND);
        return STATUS_SUCCESS;
    }

    memset(&fd, 0, sizeof fd);
    MultiByteToWideChar(CP_UTF8, 0, pat, -1, fd.cFileName,
                        (int)(sizeof fd.cFileName / sizeof(WCHAR)));
    fd.dwFileAttributes = is_dir ? FILE_ATTRIBUTE_DIRECTORY
                                 : FILE_ATTRIBUTE_NORMAL;
    if (!is_dir) {
        fd.nFileSizeHigh = (DWORD)(size >> 32);
        fd.nFileSizeLow = (DWORD)(size & 0xFFFFFFFF);
    }
    ft = (ctime + 11644473600ULL) * 10000000ULL;
    fd.ftCreationTime.dwLowDateTime = (DWORD)ft;
    fd.ftCreationTime.dwHighDateTime = (DWORD)(ft >> 32);
    fd.ftLastWriteTime = fd.ftCreationTime;
    fd.ftLastAccessTime = fd.ftCreationTime;
    fill(&fd, info);
    CB_LEAVE(CB_FIND);
    return STATUS_SUCCESS;
}

static DOKAN_OPERATIONS g_ops = {
    .ZwCreateFile = DkZwCreateFile,
    .GetFileInformation = DkGetFileInformation,
    .FindFiles = DkFindFiles,
    .FindFilesWithPattern = DkFindFilesWithPattern,
    .ReadFile = DkReadFile,
    .WriteFile = DkWriteFile,
    .FlushFileBuffers = DkFlushFileBuffers,
    .Cleanup = DkCleanup,
    .SetAllocationSize = DkSetAllocationSize,
    .SetEndOfFile = DkSetEndOfFile,
    .SetFileAttributes = DkSetFileAttributes,
    .SetFileTime = DkSetFileTime,
    .MoveFile = DkMoveFile,
    .DeleteFile = DkDeleteFile,
    .DeleteDirectory = DkDeleteDirectory,
    .CloseFile = DkCloseFile,
    .Mounted = DkMounted,
    .Unmounted = DkUnmounted,
    .GetDiskFreeSpace = DkGetDiskFreeSpace,
    .GetVolumeInformation = DkGetVolumeInformation,
};

int wmain(int argc, wchar_t **argv)
{
    int err;
    char img[1024];

    if (argc < 3) {
        fprintf(stderr, "usage: invf-dokan <image> <mountpoint>\n");
        return 2;
    }
    WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, img, sizeof img, NULL, NULL);
    {
        const char *d = getenv("INVFS_DEBUG");
        g_debug = (d && *d && *d != '0');
        d = getenv("INVFS_PROF");
        g_prof = (d && *d && *d != '0');
        d = getenv("INVFS_PROF_N");
        if (d && *d) {
            int n = atoi(d);
            if (n > 0) g_prof_bucket = n;
        }
    }
    if (g_debug || g_prof)
        setvbuf(stderr, NULL, _IONBF, 0);   /* live debug log (crash-safe) */

    g_vol = vol_open(img, &err);
    if (!g_vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        return 1;
    }
    InitializeCriticalSection(&g_lock);

    DOKAN_OPTIONS opt;
    memset(&opt, 0, sizeof opt);
    opt.Version = DOKAN_VERSION;
    opt.SingleThread = FALSE;
    opt.Options = DOKAN_OPTION_ALT_STREAM | DOKAN_OPTION_CASE_SENSITIVE |
                  DOKAN_OPTION_MOUNT_MANAGER;
    if (g_debug)
        opt.Options |= DOKAN_OPTION_DEBUG | DOKAN_OPTION_STDERR;
    opt.MountPoint = argv[2];
    /* Ceiling on how long the driver waits for a callback, not a delay: a
       single write goes through a transcoder while holding g_lock, and 10 s
       was low enough that a large file behind another write got aborted
       mid-copy instead of just queued. */
    opt.Timeout = 120000;

    wprintf(L"InvariantFS mounted via Dokan: %llu names\n",
            (unsigned long long)vol_name_count(g_vol));
    fflush(stdout);
    DokanInit();
    int rc;
    __try {
        rc = DokanMain(&opt, &g_ops);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wprintf(L"DokanMain EXCEPTION: 0x%lx\n", (ULONG)GetExceptionCode());
        fflush(stdout);
        rc = -1;
    }
    DokanShutdown();
    wprintf(L"DokanMain returned: %d\n", rc);
    fflush(stdout);
    /* Unmounted normally stops it; on an abnormal exit it is still running
       and would race vol_close */
    stop_sweep_thread();
    vol_close(g_vol);
    DeleteCriticalSection(&g_lock);
    return rc;
}
