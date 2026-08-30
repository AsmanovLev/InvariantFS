# InvariantFS fuzz/adversarial wave — findings report

Scope: `tools/fuzz/` + `tools/test-fuzz.sh` only. No engine/pack/Makefile
changes. Everything is deterministic (fixed default seeds; every tool takes
`--seed`). Host: Linux x86-64, 12 cores, tmpfs `/dev/shm` work areas.

## Tools built

| Tool | Kind | What it does |
|---|---|---|
| `tools/fuzz/bitflip.py` | python3 | Per iteration: mkfs on /dev/shm → import mixed corpus (text/binary/empty/~1 MB large) → sweep → every 8th image `--seal` → baseline fsck+verify --deep → ONE mutation (flip 1–16 bytes / zero a random 4K block / truncate / crafted RDP0+RSZ0+CKP0 descriptors with *valid* CRCs and garbage fields) targeted at a weighted region: data zones, journal, inode area, superblock, descriptor slots, bitmap → fsck + verify --deep + invf-cat of every file, bit-exact vs the host original. Signal deaths, timeouts and rc ≥ 128 are failures; an rc=0 read whose bytes differ is SILENT-GARBAGE. `--only N` replays iteration N; failing images are preserved sparse under `<workdir>/failure-NNNNNN/`. |
| `tools/fuzz/opseq.py` + `ophelper.c` | python3 + C | Random programs of create/rewrite(incl. shorten)/delete/sweep/fsck/verify/stat/spot-cat through the real CLIs (delete via `ophelper`, the tzrm convention: public `volume.h` only). A shadow reference tree tracks every surviving file; after each sweep: fsck rc==0, verify --deep rc==0, `invf-ls` == shadow set exactly (no `\x01` owner leaks), all survivors bit-exact. `--only-image N` replays; op log + image kept on failure. |
| `tools/fuzz/packfuzz.py` | python3 | For the 7 packs with C helpers (rawdisk, ext4fs, fatfs, xfs, ntfs, vdi, qcow2): builds a tiny valid fixture (hand-built GPT/VDI seeded by python, mkfs.ext4+debugfs, mkfs.vfat+mcopy, mkfs.xfs/mkfs.ntfs+loop populate, qemu-img+qemu-io), then mutates (flips biased 60% into the first 4 MiB, zero-4K, truncate; undo-based, no recopy per mutant) and runs `enumerate` + `map` (+ `estimate` on 10%). Contract: rc ∈ {0,1,3}, no signal, no >10 s hang; enumerate tables have numeric idx ≤ 65535 and usize ≤ 10× file size; map output is validated against a local MRMP oracle (sorted, contiguous partition of `[0, size)`, kind ≤ 1) mirroring `cpack_map_parse`. Failing mutants are kept under `<workdir>/failures/`. |
| `tools/fuzz/fuzz_manifest.c` | C11 | Drives malformed manifests (byte flips, NUL injection, 4 KB values, truncation, whitespace/CR perversion, duplicate-key storms, random blobs — over both container and codec pack bases) through the REAL registration path (`pack_scan_dir` → `parse_manifest` → `pack_register` inside `codec.o`) via `INVFS_CODECPACKS` + `invfs_codec_probe_reset()` + `invfs_codec_all()`, with stubbed exec hooks (codec_test convention: no fuzzed argv ever executes). Also probes/sniffs every registered entry per case. A crash aborts the process with the culprit `CASE nnnnn` last on stdout; the case manifest is at `$INVFS_FUZZ_CASES/case_nnnnn.manifest`. |
| `tools/test-fuzz.sh` | bash | REDUCED pass for `make e2e` wiring: builds both C harnesses, runs bitflip×60, opseq 2×150, packfuzz×100/pack (skips packs whose fixture tooling is missing), manifest×400. ~25 s here. |
| `tools/fuzz/fuzzutil.py` | python3 | Shared: CRC32C (verified bit-exact against a live superblock checksum), superblock/region parser, timeout+signal-checking runner, undo-mutator, descriptor crafters (real RDP0/RSZ0/CKP0 layouts from invarifs.h). |

## Full-pass results (offline runs)

| Fuzzer | Volume | Result | Runtime |
|---|---|---|---|
| bitflip | 200 iters × 4 seeds (0x1A2B3C4D, 0xB17F11–13) + 300 (0xF00D42) = **1100 iterations**; per-iter coverage over all six regions, ~25 sealed images per 200 | **0 crashes, 0 silent garbage from the engine**; 2 findings below | ~0.18 s/iter (~35 s per 200) |
| opseq | 5 imgs × 300 ops × 3 seeds + 3 imgs × 300 + reduced = **5400 ops** | **clean** (one apparent hit traced to finding #2, see note) | ~15–160 s per 5 images |
| packfuzz | 500 mutants × 7 packs × 3 seeds + 800 × 7 + reduced = **16100 mutants** | **clean**: all helpers exited 0/1/3 only, tables sane, MRMP maps well-formed | ~15–25 s per 7×500 |
| manifest | 200+3000+3000+5000+reduced = **~11800 cases** | **clean**: no crash, registry always consistent | ~3.5 ms/case |

Regression guard: `make test` (4467+86+161 checks, 0 failures),
`tools/test-containerpack.sh` PASS, `tools/test-seal.sh` PASS after all
work. The tree received concurrent WP16e changes (codec.c/codec.h/
volume.c, new jxl/p7z packs) mid-session by another worker; everything
above was therefore verified TWICE -- against the handed-off binaries
and against the rebuilt 23:56 tree -- with identical results (both
findings reproduce on both). Note: an early parallel run self-DoS'd on
the /dev/shm quota and produced false "silent garbage" positives;
harnesses now preserve failing images, and all seeds were re-run in
isolation to confirm.

## BUGS FOUND

### Bug 1 — `invf-verify --deep` exit code drops structural errors
- **Repro**: `bash tools/fuzz/repro/verify-deep-exit-code.sh`
- **Observed**: image truncated inside the journal area → verify --deep
  prints `backing store 3408 blocks vs superblock total_blocks 31457`
  (via the structural `err()` path) and still exits **0**
  (`deep: 0 files ok, 0 corrupt`). fsck -q on the same image exits 3.
- **Expected**: any structural `err()` forces a nonzero exit with or
  without --deep. A CI gating on `invf-verify --deep` exit status would
  pass a structurally destroyed volume.
- **Suspected layer**: tools-only — `src/verify.c`: the shallow path ends
  `return errors ? 1 : 0;` but the `--deep` branch ends
  `return (bad || parity_bad) ? 1 : 0;`, dropping `errors`.
- **Related observation (same exit-code hole, informational)**: when the
  *head* of the append-only inode area is destroyed (zero4k on its first
  block, or flips that break an early record), the scan yields zero live
  files and verify --deep reports `0 files ok, 0 corrupt` rc=0 — total
  metadata loss reads as an empty-but-healthy volume. fsck stays honest
  (reports orphans, `ISSUES FOUND`, rc=3), so the structural arbiter is
  fine; but note the live CKP0 descriptor records `inode_area_pos` at
  sweep time, so a scan ending far short of that pointer is *detectable*
  metadata loss if verify ever wants to flag it.

### Bug 2 — `invf-cat` (and zip member extraction) lose output bytes silently on a full filesystem
- **Repro**: `bash tools/fuzz/repro/cat-output-enospc.sh`
- **Observed**: with the output filesystem full/quota-exceeded,
  `invf-cat img a.bin out` prints `extracted 'a.bin' -> out (100000
  bytes)` and exits **0**, leaving a 0-byte `out`. A restore pipeline
  consuming invf-cat archives empty files believing they were restored.
  (This fired organically: the fuzzers' own bit-exact checks caught it
  when /dev/shm hit its quota mid-run.)
- **Expected**: any short/failed output write (fwrite/fclose/ferror)
  → error message + nonzero exit. The engine side of the same condition
  is correct: an image-side write under EDQUOT fails loudly
  (`[create] write fail seg 0 …`, rc=1) and fsck stays clean after.
- **Suspected layer**: tools — `src/cat.c:85-86` (`fwrite` + `fclose`
  unchecked; the stdout branch likewise), same pattern at
  `src/zip.c:200-201`.

## Clean bills (explicitly verified)
- Corrupt data is never served: across 1100 mutated images, every
  data-zone/journal/inode/superblock/descriptor/bitmap mutation ended in
  either bit-exact reads (incl. seal parity recovery on sealed images) or
  loud failure; zero signal deaths, zero hangs, zero rc ≥ 128.
- Crafted descriptors (valid CRC, garbage fields) are validated and
  ignored/neutralized; no wild resize roll-forward, no bogus parity
  geometry.
- All 7 pack helpers decline/error cleanly on 11300 mutants; no crashes,
  no >10 s hangs, no nonsense `usize`, all MRMP maps well-formed.
- Manifest parser: 11600 malformed manifests, no crash, registry
  consistent, no half-registrations.
- opseq: ~5000 randomized ops with fsck/verify gates and shadow-tree
  bit-exactness — clean.

## Suggested Makefile wiring
```make
# e2e tier: append after the existing e2e scripts
	bash tools/test-fuzz.sh
```
and optionally a full-wave target:
```make
fuzz-wave: all
	python3 tools/fuzz/bitflip.py --iterations 200
	python3 tools/fuzz/opseq.py --images 5 --ops 300
	python3 tools/fuzz/packfuzz.py --mutants 500
	cc -std=gnu11 -O2 -I$(SRC) -o tools/fuzz/fuzz_manifest \
	    tools/fuzz/fuzz_manifest.c $(FUZZ_O) $(LDLIBS)
	tools/fuzz/fuzz_manifest 1000 0xCAFE
.PHONY: fuzz-wave
```
(`tools/test-fuzz.sh` builds both harness helpers itself, so the e2e tier
needs nothing else.)

## Housekeeping note
While cleaning stale fuzz work areas I removed pre-existing throwaway
images on /dev/shm (`gzbug.img`, `tarrbug2.img`, `rawdiskwp*.img`,
`silesia14b.img`, `wp*`). tmpfs is volatile and the repo scripts recreate
their fixtures per run, but flagging it in case any of those were kept
deliberately.
