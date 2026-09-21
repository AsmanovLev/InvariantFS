# WP72 (DRAFT, for consultation) — Shadow→B-tree, RAW→WAL-LST: design options, evidence, and a freeze-safe migration order

Status: **consultation draft** — options with pros/cons, no code committed for
the reorganization itself. Written on the `fix/mapper-consistency` branch after
the WP71a–j consistency wave (10 commits) turned the engine-level e2e green and
produced the measurements quoted below. Author-facing questions are in §7.

Scope note: this document deliberately does NOT pick one design. The format is
not frozen, the v3-record migration is in flight on the author's side, and a
btree that lands before that migration is a btree rebuilt twice. §6 gives an
ordering that makes each step independently shippable.

---

## 1. As-is (verified against main @ a685d8d + this branch)

Metadata today is a three-layer sediment:

1. **Records layer (authoritative).** Append-only INOD records with
   variable-length names (WP58a "v3 layout"), AST recipe header v1/v2
   (WP22a), INO2 ext, trailing CRC32C; position-kill DELT tombstones.
   Records live in **MET0 mapper extents** (WP30) allocated from the shadow
   zone; `\x01…` owners get dedicated extents (WP52). The in-memory name/id
   hash indexes are **rebuilt by a full walk of every live extent at every
   open** (`vol_open_inner` scan). There is no on-disk index of any kind:
   name→record is O(all records), content→pba does not exist.
2. **Journal layer (auxiliary).** Two-slot chained L2P journal (WP22d),
   36B MAP/UNMAP entries; since WP27 file records self-describe their pbas,
   so the journal only carries **owner-WAL shapes** (batch/seal-parity/
   retention/sidecar maps) — plus the 24B metadata-extent WAL records
   (WP30, type 0x10–0x14) that `jrn_append_pending` interleaves into the
   same stream. **The meta-WAL has no reader** (the mapper table is
   persisted wholesale); WP71f had to teach every journal walker to step
   over it because replay/rollback/resize all mis-parsed the mixed stream.
3. **Consolidation layer (split-brain).** Legacy linear-area compaction
   (WP22e, CMP0 staging + crash protocol) refuses on mapper volumes (WP30
   gate; WP71i made the refusal loud); the mapper-era equivalent is the
   extent **merge machinery** (`vol_meta_extent_merge/shrink`,
   `vol_meta_merge_run` wired into the sweep) with thresholds
   (footprint >70%, extent_count >128) that small/medium volumes never hit.
   Both are mutually exclusive with a live CKP0 checkpoint (rollback pins
   absolute record positions), and **every sweep arms a checkpoint**, so in
   practice metadata churn is never reclaimed until an explicit
   realize/rollback window. Measured: 338 files + 3 sweeps → 682
   tombstones (~2× metadata amplification), inode area "100% used" on the
   active extent, compaction perpetually declined.

Data-side facts that constrain any btree:

- Files are **content + recipe**; segments are framed `[csize][crc32c]`,
  content-addressed only at sweep-time dedupe (BLAKE3 over stored bytes,
  **full re-hash of every live segment on every sweep** — O(volume) per
  pass; measured 706 segments hashed for a 20 MiB dedupe win).
- The AST recipe of a 1 TB file is a ~384 MB **single record** (doc/02
  admits it). Any random write to a large file rewrites its whole record
  (write = implicit downgrade to RAW, then re-sweep re-batches).
- Seal parity stripes are pba-range keyed over the shadow zone; WP71f
  excluded mapper extents from membership (they drift on every append).

## 2. What "shadow→btree" could mean (three different trees!)

The phrase covers three distinct indexes with very different risk profiles.
They are separable; nothing forces doing all three.

### Option A — Extent tree: (inode_id, file_offset) → (pba, len, algo, zone)

The btrfs-style answer: the AST leaves the record and becomes a persistent
COW b+tree keyed by (object, offset).

**For:**
- Kills the 384 MB-recipe-in-one-record worst case: range reads touch
  O(log n) nodes; ranged writes become O(log n) node updates instead of a
  whole-record rewrite + RAW downgrade + re-sweep.
- Prerequisite for real snapshots/clone/reflink (today: checkpoint +
  rollback only) and for incremental reseal keyed by dirty ranges.
- Makes partial-file dedupe (sub-segment sharing) representable.

**Against:**
- Largest surgery in the tree: read path (`vol_read*`), write path
  (`vol_write*` sessions), sweep re-batching, fsck rebuild, rollback,
  resize — every consumer of "the record is self-describing" changes
  contract. That self-describing property is exactly what makes their
  recovery model simple ("records are authoritative; fsck rebuilds
  everything else from them").
- Crash consistency needs COW + an atomic root commit. The house pattern
  exists (RSZ0/CKP0/CMP0: staging + one block-0 descriptor write), but a
  per-mutation COW tree is a new durability discipline, not a descriptor.
- Interacts with the in-flight v3 record work: doing A before v3 lands
  means rebuilding A's leaf format twice.

### Option B — Name index tree: name → (inode_id, record locator)

A persistent, sorted, on-disk index of the flat namespace; records stay
authoritative, the tree is a **derived, rebuildable cache**.

**For:**
- Directly kills the two O(n)-per-open costs (full extent walk to rebuild
  the hash; readdir/lookup are already RAM-fast after that). Rootfs-scale
  (millions of names) becomes viable — today open cost grows with total
  record churn, not live set.
- **Freeze-safe by construction**: because the tree is derived, it can
  ship behind a descriptor (BT0) with "absent = build at open from the
  record walk" semantics. Old volumes upgrade on first mount; a corrupt
  tree degrades to today's behavior, never to data loss. This matches the
  project's own recovery philosophy.
- Small surface: `vol_open` scan, `idx_*`, `vol_list_dir`, fsck (verify or
  rebuild), record-append paths (one extra transactional insert; torn
  insert = stale tree = rebuild).

**Against:**
- Two sources of truth (records + tree) need a sync discipline: insert
  must be ordered with the record append (record first, tree second; a
  tree entry without a record is ignorable, a record without a tree entry
  is found by fallback scan/rebuild). Position-kill semantics must be
  mirrored (delete = tree remove at fold time or tombstone-tolerant
  lookup).
- Does not help big-file recipes or dedupe.

### Option C — Content index: BLAKE3(segment) → pba (+ soft refcount)

The missing piece the docs promised in 2024 ("write-time CAS + Bloom",
retracted to offline re-hash in WP12h).

**For:**
- Dedupe becomes O(new segments) instead of O(volume) per sweep; enables
  online (write-time) dedupe for RAW landing, which is the single biggest
  sweep-cost lever measured today.
- Gives template-zone/dictionary work (doc/17) a place to hang keys.

**Against:**
- Refcount lifecycle: the project explicitly retracted journal refcounts
  (PB7) in favor of retire-time referenced-elsewhere bitmaps; a persistent
  index revives the problem in a new shape (index entry outliving/preceding
  its last reference). Mitigation: keep the index **advisory** — a hint
  cache; the retire-time bitmap guard remains the correctness mechanism
  (a stale hint costs a re-hash, never a double-free).
- Index size: ~40 B per unique segment (32 B hash prefix + pba + flags);
  1 M segments ≈ 40 MB — needs its own extents/tree, i.e. it presupposes
  the BT0 page machinery of Option B anyway.

### Recommendation

**B → C → (maybe) A, in that order, each behind its own descriptor.**
B and C are derived structures (rebuildable ⇒ shippable pre-freeze, safe
under crashes by fallback); A changes the source of truth and should wait
for the v3 freeze decision. If the author's "shadow→btree" specifically
means A, then at minimum land the BT0 page/COW machinery for B first — A
will reuse it node-for-node.

## 3. What "RAW→WAL LST with lazy pruning" could mean

Reading the current code as an LSM: memtable = the RAM name/id hash;
immutable sorted runs ≈ mapper extents (append-ordered, not sorted);
WAL = the record stream itself (CRC-framed) + the L2P journal for owner
shapes; **compaction = the missing piece** (linear CMP0 is dead on mapper;
merge exists but is threshold-gated and checkpoint-blocked).

### Fork 3.1 — Pruning unit

- **Extent-level GC** (repack live records of a mostly-dead extent into the
  active one, free the extent): matches `vol_meta_extent_merge` that
  already exists; bounded work per sweep; needs record-position
  translation for anyone holding positions (checkpoints, the RAM index,
  open file handles mid-sweep — today handled by "sweep drains only with
  open_handles==0").
- **Record-level rewrite** (rewrite each superseded record in place):
  append-only purity says no; skip.

### Fork 3.2 — Pruning vs the rollback checkpoint (THE blocker today)

Checkpoints pin **absolute** record positions; pruning moves records.
Current answer: refuse (compaction under CKP0 is declined loudly; and
every sweep arms a CKP0 → pruning never runs). Options:

1. **Refuse (status quo).** Simple, safe, and the reason metadata churn
   accumulates 2×. Only prunes in realize/rollback windows.
2. **Rebase table.** Pruning under a live CKP0 writes a position
   translation side-table (old_pos→new_pos for every moved live record)
   into the checkpoint staging; rollback applies it when restoring.
   Precedent exists: WP58b already rebases the mapper append cursor on
   restore. Cost: staging grows; rollback logic gains a translation step;
   crash-consistency of (pruned extent + translation + CKP0) needs the
   staging-first discipline they already use.
3. **Epoch/generation keys instead of positions.** Checkpoints stop
   pinning positions and pin (inode_id, generation) — records carry a
   monotonous version; rollback restores "state as of gen G" by folding
   the record stream with a cutoff. This is the clean LSM answer and pairs
   naturally with the name-btree (B), whose locators can be
   generation-stamped. Bigger change to CKP0 semantics; best done
   together with the v3 format work, not before.

Recommendation: **2 now (unblocks pruning with bounded risk), 3 at freeze.**

### Fork 3.3 — Lazy triggers

Watermarks, not schedules: per-extent dead-fraction > 30% (the doc/06
number) OR extent_count > N OR area_used/live > 2×; bounded work per sweep
(1–2 extents), never on the write path; `INVFS_NO_PRUNE=1` opt-out to
mirror `INVFS_NO_COMPACT`. The merge machinery's existing thresholds slot
into this unchanged — the work is the checkpoint interaction (3.2) and
making it actually fire (today's 70%/128-extent gates never trip on
real volumes: the leg-A churn volume sat at 6 extents/192 blocks with 97%
dead bytes and merge never ran).

### RAW zone itself

Keep RAW as the landing zone (LZ4/effort-ladder, WP23/WP26 are good and
measured). The "WAL" reading of RAW that adds value: give RAW segments
the same record-style CRC framing they already have, plus a per-segment
journal entry at write time (today the journal only carries owner shapes)
so that a kill -9 between segment write and record append is recoverable
by replay instead of relying on fsck's orphan reclaim. That is a small,
freeze-neutral addition (a new journal op type in the mixed stream the
WP71f walkers now handle).

## 4. Journal cleanups that fall out (cheap, do regardless)

- **Give the meta-WAL a reader or delete it.** It is write-only today
  (WP71f proved nothing consumes it; the mapper table is persisted
  wholesale at every alloc). Either implement mapper roll-forward from it
  (crash between table write and MET0 write — currently masked by
  write ordering) or stop appending it and reclaim the 24B records.
  Deleting is the LST-consistent move if pruning (3.2) lands.
- **Unify record framing**: journal entries (36B/24B mixed), record
  stream (INOD/DELT), and future btree pages should share one
  type-tagged, self-describing frame `[u8 type][u24 len][payload][crc32c]`
  so walkers never again depend on a fixed stride. This single change
  would have prevented the entire WP71f class of bugs (three independent
  walkers mis-striding a mixed stream).
- Dead constants: `INVFS_JRN_SWEEP/CHECKPOINT`, `sweep_cursor` — delete
  with the v3 nuke.

## 5. Evidence base (this branch, sandbox: 2 vCPU / 1 GB RAM / overlayfs)

A/B, identical 51 MiB corpus (250 text + 6 ELF + 8×1 MiB random + 3 dups),
main @ a685d8d vs fix/mapper-consistency:

| leg | main | fixed |
|---|---|---|
| resize grow 2G→3G (unsealed) | rc=0, **live 273→0, orphans 3351** (silent namespace loss) | rc=0, **live 272→272, orphans 0** |
| verify --deep after sweep --seal | **rc=1, parity 2 missing** (maps lost in mixed-stream replay) | **rc=0, 103 sealed, 0/0/0** |
| degraded 2-dev `invf-ls` | **0 file(s)** | **2 file(s)** + RAW-mirror read bit-exact |
| import ×3 | 149/160/173 ms | 155/151/151 ms (parity) |
| sweep (batching, PPMd/ZSTD) | 5.35 s | 5.31 s (parity) |
| extract-all 267 files, byte-identity | 19.3 s, 0 diffs | 18.8 s, 0 diffs |
| TEXT compression | 41.9→4.0 MiB (10.5×) | identical bytes |
| `invf-ls` (post-sweep volume) | 295 ms | **13 ms** (walker vs raw positional scan) |

Suites: 15 engine-level e2e suites green (rollback 13 legs, multidev 9,
resize incl. 3 crash legs, seal 19, fuzz 4×400 cases, dedupe, astv2
incl. 4 GiB streaming on 1 GB RAM, mapper-crash 24 checks ×2 runs,
textzone, binbatch, conbatch, flushfail, heat, stats/sweep-mapper,
compact M-legs, l2p L2Q); unit 4722/4722. Not validated here (sandbox):
FUSE legs, dm-flakey soak, systemd/initramfs boot, codec packs needing
external tools (cjxl/djxl/ffmpeg).

## 6. Freeze-safe ordering (the "total refactor" plan)

1. **Land WP71a–j** (this branch) → engine e2e green → tag. [done here]
2. **v3-only nuke** (author's in-flight work; coordinate — this branch
   deliberately did NOT touch AST versioning to avoid conflicting):
   single record/recipe version, delete v1/v2 readers, `invf-migrate-v2`,
   CVT0 arming, Windows trees (dokan/winfsp, `src-extracted/`),
   `build_linux.sh` (WP8: hardcodes `/mnt/d/VFS`, cannot link post-split),
   phantom `tools/busybox-src` gitlink (tests no longer need it after
   WP71c/c2), dead constants. Loud refusal to open pre-v3 volumes with a
   pointer to the last release that reads them.
3. **Frame unification** (§4) — still pre-freeze, format break allowed.
4. **BT0 name index** (Option B) behind a descriptor, derived/rebuildable.
5. **Pruning with checkpoint rebase** (§3.2 option 2) — reclaims the 2×
   churn; delete legacy CMP0 compaction in the same wave (dead on mapper
   since WP30).
6. **CT0 content index** (Option C, advisory) — dedupe goes incremental.
7. **Freeze v4**; only then consider Option A (extent tree) and
   generation-epoch checkpoints (§3.2 option 3).

## 7. Questions for the author (the consultation)

1. When you say "shadow на btree" — which tree do you mean: the
   extent/recipe tree (A), the name index (B), or the content index (C)?
   My recommendation is B→C→A; if your WIP already sketches A, I'd still
   land the BT0 page machinery under B first (A reuses it).
2. Pruning under a live checkpoint: rebase table (my §3.2-2, unblocks now)
   vs generation epochs (§3.2-3, cleaner but a CKP0 semantics change)?
3. The meta-WAL (write-only 24B records): roll-forward reader, or delete?
   (I'd delete unless your WIP plans mapper crash-recovery from it.)
4. Nuke sequencing: this branch avoided all format-version surgery to not
   collide with your v3 WIP. After your v3 lands, want me to do the
   mechanical nuke wave (Windows trees, migrate-v2, dead constants,
   phantom gitlink) as a follow-up branch?
5. Frame unification (§4) is a small format break that would have
   prevented the whole WP71f bug class — fold it into v3 while the format
   is open?

---

## 8. Elaborations (added after the author's consultation answers)

### 8.1 Option B on-disk shape (the recommended first tree)

B+tree over raw name bytes (the namespace is flat: name == path, so a
byte-ordered tree gives readdir as a leaf-range scan for free; a hashed
tree would need a side list for listing).

- Page = 4096 B: `[BT0P magic][u16 level][u16 count][crc32c][entries]`.
  Interior entry: `name-prefix + child pba`. Leaf entry:
  `name + inode_id + record locator (extent idx, offset) + gen` (gen =
  §8.2 hook, zero until epochs land).
- Root lives in a block-0 descriptor **BT0** at the first free 0x20
  boundary (0x3E0+ is PCK0-conflicted; pick past PCK0's max extent or
  extend the descriptor alley): `{magic, version, root_pba, epoch,
  name_count, crc32c}` -- the RDP0/CKP0 convention exactly.
- Update protocol = their house pattern generalized: allocate COW pages
  bottom-up, write them, barrier, then ONE block-0 write flips
  (root_pba, epoch). A crash before the flip leaves the old epoch fully
  valid; a torn new page is caught by its CRC and is unreachable from
  the old root. Freeing superseded pages: lazy, via the same retention
  discipline as sweep (never free a page an older epoch can reach while
  a checkpoint/time-travel view pins it).
- Transactional pairing with records: the record append is the commit
  point (it is CRC-framed and authoritative); the tree insert follows.
  Lookup: tree hit -> verify the record at the locator (CRC + name +
  live under the fold); verify fail or tree miss -> fall back to the
  extent walk (correct, slower). A stale tree entry is therefore a
  performance event, never a correctness one -- this is what makes B
  shippable pre-freeze.
- Rebuild: one full extent walk (today's open cost) -> write pages ->
  flip BT0. fsck -f gains "tree missing/divergent -> rebuild".

### 8.2 Rebase vs epochs, concretely

Checkpoint pins: `inode_area_pos` + `journal_pos` (absolute) + staged
journal prefix + retention registry. Pruning moves live records, so:

- **Rebase table (option 2):** the prune pass appends, into the
  checkpoint's staging, a translation run `(old_pos -> new_pos)` for
  every live record it moved, CRC-framed like the journal. Rollback
  loads the translation before decapitating: pinned `inode_area_pos`
  maps through it; per-record position-kills in the restored prefix map
  through it too (a kill naming a moved record must kill its NEW
  position). Chaining: a second prune under the same checkpoint appends
  a second run; rollback folds runs in order. Crash-safety: the
  translation run is written and barriered BEFORE the prune rewrites the
  destination extent (staging-first, the RSZ0 rule); a crash mid-prune
  leaves old records intact (append-only source) and the translation
  covers exactly what moved.
- **Epochs (option 3):** every record carries `gen` (u64, allocated
  from a volume counter persisted in MET0/superblock). CKP0 pins
  `gen_cutoff` + the journal prefix as today. Rollback = fold the
  record stream keeping only versions with gen <= cutoff (position
  truncation disappears; so does the rebase problem -- moving a record
  preserves its gen). Pruning, merges, even whole-extent rewrites become
  checkpoint-transparent. Cost: a v3 record field (the author's WIP owns
  it), rollback changes from O(1) truncate to a filtered fold (bounded
  by the name index once B exists), and time-travel views (`vol_open_at`)
  re-implement their scan stop as a gen filter.

They compose: ship rebase now (contained in vol_rollback + merge), fold
epochs into the v3/freeze wave and delete the rebase machinery there.

### 8.3 meta-WAL: delete, with the reasoning

The only crash window a mapper WAL could cover is (extent alloc ->
table entry -> MET0 cursor -> bitmap bits) landing partially. Audit of
what actually guards each step today: table+MET0 persist at alloc time
(meta_get_append_pos does both, synchronously); the bitmap lag is now
covered at open by WP71j (extent spans forced used from the table --
the mapper-crash suite kills mid-import at extent boundaries and checks
exactly this). So the WAL has no window left to protect, and no reader
ever shipped. Deleting it: jrn_push_meta_op/meta_journal_* become
no-ops (then removed), the journal stream returns to single-stride,
the WP71f steppers stay as defense-in-depth. If B/C trees later want a
page-commit WAL, design it for pages (§8.1's flip is already the commit
point; a WAL would only speed up multi-page split recovery).

### 8.4 Frame unification (fold into v3 -- recommended: yes)

`[u8 type][u24 payload_len LE][payload][u32 crc32c over type..payload]`
for BOTH append streams (journal entries and inode records). Types:
0x01 MAP, 0x02 UNMAP, 0x20 INOD, 0x21 DELT, 0x30+ reserved (btree page
commit markers, gen-counter bumps, future ops). Rules: a walker reads
the 4-byte head, knows the stride, verifies CRC, steps; the first bad
frame is the torn tail (today's replay convention, now stride-agnostic);
the journal's chain-CRC stays as a second layer over frames. Overhead:
+4 B per journal entry vs today (records keep their existing 4 B CRC;
rec_len folds into payload_len). This retires the entire WP71f bug
class (five walkers across replay/rollback/resize/fsck mis-striding a
mixed stream) and makes "add a new op" a type value instead of a
walker rewrite in N places. It is a format break -- which is exactly why
it belongs inside the author's v3 wave while the format is open, not
after the freeze.

### 8.5 Nuke wave status (Q4 answered "go")

Branch `nuke/legacy-wave-1`, commit WP73: invf-migrate-v2 + CVT0 (block-0
0x360 stays reserved-zero) + man/test/Makefile entries; v1 open-refusals
re-pointed at invfs <= v0.4.x; phantom tools/busybox-src gitlink removed
(suites fall back to repo sources since WP71c/c2); Windows doc remnants
(doc 09/14, dokan/winfsp/devtest doc files, DOCMAP lanes); dead journal
constants SWEEP/CHECKPOINT. NOT touched, by design, until the author's
v3 lands: v1/v2 AST recipe readers + test-astv2, INOD v1 shapes, the
superblock sweep_cursor field (layout churn for zero pre-freeze gain),
harmless _WIN32 ifdefs in shared code. Build green, unit 4722/4722,
fuzz/conbatch/resize/multidev re-run PASS.
