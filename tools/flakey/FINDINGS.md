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

## Tier status

legs 1 (baseline), 3 (torn sweep), 4 (crash mid-seal) PASS;
leg 2 FAILS on F1 (by design — do not weaken the assertions);
leg 5 FAILS on F2. Suite runtime ≈ 6–8 min at defaults (FLAKEY_SOAK_S=210).
