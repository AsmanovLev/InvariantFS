# WP46 — WP45 suite expectation fixes

Branch: `main` (test-only, no engine change)

## Scope (ONLY these)
- `tools/test-heat-mapper.sh` — `call_pump()` batching bug.
- `tools/test-stats-mapper.sh` — population / invf-ls / unclaimed
  expectations vs the engine's actual (correct) semantics.

## Why
After WP41-45 merged, three WP45 assertions failed against a correct
engine. Root causes were all in the tests:

1. **heat 10/100.** `call_pump()` ran an inner `while IFS= read -r _name;
   ...; done < "$TOUCHED"` inside an outer 100-iteration loop. The inner
   redirection REOPENED `$TOUCHED` on every batch, so the same first 10
   names were pumped 10 times and the remaining 90 never were. Exactly
   10/100 names carried heat — a test artifact, not a mapper bug.

2. **stats population 30001 vs canonical 30000.** `invf-stats`
   "regular files" counts the 0x01 internal owner record(s) (`\x01tier0`
   and, when mirrored, `\x01rawm`) that every two-device volume carries.
   The test compared against the source tree only.

3. **invf-ls 30160 vs 30000.** `invf-ls` lists every live record:
   30000 files + 120 directory anchors + 40 symlinks = 30160. The test
   expected regular files only.

4. **unclaimed 128.6 MiB on a fresh volume.** On a v0.3.0+ mapper volume
   the dynamic metadata extents are allocated but referenced by MET0/mapper
   rather than AST segments, so they legitimately appear as "unclaimed"
   (fsck only reclaims real orphans). The fixed 16 MiB cap was wrong; the
   assertion now bounds unclaimed below 50% of used (a true leak is ~100%).

## Design
- `call_pump()`: accumulate all touched names into the function's
  positional parameters in one pass, then a single
  `invf-l2ptest pump "$IMG" "$@"` invocation.
- stats: population accepted in `[CANON_FILES, CANON_FILES+2]`;
  `invf-ls` expected in `[files+dirs+links, +2]`; unclaimed asserted
  `< 50% of used` MiB (parsed from the stats output).

## Validation
```
$ sh -n tools/test-heat-mapper.sh && bash -n tools/test-*.sh   # SYNTAX-OK
$ INVFS_E2E_AGENT=wp46 bash tools/run-e2e.sh tools/test-heat-mapper.sh
   -> all 100 touched names rheat>0; untouched controls rheat=0 (no SKIP)
$ INVFS_E2E_AGENT=wp46 bash tools/run-e2e.sh tools/test-stats-mapper.sh
   -> 6 passed, 0 failed (no SKIP)
$ INVFS_E2E_AGENT=wp46 bash tools/run-e2e.sh tools/test-sweep-mapper.sh
   -> 6 passed, 0 failed
```

## Out of scope
- Any src/ change. The engine behavior is correct and verified on the
  15 GiB stage3 volume (`invf-stats`: 54089 files / 3104 dirs / 8988
  symlinks, 1084.0 MiB logical, RAW 1.45x).
- Remaining mapper-unaware walkers: `vol_textzone.c` tz_owner_write,
  `vol_dirs.c` rename/sibling collectors, `vol_records.c`
  vol_delete_siblings, `vol_read.c` fallback scan, `vol_tier.c` /
  `vol_ast.c` hint-validation — follow-up WP47+.

## Coordination notes
- Test-only; merged directly to main (docs/test exception per AGENTS §2).
- e2e through the standard locked runner with `INVFS_E2E_AGENT=wp46`.
