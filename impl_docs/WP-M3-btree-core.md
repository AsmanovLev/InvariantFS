# WP-M3 — base B+-tree core (search / COW upsert / split / merge / scan)

**Branch:** `wp/M3-btree-core`
**Worktree:** `/tmp/invfs-wp-M3`
**Severity:** HIGH (format-breaking; the stable tier's core algorithm)
**Source:** `impl_docs/design-meta-v3.md` §3 (read path), §4 (write path), §5 (fold), §12 (format), §15.1, §16
**Estimated effort:** large (tree algorithm + unit harness)

---

## Scope

Implement the **immutable base B+-tree** over WP-M2's page format: point
search, copy-on-write upsert, node split and merge, and an ordered
range scan (the base side of `readdir`).

Files:
- `src/core/vol_btree.h` — new: tree API, key/value abstraction, cursor.
- `src/core/vol_btree.c` — new: search, COW upsert, split/merge, scan.
- `Makefile` — add `vol_btree.o` to `CORE` (lines 30-34) and a new
  `infv-btree_test` binary to `test` (lines 133-138).
- `tools/btree_test.c` (or `src/cli/btree_test.c`) — new unit harness.

No delta integration, no fold orchestration, no inode row schema.

---

## Why

The base is the O(log N) stable tier (design §1: replaces the O(N) mount
scan and dead-record bloat). §3–§5 all assume a B+-tree with a COW upsert
that shares untouched pages. Until search/upsert/split/merge are unit-
tested, the inode tree (WP-M5), fold (later WP), and fsck (WP-M4) have no
substrate.

---

## Design

### Frozen API

```c
typedef struct { const uint8_t *p; uint16_t n; } bt_key;   /* ordered bytes */
typedef struct { const uint8_t *p; uint16_t n; } bt_val;

int  btree_search(invfs_volume *v, invfs_blkptr root,
                  bt_key key, bt_val *val_out, int *found);
int  btree_upsert(invfs_volume *v, invfs_blkptr root, bt_key key,
                  bt_val val, invfs_blkptr *new_root_out);   /* COW */
int  btree_delete(invfs_volume *v, invfs_blkptr root, bt_key key,
                  invfs_blkptr *new_root_out);               /* tombstoneless */
typedef int (*bt_scan_cb)(void *ctx, bt_key k, bt_val v);
int  btree_scan(invfs_volume *v, invfs_blkptr root,
                bt_key lo, bt_key hi, bt_scan_cb cb, void *ctx);
```

- Key ordering is **byte-lexicographic** (the design doc does not define a
  comparator beyond "ordered"; byte order is the only non-speculative
  choice and makes the delta merge in WP-M6 well-defined).
- `btree_search` is **lock-free**: the base is immutable between folds
  (§3, §9); no lock is taken.
- `btree_upsert` is COW: every modified page is written to a **new pba**
  via `mbuf_alloc`/`mbuf_write` (WP-M2); untouched subtrees keep the old
  `blkptr`s. It returns a **new root** and never mutates the input tree.
- `btree_delete` physically removes the key from the copied path; there is
  no tombstone (§4, §10). Node **underflow** triggers merge/redistribute
  with a sibling; the design doc does not fix a fill factor, so this WP
  uses a conservative minimum (half-full) and documents it.

### Root publication

`btree_upsert` does not publish. The caller (WP-M5/fold) writes the new
root into the RT30 double-slot (`root_slot[2]`, `seq`; WP-M1 Design) and
barriers. `seq` is the atomicity anchor (higher seq + valid CRC wins),
reusing the `l2p_replay` idiom (`volume.c:644-669`).

### Crash / durability ordering

1. Write all COW pages (`mbuf_write`), then `vmux_barrier`
   (`volume_internal.h:132`).
2. Only then write the RT30 root slot with the incremented `seq`.
3. A crash before (2) leaves the previous root valid; a crash after (2)
   with a torn slot is resolved by seq+CRC (not torn-page redo — the
   design explicitly avoids a WAL, §11).

---

## Validation

1. `make test` — existing 4 binaries plus `infv-btree_test`
   (`Makefile:133-138`). Cases: insert/search/delete over N=1..10^5 random
   keys; iterator equals sorted model; COW invariant — a pre-upsert root
   still reads the old values and its pages are byte-identical
   (page-hash comparison); split at every level; merge/underflow on
   deletes; duplicate-key upsert replaces in place; bad `blkptr` CRC is
   refused.
2. `INVFS_V3=1 invf-mkfs t.img 1`; build a tree through the API and reopen
   the image — the same scan sequence is produced.
3. `bash tools/run-e2e.sh tools/test-meta-v3.sh` — WP-M1 leg 0 still
   passes.

---

## Out of scope (do NOT touch)

- Delta log/index, overlay reads, fold, save point, reclaim.
- Inode/dirent row layout or namespace operations (WP-M5/M6).
- fsck semantics beyond "bad pointer is refused" (WP-M4).
- Page-format changes (frozen by WP-M2); the entry encoding lives here.
- The v2 record path.

---

## Coordination notes

- Subagent ID: `wp-M3-btree-core`; run e2e with
  `INVFS_E2E_AGENT=wp-M3-btree-core`.
- E2E gate: `bash tools/run-e2e.sh tools/test-meta-v3.sh`.
- Dependencies: **WP-M2** (`wp/M2-base-page-alloc`) — page format and
  allocator; **WP-M1** — RT30 root slots.
- Blocks: WP-M4 (fsck page/root validation), WP-M5 (inode tree).
- The `bt_scan` callback signature is the contract WP-M6's readdir merge
  will use; do not change it without a new WP.
