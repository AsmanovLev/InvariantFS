/*
 * blkio.h -- backing-store abstraction: image file or raw block device.
 *
 * Until now mkfs.c and volume.c each carried their own private copy of a
 * four-function platform I/O shim (open/seek/read/write). Two copies of the
 * same layer is survivable while it only wraps ReadFile; it is not survivable
 * once the layer has to do sector alignment and read-modify-write, because the
 * two copies would drift and the drift would corrupt volumes. This is the one
 * copy.
 *
 * The interesting part is the device path. InvariantFS addresses its backing
 * store in bytes, not sectors: an inode record is ~336 bytes and the record
 * CRC that follows it is 4. A raw device rejects that -- direct access to a
 * volume requires every transfer to start on a sector boundary and to be a
 * whole number of sectors. So device I/O goes through an aligned bounce
 * buffer, with read-modify-write for partial sectors, and callers keep their
 * byte-granular view.
 *
 * On Windows, a device is opened FILE_FLAG_NO_BUFFERING |
 * FILE_FLAG_WRITE_THROUGH. That is deliberate and it is why the alignment
 * work is unavoidable there: a buffered volume handle would let the Windows
 * cache manager reorder writes, and InvariantFS has a journal whose entire
 * purpose is that the journal record reaches the platter before the data it
 * describes. Buffering would make crash recovery a fiction.
 *
 * POSIX has no such open. blkio_open uses a plain O_RDWR for an image file
 * AND for a raw device (blkio.c), with no O_DIRECT, no O_SYNC and no
 * O_DSYNC, so a write is a page-cache write and the one and only durability
 * point is the fsync() in blkio_flush. Do not read the guarantee above as
 * platform-independent; it is not. Write ordering is established by
 * InvariantFS (write in the required order, then make the whole set durable
 * in one barrier) and power-loss ordering is delegated to the host
 * filesystem and device honouring fsync(2). blkio_test.c asserts the open
 * flags so this paragraph cannot quietly stop matching the code.
 *
 * Image files keep the old direct path -- no alignment, no bounce, no
 * behaviour change. Setting INVFS_FORCE_DEV=1 in the environment overrides
 * that and drives an image file through the aligned path as well; it exists
 * so the device code can be exercised in tests, on an ordinary file, without
 * elevation or a spare flash drive.
 */
#ifndef INVFS_BLKIO_H
#define INVFS_BLKIO_H

#include <stdint.h>
#include <stddef.h>

/* Alignment granularity for device I/O. Equal to INVFS_BLOCK_SIZE, and a
   multiple of every logical sector size we accept (512 or 4096), so one
   constant satisfies both the device and the filesystem. */
#define BLKIO_ALIGN   4096u

/* Bounce buffer size. Large transfers are chunked through it. */
#define BLKIO_BOUNCE  (1024u * 1024u)

/* blkio_open flags */
#define BLKIO_CREATE     0x01  /* file: create/truncate. Devices: refused. */
#define BLKIO_EXCLUSIVE  0x02  /* device: FSCTL_LOCK_VOLUME + DISMOUNT first */

/* blkio_open / blkio_probe error codes (negated on return) */
#define BLKIO_E_OPEN       1  /* CreateFile/open failed (often: not elevated) */
#define BLKIO_E_REFUSED    2  /* path rejected by the safety guard */
#define BLKIO_E_GEOMETRY   3  /* could not learn size or sector size */
#define BLKIO_E_SECTOR     4  /* sector size does not divide BLKIO_ALIGN */
#define BLKIO_E_LOCK       5  /* lock/dismount refused: volume still in use */
#define BLKIO_E_NOMEM      6
#define BLKIO_E_TOOSMALL   7  /* device smaller than the requested volume */

typedef struct blkio {
#ifdef _WIN32
    void    *h;           /* HANDLE */
#else
    int      fd;
#endif
    uint64_t pos;         /* cursor, in bytes */
    uint64_t cap;         /* usable capacity, rounded down to BLKIO_ALIGN */
    unsigned sector;      /* logical sector size; 1 for an image file */
    int      is_dev;
    int      aligned;     /* transfers must go through the bounce buffer.
                             Always set for a device; also set for an image
                             file when INVFS_FORCE_DEV is in the environment,
                             so the alignment path can be tested without
                             elevation or real hardware. */
    int      locked;      /* volume was locked and dismounted by us */
    unsigned char *bounce;      /* aligned scratch; NULL for image files */
    void          *bounce_base; /* what to free (bounce may be offset) */

    /* Transfer counters. Every read and write the layer actually issues to
       the handle is counted here -- not the byte-granular calls above it, the
       real ones. On a device each of these costs a synchronous round-trip
       (tens of milliseconds on flash), so the count, not the byte total, is
       what predicts how long a copy takes. Set INVFS_IO_STATS=1 to have
       blkio_close print them. */
    uint64_t n_read, n_write;   /* transfers issued */
    uint64_t b_read, b_write;   /* bytes moved, including alignment padding */

    /* ---- group commit (see blkio.c) --------------------------------
     * write_gen counts writes issued on this handle; flushed_gen is the
     * highest write_gen a COMPLETED physical flush is known to cover. Both
     * are reset to 0 by blkio_open, so a freshly opened handle starts
     * consistent. Plain uint64_t on purpose: they are only ever touched
     * through __atomic_* in blkio.c, which keeps <stdatomic.h> out of every
     * includer. The mutex and condvar that serialise the flushers are
     * file-static in blkio.c, shared by every handle in the process. */
    uint64_t write_gen;
    uint64_t flushed_gen;
    /* 1 while some thread is inside the physical flush. Guarded by the
     * file-static mutex in blkio.c; a uint64_t so it can be read with the
     * same __atomic_* helpers as the generations. */
    uint64_t flush_busy;
} blkio;

/* Turn a user-supplied backing-store name into a form CreateFileW accepts:
 *   "W:"        -> "\\.\W:"      (drive-letter shorthand for the volume)
 *   "\\.\W:"    -> unchanged
 *   "/dev/sdb1" -> unchanged
 *   anything else (an ordinary path) -> unchanged
 * Returns `out`. Does not touch the filesystem. */
const char *blkio_normalize(const char *path, char *out, size_t outsz);

/* Is this name a raw device rather than an image file? Pure string test, so
   callers can branch on it before opening anything. */
int blkio_looks_like_device(const char *path);

/* Human-readable text for a negative blkio_open return. */
const char *blkio_strerror(int rc);

/* 0 on success, -BLKIO_E_* on failure. */
int  blkio_open(blkio *io, const char *path, int flags);
void blkio_close(blkio *io);

int  blkio_seek(blkio *io, uint64_t off);
int  blkio_read(blkio *io, void *buf, size_t len);
int  blkio_write(blkio *io, const void *buf, size_t len);

/* Position-explicit forms. seek/read/write are thin wrappers over these. */
int  blkio_pread(blkio *io, uint64_t off, void *buf, size_t len);
int  blkio_pwrite(blkio *io, uint64_t off, const void *buf, size_t len);

/* Image file: set the file length (sparse). Device: verify `size` fits,
   changing nothing -- a device's size is a fact, not a setting. */
int  blkio_chsize(blkio *io, uint64_t size);

/* Usable bytes. For a device this is the partition length as the OS reports
   it, rounded down to BLKIO_ALIGN; for a file, the current file size. */
uint64_t blkio_capacity(blkio *io);

int  blkio_is_device(const blkio *io);
int  blkio_flush(blkio *io);

#endif /* INVFS_BLKIO_H */
