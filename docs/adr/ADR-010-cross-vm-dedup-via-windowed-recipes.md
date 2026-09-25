# ADR-010: Cross-VM Deduplication via Windowed Recipes (`INVFS_ALGO_WINDOW_SRC`)

## Status

Proposed — 2026-09-25

## Context

InvariantFS guarantees byte-exactness and runs block-level deduplication across all 64 KiB segments that land in the Shadow zone (`vol_sweep_dedupe`, ADR-001). Deduplication is content-addressed: identical segments share a single Physical Block Address (PBA), saving both disk space and I/O bandwidth.

### The Shannon Wall: Compressed Containers Resist Block Dedup

Empirical evidence from benchmarks on Ubuntu cloud images exposes a fundamental limitation:

| Corpus | Composition | PBAs deduped | Bytes freed | Notes |
|---|---|---|---|---|
| **A**: `test1.qcow2` + `ubuntu2404-base.qcow2` | test1 derived directly from u24 (cloned, minimal changes) | **15 144** | **~393 MiB** | Almost every unmodified cluster shared PBA |
| **B**: `ubuntu2204-base.qcow2` + `ubuntu2404-base.qcow2` | Two independent Canonical cloud images, same Ubuntu rootfs families | **1** | **~64 KiB** | Only the QCOW2 header magic matched |

The reason is straightforward but unavoidable:

- Canonical publishes QCOW2 images with `QCOW_OFLAG_COMPRESSED` enabled. Each cluster is zlib-deflated with a **per-cluster compression context**: the LZ77 sliding window and Huffman tree state at cluster $N$ depends on the bytes preceding it within the cluster's preceding file position.
- Two Ubuntu 24.04 images, even if they install identical packages, contain byte-different compressed clusters because QEMU wrote them in different zlib contexts.
- The compressed stream is **high-entropy pseudo-random noise** at the 64 KiB segment granularity. A BLAKE3-based block dedup cannot find matches — by the pigeonhole principle, two independently-compressed streams of the same source bytes produce nearly disjoint byte sets.
- This applies equally to ZIP, TAR.GZ, GZIP, PNG (IDAT), and every container that uses context-dependent compression.

### Decomposition Opens the Door

When `qcow2.codecpack` decomposes a QCOW2 image into a single member `!mbr0001-diskimg` (raw guest disk stream), the per-cluster zlib context is discarded. The decoded cluster bytes for `libc.so` block 17 in Ubuntu 22.04 are bit-identical to the same block in Ubuntu 24.04 — because at the guest-disk level, the file is the same uncompressed bytes.

This is the architectural observation that motivates the present decision:

> **Cross-VM deduplication is impossible at the container level (compressed) and inevitable at the guest-disk level (uncompressed). InvariantFS must bridge these two layers.**

The mechanism for that bridge is what this ADR specifies.

### The Gap in Today's Recipes

Every existing recipe opcode (`INVFS_ALGO_NONE` through `INVFS_ALGO_P7Z = 23`, see `src/core/invarifs.h:103-138` and `src/codecs/codec.c:1020-1045`) describes a **self-contained segment**: a (PBA, file_offset, length, algo) tuple that the read path decodes by fetching the segment at PBA and applying the named codec. There is no opcode that says "this file is bytes $[a,b]$ of inode $X$, optionally transformed".

The closest pre-existing facility is **container-member windows**: ZIP recipes (`vol_ast.c:191-357`) describe their members as `{name, csize, usize, data_off}` tuples that point into the parent archive bytes. But those are container-children metadata resolved by re-reading the container byte stream — not generic recipe entries, and not reusable for cross-file dedup.

`vol_decode_ast_entries` (`src/core/vol_read.c:371`) has no branch for "delegate to another file's read path".

## Decision

InvariantFS introduces a single new recipe opcode and a small containerpack ABI extension to make cross-VM deduplication tractable at the guest-filesystem level.

### 1. New Recipe Opcode: `INVFS_ALGO_WINDOW_SRC = 24`

A new constant is added to `src/core/invarifs.h` immediately after `INVFS_ALGO_P7Z`:

```c
#define INVFS_ALGO_WINDOW_SRC 24  /* recipe entry: window into another inode */
```

A recipe entry carrying this opcode is a **reference**, not a payload. The semantics are:

> "This recipe entry's bytes are `e.length` bytes of file `inode_id = e.src_inode`, starting at file offset `e.src_off`, optionally transformed by `e.transform_kind` with parameters `e.repro_*`."

The `invfs_ast_block_entry` struct (`invarifs.h:832-852`) is extended with a sibling field group (or a parallel `invfs_ast_window_entry` is added to the recipe AST, marked by `e.algo == INVFS_ALGO_WINDOW_SRC`):

```c
uint64_t src_inode_id;     /* inode_id of the source file */
uint64_t src_off;          /* byte offset in source file */
uint64_t src_len;          /* exact byte length of window */
uint8_t  transform_kind;   /* 0=verbatim, 1=zlib_decompress */
uint8_t  repro_level;      /* zlib level (1..9) */
uint8_t  repro_mem;        /* memLevel (1..9) */
uint8_t  repro_strategy;   /* Z_DEFAULT_STRATEGY etc. */
int8_t   repro_wbits;      /* -12 (qcow2), -15 (raw), 15 (zlib), 31 (gzip) */
uint8_t  reserved[3];
```

Reserved transform kinds for Phase 2:

- `2 = zstd_decompress`
- `3 = lz4_decompress`

MVP implements only `0` and `1`.

### 2. Read-Path Branch in `vol_decode_ast_entries`

`src/core/vol_read.c:vol_decode_ast_entries` (around line 371) gains a new dispatch arm:

```c
if (e->algo == INVFS_ALGO_WINDOW_SRC) {
    /* 1. Recursive read of source range */
    if (vol_read_range(v, e->src_inode_id,
                       e->src_off, e->src_len, scratch) < 0) {
        return -1;
    }
    /* 2. Optional inverse transform */
    if (e->transform_kind == 1) {
        if (invfs_deflate_decompress(scratch, e->src_len,
                                     e->repro_wbits,
                                     e->repro_level,
                                     e->repro_mem,
                                     e->repro_strategy,
                                     dst, &out_len) < 0) {
            return -1;
        }
    } else {
        memcpy(dst, scratch, e->src_len);
    }
    continue;
}
```

A recursion depth counter (per-`vol_read_range` call, propagated through the `volume` handle or as a thread-local) bounds WINDOW_SRC chains to `INVFS_WINDOW_MAX_DEPTH = 4`. Exceeding the bound falls back to materializing the source into Shadow and continuing — the same pattern used by `algo_is_whole_file` at `vol_read.c:1330-1351`.

### 3. Containerpack ABI Extension: `INVFS_PACK_CMD_GUEST_ENUM = 6`

`src/codecs/codec.h:119-129` adds a new containerpack command:

```c
INVFS_PACK_CMD_GUEST_ENUM = 6  /* {in} {out} -> guest file index */
```

`IVPACK_API_VERSION` in `src/include/ivpack_api.h:19` is bumped from `2` to `3`. Plugins compiled against v2 ignore the new command value (their `switch (a->cmd)` falls through to `IVPACK_RC_USAGE`), and the host only dispatches `GUEST_ENUM` when the loaded plugin's manifest declares `has_guest_enum = yes`.

The qcow2 codecpack's manifest gains the corresponding entry:

```
guest_enum = bin/qcow2 guest-enum {in} {out}
has_guest_enum = yes
```

`tools/codecpacks/qcow2.codecpack/qcow2.c` adds a `cmd_guest_enum` handler that:

1. Decomposes the qcow2 normally into a temp `diskimg`.
2. Forks the `ext4fs` codecpack's `enumerate` command over the temp file (re-using the existing fork+exec path in `helper_exec.c`).
3. Parses the returned member table; for each member, computes guest-LBA extents via the existing ext4fs `extract` command (or by re-using a snapshot of the ext4 extent trees).
4. Emits one record per guest file: `<path>\t<guest_lba>\t<length>\t<transform_kind>\t<repro_level>\t<repro_mem>\t<repro_strategy>\t<repro_wbits>\n`.

Transform kinds are determined by examining the qcow2's L2 table at the corresponding cluster offsets: clusters with `QCOW_OFLAG_COMPRESSED` get `transform_kind=1` and the recorded zlib parameters; uncompressed clusters get `transform_kind=0`. The `deflate_repro` helper (`src/codecs/deflate_repro.{h,c}`) supplies parameter search where the cluster's original zlib parameters are not stored.

### 4. Sweep Integration: Auto-Publish Guest Files

`src/core/vol_sweep.c:vol_containerpack_sweep` (currently `vol_cpack.c:2345-2712`) is extended after the existing `!mbr*` siblings are committed:

```c
if (def->has_guest_enum) {
    if (invfs_codec_pack_cmd(pc, INVFS_PACK_CMD_GUEST_ENUM,
                             pin, NULL, NULL, NULL, NULL, NULL,
                             guest_index_path) == 0) {
        FILE *f = fopen(guest_index_path, "r");
        while (parse_guest_record(f, &gr) == 0) {
            char sibling_name[INVFS_NAME_MAX + 16];
            snprintf(sibling_name, sizeof(sibling_name),
                     "%s!%s", base_name, gr.relative_path);
            invfs_ast_block_entry win = {0};
            win.algo = INVFS_ALGO_WINDOW_SRC;
            win.src_inode_id = diskimg_inode_id;
            win.src_off = gr.guest_lba * cluster_size;
            win.length = gr.length;
            win.transform_kind = gr.transform_kind;
            /* ... populate repro_* fields from gr ... */
            vol_v3_publish_blob_inode(v, sibling_name, &win, 1);
        }
        fclose(f);
    }
}
```

Each guest file becomes an inode in the same directory as the source container, with a single-entry recipe that references the existing `!mbr0001-diskimg` member. The naming convention `<source>!<sanitized_path>` follows the established `!`-separator pattern (`vol_cpack.c:1745-1752`). Example:

```
ubuntu2404.qcow2                          # original compressed image
ubuntu2404.qcow2!mbr0001-diskimg          # raw guest disk (existing)
ubuntu2404.qcow2!etc-passwd               # NEW: window into diskimg
ubuntu2404.qcow2!usr-lib-x86_64-libc.so.6 # NEW: window into diskimg
```

After sweep, two Ubuntu cloud images that share even one guest file will see that file's inode share a single PBA in the Shadow zone — automatically, by the existing dedup pass.

### 5. Reference Counting: Windows Protect Their Sources

`src/core/vol_ast.c:vol_v3_free_recipe_blocks` (lines 67-116) gains a window-aware path. The pattern mirrors the existing shared-text-batch reference logic (`vol_ast.c:91-97`):

- Each `INVFS_ALGO_WINDOW_SRC` recipe entry increments a `windows_to` counter on `src_inode_id` at sweep time.
- The counter is stored in the recipe-blob B+tree (key prefix `INVFS_V3_RECIPE_KEY_WINDOW`, alongside the existing `INVFS_V3_RECIPE_KEY_*` prefixes at `invarifs.h:1176`).
- `vol_v3_free_recipe_blocks` refuses to reclaim the source's data blocks while `windows_to > 0`.
- Folding decrements the counter when a window entry's inode is dropped.

### 6. Lazy Activation

Decomposition and guest-file publishing fire **only during `invf-sweep`**, triggered manually (offline `invf-sweep <img>`, FUSE `kill -USR1`, or the `user.invfs.sweep` xattr). There is no eager write-path decomposition in MVP.

This matches the existing lazy-sweep convention documented in `AGENTS.md` §2.5 and ensures write-path latency remains unaffected. Phase 2 may consider a bounded background queue for write-triggered decomposition (see WP discussion).

## Consequences

### Positive

- **Cross-VM deduplication becomes real, not theoretical.** A 10-VM Ubuntu deployment that today stores 10 × 600 MiB compressed qcow2 images (~6 GiB total) deduplicates the ~50k identical guest files (kernel modules, libc, openssl, systemd, locale data, …) down to one copy in Shadow. Expected savings: 5-9 GiB depending on Ubuntu version spread.
- **Storage-efficient archival of OS fleets.** Image baselines, CI containers, and dev VM caches all collapse to one canonical copy plus tiny per-instance diffs.
- **Reuses every existing mechanism.** Block dedup, ARC cache, COW B+tree, sweep — none of these change. The new opcode slots into `vol_decode_ast_entries` like any other segment decoder.
- **Backward-compatible ABI.** API v3 plugins ignore unknown commands; v2 hosts ignore the new opcode at the recipe level (it remains a 32-byte entry whose algo value is simply not in their switch).
- **Bit-exactness preserved.** Window reads either succeed (window into materialized source) or fail (source unlinked → counter holds it). There is no silent data loss path.

### Negative

- **Read-path recursion depth becomes a real concern.** A window into a qcow2 cluster that itself references a delta-lossless recipe (Q2R3) hits two layers; add ARC and the read path becomes a small DAG. Bounded by `INVFS_WINDOW_MAX_DEPTH = 4` to prevent pathological chains.
- **ARC pressure.** Each distinct guest file becomes an ARC tag entry. A 50k-file Windows VM rootfs adds 50k ARC tags on first read; bounded by `INVFS_WINDOW_MAX_FILES = 50000` per sweep to prevent OOM in pathological cases.
- **Refcount correctness is critical.** A bug in the `windows_to` counter leaks disk space; a bug in fold-time decrement prematurely frees still-referenced sources. The shared-batch code path at `vol_ast.c:91-97` is the model; the new path needs its own dedicated unit test.
- **MVP only supports deflate repro.** Phase 2 adds zstd and lz4 transforms; until then, compressed-cluster sources with non-deflate codecs fall back to verbatim (transform_kind=0) and gain no additional savings.
- **Sweep cost grows.** A 50k-file Windows guest disk adds ~50k inode publications to a single sweep invocation. Wall-time impact: order-of-minutes for a single large image; amortized across images by parallel sweep (already supported).

### Out of Scope (this ADR)

- Eager write-path decomposition. The lazy sweep trigger is the MVP choice; eager-decompose is a follow-on discussion (likely a separate ADR if pursued).
- Zstd/LZ4 transform kinds. Reserved opcode values but not implemented; Phase 2.
- Guest filesystem types beyond ext4 inside qcow2. FAT32/NTFS/XFS windows require their own containerpack's `GUEST_ENUM` implementation; qcow2+ext4 is the MVP target because it is the dominant cloud image format.
- Auto-creation of a hierarchical `.guestfs/` tree inside the volume. MVP uses flat sibling naming (`<source>!<sanitized_path>`); directory-style exposure is a Phase 2 UX improvement.

## Amendment: Multi-member QCOW2 decomposition

The implementation direction was revised after measuring the actual container ABI. The generic `INVFS_ALGO_WINDOW_SRC` recipe opcode and `GUEST_ENUM` command are deferred; they are not required for the native nested-container path.

QCOW2 now emits two members:

1. `idx=1`, `diskimg`: the full virtual-size guest disk, with unallocated regions zero-filled. Nested `rawdisk` and filesystem codecpacks consume this member.
2. `idx=2`, `rankimg`: the allocated-cluster stream in rank-packed order. Q2R1/Q2R2 recipes and MRMP reference this member, preserving bit-exact original-image reads and rebuild.

This keeps the existing seekable MRMP format unchanged while allowing a sparse QCOW2 image to be parsed as a complete raw disk. Decomposition remains lazy and is activated by `invf-sweep`; no write-path behavior changes.

The tradeoff is additional member storage and extraction work. Filesystem-specific enumeration and publication remain the responsibility of the delegated `ext4fs` codecpack; this amendment is limited to QCOW2.
