# WP11 design note: JPEG→JXL on Linux + the POSIX tool-exec layer

Landed as part of ff84bf8 ("WP10+WP11"). Replaces the Windows-only
`CreateProcessW` hardcodes on the JXL lane (audit PB6) and brings the JPEG
branch of the sweep to POSIX. PMP/APE external tools ride the same exec
layer; WV registers sniff+probe only (no transcode path yet).

## What landed (volume.c)

- `tool_resolve(name)` — search order `$INVFS_TOOLS/<name>` →
  `/usr/lib/invfs/tools/<name>` → bare name (execvp's PATH search). No other
  absolute paths.
- `tool_exec(argv)` — fork/execvp, fixed argv arrays, no shell anywhere; the
  child is muted to /dev/null (matching the CREATE_NO_WINDOW side); hard
  120 s timeout → SIGKILL, so a wedged helper cannot hang a sweep.
- `tool_tmpdir()` — fresh mkdtemp scratch dir per transcode, `/dev/shm`
  (tmpfs) first, `/tmp` fallback: intermediates can be tens of MB and should
  not wear the flash the volume lives on, and a leftover output from an
  earlier run can never be misread as this run's.
- JPEG sniff `FF D8 FF` registered in the codec registry (the `jxl` entry:
  INVFS_ALGO_JXL, EXTERNAL, generation 1; JPEG is an *input* format of that
  codec) and matched in the sweep.

## JPEG branch gates (vol_sweep_file_inner)

1. **Probe gate** — cjxl absent → the file stays RAW and UNSTAMPED. Falling
   into the generic path would be terminal for it (zone!=RAW and a
   GENERIC/UNCOMPRESSIBLE stamp never re-arm when a tool appears), so it
   waits, and the first sweep after cjxl is installed picks it up.
2. **Admission** — `jpeg_raw_estimate()` walks the marker stream for
   SOF0/SOF1/SOF2 (baseline/extended/progressive) and returns ~w·h·3, the
   pixel buffer djxl materializes on decode-back; no trial decode. Over
   `dec_mem_limit` → generic ZSTD-19 stamped GENERIC_MEMLIMIT{JXL,gen} (a
   raised limit re-arms the path). Geometry unknown → admit and let cjxl try.
3. **Transcode** — `cjxl --lossless_jpeg=1`: the JXL stores the original
   JPEG bitstream, so djxl reconstructs the same bytes; size guard
   `jxl < jpeg`.
4. **Guard** — the blob must djxl-decode back to a memcmp-equal copy of the
   original JPEG before anything is replaced. Success → class
   CODEC{JXL,gen} (cls=2, algo=4).

## Test coverage

- `tools/test-jxl.sh` (persistent): 5 photo JPEGs (~50 KB..2 MB) + 1 tiny →
  sweep prints 6 "JPEG -> JXL (lossless)" lines → class CODEC{JXL,1} on all
  → verify --deep 0 corrupt → invf-cat sha256-exact. Negative leg:
  `INVFS_DEC_MEM_LIMIT=256K` → the five photos (raw estimate 518 KB..7 MB)
  fall back to generic with GENERIC_MEMLIMIT{JXL,1} and still read
  bit-exactly; the tiny one (128×128 = 48 KB) is admitted even there — the
  control that admission, not blunt refusal, gates the codec.
- `tools/bench-fs.sh` — corpus bench; Silesia numbers in doc/16 B30.

## Known gaps (→ WP12)

- (b) A MEMLIMIT/GUARD{JXL} retry re-enters vol_sweep_one, but the JPEG
  branch fires only on RAW-zone files — the upgrade path for an already
  generic-stored JPEG is incomplete.
- (c) PNGR/FLACR transcode paths are still Windows-only (`#ifdef _WIN32`);
  their exec dependencies resolve on Linux now.
- (d) No RLIMIT_AS in tool children (WP10 §10 hardening) — the timeout
  bounds time, not memory.
- (e) probe() runs per file; tool availability is not cached.

## WP16e addendum (2026-08-30): the lane is a codecpack now

The builtin JPEG branch this document describes is RETIRED. The lane lives
in `tools/codecpacks/jxl.codecpack/` (algo 4, the WP16e override: a pack
claiming a builtin EXTERNAL placeholder's algo replaces it; stream codecs
and container builtins can never be claimed — see WP16-containerpacks.md
"WP16e" for the full pattern and precedence rule). The gates above map
onto the generic pack sweep as: probe gate = the pack probe (tools absent
-> wait RAW, unstamped); admission = the pack's `estimate` command
(`jxlest`, the same SOF w*h*3 walk, packaged as a streaming C helper);
transcode/guard = `cjxl {in} {out} --lossless_jpeg=1` +
djxl decode-back memcmp inside vol_pack_sweep. Reads dispatch through the
registry entry (the pack trampoline; the builtin djxl wrapper remains as
the pack-absent fallback for pre-migration blobs and EXER-carved parts).
Gap (b) is long closed (WP12(b) vol_jxl_retry — now re-entering the pack
sweep); gap (d) is closed here too: WP12(d) gives every tool child an
RLIMIT_AS ceiling (max(2*dec_mem, 256MB) for packs with a manifest
dec_mem, 2GB default otherwise) and the pack exec resolves bare argv[0]
through $INVFS_TOOLS like the probe always did.
