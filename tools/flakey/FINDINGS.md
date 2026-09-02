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
