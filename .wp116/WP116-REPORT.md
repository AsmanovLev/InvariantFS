# WP116 — Meta-v3 COW base-page reclaim: verification, design, safety argument

**Branch/worktree:** `wp/116-v3-reclaim-rootstack` @ `/tmp/invfs-wp116`, based on `main` 500996f.
**Status:** mechanism verified and independently measured; design + safety argument delivered;
**no reclaim fix shipped.** One independent documentation-correctness commit shipped.

---

## 1. Verification of the handover's claims

Every `file:line` claim was re-derived from the source. Summary: **the mechanism is
confirmed exactly, with three corrections and one significant addition.**

| Claim | Verdict | Evidence |
|---|---|---|
| `v3_publish()` never frees the superseded generation | **CONFIRMED** | `src/core/vol_btree.c:1993`. Body is: (allocate empty leaf if `root.pba==0`) → `vol_v3_bitmap_flush` → `vmux_barrier` → `mbuf_root_publish`. There is no free, no push onto any list. `old_gen` is accepted and used only for the empty-leaf generation stamp. |
| Called once per file created | **CONFIRMED, function name wrong** | The function is `vol_v3_inode_put` (`:2953`), **not** `vol_v3_inode_base_put` — that symbol does not exist anywhere in the tree. Its `v3_publish` call is at **`:2977`** (not 2975). |
| Called once per recipe supersede (3 sites) | **CONFIRMED but INCOMPLETE** | `vol_v3_recipe_store` (`:3118`) publishes at `:3147` and `:3181` (brief said 3144/3169/3178 — off by ~3–12 lines, same functions). |
| — **missed by the handover** | **NEW FINDING** | There are **two more publishers** the handover did not list: `vol_v3_dirent_put` (`:3481`, publish at `:3502`) and `vol_v3_dirent_del` (`:3505`, publish at `:3529`). Every dirent create and delete abandons a base-page generation too. The leak is therefore driven by *namespace mutation count*, not just file count. |
| `btree_reclaim_pinned()` is the only reclaim path and is one generation wide | **CONFIRMED** | `:1809`. Marks `keep_root` + `pinned_root` into a fresh `seen` bitmap, then `bt_free_rec(old_root)`. `bt_free_rec` (`:1739`) walks **only** the subtree of `old_root`; a root abandoned by an earlier publish is unreachable from the next `old_root` and is therefore never visited. |
| Its only caller is `fold_reclaim_hook` (`vol_fold.c:268`) | **CONFIRMED** | `grep` over `src/` and `tools/`: the sole call site of `vol_reclaim_mark_and_free` (which is a thin wrapper over `btree_reclaim_pinned`, `vol_reclaim.c:74-84`) is `fold_reclaim_hook`. |
| Gated at 64 MiB / 262144 records / 3600 s | **CONFIRMED** | `vol_fold.c:84-86`: `FOLD_TRIGGER_BYTES (64ull<<20)`, `FOLD_TRIGGER_RECORDS 262144ull`, `FOLD_TRIGGER_AGE_S 3600ull`. Tested in `vol_v3_fold_request` (`:447`). |
| `tools/invf-sweep.c` never calls `vol_v3_fold_request` or `vol_reclaim_schedule` | **CONFIRMED** | The only occurrences of either symbol in the file are inside **comments** (`:1412`, `:2101`, and a third at `:1528` saying the fold is "unconditional inside the run"). Zero call sites. |
| Only the FUSE drain calls them (`vol_sweep.c:2361-2364`) | **CONFIRMED** | Inside `vol_sweep_pending()` (`:2326`), whose own comment says "Called when the daemon holds the volume exclusively". Its only non-test caller is `src/cli/fuse_fs.c:1849`. `invf-sweep` never reaches it. |
| The two comments claiming the fold "always runs as part of a normal sweep" are false | **CONFIRMED — and there are THREE, not two** | `:1412-1413`, `:2100-2104`, and `:1528` ("since the fold is unconditional inside the run"). Shipped the fix; see §6. |
| `vol_reclaim_reader_snapshot` / `vol_reclaim_release` are called from nowhere | **CONFIRMED** | `vol_reclaim.c:37` and `:42`. `g_readers_in_flight` is only ever *decremented* (`:44`) and never incremented, so it is permanently 0 and `vol_reclaim_drain` (`:48`) is a no-op `while` loop. There is **no reader-side safety protocol today**. |
| Forcing the fold makes it worse (193 → 557) | **CONFIRMED, reproduced exactly** | See §2.3. |
| `invf-fsck -f` reclaims nothing | **CONFIRMED, with the reason** | `invf-fsck -f` reports `pages walked: 5` — it validates only the tree reachable from the published root, so it has no notion that 193 other pages exist. Reclaim is structurally out of its reach, not merely disabled. |

### 1.1 The RT30 double slot is an additional, unlisted correctness hazard

**This is the most important thing I found that the handover did not mention.**

`mbuf_root_publish` (`vol_metabuf.c:325`) writes the new root into `v->rt30.root_slot[seq & 1]`
and bumps `seq`. Because the slot is chosen by parity, **immediately after a publish the
*other* slot still names the previous root.** `mbuf_root_read` (`:357`) deliberately reads
**both** slots and keeps whichever has the higher page gen (`:390`), falling back to the
other slot if the best one fails `mbuf_page_validate` — that fallback is WP86's damage
tolerance.

But `fold_reclaim_hook` frees everything in `old_root` not reachable from `new_root`, and
`old_root` **is** the root the surviving RT30 slot still names. A freed base page is not
overwritten immediately, so it still passes `mbuf_page_validate` (the CRC is intact). So if
the newly published root page is later damaged — exactly WP86's failure class — `mbuf_root_read`
falls back to a root whose pages have been freed, and **silently adopts a stale namespace**:
every key folded since that root vanishes, with no error and no EIO.

This does not need the leak to be fixed to be true today. It is an independent latent
correctness bug in the *existing* reclaim path, and it is a hard constraint on any fix:
**"reachable from the current root" is the wrong liveness predicate. "Named by any RT30
slot" is the minimum.** §3 folds this into the design.

---

## 2. Independent measurement

### 2.1 Instrument

`bin/wp116_census` (read-only; never writes, never flushes) and `bin/wp116_fold` (mutating,
run only on throwaway copies). Both live in `.wp116/` in the worktree and are **uncommitted**;
they exist to produce this report, not to ship. Source: `.wp116/wp116_census.c`,
`.wp116/wp116_fold.c`. The one product-tree change they need is a 14-line read-only
`btree_mark_debug()` wrapper in `vol_btree.c` (also uncommitted) so the tool reuses
`bt_mark_rec` rather than duplicating the walk.

Method caveat worth recording: a v3 `mkfs` marks the **whole metadata zone allocated**, so
"pread every allocated block" costs 68 GB of reads on a 1 GB volume (~36 s). The census scans
allocated blocks **outside** the metadata zone in full (small) and **caps** the in-zone scan,
printing exactly how much it covered so the number can never be mistaken for a full census.
On these volumes the cap was never binding (`SKIPPED by cap 0`).

### 2.2 60-file reproducer — reproduces the handover's numbers

`INVFS_META_FRAC=16`, 1 GB volume (`invf-mkfs <img> 1`; note the argument is in **GB**, not MB).

```
empty volume            alloc = 24,585 blocks      <- exact match, handover 24,585
after 8 sweeps          alloc = 24,826 blocks      <- delta 241 blocks = 987,136 B
payload                 143,580 B
census:  v3 base pages (BPG3)      198
         live (from published root)   5
         UNREACHABLE               193   =  790,528 B
```

Every handover figure reproduces: 198 pages, live 5, 193 unreclaimed, empty 24,585.
My expansion is 6.875x against the handover's 6.008x — the difference is corpus, not
mechanism (my payload is 143,580 B vs their 171,126 B).

**Never converges, confirmed:** occupancy after sweeps 1–8 was
`24,886 / 24,826 / 24,826 / 24,826 / 24,826 / 24,826 / 24,826 / 24,826`. Flat from sweep 2 on.

**The generation histogram is the smoking gun** — one abandoned page per generation, a clean
staircase to gen 121 for 60 files:

```
gen 1..15: 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 pages    (…continuing to gen 121)
```

### 2.3 Forcing the fold makes it worse — reproduced exactly

On a copy, one forced `vol_v3_fold()`:

```
before fold:  198 base pages,   5 live, 193 UNREACHABLE
after  fold:  567 base pages,  10 live, 557 UNREACHABLE   <- handover: 193 -> 557
```

Further forced folds are no-ops (the delta is empty), so it settles at 557. The jump is a
one-time cost of the *first* fold: the fold rewrites the whole tree (live 5 → 10) and the hook
frees only the one-generation diff it was handed, so every generation abandoned by the 121
earlier publishes is still stranded, plus the pre-fold tree.

### 2.4 Size-independent, linear in file count

| files | lines/file | payload B | total base pages | unreachable | pages/file |
|---|---|---|---|---|---|
| 10 | 40 | 23,790 | 21 | 20 | 2.00 |
| 20 | 40 | 47,580 | 41 | 40 | 2.00 |
| 40 | 40 | 95,160 | 116 | 113 | 2.83 |
| 60 | 40 | 142,740 | 198 | 193 | 3.22 |
| **60** | **400 (10×)** | **1,453,260** | **198** | **193** | **3.22** |

The 10× row is the confirmation: **identical 193 unreachable pages and identical 198 total**
for 10.2× the payload. The cost is a function of mutation count, not data volume. Pages/file
rises gently with file count (2.00 → 3.22) because the live tree also deepens.

### 2.5 Silesia — leak confirmed; the compression ratio was NOT reproduced

`/srv/corpora/silesia`, 12 files, 211,938,580 B, 1 GB volume. This corpus is three orders of
magnitude below the ~46k-inode `invf-sweep` hang in `INCIDENTS.md`, so it was in scope.

The first sweep attempt was **OOM-killed** (rc 137) — the host has 7.7 GB total with 3.4 GB
of tmpfs already in use and only 2.9 GB available. A retry under `ulimit -v 2000000` and a
32 MB ARC limit completed (rc 0) but only swept 2 of 825 files and flushed 1,357 text batches
as *deferred*, so no real compression happened.

```
empty alloc            24,585 blocks
swept alloc            43,362 blocks      delta 18,777 blocks = 76,910,592 B
census: 9,250 base pages, 142 live, 9,108 UNREACHABLE = 37,306,368 B
        = 48.5% of everything stored above the empty volume
```

**The leak measurement is valid** — an unreferenced base page is unreferenced regardless of
how well the sweep compressed. **The ratio claim is not reproduced here**: my measured ratio is
0.3629x, versus the handover's 2.1428x, because compression did not run. I did not get
2.1428x → 3.6999x and am not asserting it.

**The handover's Silesia arithmetic is internally inconsistent:**

```
9,806 pages x 4,096 B = 40,165,376 B  ≠  41,623,552 B (claimed)
41,623,552 B / 4,096  = 10,162 pages  ≠  9,806 pages (claimed)
```

The page count and the byte count cannot both be right; they differ by 356 pages (1,458,176 B).
My independent measurement is 9,108 pages / 37,306,368 B — within 7% of the claimed page
count and 10% of the claimed byte count, so the **substance** holds, but the specific
"9,806 = 41,623,552 B" pair should not be quoted.

---

## 3. Design

### 3.1 The invariant the current code gets wrong

Liveness today is expressed as *"not reachable from the one root the caller happened to
hand me."* That is not a liveness predicate at all — it is a function of when you look. The
correct predicate is a **set of roots that something durable still names**, and that set is
larger than one.

### 3.2 The root stack (recommended)

**Shape.** Maintain, per volume, a durable **root stack**: an ordered list of the last N
published base roots, N ≥ 2, carried in a new descriptor next to RT30 (block 0 has room;
`SPN0` already sits at `0xA20` and the WP86-era descriptors are all in block 0). Each entry
is `{pba, gen, crc}` — a full `invfs_blkptr`, so an entry that fails its own CRC is dropped
rather than trusted. Reclaim marks from **every live root** and frees the rest.

The live root set at any moment is:

```
L = { every entry in the durable root stack }
  ∪ { every root named by an RT30 root_slot }      <- §1.1, the non-negotiable part
  ∪ { v->pinned_root, if a save point is live }
  ∪ { the root of any root-stack entry a live reader has pinned }
```

and reclaim becomes:

```
mark(L)                       -- union, not "the one root"
free(every allocated BPG3 page not in mark(L))
```

Note this changes the traversal from *reachability-from-old-root* to *scan-and-collect*.
That is the single most important structural consequence and it is what makes the fix
possible at all: a reachability walk can only ever see what it can reach, so a stack of
depth N still only frees N generations. Sweep the bitmap for allocated `BPG3` pages and free
the unmarked ones, and depth becomes irrelevant — a root abandoned by the very first publish
on the volume is reclaimed on the first pass that can see it, however old.

**Durability.** The stack is written with the same discipline as RT30: the new entry is made
durable *before* the root that supersedes an evicted entry is published, and eviction only
happens once the entry it drops is covered by a newer published root. A crash therefore
either keeps a superset of the roots that matter (safe, costs a little space) or exactly the
right set. It never drops a root that is still the only name for live data.

**What it changes:** `vol_reclaim_mark_and_free`'s contract (from two roots to a root *set*),
a new block-0 descriptor, `v3_publish` pushing instead of discarding, and the reclaim
collector replacing `bt_free_rec`.

**What it costs:** one extra descriptor (tens of bytes in block 0); a bitmap scan per reclaim
pass, O(allocated pages) rather than O(tree height) — but that scan is I/O-cheap relative to
a sweep and is the price of not leaking; retained space of N generations, which for a 60-file
corpus is a handful of pages.

**What it breaks:** anything that assumed `old_root` fully describes what may be freed. That
is `btree_reclaim`/`btree_reclaim_pinned` and their unit tests (`src/cli/btree_test.c:959,973`).
And it makes the §1.1 stale-root fallback *correct* rather than merely survivable.

### 3.3 Alternatives considered

**(a) Free the previous root inline, inside `v3_publish`.** Cheapest possible change — one
`mark(new) / free(old)` at the publish site. **Rejected:** it is one generation wide in the
*other* direction and is exactly as wrong. It frees the tree the surviving RT30 slot still
names (§1.1), it frees pages a concurrent reader may be walking, and it puts a
free-in-the-publish-path into the hot write path.

**(b) Reclaim relative to the oldest of N *retained* roots, keeping the reachability walk.**
Better than (a), but structurally capped: a root older than the retained window is still
unreachable from the oldest retained root and is still never freed. With N=2 the 60-file
corpus would stop leaking after the first two publishes and stay flat — the leak is
*bounded*, not eliminated, and the bound is invisible in the numbers. This is the tempting
option because it reuses existing code; it is the wrong one.

**(c) Periodic full orphan sweep over the bitmap (no stack at all).** Walk every allocated
`BPG3` page; mark from the current root, the RT30 slots, and the save point; free the rest.
This is (3.2) minus the stack, and it is genuinely simpler — the root stack is only needed if
you want to reclaim *during* normal operation rather than at reclaim time.

I recommend shipping **(c) first, then (3.2)**. (c) alone, driven by a reclaim that runs
after the fold, closes the whole measured leak with no new on-disk structure and no new
durability argument. The stack then becomes an *optimisation* (reclaim more eagerly, and
keep the RT30 fallback genuinely live) rather than a correctness requirement. If the stack
turns out to be hard to get right, (c) still ships the payoff.

**(d) Do nothing at reclaim; make the leak impossible by not COW-ing per file.** I.e. go
back to routing file creates through the delta (which `vol_v3_inode_delta_put` already does —
see the WP-M12 comment at `vol_btree.c:2059` saying the base-only helpers are kept for the
fold). This is the design the code was already migrating to and is the real fix for the
per-file component. **Not pursued here** because it is a much larger change than a reclaim
fix, and because the reclaim machinery must be correct regardless — the dirent publishers
(§1) and the fold still publish.

### 3.4 Two things that are separately worth fixing

- **A `v3_publish` that leaks is not the only leak.** Even with a perfect root stack, an
  unreachable page is only reclaimed when something walks the bitmap. Nothing today does
  except `invf-fsck` and, once fixed, the fold. So the stack alone is inert without a
  trigger — and the trigger is the missing `vol_v3_fold_request` call in `invf-sweep` (§6).
- **`invf-fsck -f` should reclaim orphans.** It is the one place that already has a
  whole-volume view, it is explicitly the repair tool, and it currently reports `OK` on a
  volume that is 48% leaked. Making it run the (c) collector would both fix the leak on
  existing volumes and give the validation suite something to assert against.

---

## 4. Reader-safety protocol (what `vol_reclaim.c:37,42` was for)

**The problem.** Reclaim frees a page that a reader may be inside `io_pread`-ing right now.
On this code base that is not a theoretical hazard: `mbuf_read` (`vol_metabuf.c:85`) is a bare
`io_pread` with no allocation check, and `bt_mark_rec`/`bt_free_rec` hold only a stack buffer —
there is no page handle, no refcount, and no lock between "read the page" and "free the page".
The existing epoch protocol is a shell: `g_readers_in_flight` is never incremented, so
`vol_reclaim_drain` always returns immediately having waited for nobody.

**The protocol.** A **per-page pin, acquired before the read, released after** — a
read-side RCU / hazard-pointer scheme, which is what `g_fold_epoch` was gesturing at but is
not what it implements. Epochs alone are the wrong tool here because the reclaim pass is
O(volume) and can outlast any fixed number of grace periods; a page can be freed the moment
the last pin on *it* drops.

Concretely:

1. **Pin acquisition precedes the read, always.** Every read of a base page goes through one
   choke point (today `mbuf_read_ptr`, and the mbuf cache read path) which, before the
   `io_pread`, publishes `(volume, pba)` into a per-CPU hazard slot and issues the required
   memory barrier; only then reads. If the read is satisfied from the mbuf cache the pin is
   still taken, because the cache can be invalidated mid-read.
2. **The reclaim collector intersects with the hazard set.** When the collector is about to
   free page *p*, it checks *p* against every CPU's published hazard slot. If any slot names
   *p*, *p* is skipped this pass. The check is O(1) per page against O(cores) slots — no
   reader-side bookkeeping on the hot path beyond one store.
3. **Bounded deferral, not unbounded pinning.** A page skipped this pass is simply reclaimed
   next pass. There is no correctness dependency on a reader ever finishing; a crashed or
   wedged reader costs one pass of space, not the reclaim.
4. **The free itself is a bitmap clear, and the ordering is already right.** `mbuf_free` →
   `vol_free_blocks` → `vol_free_run` clears the allocation bit *after* the content is
   already unreachable. The hazard check must be ordered before that clear; after it, a new
   reader is refused by the allocator rather than handed a half-dead page.
5. **Reclaim runs single-threaded against a quiesced writer.** Fold and reclaim already hold
   the volume write lock. The collector must additionally run with the delta/index writers
   excluded, so no new root can be published into the set being collected.

**Why this cannot be "just call the snapshot function".** `vol_reclaim_reader_snapshot`
returns a *global epoch*, and nothing bumps it per-reader or per-page. Making it real means
replacing it, not wiring it up. Any prototype must therefore add the pin to the read path
too — which is exactly why I did not ship one.

---

## 5. Validation bar before this may ship

Per AGENTS.md §1.2. **Every one of these must pass; none of them is optional.**

**A. The instrumentation must first be able to see the bug.** `bin/wp116_census` on the
60-file corpus must report `198 base pages / 5 live / 193 unreachable`. If the census cannot
reproduce that, nothing below is measuring anything.

**B. Unit level.**
```
$ make test
```
Currently 0 failures; must stay 0. `invf-btree_test` (`src/cli/btree_test.c:959,973`) must be
extended with a **multi-generation** case: publish G1…G5, then reclaim, then assert that
**every** page of G1..G4 is free and every page of G5 is allocated. The existing tests only
ever exercise the one-generation diff, which is why this bug survived.

**C. The leak closes, on the reproducer.**
```
$ INVFS_META_FRAC=16 bin/invf-mkfs /home/user/wp116/test.img 1
$ bin/invf-import /home/user/wp116/test.img <corpus60>
$ for i in 1 2 3 4 5 6 7 8; do bin/invf-sweep /home/user/wp116/test.img; done
$ bin/wp116_census /home/user/wp116/test.img
```
Expect: `UNREACHABLE base pages 0` (today: 193, stable across all 8 sweeps). Occupancy must
be **non-increasing** across the 8 sweeps, and the *final* figure must be below the
post-import figure.

**D. Bit-exactness is the gate that matters most.** For every corpus:
```
$ for f in <corpus>/*; do bin/invf-cat img "f$(basename $f)" | cmp - "$f" || echo "MISMATCH $f"; done
$ bin/invf-verify --deep img        # 0 corrupt
$ bin/invf-fsck img                 # rc 0, OK
```
The `test-rollback.sh` **[B]** leg is STRICT (`--`) and must stay green: a save point must be
able to restore a tree whose pages the reclaim decided were garbage.

**E. Crash safety at the reclaim boundary.** New fault-injection stages alongside the
existing `INVFS_FOLD_ABORT_AT` (`vol_fold.c`), at minimum:
`after-stack-write`, `after-mark`, `after-first-free`, `after-root-publish`. After each, the
volume must reopen, `invf-fsck` must report OK, and every file must still be bit-exact. A
crash mid-reclaim must never lose a key.

**F. Concurrency — the gate that does not exist yet and must be built.** This is the one I
would insist on. `bin/invf-concurrency_test` must be extended with a **reader-vs-reclaim
harness**: N reader threads in a tight `invf-cat` loop against a volume whose fold+reclaim is
running continuously. Assert (a) zero read errors, (b) zero bit mismatches, (c) no reader ever
observes a page that fails validation. **Until this harness exists and passes on the
*unfixed* code as a control, the fix's safety argument is an argument and not a result.** Run
it for at least 10^7 reads.

**G. E2E**, acquiring the lock per AGENTS.md §1.5:
```
$ INVFS_E2E_AGENT=wp116 bash tools/run-e2e.sh tools/test-writepath.sh
$ INVFS_E2E_AGENT=wp116 bash tools/run-e2e.sh tools/test-rollback.sh
$ INVFS_E2E_AGENT=wp116 bash tools/run-e2e.sh tools/test-textzone.sh
$ INVFS_E2E_AGENT=wp116 bash tools/run-e2e.sh tools/test-sweepboot.sh
```
plus `make e2e` in full before merge. `test-rollback.sh` is the one that will catch a
reclaim that frees a pinned tree.

**H. No regression on the incumbent.** `Benchmark.md`'s Silesia ratio must not move by more
than measurement noise, and the Meta-v3 expansion on the 60-file corpus must fall from
~6.9x to the range implied by the reclaim (i.e. the 193 pages must actually come back).

---

## 6. What I shipped

One commit, `01ca8f0`, touching **`tools/invf-sweep.c` only** (+16 −6):

> `docs: correct three invf-sweep comments that claim the fold always runs`

Three comments asserted the Meta-v3 fold is "unconditional inside the run" / "always runs as
part of a normal sweep". That is false for the offline path and the false claim is what hid
the leak. Per AGENTS.md §1.7 a doc that contradicts the code is a bug in the doc, so this is
the correct direction to fix it. I found a **third** instance of the same claim (`:1528`, the
`--compact` help text) that the handover did not list; all three are fixed.

The commit body records the measured leak and states explicitly that the fix is a root stack
plus a reader-pin protocol and is **not** in this commit.

Left deliberately uncommitted in the worktree: `Makefile` (+ the census/fold targets),
`src/core/vol_btree.c` (`btree_mark_debug`, 14 lines, read-only), and `.wp116/`.

---

## 7. What I did not determine

1. **Why the redundant fold hooks return −1.** `vol_v3_fold_request` calls `fold_reclaim_hook`
   up to **three** times with identical arguments (`vol_fold.c:440`, `:482`, and
   `vol_reclaim_schedule` at `:507`) when the fold actually fires. I confirmed by direct
   experiment that a second pass over an already-reclaimed root returns **−1** and frees
   nothing — so there is **no double free**, which refutes the hazard I was looking for. The
   counter stays exact (`free_blocks == vol_count_free` throughout). But *why* it fails is
   unresolved: `mbuf_read_ptr` (`vol_metabuf.c:122`) checks `checksum`/`gen`/CRC on a page
   whose content is intact, so it should have succeeded. The error is discarded by
   `(void)vol_reclaim_mark_and_free(...)`, so it is invisible in production. Worth a
   follow-up: a silent −1 in the reclaim path is exactly the shape of a bug that hides.
2. **Whether the §1.1 stale-root fallback is reachable in practice.** The hazard is real from
   the code; I did not construct the damage sequence (publish → reclaim → damage the new root
   page) to demonstrate a real silent namespace rollback. It should be built as a test.
3. **The Silesia compression ratio (2.1428x → 3.6999x).** Not reproduced — the sweep OOMs on
   this host and, under a memory cap, swept 2 of 825 files. The *leak* is confirmed at 9,108
   pages / 37.3 MB / 48.5% of stored; the *ratio* is not.
4. **The exact cause of the `invf-import`-time page placement in the shadow zone.** Base pages
   land around pba 72,336–91,182 while the metadata zone is 1..24,584 and fully marked
   allocated at mkfs, so `mb_alloc_meta_zone` (documented in AGENTS.md §2.7) always overflows.
   This interacts with the leak — the leak's pages are being taken from the *shadow* pool, not
   the metadata reserve — but I did not chase the interaction.
5. **Whether the reclaim collector's bitmap scan is fast enough at scale**, and whether it
   reproduces or aggravates the `invf-sweep` non-termination in `INCIDENTS.md` (which is
   itself still unlocalised). I did not go near the 46k-inode scale, per instructions.
6. **A prototype reclaim.** I deliberately did not write one. See §8.

---

## 8. Should this be prototyped at all?

**My recommendation: yes, but only §3.3(c) — the bitmap-scan orphan collector — and only
behind an env var, and not as a merge candidate.**

The reasoning is about blast radius, not difficulty. The dangerous part of this change is
not the traversal; it is that **freeing is irreversible and the existing safety protocol is a
no-op**. `g_readers_in_flight` is never incremented, so `vol_reclaim_drain` waits for nobody,
and a collector driven from `invf-sweep` — an *offline* tool whose volume is by definition
unmounted, but which today is the only thing anyone runs — would be the first code in the
tree to free base pages with **zero** reader protection, on a subsystem where a wrong free is
silent data loss.

So the ordering that I would insist on:

1. **First**, make the read path real: implement the hazard-pointer pin in `mbuf_read_ptr`
   and the mbuf cache read path, and make `vol_reclaim_drain`/`g_readers_in_flight` actually
   mean something — or delete them, because a protocol that looks implemented but is not is
   worse than none. Ship that on its own; it is safe and it is pure overhead until something
   reclaims.
2. **Then** build the §5F concurrency harness and run it on **unfixed** `main` as a control.
   A safety protocol with no test that fails without it is not a safety protocol.
3. **Then** land §3.3(c) behind `INVFS_RECLAIM_ORPHANS=1`, default off, logging every page
   it frees so the first runs are auditable rather than trusted.
4. **Only then** consider defaulting it on, and only then the root stack — at which point
   the stack is an optimisation, not a correctness requirement.

A previous agent declined to ship a fix for exactly this reason. I agree with the refusal
and I want to be precise about why it still holds: I did not find a shortcut past it. What I
did find is that the refusal was slightly too broad — **the documentation bug was shippable,
and I have shipped it**, and the bitmap-scan collector is a smaller and safer first step than
the root stack the handover proposed. But the core hazard is unchanged: until the read path
has real pins and a harness that proves they work, any reclaim fix is a guess with a
plausible story attached.

---

```
WP: wp/116-v3-reclaim-rootstack
Files changed:
  tools/invf-sweep.c        (COMMITTED 01ca8f0: three false "the fold always runs" comments corrected; +16 -6)
  src/core/vol_btree.c       (UNCOMMITTED instrumentation: btree_mark_debug, 14 lines, read-only)
  Makefile                   (UNCOMMITTED instrumentation: wp116_census / wp116_fold targets)
  .wp116/                    (UNCOMMITTED: wp116_census.c, wp116_fold.c, repro.sh, silesia.sh, scaling.sh)
Tests run:
  $ make -j8 all                       # clean
  $ make test                          # 0 failures (4467+4765+169+86+83+80+56+42+28+... checks)
  $ 60-file reproducer, 8 sweeps      # 198 base pages / 5 live / 193 unreachable; occupancy FLAT at 24,826
  $ forced vol_v3_fold on a copy      # 193 -> 557 unreachable (reproduces the handover exactly)
  $ invf-fsck -f                       # "pages walked: 5", reclaims nothing, still 193
  $ scaling: 10/20/40/60 files, 10x size  # linear in file count, IDENTICAL at 10x file size
  $ Silesia 12-file sweep              # 9,108 unreachable pages = 37,306,368 B = 48.5% of stored
                                        # (first attempt OOM-killed rc=137; retry swept only 2/825 files)
  $ triple-hook double-free probe      # second diff returns -1, frees nothing, counter exact -> NO double free
Result: PASS  (mechanism verified + reproduced; doc fix shipped; design delivered; NO reclaim fix shipped)
Remaining TODOs:
  - Build the hazard-pointer pin in the read path (mbuf_read_ptr + mbuf cache); make
    vol_reclaim_reader_snapshot/release real or delete them. Ships nothing on its own.
  - Build the §5F reader-vs-reclaim concurrency harness and run it on unfixed main as a CONTROL.
  - Land the bitmap-scan orphan collector (§3.3c) behind INVFS_RECLAIM_ORPHANS=1, default off.
  - Add the missing vol_v3_fold_request/vol_reclaim_schedule call to tools/invf-sweep.c — without
    a trigger, a correct root stack is inert. Bundling this with a reclaim fix is a separate WP.
  - Extend invf-btree_test.c:959,973 with a MULTI-generation reclaim case (G1..G5).
  - Localise why a redundant fold_reclaim_hook returns -1 and swallows it (vol_fold.c:440/482/507).
  - Demonstrate (or refute) the RT30 stale-root fallback rollback from §1.1 as a test.
  - Re-run Silesia on a host with memory to confirm/refute the 2.1428x -> 3.6999x ratio claim.
  - Update impl_docs/AUDIT.md and INCIDENTS.md status fields (NOT done — deliberately, so as not
    to pre-announce an unfixed defect; the new dirent publishers at vol_btree.c:3502/:3529 should
    be added to the finding).
```
