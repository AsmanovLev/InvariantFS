# WP22b flakey tier — findings

Found by `tools/test-flakey.sh` (dm-flakey over loop, volume directly on the
raw dm device) on the engine as built 2026-08-31 (HEAD `a07c4bfd` + the
in-flight WP22a working-tree edits). Both reproduce against the preserved
images under `artifacts/`.

## F1 — silent loss of fsync-acknowledged files after an error window
(leg 2, reproduced 3/3 with the full leg; the standalone
`repro-inode-hole.sh` is the same shape but timing-luckier, YMMV)

Repro: `FLAKEY_ONLY=2 bash tools/test-flakey.sh` (fails at the storm-file
check; ~2.5 min).

Mechanism (proven against `artifacts/leg2-error-storm-20260831-055251/backing.img`
with a raw inode-area walk):

1. During the 6 s full-error window, commits fail loudly (EIO to the
   writers — good), but an inode-area append is a *buffered* pwrite: it
   lands in the bdev page cache and `vol_write.c:vol_write_commit`
   advances `inode_area_pos` unconditionally; the failure only surfaces at
   the next `fsync` (`vol_sync`), by which time the cursor has moved on.
2. The storm-era dirty pages die in writeback → a **zero hole** in the
   append-only inode area on the device (observed: stop at byte 34471759,
   then 618 *valid, CRC-correct* records beyond it — storm-240..363).
3. Post-storm commits (fsync rc=0, healthy device) append *past the hole*.
4. Next open: `vol_open`'s scan stops at the zero gap (the valid
   end-of-log marker) → those records are unreachable. fsck sees their
   data blocks as orphans (72,896), frees them with `-f`, reports OK.
   verify --deep is blind (the files are not live). 124 fsync-acked files
   silently absent after a CLEAN unmount.

Fix direction (engine lane): a failed flush/commit must not leave the
append cursors past unpersisted records — e.g. latch the volume
error/dirty and re-anchor the tail from the device before any further
append (or treat flush failure as mount-fatal). No silent continuation.

## F2 — durable file left present-but-unreadable; fsck/verify disagree
(leg 5 soak, 1/1 soak run so far)

Repro: `FLAKEY_ONLY=5 FLAKEY_SOAK_S=75 bash tools/test-flakey.sh` with
seed 20260831 (op sequence is seeded; timing jitter exists), artifacts in
`artifacts/leg5-soak-20260831-060911/` (image + oplog.txt + model.json).

Sequence (from oplog): `rename t.tar -> r03.bin` acked during a
drop_writes window; a `sweep --realize` and a `sweep` (checkpoint #2)
under drop windows; at the gate, `invf-rollback`. Result:

- `invf-ls`: r03.bin present (inode 5, 8396800 bytes);
- `invf-cat r03.bin`: **fails loudly** ("L2P miss: inode 5 seg 0") — good;
- `invf-verify --deep`: `CORRUPT: r03.bin` — honest;
- `invf-fsck`: **OK, l2p misses: 0** — the structural checker cannot see
  the damage the read path hits, and `--repair` territory is never
  reached (H6's gate only engages on a *counted* l2p_miss).

The defect vs the crash contract: drops may only lose *new* writes.
t.tar's content and mappings were durable since the pre-chaos import;
the half-landed rename + torn journal/L2P state destroyed a committed
mapping of a durable file. A legal outcome would have been: rename lands
fully (r03.bin readable) or vanishes (t.tar readable). Present +
permanently unreadable + fsck-blind is neither.

## F3 — drop window silently un-maps fsync-acknowledged files
(leg 5 soak; reproduces with `FLAKEY_ONLY=5 FLAKEY_SEED=20260831 bash
tools/test-flakey.sh`, soak seed = SEED+500)

Symptom at the gate: fsck rc=3 with
`l2p_miss: s20.bin (inode 486): 13 segment(s) without an L2P mapping` —
a LIVE record whose segments the journal no longer maps
(present-but-unreadable frankenstate).

Mechanism (verified against the code): `vol_flush` rewrote the journal as
a suffix from the `l2p_dirty` watermark plus a terminator, and every
delete/heat touch dragged `l2p_dirty` backwards (`l2p_remove` compacts the
in-memory table), so a later, UNRELATED flush re-wrote journal positions
that were already durable. dm-flakey's drop_writes makes an arbitrary
subset of in-flight writes vanish — including, here, pages of OLD
fsync-acknowledged mappings being rewritten for unrelated reasons. The
durability axiom under silent drops is "a barrier completed OUTSIDE the
loss window covers everything submitted before it"; a rewrite submitted
inside a window cannot be repaired by any later fsync, because the
watermark believes it persisted. The aggravating chain: a sweep armed
CKP0 during a drop window, and a later clean no-op sweep auto-realized
the checkpoint BEFORE integrity was confirmed (realize-then-arm), so the
safety net was gone by the time the damage surfaced.

Fix (WP22d): the journal is append-only within a slot — nothing durable
is ever rewritten. Deletes append UNMAP ops, remaps append new MAP ops
(replay is newest-wins), and the WP19 heat pad rides the same appends
(read touches queue one refresh entry per pair per flush at most; a bulk
decay compacts). When the log fills (or fsck rebuilds the table, or a
legacy volume migrates), the whole table is imaged into the INACTIVE of
two journal slots with a JRN0 header (seq, image bytes, whole-image CRC)
→ blkio_flush → the superblock selector (sb.pad2) flips → blkio_flush.
Replay validates both slots and picks the highest-sequence CRC-valid one,
so a torn compaction always leaves the old slot authoritative. Every
entry crc in a slot is CHAINED from the previous one, so a dropped write
mid-log stops the replay exactly at the hole (no phantom stale tail).
Recovery folds to a consistent cut at open: a record whose segments lack
mappings is hidden — the name falls back to its newest fully-mapped
version (the inode area is append-only, older versions survive), or is
absent; fsck -f quarantines the torn versions with position-kill
tombstones so the final fsck is OK with the losses explicitly listed.
The sweep realizes the previous checkpoint only AFTER arming the new one
(realize-after-arm). New legs: test-flushfail.sh F (legacy→slot
migration), G (mid-compaction kill at image/barrier/flip via
INVFS_COMPACT_ABORT_AT), and test-flakey.sh leg 6 (compaction under
drop windows).

### F3 follow-ons found while greening the WP22d soak (all fixed)

The append-only/double-slot journal removed F3's map loss. The same soak
then surfaced a ladder of adjacent drop-window defects, each fixed and
locked in by a leg:

- **Legacy migration targeted slot 0.** The flat log begins at the journal
  base = slot 0's header block, so imaging slot 0 destroyed the fallback
  it was meant to survive: a torn migration left neither a valid slot nor
  a replayable log. Migration now images into slot 1, and replay, seeing
  pad2==LEGACY with no CRC-valid slot, falls back to the flat log.
  (test-flushfail.sh leg G2, incl. the "torndrop" stage that corrupts the
  slot-1 image bytes deterministically.)
- **fsck -f could not quarantine on a dirty volume.** The quarantine's
  vol_mark_dirty refuses needs_recovery, which is exactly the state being
  repaired (fsck -f IS the recovery) — the scan failed mid-quarantine
  (soak: "scan failed", ladder dead-ended). The fix marks DIRTY directly
  and clears needs_recovery after a successful rebuild (vol_fsck.c).
- **Torn journal staging of a checkpoint.** Under drop windows the WP21
  stage's read-back verify is defeated by the bdev page cache (the bytes
  are read from cache, not the device), so CKP0 can name a stage that
  never landed; the rollback then rightly refuses (rc=3, volume
  untouched). Both recovery ladders (test-flakey.sh recover(), soak.py
  gate) now accept the post-sweep state via --realize instead of
  dead-ending.
- **Bitmap/journal divergence.** A flush writes the dirty bitmap range
  and the journal append as separate pages under one barrier; a window
  can drop the bitmap pages and keep the journal's → the disk bitmap
  calls a MAPPED block free → the next allocation hands it out from under
  its live file (observed: a live record's segment range held a foreign
  valid frame → verify CORRUPT, or worse). vol_open now forces every
  journal-mapped block USED in the runtime bitmap (never clears; a false
  positive is a leak fsck reclaims) and marks the range dirty so the
  first flush converges it on disk. (test-flushfail.sh leg I.)
- **Content-level cut.** A segment's data can be dropped after its map
  is durable (maps are re-durabilized by every compaction; data is
  written once). The map-level cut is blind to it; the segment CRC is
  not. fsck -f with INVFS_FSCK_CONTENT=1 reads every live segment
  (zone != TEXT) and quarantines records whose content fails (the name
  falls back or goes absent; losses listed loudly). The ladders engage it
  when verify --deep reports CORRUPT.
- **Quarantine convergence.** scanset_delt's torn-retire guard kept a
  fallback whose successor was broken even when the fallback ITSELF was
  broken (unreadable, useless) — a quarantined chain never converged
  (every fsck re-reported the same cut). The guard now only protects a
  healthy target. (volume.c)
- **rename-onto-a-container orphaned its members.** vol_rename replaced
  an existing destination with vol_delete_file only — no sibling cascade
  (vol_unlink has it) — stranding the container's "name!partN" records as
  parentless live records (the soak's GHOST). The victim path now
  cascades. (vol_dirs.c)
- **Soak model: container members are not ghosts.** The gate's GHOST
  check now treats "parent!member" names as engine-generated content of
  the (present) parent, never written directly. (soak.py)

## F5 — mkfs on a dirty device resurrects the previous volume (leg 6)

**Found by:** leg 6 (compact-flip), the first leg with a *pre-chaos* fsck
gate — the bug is pre-existing, latent since the device path exists.

**Symptom:** full-suite leg 6 fails at `fsck pre-compact` with the volume
full of leg 5's files (dozens of `l2p cut`/`l2p_miss`). The probe fsck
right after mkfs shows a fresh volume; after the first imports the old
volume's records come back to life.

**Root cause:** mkfs's device erase zeroed only the FIRST BLOCK of the
inode area (plus the 1 MB head and both journal slot headers). The record
scan stops at the first invalid record; fresh import records outgrow the
zeroed 4 KiB head within a few files, and everything past it is the
previous volume's still-CRC-valid records — adopted wholesale, with an
empty journal → every segment unmapped (l2p_miss flood). Image files never
hit this: CREATE_ALWAYS gives an all-zero area.

**Fix:** mkfs now zeroes the whole inode area on devices (chunked loop);
leg 6 gained a post-mkfs regression gate (`live files: 0`).

**Also fixed in this session:** `want_leg` learned comma lists
(`FLAKEY_ONLY=5,6`) and kept the empty-means-all default (a first version
of that change silently skipped every leg — a "1 s PASS" suite).

**Meta-lesson:** never edit a bash script while it runs (incremental
reads + byte-offset shift executed a `_leg` fragment mid-suite once);
edit between runs only.

## F6 -- read during error window returns wrong/short bytes as success
(leg 5 soak; reproduces with `FLAKEY_ONLY=5 FLAKEY_SEED=20260831 bash
tools/test-flakey.sh`, soak seed = SEED+500; confirmed 3/3 incl. one
quiet-box run. Artifacts: tools/flakey/artifacts/leg5-soak-20261007-175219
(first) and leg5-soak-20261007-210622 (with captured garbage bytes).)

**Symptom:** after a drop-mode sweep, a FUSE readback issued while the dm
device is in full-error mode returns 34,484 bytes of high-entropy content
for a 958,369-byte text file -- hash matches nothing ever written -- with
SUCCESS (no error). Same-run offline reads (before and after, via
invf-cat) return the correct bytes: on-disk state is and was correct.
Distinct from F4 (zero rollbacks in the run; nothing resurrected) and
from F2 (file stays readable).

**Established by elimination:**
- Not on-disk corruption: offline reads correct before, during (other
  files), and after. Not harness history: deterministic model, one write.
- Not whole-file aliasing: the garbage hash occurs NOWHERE else in the
  run (48 unique content hashes checked); not a torn mix either (zero
  common prefix bytes with the correct content).
- Not zstd-framed data (rejected by unzstd); LZ4-block-or-foreign-segment
  shaped, unproven which.
- The read ran in error mode (oplog t=+22.3, chaos still error): segment
  fetches were failing. A sibling readback in the same mode correctly
  ERRORED (rand.bin EIO). So partial success is per-read, not per-mode.

**Candidate loci (unproven):**
- (a) read_range's segment loop (src/core/vol_read.c) skips non-covering
  entries via break/continue and returns short `got` with SUCCESS -- the
  one shape in the read path that yields short-without-error. Fits the
  SHORT half, not the foreign-from-byte-0 half.
- (b) Stale in-memory recipe/overlay view in the daemon (drop-sweep wrote
  recipe updates that partially landed), combined with (a).
- (c) ARC serving a foreign/decoded-mismatched window for the head.
- Ruled out: RACE-F4 resurrection (no rollback), harness model bug,
  contention (quiet-box repro), geometry (100G sparse control passes).

**Fix direction:** regardless of which locus produced THESE bytes, a read
that cannot assemble the requested window must fail EIO, never return
wrong/short bytes as success. Audit read_range + FUSE handler + ARC get
paths for partial-success returns; regression test: FUSE read of a live
file with dm in error mode must EIO (never short-success). The captured
garbage blob (garbage-s30.bin.bin in the 210622 artifacts) is the oracle
for dissecting head-origin once the return-code contract holds.

**Status 2026-10-07:** two halves landed. (1) read_range returns -1 on
coverage shortfall (writer contract: no unmapped LBA ever exists, so a
hole is corruption). (2) commit_wctx failure now table_remove_name
instead of table_sync_one (complete-or-absent via absent, mirroring
invf_fsync's barrier path) -- proven on a deterministic 30s repro
(O_TRUNC write fails under dm-error, readback EIOs instead of serving
the failed bytes), guarded by new leg 9 (trunc-abort readability).
The F4 seed still fails, LOUDLY at readback instead of silently: the
foreign-head origin (stale mapping vs ARC vs torn recipe) is open.

**Status 2026-10-07 late:** same seed (20261331) re-run WITH the F6 fix
(binary built 21:28 from fixed source, commit 2dde445) STILL fails --
but a different file and shape: s18.bin (inode 58, 22083 B) CORRUPT at
final verify (artifacts leg5-soak-20261007-214056). Timeline exonerates
F6: last write t=36.0 ok, readback ok t=60.5, then sweeps at t=68.0
(130 orphan pages) and t=70.1, corrupt at t=74.7 -- the file was
quiescent; a sweep damaged it. Recipe seg0 names pba 16460 (algo 5,
len 22083) but that block holds non-segment bytes (framing reads
csize=1290470656, payload looks like metadata records): stale-PBA /
use-after-free shape -- sweep freed or moved the segment while the
recipe still names it, and the block now serves metadata. Fuse.log
also shows transient segment/text-batch CRC mismatches for many other
inodes mid-run. Next: bitmap/alloc audit of pba 16460 (free vs live
reference) + which sweep stage last touched inode 58.

## F7 -- sweep starting in drop mode publishes without a landed window
(seed 20261331 traced with CORRUPT_DEBUG, artifacts
leg5-soak-20261007-215754, trace /home/user/flakey-work/cd-20261331.log)

**Symptom:** run dead-ends at round 4 (t=+8.2): fsck rc=3, four base
pages unreadable (121589, 121591, 118100, 121593) + torn root_slot[1]
(pba 118101), CANNOT REPAIR without --discard-reachable, and
`invf-rollback` says `no save point`. A full generation published with
no way back.

**Attribution (the trace earns its keep):** all five dead pages were
ALLOC'd as META (type=1) under tag `batches` (trace lines 1099-1114,
the sweep's batches stage) and never freed. The allocator is EXONERATED:
a full bitmap-model replay of the trace (every ALLOC/FREE from mkfs on)
shows zero double-allocs and zero free-of-free. The blocks were freed by
nobody and written once -- the writes just never landed: the whole sweep
ran inside a drop_writes window (`cli sweep (mode=drop)` at t=7.8) and
returned rc=0 `volume durable`.

**Mechanism:** prepare's `spt0_capture()` got rc=0 for a write the device
discarded, and `spt0_info()` -- which reads IN-MEMORY state
(`v->savepoint_live`, vol_spt0.c) -- confirmed it. The sweep proceeded
believing a rollback window existed. It did not (rollback: `no save
point`). Root publish landed, page writes did not: torn generation, no
fallback. The USR1/xattr path has the SAME shape (fail-closed only
covers explicit refusal, never a silently dropped capture), so all four
sweep triggers are fail-open under drop-at-capture.

**Fix direction:** verify-after-capture -- re-read SPT0 from the DEVICE
and compare base_root/delta_end against what was just captured; on
mismatch abandon the pass before writing anything (under drop, reads
return stale data, so a dropped capture is detectable; under error,
reads EIO, also detectable). Applies to offline prepare AND the FUSE
worker captures. Separate harness question: the ladder dead-ends on
DAMAGED without trying invf-rollback first -- for THIS incident there
was no window so rollback could not have helped, but a landed window
with a torn publish is exactly what rollback is for and the ladder
never pulls it.

**Status 2026-10-07:** diagnosed, not fixed. Note the manifestation
moves with timing: same seed gave F6-shape (s30), stale-PBA (s18), now
F7 (base pages) -- the seed is a bug FAMILY, not one bug.

**Status 2026-10-07 late:** FIXED (commit pending in this batch):
capture-verify-refuse -- `spt0_verify_live()` re-reads SPT0 from the
device and requires base_root+delta_end+generation-nonce identity, with
the re-read through a fresh O_DIRECT open (page cache would return our
own bytes; BLKFLSBUF needs privilege). All sweep triggers abandon the
pass on mismatch. Proven on a deterministic always-drop dm-flakey
repro: pre-fix the sweep returned rc=0 `volume durable`; post-fix it
refuses rc=1 `capture did not land`, volume byte-untouched, and a
healthy-mode sweep still completes. Chaos-soak validation pending.

## F8 -- generation walk replays the wrong side of the pinned head
(seed 20261331, artifacts leg5-soak-20261007-222059 and -222550)

**Symptom (the s18/r00 shape):** a quiescent small file is CORRUPT at
final verify (segment CRC mismatch, recipe still naming the blocks)
after a HEALTHY-device sweep, while every op readback stayed green.
Same PBA region dies every run (16460, then 16453 -- deterministic
allocator, deterministic victim).

**Attribution (upgraded CD trace: ms timestamps, tids, COMMIT/RETIRE):**
the victim's 4 blocks were ALLOC'd once by a file COMMIT, never freed
-- then a sweep's PREPARE stage freed them (spn_reclaim, tag `prepare`)
with no RETIRE anywhere, and daemon writes reallocated the range with
foreign bytes. Use-after-free with both ends named in the trace.
Allocator exonerated again (bitmap replay: zero violations).

**Mechanism:** `gen_prefix_replay`'s loop skipped `d > head_depth` --
exactly backwards. Segments are collected newest-first, so that replays
the captured head plus everything NEWER and skips everything OLDER,
while the comment above it describes the opposite contract (and the
head_bump cut only makes sense that way). With captured==current head
-- the normal offline-sweep case -- the replay covered ONLY the head
segment: any row whose latest record sat in a non-head segment and was
not yet folded to base was never visited, zero indeterminacy counted,
and the reclaim freed its blocks. One flipped comparison (`>` to `<`).
Two adjacent silent gaps failed closed at the same time: a torn segment
header broke collection mutely, and a torn record tail broke parsing
mutely (`rc <= 0` conflated clean end with torn). The walk now refuses
on any unreadable delta data; the capture (hence the sweep) refuses
with it. The restore path shares the walk and inherits the refusal.

**Regression:** `spn_walk_tear_test` (in-shard): buried victim +
sabotaged mid-chain header. Control must visit the victim (fails
pre-fix: 0 rows visited); torn chain must refuse (fails pre-fix:
silent rc=0). 12/12 post-fix; both legs verified red via stash.

**Status 2026-10-07:** fixed, unit-proven, soak validation pending.
Note: ARC/overlay masking is why op readbacks stay green while the
platter is bad (verify reads offline/cold) -- that observation stands
but was not the mechanism here.

## Tier status

legs 1 (baseline), 3 (torn sweep), 4 (crash mid-seal) PASS;
leg 2 FAILS on F1 (by design — do not weaken the assertions);
leg 5 FAILS on F2. Suite runtime ≈ 6–8 min at defaults (FLAKEY_SOAK_S=210).

**WP22d update (2026-09-01/02):** F1/F2 fixed by WP22c, F3 by WP22d (+
the follow-ons above). With the WP22d engine, leg 5's original F3
symptom (live record, unmapped segments, fsck rc=3 l2p_miss) is gone —
the consistent cut hides/quarantines torn records and the ladder
converges to fsck OK. Pre-WP22d binaries fail the same seed at round ~32
with the classic F3 l2p_miss; the WP22d engine runs the full 210 s soak
(220+ rounds, 57 gates). Legs 1–6 PASS at seeds 20260831, 1, 42.
