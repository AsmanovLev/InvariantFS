# Roadmap — research & planned work

Live tracker for ideas under evaluation. Status words: `research`
(measuring), `planned` (decided, unscheduled), `in-progress`,
`dead` (measured out, with the numbers), `parked` (revisit on
condition). Numbers below are measured (`docs/benchmarks/`) or they
are not quoted.

## Codec lanes

- `in-progress` — WP615: `jxlest` dual CLI + `.so` (`wp/615-jxl-so-plugin`).
- `planned` — WP616: content batching (classifier table + composer +
  read-slicing). RAW/opaque first; photo-transcode-first per
  `docs/benchmarks/JXL-BATCH.md`.
- `planned` — WP615-followup: demote FLAC/JXL/PNGR builtins to fat
  packs (read-forever grandfathering for old recipes).
- `research` — NBLI prototype bench vs our JXL lane (ratio + speed).
  `fNBLI` closed binary is disqualified (no binaries, no audit).
- `research` — avrecode video lane (Dropbox, BSD): lossless H.264
  re-compression back to exact source bytes — the video analogue of
  the JXL-reconstruction/JPEG and PNGR-rebuild pattern. Bench ratio +
  roundtrip on a sample MP4 first. Hard requirement: exec-sandboxed
  only (upstream says "trusted inputs only, does not validate input").
- `dead` — JXL-multiframe photo album: 0% gain (even -3.5%), APNG-input
  quirk, no per-frame JPEG reconstruction in format/tools
  (`docs/benchmarks/JXL-BATCH.md`, multiframe legs).
- `dead` — animated-AVIF album: 3.3x worse stock, 5.3x worse max
  effort, 2.3x worse min partitions (`docs/benchmarks/JXL-BATCH.md`,
  AVIF legs). No inter-frame gain in lossless path on tested configs.
- `parked` — learned codecs (L3C watch first, SReC later): need
  integer-deterministic inference + install-time device certification
  + versioned weights before any lane. ncnn/Vulkan port only after an
  integer base exists. MoE router after that (expert-ID per segment
  rides the pack `id-name-hash` table).

## Pack system (WP614, `wp/614-anchored-packs`)

- Anchored `.invariantfs/` semantics, bare release + content-addressed
  pack cache + `@recommended`/`@all` sets, `install --volume` from
  cache, worker hardening (closes WP101 gaps), memory controller
  (per-call arena + namespace malloc interposition + RSS asserts),
  network default-deny except `invf-pack` fetch, upgrade =
  re-resolve + prove-convergence.
- Follow-up: fatten thin packs (`ape`/`flacr`/`wavpack`), `old_packs/`
  retention + `gc` (designed in `docs/adr/ADR-007-plugin-architecture-ivpack.md`,
  not built).

## Measurement

- `docs/benchmarks/QCOW2-COMPRESSION-BENCHMARK.md` (live): 2.89x VM
  corpus, 0.960x honest pre-compressed, dedupe + I/O tables.
- `docs/benchmarks/JXL-BATCH.md` (live): per-file transcode wins,
  batching second, albums dead.
- Planned FS-suite: stage3 rootfs across erofs/squashfs/btrfs (+zfs/
  bcachefs blocked on modules here).

## Presentation

- `docs/video/CONCEPT.md` v2 + `docs/video/scenes.py` (draft render is a local build product, untracked by policy): hook TBD
  (no "almost right is wrong"), staged-verification spine, no seal,
  GitHub CTA. Sound: music-first, layered. NUMBERS only from live
  benchmark docs above.
