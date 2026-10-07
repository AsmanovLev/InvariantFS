/*
 * blkio.c -- backing-store abstraction: image file or raw block device.
 * See blkio.h for why this exists and why devices are unbuffered.
 */
#define _CRT_SECURE_NO_WARNINGS

#include "perf_counters.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <pthread.h>   /* group commit: the flusher mutex/condvar */
#ifdef __linux__
#include <linux/fs.h>
#endif
#endif

#include "blkio.h"

/* ------------------------------------------------------------------ *
 * path handling
 * ------------------------------------------------------------------ */

int blkio_looks_like_device(const char *path)
{
    if (!path || !path[0])
        return 0;
#ifdef _WIN32
    /* "\\.\X:" or "\\?\X:" -- a volume. */
    if ((path[0] == '\\' && path[1] == '\\' &&
         (path[2] == '.' || path[2] == '?') && path[3] == '\\')) {
        const char *tail = path + 4;
        size_t n = strlen(tail);
        if (n == 2 && tail[1] == ':')
            return 1;
        /* \\.\PhysicalDriveN, \\.\HarddiskN, \\.\HarddiskVolumeN, \\?\Volume{..} */
        if (_strnicmp(tail, "PhysicalDrive", 13) == 0 ||
            _strnicmp(tail, "Harddisk", 8) == 0 ||
            _strnicmp(tail, "Volume", 6) == 0 ||
            _strnicmp(tail, "CdRom", 5) == 0)
            return 1;
        return 0;
    }
    /* bare "W:" -- a drive letter with no path after it */
    if (path[1] == ':' && path[2] == 0 &&
        ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')))
        return 1;
    return 0;
#else
    /* A path is a device when it IS one, not when it merely lives under
     * /dev: /dev/shm is a tmpfs, and so is every /dev/<dir> some systems
     * mount there. Stat it and ask the kernel -- a regular-file volume image
     * must open as a file (get its size with fstat, not BLKGETSIZE64), and
     * BLKGETSIZE64 on a tmpfs file fails with ENOTTY, which used to make
     * every volume under /dev/shm unopenable by path. */
    {
        struct stat st;
        if (path[0] != '/') return 0;
        if (stat(path, &st) != 0) return 0;   /* not there yet (BLKIO_CREATE) */
        return S_ISBLK(st.st_mode) ? 1 : 0;
    }
#endif
}

const char *blkio_normalize(const char *path, char *out, size_t outsz)
{
    if (!path || !out || outsz < 8)
        return path;
#ifdef _WIN32
    if (path[1] == ':' && path[2] == 0) {
        _snprintf(out, outsz, "\\\\.\\%c:", path[0]);
        out[outsz - 1] = 0;
        return out;
    }
    /* A drive letter with a trailing separator ("W:\") is what tab-completion
       and most shells hand over. It means the same volume. */
    if (path[1] == ':' && (path[2] == '\\' || path[2] == '/') && path[3] == 0) {
        _snprintf(out, outsz, "\\\\.\\%c:", path[0]);
        out[outsz - 1] = 0;
        return out;
    }
#endif
    (void)outsz;
    return path;
}

const char *blkio_strerror(int rc)
{
    switch (rc < 0 ? -rc : rc) {
    case BLKIO_E_OPEN:     return "cannot open backing store "
                                  "(a raw device needs Administrator rights)";
    case BLKIO_E_REFUSED:  return "refused: this device is not a safe target";
    case BLKIO_E_GEOMETRY: return "cannot determine device size or sector size";
    case BLKIO_E_SECTOR:   return "device sector size is not a divisor of 4096";
    case BLKIO_E_LOCK:     return "cannot lock/dismount the volume "
                                  "(close anything using it, incl. Explorer)";
    case BLKIO_E_NOMEM:    return "out of memory";
    case BLKIO_E_TOOSMALL: return "device is smaller than the requested volume";
    default:               return "unknown error";
    }
}

#ifdef _WIN32
/* Whole physical disks are never a valid target. A partition table lives at
   LBA 0 and the neighbouring partitions live right after it, so writing a
   filesystem at PhysicalDriveN offset 0 destroys the partition table and
   whatever else shares the disk. A volume handle, by contrast, is windowed by
   the OS onto exactly one partition -- a seek past its end fails instead of
   landing in the neighbour. Only volume handles are accepted. */
static int dev_path_allowed(const char *p)
{
    const char *tail;
    if (!(p[0] == '\\' && p[1] == '\\' && (p[2] == '.' || p[2] == '?') &&
          p[3] == '\\'))
        return 0;
    tail = p + 4;
    if (_strnicmp(tail, "PhysicalDrive", 13) == 0)
        return 0;
    /* \\.\Harddisk0Partition6 addresses a partition, but through the disk
       object -- a driver bug or an off-by-one in our own seek arithmetic is
       not clamped the way a volume handle clamps it. Not worth the risk. */
    if (_strnicmp(tail, "Harddisk", 8) == 0)
        return 0;
    if (strlen(tail) == 2 && tail[1] == ':')
        return 1;                       /* \\.\W: */
    if (_strnicmp(tail, "Volume{", 7) == 0)
        return 1;                       /* \\?\Volume{guid} */
    if (_strnicmp(tail, "HarddiskVolume", 14) == 0)
        return 1;
    return 0;
}
#endif

/* ------------------------------------------------------------------ *
 * open / close
 * ------------------------------------------------------------------ */

#ifdef _WIN32

static int dev_geometry(HANDLE h, uint64_t *len, unsigned *sector)
{
    GET_LENGTH_INFORMATION gli;
    DWORD ret = 0;
    STORAGE_PROPERTY_QUERY spq;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR saad;
    DISK_GEOMETRY_EX geo;

    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0,
                         &gli, sizeof gli, &ret, NULL))
        return -1;
    *len = (uint64_t)gli.Length.QuadPart;

    /* Preferred source: the storage stack's alignment descriptor, which
       reports the logical sector size directly. */
    memset(&spq, 0, sizeof spq);
    spq.PropertyId = StorageAccessAlignmentProperty;
    spq.QueryType  = PropertyStandardQuery;
    memset(&saad, 0, sizeof saad);
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &spq, sizeof spq,
                        &saad, sizeof saad, &ret, NULL) &&
        saad.BytesPerLogicalSector) {
        *sector = saad.BytesPerLogicalSector;
        return 0;
    }
    /* Fallback: drive geometry. Works on volume handles too. */
    memset(&geo, 0, sizeof geo);
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                        &geo, sizeof geo, &ret, NULL) &&
        geo.Geometry.BytesPerSector) {
        *sector = geo.Geometry.BytesPerSector;
        return 0;
    }
    /* Last resort. 512 is the universal floor and always divides 4096, so an
       over-conservative guess costs nothing but a slightly larger bounce. */
    *sector = 512;
    return 0;
}

static int dev_lock(HANDLE h)
{
    DWORD ret = 0;
    int attempt;
    /* FSCTL_LOCK_VOLUME fails while any handle to the volume is open --
       Explorer, an antivirus scanner or a search indexer will all hold one
       briefly. Retrying is normal practice and usually succeeds. */
    for (attempt = 0; attempt < 20; attempt++) {
        if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &ret, NULL))
            break;
        Sleep(250);
    }
    if (attempt == 20)
        return -1;
    /* Dismount so the mounted filesystem (FAT32, NTFS, ...) stops believing
       it owns these sectors and does not write its own metadata back over
       ours when it is finally unmounted. */
    if (!DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ret, NULL))
        return -1;
    return 0;
}

#endif /* _WIN32 */

/* Forward declarations: blkio_write (above) needs the policy clock, and
 * blkio_open needs the policy applier, but both are defined further down with
 * the rest of the policy code. */
static uint64_t pf_now_ms(void);

int blkio_open(blkio *io, const char *path, int flags)
{
    /* INVFS_FORCE_DEV=1 makes an image file take the device code path:
     * sector alignment, the bounce buffer, read-modify-write for partial
     * sectors -- everything except NO_BUFFERING (a plain file cannot be
     * opened unbuffered at an arbitrary size, and the point here is the
     * alignment logic, not the flush behaviour).
     *
     * Without this, the device path could only be exercised on real
     * hardware with elevation, so a bug that lived there survived every
     * test run: writing a 64-byte journal record per 64 KB segment was
     * free on a buffered image and ~80 ms on the flash drive, which is
     * what killed the mount mid-copy. Now `INVFS_FORCE_DEV=1` on any
     * image reproduces the raw-device access pattern. */
    const char *fdev = getenv("INVFS_FORCE_DEV");
    int force_dev = (fdev && *fdev && *fdev != '0');

    memset(io, 0, sizeof *io);
    /* Deferred-commit policy comes from the environment so every tool inherits
     * it without a new argument on every CLI. With no environment set, all
     * thresholds stay zero, which IS strict -- byte-for-byte the behaviour
     * from before this existed.
     *
     * MUST BE AFTER THE memset ABOVE. Calling it before the memset silently
     * wipes every threshold, which is exactly what happened twice: the policy
     * appeared to do nothing, no error was raised, and INVFS_COMMIT_BYTES=16
     * produced byte-identical results to strict. If a similar call is ever
     * needed earlier in this function, do not move it up. */
    blkio_apply_env_policy(io);
    /* Before anything that can fail into blkio_close(): the bounce mutex is
       destroyed there, and destroying an uninitialised mutex is undefined. */
    (void)pthread_mutex_init(&io->bounce_mu, NULL);

#ifdef _WIN32
    {
        int is_dev = blkio_looks_like_device(path);
        DWORD share, disp, attr;
        HANDLE h;

        if (is_dev) {
            if (!dev_path_allowed(path))
                return -BLKIO_E_REFUSED;
            if (flags & BLKIO_CREATE)
                ;  /* mkfs on a device is legitimate; CREATE just means
                      "we are about to overwrite it", not "make a new file" */
            share = FILE_SHARE_READ | FILE_SHARE_WRITE;
            disp  = OPEN_EXISTING;
            attr  = FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
        } else {
            share = FILE_SHARE_READ;
            disp  = (flags & BLKIO_CREATE) ? CREATE_ALWAYS : OPEN_EXISTING;
            attr  = (flags & BLKIO_CREATE) ? FILE_ATTRIBUTE_SPARSE_FILE
                                           : FILE_ATTRIBUTE_NORMAL;
        }
        {
            wchar_t wpath[1024];
            MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024);
            h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, share, NULL,
                            disp, attr, NULL);
        }
        if (h == INVALID_HANDLE_VALUE)
            return -BLKIO_E_OPEN;

        io->h = (void *)h;
        io->is_dev = is_dev;

        if (is_dev) {
            uint64_t len = 0;
            unsigned sec = 0;
            if (dev_geometry(h, &len, &sec) != 0) {
                CloseHandle(h);
                return -BLKIO_E_GEOMETRY;
            }
            if (sec == 0 || BLKIO_ALIGN % sec != 0) {
                CloseHandle(h);
                return -BLKIO_E_SECTOR;
            }
            io->sector = sec;
            io->cap = len - (len % BLKIO_ALIGN);

            if ((flags & BLKIO_EXCLUSIVE) && dev_lock(h) != 0) {
                CloseHandle(h);
                return -BLKIO_E_LOCK;
            }
            io->locked = (flags & BLKIO_EXCLUSIVE) ? 1 : 0;
        } else {
            LARGE_INTEGER sz;
            io->sector = force_dev ? 512 : 1;
            io->cap = GetFileSizeEx(h, &sz) ? (uint64_t)sz.QuadPart : 0;
            if (flags & BLKIO_CREATE) {
                DWORD dummy = 0;
                DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                                &dummy, NULL);
            }
        }
    }
#else
    {
        int is_dev = blkio_looks_like_device(path);
        int fd, oflags = O_RDWR;
        if (!is_dev && (flags & BLKIO_CREATE))
            oflags |= O_CREAT | O_TRUNC;
        fd = open(path, oflags, 0644);
        if (fd < 0)
            return -BLKIO_E_OPEN;
        io->fd = fd;
        io->is_dev = is_dev;
        if (is_dev) {
            uint64_t len = 0;
            unsigned sec = 512;
#if defined(BLKGETSIZE64)
            if (ioctl(fd, BLKGETSIZE64, &len) != 0) { close(fd); return -BLKIO_E_GEOMETRY; }
#else
            { off_t e = lseek(fd, 0, SEEK_END);
              if (e < 0) { close(fd); return -BLKIO_E_GEOMETRY; }
              len = (uint64_t)e; lseek(fd, 0, SEEK_SET); }
#endif
#if defined(BLKSSZGET)
            { int s = 0; if (ioctl(fd, BLKSSZGET, &s) == 0 && s > 0) sec = (unsigned)s; }
#endif
            if (BLKIO_ALIGN % sec != 0) { close(fd); return -BLKIO_E_SECTOR; }
            io->sector = sec;
            io->cap = len - (len % BLKIO_ALIGN);
        } else {
            struct stat st;
            io->sector = force_dev ? 512 : 1;
            io->cap = (fstat(fd, &st) == 0) ? (uint64_t)st.st_size : 0;
        }
    }
#endif

    /* A device always needs the aligned path; an image file needs it only
       when the test hook asks for it. Everything below this point branches
       on `aligned`, not on `is_dev`. */
    io->aligned = io->is_dev || force_dev;
    if (force_dev && !io->is_dev) {
        io->cap -= io->cap % BLKIO_ALIGN;
        fprintf(stderr, "blkio: INVFS_FORCE_DEV -- %s driven through the "
                        "aligned device path\n", path);
    }

    if (io->aligned) {
        /* The buffer address itself must be sector-aligned for unbuffered
           I/O, so over-allocate and align by hand -- aligned_alloc is C11
           and _aligned_malloc is MSVC-only. */
        io->bounce_base = malloc(BLKIO_BOUNCE + BLKIO_ALIGN);
        if (!io->bounce_base) {
            blkio_close(io);
            return -BLKIO_E_NOMEM;
        }
        {
            uintptr_t a = (uintptr_t)io->bounce_base;
            a = (a + (BLKIO_ALIGN - 1)) & ~(uintptr_t)(BLKIO_ALIGN - 1);
            io->bounce = (unsigned char *)a;
        }
    }
    io->pos = 0;
    return 0;
}

/* F7: raw fd for cache-invalidate ioctls. See blkio.h. */
int blkio_raw_fd(const blkio *io)
{
#ifdef _WIN32
    (void)io;
    return -1;
#else
    return io ? io->fd : -1;
#endif
}

void blkio_close(blkio *io)
{
    {
        const char *s = getenv("INVFS_IO_STATS");
        if (s && *s && *s != '0')
            fprintf(stderr, "blkio stats: %llu reads (%llu KB), "
                            "%llu writes (%llu KB)\n",
                    (unsigned long long)io->n_read,
                    (unsigned long long)(io->b_read / 1024),
                    (unsigned long long)io->n_write,
                    (unsigned long long)(io->b_write / 1024));
    }
#ifdef _WIN32
    if (io->h) {
        if (io->locked) {
            DWORD ret = 0;
            /* Explicit unlock. Closing the handle would release it anyway,
               but doing it here means the volume is back before we return. */
            DeviceIoControl((HANDLE)io->h, FSCTL_UNLOCK_VOLUME, NULL, 0,
                            NULL, 0, &ret, NULL);
        }
        CloseHandle((HANDLE)io->h);
        io->h = NULL;
    }
#else
    if (io->fd >= 0) { close(io->fd); io->fd = -1; }
#endif
    free(io->bounce_base);
    io->bounce_base = NULL;
    io->bounce = NULL;
    (void)pthread_mutex_destroy(&io->bounce_mu);
}

/* ------------------------------------------------------------------ *
 * raw, aligned transfers -- the only place that touches the handle
 * ------------------------------------------------------------------ */

static int raw_pread(blkio *io, uint64_t off, void *buf, size_t len)
{
    io->n_read++;
    io->b_read += len;
#ifdef _WIN32
    OVERLAPPED ov;
    DWORD got = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    if (!ReadFile((HANDLE)io->h, buf, (DWORD)len, &got, &ov) || got != len)
        return -1;
    return 0;
#else
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(io->fd, (char *)buf + done, len - done,
                          (off_t)(off + done));
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
#endif
}

static int raw_pwrite(blkio *io, uint64_t off, const void *buf, size_t len)
{
    io->n_write++;
    io->b_write += len;
#ifdef _WIN32
    OVERLAPPED ov;
    DWORD put = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset     = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    if (!WriteFile((HANDLE)io->h, buf, (DWORD)len, &put, &ov) || put != len)
        return -1;
    return 0;
#else
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(io->fd, (const char *)buf + done, len - done,
                           (off_t)(off + done));
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
#endif
}

/* ------------------------------------------------------------------ *
 * byte-granular transfers over an aligned device
 * ------------------------------------------------------------------ */

/* Read [off, off+len) from a device whose transfers must be aligned.
   Strategy: widen the request outward to alignment boundaries, read whole
   chunks into the bounce buffer, copy out the part the caller asked for. */
static int dev_pread(blkio *io, uint64_t off, void *buf, size_t len)
{
    unsigned char *out = (unsigned char *)buf;

    /* One bounce buffer per volume, many threads per volume: hold it for the
       whole transfer so no thread can read out of the region another is
       filling. The decompression that follows is still parallel; only the
       device hand-off is serialised, and on a device each of those is a
       synchronous round-trip anyway. */
    (void)pthread_mutex_lock(&io->bounce_mu);

    while (len) {
        uint64_t base = off & ~(uint64_t)(BLKIO_ALIGN - 1);
        size_t   skip = (size_t)(off - base);
        size_t   want = len;
        size_t   span;

        if (want > BLKIO_BOUNCE - skip)
            want = BLKIO_BOUNCE - skip;
        span = (skip + want + BLKIO_ALIGN - 1) & ~(size_t)(BLKIO_ALIGN - 1);

        /* Never read past the end of the device: the last aligned chunk may
           extend beyond it, and an unbuffered read that crosses the end
           fails outright rather than returning short. */
        if (base + span > io->cap) {
            if (base >= io->cap)
                goto fail;
            span = (size_t)(io->cap - base);
            if (span < skip + want)
                goto fail;
        }
        if (raw_pread(io, base, io->bounce, span) != 0)
            goto fail;
        memcpy(out, io->bounce + skip, want);
        out += want;
        off += want;
        len -= want;
    }
    (void)pthread_mutex_unlock(&io->bounce_mu);
    return 0;
fail:
    (void)pthread_mutex_unlock(&io->bounce_mu);
    return -1;
}

/* Write [off, off+len) to a device. Whole aligned chunks go straight out;
   a partial head or tail chunk is read first, patched, and written back.
   Read-modify-write is what lets the rest of InvariantFS keep writing 4-byte
   CRCs and 336-byte inode records to a device that only accepts sectors. */
static int dev_pwrite(blkio *io, uint64_t off, const void *buf, size_t len)
{
    const unsigned char *in = (const unsigned char *)buf;

    /* The read-modify-write below is only atomic with respect to other users
       of the bounce buffer if the whole read-patch-write cycle is held. Two
       threads interleaving between the read-back and the patch would write one
       thread's sector over the other's, and both would report success. */
    (void)pthread_mutex_lock(&io->bounce_mu);

    while (len) {
        uint64_t base = off & ~(uint64_t)(BLKIO_ALIGN - 1);
        size_t   skip = (size_t)(off - base);
        size_t   want = len;
        size_t   span;
        size_t   tail;

        if (want > BLKIO_BOUNCE - skip)
            want = BLKIO_BOUNCE - skip;
        span = (skip + want + BLKIO_ALIGN - 1) & ~(size_t)(BLKIO_ALIGN - 1);

        if (base + span > io->cap)
            goto fail;

        /* Only the two end blocks can be partially overwritten, so only they
           have to be read back first. This used to read the whole span
           whenever the transfer was not exactly aligned, which it almost
           never is: a compressed 64 KB segment ends at an arbitrary byte, so
           writing it re-read all 64 KB. Over a 6.2 GB copy that was 6.9 GB of
           reads the device did not need to do -- it roughly doubled the cost
           of every segment. Now a segment write reads at most 4 KB. */
        tail = (skip + want) & (BLKIO_ALIGN - 1);   /* 0 == ends aligned */

        if (skip && tail && span == 2 * BLKIO_ALIGN) {
            /* Head and tail are the only two blocks and they are adjacent:
               one read of both beats two reads of one. */
            if (raw_pread(io, base, io->bounce, span) != 0)
                goto fail;
        } else {
            if (skip) {
                if (raw_pread(io, base, io->bounce, BLKIO_ALIGN) != 0)
                    goto fail;
            }
            if (tail) {
                /* Last block of the span. If it is the same block we just
                   read for the head, that read already covered it. */
                size_t last = span - BLKIO_ALIGN;
                if (!(skip && last == 0)) {
                    if (raw_pread(io, base + last, io->bounce + last,
                                  BLKIO_ALIGN) != 0)
                        goto fail;
                }
            }
        }

        /* The caller's buffer is not sector-aligned, so the payload goes
           through the bounce even when no read was needed. */
        memcpy(io->bounce + skip, in, want);
        if (raw_pwrite(io, base, io->bounce, span) != 0)
            goto fail;

        in  += want;
        off += want;
        len -= want;
    }
    (void)pthread_mutex_unlock(&io->bounce_mu);
    return 0;
fail:
    (void)pthread_mutex_unlock(&io->bounce_mu);
    return -1;
}

/* ------------------------------------------------------------------ *
 * public API
 * ------------------------------------------------------------------ */

int blkio_pread(blkio *io, uint64_t off, void *buf, size_t len)
{
    if (len == 0) return 0;
    if (io->aligned)
        return dev_pread(io, off, buf, len);
    return raw_pread(io, off, buf, len);
}

/* Group-commit watermarks. The write side counts writes (blkio_pwrite); the
 * flush side turns "everything written so far" into durability (blkio_flush).
 * SEQ_CST throughout: the counters are the entire synchronisation argument
 * between a writer and a flusher, so they must not be reordered against the
 * pwrite syscall or against each other. */
static uint64_t gc_load(const uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

static void gc_store(uint64_t *p, uint64_t v)
{
    __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

static void gc_bump(uint64_t *p)
{
    __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}

int blkio_pwrite(blkio *io, uint64_t off, const void *buf, size_t len)
{
    /* Accumulate for the deferred-commit policy HERE, not in blkio_write.
     *
     * Measured: with the accumulator in blkio_write, `blkio writes` was 0 for
     * a real 34,209-file import, because vmux_pwrite calls blkio_pwrite
     * directly and never goes through blkio_write. dirty_bytes therefore stayed
     * 0, the 16 MB threshold could never be satisfied, and the whole 26.4x
     * improvement came from the close-time flush alone -- 1 fsync instead of the
     * ~170 that 2.7 GB / 16 MB predicts.
     *
     * Every byte that reaches the descriptor passes through here, so this is
     * the only place the accumulator sees the real traffic. */
    io->dirty_bytes   += len;
    io->last_write_ms  = pf_now_ms();
    int rc;

    if (len == 0) return 0;
    if (io->aligned)
        rc = dev_pwrite(io, off, buf, len);
    else
        rc = raw_pwrite(io, off, buf, len);
    /* Count the write for group commit (see blkio_flush). The bump is
     * AFTER the transfer returns, so a flusher that samples this counter
     * has necessarily issued its fsync after these bytes reached the page
     * cache. It is unconditional -- raw_pwrite loops on partial writes, so
     * a non-zero return can still have left bytes behind, and those must
     * not escape the next barrier. Failing to count a write is the one
     * direction that could lose a record; counting one that did not land
     * only costs an extra fsync. */
    gc_bump(&io->write_gen);
    return rc;
}

int blkio_seek(blkio *io, uint64_t off)
{
    io->pos = off;
    return 0;
}

int blkio_read(blkio *io, void *buf, size_t len)
{
    INVFS_PERF_ADD(PERF_BLKIO_READS, 1);
    INVFS_PERF_ADD(PERF_BLKIO_READ_BYTES, len);
    if (blkio_pread(io, io->pos, buf, len) != 0)
        return -1;
    io->pos += len;
    return 0;
}

int blkio_write(blkio *io, const void *buf, size_t len)
{
    /* The single most important counter in the build: everything the engine
     * asks for passes here on its way to the device, so write amplification
     * is (these bytes) / (vol_write bytes), and nothing below this line can
     * hide it. */
    INVFS_PERF_ADD(PERF_BLKIO_WRITES, 1);
    INVFS_PERF_ADD(PERF_BLKIO_WRITE_BYTES, len);
    if (blkio_pwrite(io, io->pos, buf, len) != 0)
        return -1;
    io->pos += len;
    return 0;
}

int blkio_chsize(blkio *io, uint64_t size)
{
    if (io->is_dev)
        return size <= io->cap ? 0 : -BLKIO_E_TOOSMALL;
#ifdef _WIN32
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)size;
        if (!SetFilePointerEx((HANDLE)io->h, li, NULL, FILE_BEGIN) ||
            !SetEndOfFile((HANDLE)io->h))
            return -1;
    }
#else
    if (ftruncate(io->fd, (off_t)size) != 0)
        return -1;
#endif
    io->cap = size;
    /* A forced-alignment image keeps the device's rule that the last usable
       byte sits on an alignment boundary, so the tail chunk of a transfer
       never runs off the end of the store. */
    if (io->aligned)
        io->cap -= io->cap % BLKIO_ALIGN;
    /* Count the resize for group commit. ftruncate changes the inode, and
     * it is fsync -- not the write path -- that makes the new length
     * survive a crash. Before group commit every barrier flushed, so a
     * resize followed by a barrier was persisted as a matter of course; a
     * watermark that ignored chsize would let that barrier be skipped and
     * a resized image could come back shorter than it was left. The bump is
     * placed after the change lands, so a flush that covers it was issued
     * after ftruncate returned. */
    gc_bump(&io->write_gen);
    return 0;
}

uint64_t blkio_capacity(blkio *io) { return io->cap; }
int      blkio_is_device(const blkio *io) { return io->is_dev; }

/* ---- group commit ----------------------------------------------------
 *
 * The contract blkio_flush has always had is per CALL, not per byte: when
 * it returns 0, every write this handle had issued before the call is on
 * stable storage. ADR-009 decision 3 made the delta log barrier on every
 * record, which is the strongest possible reading of that contract and
 * costs one physical flush per metadata record. Measured on a 500-file /
 * 12-byte import: 3509 fsyncs, 7.02 per file, 387.7 s of a 390.0 s wall --
 * 99.4% of the time blocked in fsync, ~0.05 s of CPU. Scaled to an
 * 84,279-file rootfs that is 65,377 s (18.2 h) of metadata flush alone.
 *
 * Nothing about the guarantee requires a flush PER CALL. It requires that
 * the flush which happened be one that COVERS this call's writes. So:
 * collapse the flushes, never the guarantee.
 *
 * Two counters per handle do it:
 *
 *   write_gen    incremented once per issued write (blkio_pwrite)
 *   flushed_gen  the highest write_gen a COMPLETED flush is known to cover
 *
 * and blkio_flush becomes: "make sure flushed_gen >= write_gen".
 *
 *   INVARIANT. If blkio_flush() returns 0, then every write issued on this
 *   handle before the call satisfies write_gen <= flushed_gen at return,
 *   and was completed (its pwrite had returned) strictly before the fsync
 *   that covers it was issued. Hence its bytes were in the page cache when
 *   that fsync ran, hence they are on stable storage when blkio_flush
 *   returns.
 *
 * Why the watermark is sound. fsync(fd) makes durable every byte written to
 * fd before the fsync CALL. So a flush that samples `cover = write_gen`
 * immediately before calling fsync covers exactly the writes with
 * gen <= cover: such a write incremented write_gen, so its pwrite had
 * already returned, and we only read that counter afterwards, so our fsync
 * call came later. flushed_gen is set to `cover` (the sample taken BEFORE
 * the fsync), never to the post-fsync value -- a write that lands while the
 * fsync is in flight has not been covered, and must not be claimed.
 *
 * Why a caller can never be skipped. my_gen is read at entry. If some
 * thread's completed flush already set flushed_gen >= my_gen, that flush
 * was issued after every write the caller had issued, so the caller is
 * covered and the fsync is pure waste. Otherwise exactly one thread -- the
 * first to find no flusher running -- becomes the flusher, and the rest
 * wait on the condvar for the flush that covers them. A flusher samples
 * `cover` for LATER than their gen (it re-reads write_gen after they
 * entered), so the wake-up can never be premature.
 *
 * Failures: a failed fsync does not advance flushed_gen, so no caller is
 * told it is durable, and the next barrier retries the flush. This is
 * strictly stronger than the old code, which returned the error but left
 * the next call to guess.
 *
 * Writers are counted in blkio_pwrite, NOT in the delta log, and that is
 * the load-bearing detail. The counter has to mean "everything written to
 * this handle", because barriers are also ORDERING barriers: the fold
 * writes COW base pages and then barriers so that RT30 may name them, and
 * the anchor refresh barriers so a tail descriptor follows its block-0
 * original. Counting only delta appends would let a later barrier be
 * skipped on the strength of an earlier flush that predates those pages --
 * publishing a root over non-durable pages. blkio_pwrite is the single
 * choke point every write to a backing store goes through, so counting
 * there makes the watermark mean what the ordering rules need it to mean.
 *
 * The counters are reset by blkio_open's memset, so a handle with no
 * writes has flushed_gen == write_gen == 0 and its first barrier is a
 * no-op -- which is the truth: there is nothing to make durable. That is
 * also why this cannot skip a needed flush on a recycled stack blkio. */
static pthread_mutex_t g_gc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gc_cv   = PTHREAD_COND_INITIALIZER;

/* The physical flush, with no group-commit logic. */
static int blkio_flush_raw(blkio *io)
{
    /* PERF: THE counter that matters, and the one missing while I was reporting
     * import as "not I/O bound".
     *
     * I instrumented vol_flush and blkio_write, saw 2 flushes and 0 blkio
     * writes for a 2,000-file import, and concluded ~90% CPU. Wrong:
     * vol_flush is the VOLUME-level flush. The fsync(2) calls happen here, one
     * layer down, uncounted.
     *
     * The evidence already in this file that I did not read (blkio.c:703):
     *
     *   "12-byte import: 3509 fsyncs, 7.02 per file, 387.7 s of a 390.0 s wall
     *    -- 99.4% of the time blocked in fsync, ~0.05 s of CPU."
     *
     * ~7 fsyncs per file, 99.4% of wall time inside them. A tmpfs control
     * confirmed it from outside: same 3.0 writes/file, same bytes, but
     * 5,000 files/s with no block device under the volume against 10.3 on an
     * SSD -- 500x, purely the presence of the sync path.
     */
    INVFS_PERF_ADD(PERF_FSYNC_CALLS, 1);
#ifdef _WIN32
    if (FlushFileBuffers((HANDLE)io->h)) return 0;
    INVFS_PERF_ADD(PERF_FSYNC_ERR, 1);
    return -1;
#else
    if (fsync(io->fd) == 0) return 0;
    INVFS_PERF_ADD(PERF_FSYNC_ERR, 1);
    return -1;
#endif
}

/* ---- deferred commit policy ------------------------------------------
 * docs/benchmarks/commit-policy.md. The decision is made HERE rather than at
 * the volume level because vol_flush is called only twice in a 34,209-file
 * import while every fsync happens inside this file -- counting the volume
 * flush says nothing about the cost. */
#define INVFS_MIN_COMMIT_BYTES 4096u

static uint64_t pf_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
    return (uint64_t)time(NULL) * 1000u;
}

/* Bare number = MiB. Suffixed K/M/G accepted. Returns 0 for absent, malformed
 * or sub-block values -- the sub-block case prints why and returns 0 so the
 * caller falls back to STRICT rather than to a silent clamp. */
static uint64_t pf_parse_bytes(const char *v)
{
    char *end = NULL;
    unsigned long long n;

    if (!v || !*v) return 0;
    n = strtoull(v, &end, 10);
    if (end == v) return 0;
    while (*end == ' ') end++;
    if (*end == 'K' || *end == 'k')      n *= 1024ULL;
    else if (*end == 'M' || *end == 'm') n *= 1024ULL * 1024ULL;
    else if (*end == 'G' || *end == 'g') n *= 1024ULL * 1024ULL * 1024ULL;
    else                                   n *= 1024ULL * 1024ULL;  /* bare = MiB */

    if (n && n < INVFS_MIN_COMMIT_BYTES) {
        fprintf(stderr,
                "[blkio] commit threshold %llu B is below the %u B block size;\n"
                "        refusing rather than clamping, because a sub-block\n"
                "        bound silently behaves like --sync. Use\n"
                "        INVFS_COMMIT_STRICT=1 if that is what you meant.\n",
                n, INVFS_MIN_COMMIT_BYTES);
        return 0;
    }
    return (uint64_t)n;
}

static int pf_env_flag(const char *name)
{
    const char *v = getenv(name);
    return (v && *v && strcmp(v, "0") != 0);
}

void blkio_set_commit(blkio *io, uint64_t bytes, uint64_t ms, uint64_t idle_ms)
{
    if (!io) return;
    io->commit_bytes = bytes;
    io->commit_ms    = ms;
    io->idle_ms      = idle_ms;
    io->dirty_bytes  = 0;
    io->last_write_ms = pf_now_ms();
}

void blkio_force_flush(blkio *io)
{
    if (io) io->force_flush = 1;
}

/* Environment so that every tool inherits the policy without a new argument on
 * every CLI:
 *   INVFS_COMMIT_STRICT=1    fsync every flush (the default)
 *   INVFS_COMMIT_NONE=1      never sync before close
 *   INVFS_COMMIT_BYTES=16    bare number = MiB; 4K / 512K / 1M / 1G accepted
 *   INVFS_COMMIT_MS=1000     wall-clock bound
 *   INVFS_COMMIT_IDLE_MS=500 idle bound
 */
void blkio_apply_env_policy(blkio *io)
{
    const char *v;
    uint64_t b = 0, t = 0, d = 0;

    if (!io) return;

    if (pf_env_flag("INVFS_COMMIT_STRICT")) { blkio_set_commit(io, 0, 0, 0); return; }

    if (pf_env_flag("INVFS_COMMIT_NONE")) {
        /* No reachable threshold: (uint64_t)-1 exceeds any dirty-byte count or
         * elapsed-ms comparison a real run can produce, so nothing fires and
         * only the flush vol_close forces happens. */
        blkio_set_commit(io, (uint64_t)-1, (uint64_t)-1, (uint64_t)-1);
        return;
    }

    b = pf_parse_bytes(getenv("INVFS_COMMIT_BYTES"));
    if ((v = getenv("INVFS_COMMIT_MS"))      && *v) t = (uint64_t)strtoull(v, NULL, 10);
    if ((v = getenv("INVFS_COMMIT_IDLE_MS")) && *v) d = (uint64_t)strtoull(v, NULL, 10);

    if (b || t || d) blkio_set_commit(io, b, t, d);
}

/* 1 = a real fsync must happen now. */
static int pf_should_flush(blkio *io)
{
    uint64_t now, dirty, last;

    if (io->force_flush) return 1;
    if (io->commit_bytes == 0 && io->commit_ms == 0 && io->idle_ms == 0)
        return 1;                             /* strict: unchanged behaviour */

    now   = pf_now_ms();
    dirty = __atomic_load_n(&io->dirty_bytes, __ATOMIC_RELAXED);
    last  = io->last_write_ms;

    if (io->commit_bytes && dirty >= io->commit_bytes) return 1;
    if (io->commit_ms    && now - last >= io->commit_ms) return 1;
    if (io->idle_ms      && now - last >= io->idle_ms) return 1;
    return 0;
}

int blkio_flush(blkio *io)
{
    uint64_t my_gen = gc_load(&io->write_gen);
    int rc, fd_ok;

    /* A handle whose descriptor is not there cannot be fsynced, and the
     * failure has to reach the caller: vol_close and vol_sync keep the
     * volume DIRTY instead of writing CLEAN precisely because blkio_flush
     * reports the error (blkio_test.c pins this). Such a handle must never
     * take the skip below -- there is no completed flush standing behind
     * it that anyone could rely on, and "return 0" would turn a broken
     * descriptor into a silent success. So the skip requires a descriptor
     * that could have been flushed in the first place. */
#ifdef _WIN32
    fd_ok = ((HANDLE)io->h != NULL && (HANDLE)io->h != INVALID_HANDLE_VALUE);
#else
    fd_ok = (io->fd >= 0);
#endif

    /* Deferral gate. Deliberately AFTER fd_ok: a handle with no descriptor must
     * never take this skip, because "return 0" would turn a broken descriptor
     * into a silent success, which is the reason fd_ok exists at all.
     *
     * A deferred flush does NOT advance flushed_gen. That is the safety
     * property: flushed_gen means "a COMPLETED physical flush covers this", so
     * leaving it alone keeps the volume honestly owing durability and the next
     * trigger -- or vol_close -- pays it. */
    if (!pf_should_flush(io)) {
        io->last_write_ms = pf_now_ms();
        INVFS_PERF_ADD(PERF_FSYNC_DEFERRED, 1);
        return 0;
    }

    pthread_mutex_lock(&g_gc_lock);
    for (;;) {
        if (fd_ok && gc_load(&io->flushed_gen) >= my_gen) {
            pthread_mutex_unlock(&g_gc_lock);
            return 0;                 /* an earlier flush already covers us */
        }
        if (!gc_load(&io->flush_busy)) {
            gc_store(&io->flush_busy, 1);
            break;                    /* we are the flusher */
        }
        pthread_cond_wait(&g_gc_cv, &g_gc_lock);
    }
    pthread_mutex_unlock(&g_gc_lock);

    /* Sample BEFORE the flush: this is the ceiling on what we may claim. */
    {
        uint64_t cover = gc_load(&io->write_gen);
        rc = blkio_flush_raw(io);
        if (rc == 0 && cover > gc_load(&io->flushed_gen))
            gc_store(&io->flushed_gen, cover);
    }

    /* The window is satisfied: reset the accumulators and clear any forced
     * flush so the next window is measured from here. */
    io->dirty_bytes   = 0;
    io->last_write_ms = pf_now_ms();
    io->force_flush   = 0;

    pthread_mutex_lock(&g_gc_lock);
    gc_store(&io->flush_busy, 0);
    pthread_cond_broadcast(&g_gc_cv);
    pthread_mutex_unlock(&g_gc_lock);
    return rc;
}

