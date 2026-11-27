# CONVERT-DESIGN — ext4 → InvariantFS, phase 1 (in place), and what phase 0 already proves

**Status:** design, not implemented. Phase 0 (`tools/invf-convert.c`, `convert` +
`verify`) is implemented, measured and gated by `tools/test-convert-ext4.sh`.
Every number in this file was measured on this box (`/var/tmp`, ext4 6.13 tools,
InvFS `v0.6.12`, 2026-11-27) and names the corpus it came from.

## 1. Why the obvious shape is not expressible, and what the inverse is

An InvFS volume is **device-absolute**. `src/cli/mkfs.c:400` sets
`metadata_zone_start = 1`, block 0 holds the superblock and every descriptor
(`src/core/invarifs.h`: superblock `0x00..0x90`, RDP0, RSZ0, DEVT `0x2A0`, CVT0
`0x360`, MET0 `0x3A0`, PCK0 `0x3C4..0x9D0`, RT30 `0x9D0..0xA00`, SPT0
`0xA00..0xA20`, SPN0 from `0xA20`), the bitmap occupies
blocks `1..ceil(total_blocks/8/4096)` (`src/cli/mkfs.c:90`, `:524-525`), the
last block holds the ANC0 tail anchor (`src/cli/mkfs.c:830`, 124 bytes at
offset 0 of `total_blocks-1`, `src/core/invarifs.h:783-843`), and a pba is a
device offset: `pba * block_size` with no base offset anywhere.

So "create a volume inside another filesystem's free space and keep both" has no
representation: the volume's identity lives at two fixed device addresses that
belong to the other filesystem. **Putting a volume inside another filesystem is
not the operation.** The operation that works is the inverse, and it is the one
btrfs-convert performs:

> build the NEW filesystem on the whole device, and keep the OLD filesystem as
> **data inside it** — one file whose bytes are the old image.

That inversion is what makes the phase-1 story coherent: the source stops being
a container the new volume lives in, and becomes content the new volume holds.
Everything else — the import, the verification — is phase 0, which is why phase
0 is the pipeline and not a toy.

## 2. What the source looks like, measured

The ext4fs codecpack answers what every byte of the source IS (`classify`,
GCFG v1 table: `cls 0` meta_structure, `1` garbage, `2` member; `flags bit0`
zero_verified — a block the pack READ and found all zero). `invf-convert
source-map <img>` prints it. Two corpora:

| | Ubuntu 24.04 cloud rootfs | phase-0 fixture (leg A) |
|---|---|---|
| image | 2 683 289 088 B (2.50 GiB) | 201 326 592 B (192 MiB) |
| block size | 4096 | 4096 |
| blocks | 655 099 | 49 152 |
| ranges in the byte map | 119 217 | 633 |
| meta_structure | 122 864 896 B (117.2 MiB) | 12 755 072 B (12.2 MiB) |
| garbage | 1 059 659 745 B (1010.6 MiB) | 176 522 164 B (168.3 MiB) |
| …of which zero-verified | 837 865 472 B (799.1 MiB) | 167.19 MiB |
| member | 1 500 764 447 B (1.40 GiB) | 12 049 356 B (11.5 MiB) |
| carry = meta + member | 1 623 629 343 B (1.51 GiB) | 24 804 428 B (23.7 MiB) |
| allocated (e2fsck) | 451 817 / 655 099 blocks = **68.97 %** | ~12 % |
| entries / member inodes | 85 067 / 69 373 | 382 / 340 |

Two numbers matter and neither is derivable from `df`:

* **carry = meta + member** (1 623 629 343 B): the bytes a conversion has to
  *carry forward* into the new tree. Garbage is not part of it.
* **carry_exact = image − zero_verified = 1 845 423 616 B (1.72 GiB)**: the
  bytes needed to reproduce the old image *bit-exactly*, because a free block
  with bit-rot is not zero and the pack only marks what it read.

The 1.72 GiB is 68.8 % of the device, while `e2fsck` says 68.97 % of the blocks
are allocated — the two figures are close and **not equal**, and the difference
is the point: zero-verified runs also occur inside blocks ext4 considers
allocated (unwritten extents), so "free space" and "safe to discard" are
different sets. Any phase-1 design that equates them is wrong.

## 3. The pipeline

```
 0  classify            pack `classify` -> the GCFG byte map. Refuses an image
                        the pack cannot read (ext2, unknown INCOMPAT, block size
                        >4K, unclean state) BEFORE anything is written.
 1  plan                from the map: the RESERVED extent set =
                          { member ranges } ∪ { meta_structure ranges }
                          ∪ { every garbage range that is NOT zero-verified }
                        (i.e. everything carrying information; only
                        zero-verified garbage may be reused)
 2  mkfs --reserve      a fresh volume laid over the whole device whose bitmap
                        arrives with the reserved set PRE-MARKED ALLOCATED, so
                        the allocator can only ever hand out blocks the source
                        does not need.  [new primitive #1]
 3  relocate            copy the blocks InvFS itself is about to occupy --
                        block 0, the bitmap blocks, the ANC0 tail -- out of the
                        source's allocation into blocks from step 2's free pool,
                        and rewrite the old-image file's recipe to name the
                        copies.  [see §5: this step is NOT optional and is not
                        one of the two mkfs primitives]
 4  import              the phase-0 tree import, unchanged: real files,
                        mode/uid/gid/packed-nanosecond mtime, symlinks,
                        hardlinks via vol_hardlink, xattrs, specials, streamed
                        content, folded every N files.
 5  old-image file      publish ONE file (".invariantfs/converted-from.ext4" or
                        operator-chosen) whose logical size is the source device
                        size and whose recipe names (a) the relocated blocks of
                        step 3 and (b) every other reserved extent IN PLACE, or
                        a sparse copy of them. Zero-copy is possible precisely
                        because pba is a device offset (§1).
 6  commit              write the commit set LAST, in one fsync:
                          block 0        superblock + descriptors,
                          blocks 1..B    the bitmap (B = ceil(total/8/4096)),
                          block N-1      ANC0.
                        [new primitive #2]
 7  verify              phase-0 `verify` against the mounted source and the
                        mounted volume, plus `invf-fsck` and
                        `invf-verify --deep`, exactly as phase 0 does today.
```

## 4. Why those two primitives, and nothing else

**#1 `mkfs --reserve <extents>` (pre-mark the source's non-garbage extents as
allocated in the NEW volume's bitmap).** Without it the new allocator's free
pool overlaps the source's live extents, and step 4 writes the imported tree on
top of the ext4 blocks it is currently reading: the copy destroys its own input.
The reserved set comes from the pack, not from a guess (§2). This is also what
makes `--reserve` a *precondition* rather than an optimisation: with it, every
block the volume hands out is by construction a block the old image does not
need; without it there is no invariant at all.

**#2 `mkfs --defer-commit` (write block 0 and the tail anchor LAST, one fsync).**
Those are the two places where the volume's existence is recorded, and both are
blocks the *old* filesystem also uses (block 0 is ext4's boot block plus, at
offset 1024, its superblock; the last block can be live ext4 data). Until they
are written, the device is still an ext4 filesystem with extra data in its free
space — mountable, fsck-clean, bit-identical in every file. One fsync of the
commit set is the single point of no return, and it is a *deliberate* one: a
conversion tool must have exactly one, so the crash matrix has exactly one
irreversible row.

Everything else is reused: the pack's table (§2), the phase-0 import and
verification (§7), `invf-fsck`, `invf-verify --deep`, and the ANC0/RT30/SPT0
machinery that already exists. No other mkfs flag is needed — but see §5: the
relocation step is not a mkfs flag, and that is a finding, not a design choice.

## 5. The commit set is exactly three writes, and the relocation is a fourth problem

The commit set, from the format (`src/core/invarifs.h`) and mkfs
(`src/cli/mkfs.c:599-643`, `:783-830`):

| write | bytes | why it is in the commit set |
|---|---|---|
| block 0 | superblock `0x00..0x90`, RDP0, RSZ0, DEVT, CVT0, MET0, PCK0, RT30 `0x9D0..0xA00`, SPT0 `0xA00..0xA20` | the volume is not a volume until this is CRC-valid; it is also ext4's boot block + superblock-at-1024 region |
| blocks `1..B` | `B = ceil(total_blocks/8/4096)` = **21 blocks** on the Ubuntu corpus | the bitmap is the allocator's only state; `metadata_zone_start = 1` (`mkfs.c:400`) |
| block `N-1` | ANC0, 124 B | the tail anchor is the backup of block 0 that is reachable from the device size alone (`invarifs.h:783`) |

**Measured complication (this is the phase-1 finding).** InvFS's bitmap lands on
blocks 1..21, which is exactly where a 4 KiB-block ext4 keeps its **GDT and
reserved-GDT blocks** (the superblock is at byte 1024, inside block 0).
i.e. two of the three commit writes land on live ext4 metadata, not on free
space. The tail block is worse: on the Ubuntu image `e2fsck` reports all 655 099
blocks potentially live, so `N-1` may be a file's data block.

Therefore the commit set is writeable only after a **relocation** step (§3
step 3) has copied those blocks somewhere the volume owns and the old-image
file's recipe names the copies. Consequences:

* the brief's "two new primitives" are necessary but **not sufficient**: with
  `--reserve` + `--defer-commit` alone, the commit destroys the source's GDT and
  the old image is no longer mountable, which breaks the one promise phase 1
  exists to keep ("the source must still be mountable and unchanged").
* relocation is bounded (block 0 + 21 bitmap blocks + 1 tail block = 23 blocks on
  that corpus), so it is cheap; it needs a recipe-splice operation, not a new
  mkfs flag, and phase 0 has no equivalent yet (phase 0's destination is a
  different file; there is nothing to relocate).
* the anchor probe already refuses to adopt a tail block it did not write
  (`invarifs.h:783-843` "never adopts or creates a tail block it did not
  write"), so a phase-1 tool must place the relocation BEFORE the ANC0 write and
  must not rely on the tail's old content.

## 6. Crash matrix

Each row is a claim plus the test that has to prove it. "Unchanged" means: every
file's bytes and every metadata field compare equal to a manifest taken before
the conversion started (the phase-0 `verify` comparison, pointed at two mounts
of the same image), and `e2fsck -fn` is clean. "Decided" means: block 0 is
either a valid ext4 superblock at 1024 or a valid InvFS superblock, never an
ambiguity — the InvFS superblock has a magic and a CRC32C over `0x00..0x7C`, so
a torn 4096-byte write is *detectable*; the recovery rule is "if the InvFS magic
and CRC are both valid, it is a volume; otherwise treat it as the source", and
the ANC0 anchor at `N-1` is the second opinion.

| # | failure point | device state | source | the test |
|---|---|---|---|---|
| C1 | after `classify` / `plan`, before any write | untouched | mountable, identical | no writes occurred: assert the image's BLAKE3 is unchanged |
| C2 | during `mkfs --reserve` (bitmap written, block 0 not) | ext4 + a stale bitmap in free space | mountable, identical, `e2fsck` clean | kill the tool at the reserve boundary (it takes a `--crash-after=reserve` knob), then mount ro + compare + fsck |
| C3 | mid-copy (some files imported, some not) | ext4 + data in free blocks | mountable, identical, `e2fsck` clean | `--crash-after=N-files`; same three assertions. The freed blocks are ext4-free by construction (§4 #1), which is what makes this row true |
| C4 | just before the commit set | as C3, plus the old-image file complete | mountable, identical | `--crash-after=pre-commit` |
| C5 | **during** the commit fsync | block 0 possibly torn | `e2fsck` may be unrepresentable (block 0 is the boot block, not FS metadata) — but every file's bytes are still readable, and the *decided* rule must answer | `--crash-after=commit-torn` (truncate the commit write to 512 B): assert "decided", and if the answer is ext4, run the C2/C3 assertions |
| C6 | after the commit, before the volume is fscked | an InvFS volume with a delta log | not an ext4 image any more; the old tree is inside the volume as the old-image file | `invf-fsck` clean and `invf-convert verify` of the mounted old-image file against the pre-conversion manifest |

Rows C2–C5 are the ones a claim cannot cover: they need a fault-injection knob
that stops the tool at a named boundary and leaves the device exactly as a power
loss would. Phase 0 has the harness (import + verify + fsck); what it lacks is
(a) the knob, (b) `--reserve`, (c) `--defer-commit`, (d) the relocation of §5.

## 7. Space rule, derived from the numbers rather than assumed

Let `D` = device bytes, `U` = allocated bytes (`e2fsck`), `M` = meta_structure
bytes, `C` = content bytes (`≈ U − M`), `α` = engine metadata cost **per inode**,
`I` = inode count, `h` = fold/commit headroom.

A phase-1 run has to fit the imported tree into the space the source is not
using:

```
   free_at_start = D − U
   need          = C + α·I + h
   D − U ≥ C + α·I + h          with C ≈ U − M
   ⇒  U ≤ (D + M − α·I − h) / 2
```

That is where "50 % free" comes from — and why it is not enough. `M` (the
source's own structural metadata, which the copied tree does not have to store
again) pushes the limit up; `α·I` (the engine's per-inode cost, which the source
had in its own form) and `h` push it down, and for a rootfs with tens of
thousands of inodes `α·I` dominates.

`α` was measured on the Ubuntu corpus: the converted volume used
2 680 057 856 B of physical blocks against 1 500 838 175 B of content over
75 888 live inodes ⇒ **α ≈ 15.5 KiB/inode** (AGENTS.md §2.7's 8.3 KiB/inode
figure was measured on empty small files; a real rootfs carries recipes, xattr
keys and delta records as well). Phase 0's tool now sizes destinations as
`content + 20 KiB/inode + max(128 MiB, 5 % of content)` and *fails the run*
below `max(2 %, 64 MiB)` free, because the first estimate (16 KiB/inode,
64 MiB) put this corpus at 99.1 % full and `invf-fsck` then refused its own fold
(`fold: FAILED applying records at #168117 of 179299`).

Plugging in the corpus: `D` = 2683 MiB, `M` = 117 MiB, `α·I` = 1147 MiB,
`h` = 134 MiB ⇒ `U ≤ 760 MiB`, i.e. the device would have to be under **28 %**
used for an in-place run. It is at 69 %. So this image is not convertible in
place at its current size — the honest answer is "grow the device to ≈ 6.4 GiB
or copy to another device", and a phase-1 tool must say that as a precondition
failure *before* the first write, not discover it at ENOSPC:

```
  precondition: free_at_start ≥ C + 20 KiB·I + max(128 MiB, 5 %·C)
  else: refuse, printing all four numbers and the shortfall
```

For the phase-0 fixture (192 MiB image, 23.7 MiB carry, 12 % used) the rule
passes with room to spare, which is why phase 0's suite is a valid harness for
the pipeline even though its corpus is small.

## 8. What phase 0 already proves, and what it does not

Proved by `tools/test-convert-ext4.sh` today (all legs green, 7 s wall):

* the byte map is a real oracle: 339 of 340 member inodes of the fixture carried
  byte-for-byte (the 340th is ext4's `orphan_file` inode, no dirent, named in the
  report), and 69 373 of 69 373 on the Ubuntu rootfs;
* the tree import is metadata-faithful: mode (incl. 04755), uid/gid, mtime to
  the nanosecond through the mount, symlinks, hardlink partitions read from the
  engine's own inode ids, xattrs including a POSIX ACL blob, char/block/fifo/
  socket specials;
* the comparison can fail: 7 injected mutations produce 11 named differences and
  exit 1;
* refusals are loud and leave nothing behind: ext2, an unknown RO_COMPAT
  feature, and a `!` name each exit 3 with the reason and no destination file;
* a real 2.5 GiB rootfs converts and verifies IDENTICAL. Measured twice, which
  is also the fold A/B (§9 question 4):

  | | no folding (`--fold-every=0`) | folding every 8192 files (default) |
  |---|---|---|
  | convert, total | 702 s | **845 s** |
  | — of which import | 691 s | 826 s (includes 8 folds) |
  | delta left on the volume | 179 299 records / 32 MiB | **0 records** |
  | `invf-fsck` on the result | `fold: FAILED … at #72794 of 179299` | OK |
  | volume free after | 5 882 of 660 193 blocks (23 MiB) | 497 905 of 746 048 blocks (1.90 GiB) |
  | verify, total | 176 s | 275 s |
  | verify per file | p50 0.86 ms, p95 1.42 ms | p50 1.28 ms, p95 2.17 ms |
  | DIFFS | 0 (IDENTICAL) | **0 (IDENTICAL)** |

  So folding costs ~20 % of the import and is the difference between a volume
  that can be compacted and one that cannot; the per-file verification cost is
  dominated by reading 1.40 GiB twice through a loop mount and a FUSE mount
  (p99 4.84 ms, max 1 051 ms — the 117 MiB kernel image).

Not proved, and out of phase 0's reach: nothing above touches the source's own
blocks, so **no crash-matrix row has been tested yet** — the fault-injection knob
and the in-place placement are phase-1 work. Equally untested: relocation (§5),
zero-copy old-image recipes (§3 step 5), and the reservation's interaction with
the sweep's dedupe (which rewrites blocks the old image still names).

## 9. Open questions

1. **Zero-copy vs copy for the old image.** A recipe may name pbas directly
   (device-absolute, §1), so the old filesystem could be preserved at zero
   storage cost. But then the volume's sweep, dedupe and seal see those blocks as
   ordinary content: dedupe would rewrite them (destroying bit-exactness of the
   old image), seal would compute parity over them, and `vol_release_superseded_
   blob` would free them if the old-image file is ever replaced. Does the format
   need a "pinned, never-rewritten" block class, and is that a new zone tag or a
   new per-block flag? Phase 0 cannot answer it; phase 1 must decide before it
   writes a recipe.
2. **The tail block.** If `N-1` is live ext4 data, relocation is required; if it
   is free, it is not. `--reserve` knows from the pack's map (a tail block inside
   a member range is live). Should mkfs refuse to place ANC0 at `N-1` when
   relocation did not happen, or should `--reserve` reserve `N-1` unconditionally
   and relocate only when the map says it is live?
3. **Block 0's boot area.** InvFS overwrites ext4's boot block (0..1023) with its
   superblock. For a *partition* image that is not filesystem metadata (ext4
   never reads it), but for a whole-disk image it is the bootloader's stage 1.
   Does phase 1 refuse a whole-disk source unless the boot area is inside the
   relocated set?
4. **The fold at conversion scale.** Measured: a single 179 299-record delta
   cannot be folded on a tight volume — `invf-fsck` refused it twice (at #72794
   and, after `invf-resize 4G`, at #168117) while the volume stayed CLEAN and
   every file verified. Phase 0 works around it by folding every 8192 files, but
   the engine-side question is open: is that a fold-space accounting defect
   (displaced base pages not reclaimed before the next allocation), and does a
   phase-1 commit need the delta empty (fold to zero) as a precondition?
5. **The metadata zone must be sized for `I` before `I` is known.** Phase 0 can
   walk the tree first (it mounts the source anyway). Phase 1 writes into the
   device while it walks, so `INVFS_META_FRAC` has to come from a pre-pass (the
   pack's member count is available from `enumerate` without a walk — that is
   what `invf-convert`'s oracle already parses), and a wrong value is an ENOSPC
   at file number 40 000 that no `--reserve` can undo.
6. **What does `invf-rollback` mean for a conversion?** Post-commit the volume's
   first save point would label "the state before the sweep" — not "before the
   conversion". A conversion is not a sweep, and rollback to "no volume" is the
   device's pre-commit bytes, which the commit by definition overwrote. Either
   the conversion arms an SPT0 whose restore is the pre-import volume (useful),
   or it must clear the save point so a later `invf-rollback` cannot pretend to
   undo a conversion (safe). Deciding this needs an ADR, and the answer is not
   obviously "reuse SPT0".
7. **A 1 KiB-block ext4.** InvFS's bitmap at blocks 1..B lands on the ext4
   superblock itself (block 1 for `-b 1024`), not just the GDT, so relocation
   covers one more critical structure. Phase 0 converts 1 KiB images fine
   (`mkfs.ext4 -b 1024` is in the suite's fixture matrix via leg C's refusal
   corpus), so the *content* side is proven; the *placement* side is not.
