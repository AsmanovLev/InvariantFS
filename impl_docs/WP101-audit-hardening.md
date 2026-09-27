# WP101 — production-readiness audit hardening

Branch: `wp/101-audit-hardening`  ·  Worktree: `/tmp/invfs-wp101`
Agent: `wp101-audit-hardening`  ·  Base: `main` @ `be2b2bf`

## Scope

Nine findings raised by the orchestrator were re-derived from the tree
rather than accepted. Eight hold (two of them partly); one is refuted as
stated. This WP implements the fixes that are cheap and safe, and corrects
the documentation that asserts things the code does not do. Files:

- `src/core/vol_sweep.c` (PMP lane: decode-back + memcmp before commit)
- `src/core/vol_read.c` (APE lane: no more min()-clamp)
- `src/core/vol_ast.c` (bound `rec_len` on the inode-hint path)
- `src/codecs/codec.c` + `Makefile` (`INVFS_CODECPACKS_SYS=0` makes the
  unit suite hermetic)
- `src/cli/fuse_fs.c` (`fallocate`: honest EOPNOTSUPP for punch/zero)
- `tools/invf-rollback.c` (stop calling a clean v3 volume dirty)
- `tools/test-sweepboot.sh`, `tools/fuzz/bitflip.py` (assert the v3
  `--seal` refusal instead of a success that can no longer happen)
- `docs/SECURITY.md`, `docs/adr/ADR-007-plugin-architecture-ivpack.md`,
  `docs/ARCH-INSTALL.md` (doc claims that outran the code)

## Why

The product's central invariant is bit-exactness: what goes in comes back
byte-identical. Every lane that *proves* a transform by decoding it back
and comparing (LZ4, ZSTD, container packs, FLACR) is doing so because a
transform that is not proven must not be committed. Two lanes did not
prove, and one of them (APE) could hand a reader uninitialised heap as
file content. Separately, the shipped tests and docs had drifted past the
code, which is itself a production-readiness defect under §1.7.

## Design

### 1. PMP/PMP3 lane gains the standard guard (`vol_sweep.c`)

Mirrors the LZ4/ZSTD lanes exactly:

```c
if (invfs_pmp_decompress(pmp, pmp_len, &back, &back_len) != 0 ||
    back_len != full_len || (full_len && memcmp(back, full, full_len) != 0)) {
    stamp INVFS_CLASS_GENERIC_GUARD;   /* stays RAW, flows to generic lanes */
    goto pmp_declined;
}
free(back);
```

Without the guard a packMP3 blob of the right length but the wrong bytes is
committed, and the read path — which only compares `m_len != e->length`,
`vol_read.c:670` — then serves it out of a file that "verified OK".

### 2. APE read path stops clamping (`vol_read.c`)

`memcpy(dst, fl, min(fl_len, e->length))` became: decode, then
`fl_len != e->length` → diagnose, free, `return -1`. A short decode used
to leave the tail of a plain `malloc` buffer untouched and return success,
so `vol_read_inode` handed back `e->length` bytes of uninitialised heap as
file content. A long decode used to be silently truncated. The ranged-read
twin (`vol_read.c:1906-1920`) already errored; this is the whole-file
assemble path that did not.

### 3. `rec_len` bounded on the inode-hint path (`vol_ast.c`)

`INVFS_REC_HDR_LEN + 1 <= rec_len <= INVFS_MAX_REC_LEN` added next to the
magic/id check, matching `vol_read.c:1253` and `volume.c:4066`. That branch
is currently unreachable (`idx_get_id` is a retired no-op stub), so this is
hardening, not a live-bug fix — but a bound that exists only on the live
path is one refactor away from a 4 GiB allocation driven by image bytes.

### 4. Hermetic unit suite (`codec.c`, `Makefile`)

`pack_scan_all()` scanned `/usr/lib/invfs/codecpacks` unconditionally, so
`invf-codec_test`'s "registry holds the 14 static entries" assertion failed
on any host that has codecpacks installed — a red `main` that has nothing
to do with the tree. `INVFS_CODECPACKS_SYS=0` (unset = production
behaviour) skips the system dir; the `test:` recipe exports it.

### 5. `fallocate` tells the truth (`fuse_fs.c`)

Mode 0 keeps returning 0 (journald's preallocation). `PUNCH_HOLE` and
`ZERO_RANGE` now return `EOPNOTSUPP` instead of a success that released
nothing.

### 6. v3 rollback message (`invf-rollback.c`)

`vol_open` sets `needs_recovery = 1` unconditionally on v3
(`volume.c:1552`), so the "was not closed cleanly" line fired on *every*
successful rollback. Replaced with the real `sb.state` and a statement of
what the pass does (restore a save point, not crash recovery).

### 7. Stale `--seal` expectations

`tools/test-seal.sh` was migrated to assert the v3 refusal;
`test-sweepboot.sh` and `tools/fuzz/bitflip.py` were missed and have been
brought in line. A refusal is an expected outcome there, not a failure.

## Validation

```
$ make -j$(nproc)                      # BUILD_RC=0, no new warnings
$ make test                            # full unit suite, see RESULTS.md
$ bash tools/check-repo-hygiene.sh     # repo hygiene: OK
$ INVFS_E2E_AGENT=wp101-audit-hardening bash tools/run-e2e.sh \
      tools/test-pngflac.sh tools/test-sweepboot.sh tools/test-fuzz.sh
```

Fuzzing of the container lanes is in `/srv/bench/prod/fuzz/` and
summarised in `/srv/bench/prod/RESULTS.md`.

## Out of scope

- **Item 5 (plugin host has no sandbox).** Real, and it stays real: no
  `SO_PEERCRED` on `accept()`, a fixed `/tmp` socket path, `shm_open(...,
  0666)`, and `dlmopen()` on a caller-supplied `pack_path` with no path
  check. Fixing it is a design change (abstract sockets or
  `/run/user/$UID`, peer credentials, a pack-dir prefix check), not a
  cheap patch, and it needs its own WP. What this WP does is stop ADR-007
  from claiming a sandbox that does not exist.
- **Item 4's code half** (extending Landlock to the builtin lanes).
  Requires a sandbox spec for the builtin lanes; the doc now says plainly
  that the two classes are not equally confined.
- **Item 1's second half:** the read-path length check stays
  length-only. That is correct — memcmp at read time would need the
  original bytes, which is what the recipe is for.
- The Windows twin of `vol_create_flac_file()` (`vol_cpack.c:1155-1257`)
  has no decode-back guard either. Not reachable from a Linux build; not
  touched.
- The APE store lane keeps storing ffmpeg's re-encode (it is opt-in via
  `INVFS_APE` and knowingly not byte-preserving). The read path now fails
  loudly instead of truncating.

## Coordination notes

- Subagent: `wp101-audit-hardening`.
- E2E gates named above; `INVFS_E2E_AGENT=wp101-audit-hardening`.
- The WP doc is deleted in the shipping commit (§1.2/§1.7).
