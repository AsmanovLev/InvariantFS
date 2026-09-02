# RACE-F4 — shared notes for the F4 debugging race

Bug: dm-flakey soak THIRD STATE — after a gate-time rollback, s16.bin reads bytes
never written under that name (silent content divergence).

Artifacts: tools/flakey/artifacts/leg5-soak-20260902-085413/
(backing.img = torn volume at failure, oplog.txt, model.json, all leg logs).
Repro: `FLAKEY_ONLY=5 FLAKEY_SEED=20260831 bash tools/test-flakey.sh`
(soak seed = SEED+500 = 20261331; deterministic op sequence, chaos timing drifts).

## Already-established facts (do not re-derive)

1. WP22d's own code comment (volume_internal.h, the ck/retmap block) names the
   mechanism: "a post-checkpoint rewrite/delete that freed a pre-checkpoint block
   for real would let a rollback resurrect a record whose pba was reallocated —
   F4, the leg-5 soak's THIRD STATE".
2. Retention is a VOLUME state (keys on ck_present), BUT: processes that did not
   arm the checkpoint run with retmap=NULL → their frees "stay allocated, nothing
   is registered" (degraded mode) → the on-disk \x01reten registry is INCOMPLETE.
   The design comment claims the unregistered ranges are reclaimed by fsck -f
   after resolution — safe for space, but does the registry incompleteness break
   rollback accounting?
3. Failing sequence (oplog r196): gate → fsck sees live checkpoint → rollback
   rc=2 refused (seal live) → `sweep --free-redundant` (87 parity blocks freed
   while CKP0 live) → rollback#2 rc=0 (42 post-sweep blocks reclaimed) →
   fsck -f OK → content check: s16.bin = bytes never written.
4. s16.bin last ops: t=+133.6 write 13590B ok (up), t=+143.0 write 719056B ok
   (drop — legal to lose), then gate. Post-rollback content must equal an older
   history entry; it equals NONE.
5. fsck orphan accounting fixed: CKP0 staging run now counts as owned
   (vol_fsck.c). test-heat.sh does realize+fsck -f housekeeping before its final
   report (degraded-retention leftovers are reclaimed by fsck -f by design).

## Hypotheses register (claim by editing before you dig; update when killed/confirmed)

- R1: seal --free-redundant frees parity blocks bypassing retention while CKP0
  live → those blocks get reused → rollback/parity interplay corrupts. UNCLAIMED
- R2: cross-process retention gap (degraded NULL-retmap): a non-armer process
  freed a pre-checkpoint block, registry incomplete, and realize/rollback then
  frees or reclaims wrongly. UNCLAIMED
- R3: rollback + WP22d double-slot journal: prefix restored into the wrong slot /
  selector not flipped → replay sees stale maps pointing at reused blocks.
  UNCLAIMED
- R4: consistent-cut recovery falls back to an older record version without
  verifying THAT version's maps (needs recursion). UNCLAIMED
- R5: bitmap rebuild after disarm vs still-referenced held blocks. UNCLAIMED
- R6: free-for-all (any other path). UNCLAIMED

## Rules

- Work in YOUR worktree/branch only (assigned at dispatch). Analysis first
  (read-only); build+test only when you have a candidate fix.
- Append findings here, prefixed with your racer id (R1..R6) + timestamp:
  `flock /tmp/invfs-e2e.lock -c 'cat >> /home/user/InvariantFS/tools/flakey/RACE-F4.md' <<EOF ... EOF`
- e2e/flakey runs: ONLY via `bash tools/run-e2e.sh ...` / the flakey script
  (already lock-aware). Never raw `make e2e` in parallel.
- Winner = root cause + fix + repro seed green + full flakey green + e2e green.
  Losers' branches are discarded; findings stay in this file.

## Findings log (append-only)

## 2026-09-03 — RESOLUTION (maintainer, no race needed)

F4 did NOT reproduce after WP22d: leg5 seed 20260831 (the original failing
seed) PASS, then full flakey suite PASS. The WP22d retention hardening
("nothing is freed while a checkpoint is live", vol_free_blocks gate) is
the mechanism-level fix for the F4 hypothesis chain (post-checkpoint free
of a pre-checkpoint block -> reuse -> rollback resurrects a stale mapping).

While hunting, a DIFFERENT latent bug was found and fixed instead — F5:
mkfs on a dirty device zeroed only the first 4 KiB of the inode area, so
imports past one block resurrected the previous volume's CRC-valid records
(leg6 pre-compact gate caught it). mkfs now zeroes the whole inode area on
devices; leg6 has a post-mkfs regression gate. See FINDINGS.md F5.

The race is stood down. If F4-class symptoms (THIRD STATE / l2p_miss after
rollback under chaos) reappear, revive the hypotheses R1-R6 above.
