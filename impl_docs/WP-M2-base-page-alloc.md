# WP-M2 — v3 base page format + metadata block allocator

**Branch:** `wp/M2-base-page-alloc`
**Worktree:** `/tmp/invfs-wp-M2`
**Severity:** HIGH (format-breaking; first engine WP after the skeleton)
**Source:** `impl_docs/design-meta-v3.md` §12 (on-disk format v3), §15.1 (v3 skeleton), §16 (test strategy)
**Estimated effort:** large (new subsystem + unit harness)

---

## Scope

Implement the **fixed-size base page format** and a **metadata block
allocator** on top of the v3 root-area descriptor frozen by WP-M1.

Files:
- `src/core/vol_metabuf.h` — new: page/`blkptr` structs, read/write API,
  allocator API.
- `src/core/vol_metabuf.c` — new: page serialize/parse (+CRC), page IO,
  free-block allocation/free, bootstrap allocator.
- `Makefile` — add `vol_metabuf.o` to `CORE` (line 30-34) and a new
  `infv-metabuf_test` binary to the `test` target (lines 133-138).
- `tools/metabuf_test.c` (or `src/cli/metabuf_test.c`, matching
  `src/cli/blkio_test.c`) — new unit harness.

No B+-tree logic (WP-M3), no delta, no inode/dirent semantics.

---

## Why

Every later v3 structure is built out of base pages addressed by
`blkptr`s. Until the page wire format and its durability rules are frozen
and unit-tested, WP-M3 (tree core), WP-M4 (fsck) and WP-M5 (inode tree)
cannot proceed. WP-M2 also owns the **only** place metadata blocks are
handed out under v3, so reclaim (later WP) can reason about one allocator.

---

## Design

### Frozen structures (add to `vol_metabuf.h`; WP-M1 declared the wire
structs in `invarifs.h` — this WP implements their IO and asserts sizes)

```c
typedef struct {            /* 16 bytes */
    uint64_t pba;           /* physical block address */
    uint32_t checksum;      /* CRC32C of the page the pointer names */
    uint16_t gen;           /* generation, bumped on every COW page write */
    uint16_t flags;         /* leaf/internal/root/pinned */
} invfs_blkptr;

typedef struct {            /* page header, packed */
    uint8_t  magic[4];      /* "MPG1" */
    uint16_t gen;
    uint8_t  level;         /* 0 = leaf */
    uint8_t  flags;
    uint16_t nentries;
    uint16_t free_lo;       /* byte offset of packed-entry area start */
    uint32_t crc32c;        /* over page bytes with this field 0 */
} invfs_page_hdr;
```

Design §12 fixes `{magic, gen, level, nentries, checksum}`; this WP adds
`flags` + `free_lo` for packed entries. Entries are packed from the end of
the page backward; `free_lo` is the low-water mark. The exact entry
encoding is **WP-M3's** decision; WP-M2 only provides a byte-array page
with a settable `nentries`/`free_lo` and validates the header.

### API

```c
int  mbuf_read (invfs_volume *v, uint64_t pba, uint8_t *page_out);
int  mbuf_write(invfs_volume *v, uint64_t pba, uint8_t *page);   /* sets crc */
int  mbuf_read_ptr (invfs_volume *v, const invfs_blkptr *p, uint8_t *page_out);
int  mbuf_verify_ptr(invfs_volume *v, const invfs_blkptr *p);    /* crc+gen */

uint64_t mbuf_alloc(invfs_volume *v, uint16_t gen);              /* 0 = ENOSPC */
void     mbuf_free (invfs_volume *v, uint64_t pba);
```

- Integration point: reuse `alloc_blocks`/`vol_free_blocks`
  (`volume.c:2777`, `volume.h:119`) but restrict to the metadata zone.
  §12 puts base/metadata on dev0, mirrored via the existing `DEVT`
  descriptor; the two-device write path is `io_write`/`vmux_barrier`
  (`volume_internal.h:148`, `:132`).
- **Bootstrap allocator:** `vol_open` cannot rely on the full allocator
  before the base root is read. WP-M2 adds a small bootstrap cursor over
  the root area allocated by WP-M1 (RT30 `page_size`, root slots), used
  for the first allocations at mkfs/open. The design doc does not specify
  the bootstrap policy beyond "bootstrap allocator" (§15.1); this WP keeps
  it a simple monotone cursor over the metadata zone, documented in a
  comment, and does **not** invent a free-list format.

### Crash / durability ordering

- A page write is: `mbuf_write` (CRC computed) → `io_write` → caller
  decides whether to barrier. `vmux_barrier` (`volume_internal.h:132`) is
  the durability point.
- **A page is never published by a `blkptr` until its bytes are durable.**
  Ordering for a COW publish: write new page + barrier, then write the
  parent/root that references it. This is the same "structure before
  reference" rule WP-M1 applies to RT30 (`WP-M1-TASK.md` Design).
- Spec says "block pointers `{pba, checksum, gen, flags}`" (§12); it does
  **not** say a torn page is repaired in place. WP-M2 treats a bad CRC as
  a hard read error, falling back only through the caller's generation
  logic (WP-M3 root double-slot / WP-M1 RT30 seq).

---

## Validation

1. `make test` — existing 4 binaries plus the new `infv-metabuf_test`
   (`make test`, `Makefile:133-138`). Cases: CRC round-trip; torn-page
   detection (flip one byte); `blkptr` gen mismatch rejected; alloc/free
   reuse; allocator exhaustion returns ENOSPC, not abort; MT/alignment of
   the packed structs.
2. `INVFS_V3=1 invf-mkfs t.img 1` then a unit round-trip through
   `mbuf_alloc`/`mbuf_write`/`mbuf_read_ptr` leaves the image fsck-clean
   under WP-M1's `invf-fsck` accept.
3. `bash tools/run-e2e.sh tools/test-meta-v3.sh` — WP-M1's leg 0 must
   still pass (no v3 regression).

---

## Out of scope (do NOT touch)

- B+-tree search/split/merge/scan (WP-M3).
- Delta log, overlay, fold, save point, reclaim.
- v2 record/mapper/`vol_records.c` paths, `vol_inode_compact`.
- Inode or dirent row semantics (WP-M5/M6).
- Changing the RT30 descriptor layout WP-M1 froze.

---

## Coordination notes

- Subagent ID: `wp-M2-base-page-alloc`; run e2e with
  `INVFS_E2E_AGENT=wp-M2-base-page-alloc`.
- E2E gate: `bash tools/run-e2e.sh tools/test-meta-v3.sh`.
- Dependencies: **WP-M1** (`wp/m1-format-v3`) must have landed the
  `invarifs.h` v3 structs, RT30 descriptor and `VOLF_V3` mount path.
- Blocks: WP-M3 (tree core), WP-M4 (fsck page validation).
