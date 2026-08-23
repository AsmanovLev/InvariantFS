/*
 * blkio.c -- backing-store abstraction: image file or raw block device.
 * See blkio.h for why this exists and why devices are unbuffered.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
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
    return strncmp(path, "/dev/", 5) == 0;
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
                return -1;
            span = (size_t)(io->cap - base);
            if (span < skip + want)
                return -1;
        }
        if (raw_pread(io, base, io->bounce, span) != 0)
            return -1;
        memcpy(out, io->bounce + skip, want);
        out += want;
        off += want;
        len -= want;
    }
    return 0;
}

/* Write [off, off+len) to a device. Whole aligned chunks go straight out;
   a partial head or tail chunk is read first, patched, and written back.
   Read-modify-write is what lets the rest of InvariantFS keep writing 4-byte
   CRCs and 336-byte inode records to a device that only accepts sectors. */
static int dev_pwrite(blkio *io, uint64_t off, const void *buf, size_t len)
{
    const unsigned char *in = (const unsigned char *)buf;

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
            return -1;

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
                return -1;
        } else {
            if (skip) {
                if (raw_pread(io, base, io->bounce, BLKIO_ALIGN) != 0)
                    return -1;
            }
            if (tail) {
                /* Last block of the span. If it is the same block we just
                   read for the head, that read already covered it. */
                size_t last = span - BLKIO_ALIGN;
                if (!(skip && last == 0)) {
                    if (raw_pread(io, base + last, io->bounce + last,
                                  BLKIO_ALIGN) != 0)
                        return -1;
                }
            }
        }

        /* The caller's buffer is not sector-aligned, so the payload goes
           through the bounce even when no read was needed. */
        memcpy(io->bounce + skip, in, want);
        if (raw_pwrite(io, base, io->bounce, span) != 0)
            return -1;

        in  += want;
        off += want;
        len -= want;
    }
    return 0;
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

int blkio_pwrite(blkio *io, uint64_t off, const void *buf, size_t len)
{
    if (len == 0) return 0;
    if (io->aligned)
        return dev_pwrite(io, off, buf, len);
    return raw_pwrite(io, off, buf, len);
}

int blkio_seek(blkio *io, uint64_t off)
{
    io->pos = off;
    return 0;
}

int blkio_read(blkio *io, void *buf, size_t len)
{
    if (blkio_pread(io, io->pos, buf, len) != 0)
        return -1;
    io->pos += len;
    return 0;
}

int blkio_write(blkio *io, const void *buf, size_t len)
{
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
    return 0;
}

uint64_t blkio_capacity(blkio *io) { return io->cap; }
int      blkio_is_device(const blkio *io) { return io->is_dev; }

int blkio_flush(blkio *io)
{
    /* Devices are already FILE_FLAG_WRITE_THROUGH, so this is a no-op for
       them on Windows; it still matters for image files. */
#ifdef _WIN32
    return FlushFileBuffers((HANDLE)io->h) ? 0 : -1;
#else
    return fsync(io->fd) == 0 ? 0 : -1;
#endif
}
