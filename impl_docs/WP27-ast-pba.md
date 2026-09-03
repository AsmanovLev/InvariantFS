# WP27 — AST v2: physical addresses in recipes (the L2P redesign)

Status: design (approved 2026-09-04: Dev-stage format freedom). Target: INVFS_VERSION=2.

## Why (the review's diagnosis, confirmed in code)

The L2P mapper is a journaled table (inode,lba)→pba that doubles as WAL and
the ONLY home of physical addresses:

1. O(n) lookup on the hot read path (vol_lookup_entry scans newest-wins).
2. Hard capacity ceiling independent of disk size: one 16 MiB journal slot ≈
   465,920 mappings.
3. Compaction = rewriting the whole table (slot flip).
4. Reads generate journal traffic (heat refresh entries).
5. SPoF: AST recipes carry only logical block_id; lose both journal slots and
   the volume is dead although every record and data block is intact.
   (WP22d's consistent-cut mitigates detection, not recoverability.)

The indirection exists for cheap re-keying (dedupe/tier remap pbas without
touching records). The position-kill tombstone discipline (v2) already makes
record rewrites cheap and precise — so the indirection can be retired.

## Design

### AST block entry 24B → 32B

```
u64 file_offset | u64 length | u32 zone:2 algo:6 block_id:24 | u32 block_offset | u64 pba
```

- `pba` = physical block address. block_id REMAINS (semantic slot id; still the
  L2P key for owner-referenced shapes: batch members, dedup dups, tier copies).
- Readers: name-index → record → entries → direct pba reads. **L2P is not
  consulted on the read path at all.**
- Writers: allocation order becomes durable chain — data blocks → bitmap →
  owner journal (if any) → record with pbas. The record append is the commit
  point, exactly like today.

### What remains in the journal (owner-WAL)

Owner-scoped maps only: `\x01tzb` batches, `\x01parity*` seal, `\x01reten`
retention, `\x01rawm`/`\x01tier` sidecars, retention/free-ordering WAL.
Thousands of entries, not hundreds of thousands: the slot ceiling and the
compaction avalanche disappear. WP22d's append-only+flip machinery stays —
it just guards a now-tiny structure.

### Derived bitmap

fsck rebuilds the bitmap from records + owner maps (complete, not partial).
Allocation still needs the bitmap in RAM; on-disk it is a cache (rebuildable).

### Heat

WP19 counters move out of journal pads into the INO2 ext (sweep rewrites
records anyway) or the small owner table for batch members. Read path never
touches the journal. (Stage-B of the l2pq wave already removed per-read
journal writes; WP27 removes the pad from MAP entries entirely.)

### Remap operations (dedupe/tier promotion/batch GC)

Record rewrite + position-kill (the house pattern). Dedupe at sweep time
rewrites the file's record with the shared pba. No L2P re-key. The WP16f
PB7-style guard (retire checks other live references) stays and gets simpler:
references are visible in records directly.

### Format/migration

- INVFS_VERSION=2; superblock vol_flags gains VOLF_ASTV2; mkfs writes it.
- Old volumes: offline converter `invf-migrate-v2` (reads v1 journal+records,
  writes v2 records with resolved pbas). No dual readers in the hot path.
- The 4 GiB/64k-segment caps ride along with WP22a v2 headers.

## Interaction audit (the dangerous dozen)

- resize: data pbas stable (grow extends tail; shrink refuses live tails) — ok.
- seal: parity stripes over shadow pbas — orthogonal; recovery rewrites data
  blocks in place (pba stable) — ok.
- rollback (WP21): truncates inode area to CKP0; records carry pbas — bitmap
  rebuild must ALSO rebuild from rolled-back state (derived bitmap helps).
- retention (`\x01reten`): owner-L2P entry, unaffected.
- containerpacks: member siblings are ordinary records with pba entries — the
  !mbrt/!mbrmap machinery is untouched; the owner-map class stays for tz.
- tier (WP25): acceleration copies = L2P-visible sidecars today; under WP27 a
  copy is either an owner-L2P entry or an extra AST entry marked "cache"
  (decide in implementation; prefer owner-L2P to keep recipes canonical).
- sweep/heat promotion: record rewrites as today.
- multi-device: pba is global-concatenated (WP25) — unchanged meaning.

## Risks

- Record size growth (+8B/segment): worst-case record bloat ~33% (note
  INVFS_MAX_REC_LEN bump).
- Write-path ordering becomes load-bearing for durability (bitmap before
  record); vol_sync discipline must be re-audited.
- Dedupe-heavy workloads churn records instead of journal keys.

## Test strategy

Format converter + fixture conversions (old e2e images), full e2e on v2
volumes, flakey legs incl. torn compaction, lookup-parity stress vs v1
semantics, bench before/after (read latency + mount time O(N)→O(records)).
