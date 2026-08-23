# InvariantFS

A semantic, content-aware filesystem. It decomposes files into typed components,
stores each with the compression algorithm best suited to that component, and
reconstructs the original **byte for byte**.

The name is short for *data invariant*, which is the one property everything else
is built around.

---

## The invariant

> **What is written comes back bit-for-bit identical. Always.**

This is not a best-effort goal — it is the constraint that decides what the
filesystem is allowed to do:

- Every compression is followed by a decompression and a BLAKE3 comparison
  against the original. A mismatch means the transform is rejected.
- Containers (ZIP, TAR, gzip) **keep their original bytes**. Members are exposed
  as byte-range windows into those bytes, not as re-serialized copies. A `.zip`
  read back from InvariantFS is the same `.zip`, so checksums and signatures survive.
- A transcode is only applied where bit-exactness has been *proven* for that
  specific file. Where it cannot be proven, the original is stored as-is.

The last point is what distinguishes this from a lossy-but-good-enough recompressor.
Storage savings are always the loser when they conflict with the invariant.

---

## Status

**Experimental. Not production ready. Do not put data you care about on it.**

What works today is the storage engine and the transcode machinery — the hard,
interesting part. What is missing is most of what makes a byte store into a
POSIX filesystem. See [Known limitations](#known-limitations) before you form
expectations; that section is deliberately blunt.

---

## Architecture

### Zones

| Zone | Role |
|---|---|
| **RAW** | Linear landing area. New writes go here immediately, LZ4-compressed, cheap and fast. |
| **Shadow** | Optimized storage, split into a **Text Zone** and a **Binary Zone** so similar data is consolidated. |
| **Metadata** | Superblock, block bitmap, L2P journal, inode area. |

Writes are decoupled from compression. Data lands in RAW at write speed; the
**sweep worker** later moves it to Shadow under the appropriate codec. Expensive
analysis therefore never sits on the write path.

### AST recipe

Each file carries a recipe that maps byte ranges of the *original* file onto
`(zone, block, offset)` tuples. Segments are stored as
`[4B csize][4B crc32c][data]`.

Two consequences worth calling out:

- **Partial reads are cheap.** Reading a FLAC file's tags touches only the text
  segments; the audio blocks are never decompressed.
- **Containers nest.** The recipe is a tree, so a zip-in-a-tar mirrors its real
  structure (bounded at depth 16).

The CRC on each segment is checked *before* decompression, so a corrupted stored
blob is caught rather than fed to a decoder.

### Journal

An append-only L2P journal (`MAP` / `UNMAP` / `SWEEP` / `CHECKPOINT`) persists the
logical-to-physical mapping. Mount replays from the last checkpoint. The
superblock carries a state byte (`CLEAN` / `DIRTY` / `RECOVERY`) driving recovery.

### Deduplication

Content addressing via BLAKE3. Identical blocks are stored once and refcounted —
this is how duplicate cover art across an album costs nothing.

---

## Codecs

**General purpose:** LZ4 (write path), ZSTD, APE (audio), WavPack, JXL. PPMd is
declared but not implemented — it belongs to the text sub-section of Shadow,
which is still a design (see `doc/02-on-disk-format.md`).

**Bit-exact transcode families.** These are the interesting ones. Each replaces a
file with a denser representation *plus a recipe* that reproduces the original
stream exactly:

| Tag | Family | How the original is reproduced |
|---|---|---|
| `FLACR` | FLAC | PCM stored as APE, plus a frame recipe rebuilding the exact FLAC bitstream. Cover art extracted and deduplicated. |
| `TARR` | TAR | Members split out; an `IVFT` recipe restores headers, padding, and alignment. |
| `GZR` | gzip | Deflate stream replicated bit-exactly by brute-forcing zlib's level × memLevel until output matches. |
| `PNGR` | PNG | Stored as lossless JXL, with an `IVPN` recipe recording row filters and the deflate parameters needed to rebuild the identical PNG. |
| `PMP` | MP3 | Recompressed by packMP3, which reproduces the exact MP3 stream from its own blob — the one family that needs no recipe. MPEG-1 Layer III only; anything else falls through to ZSTD. |

Where replication fails — an unusual encoder, an unknown deflate variant — the
file stays in its original form. That is the invariant doing its job.

---

## Building

Neither build produces a mountable filesystem by itself; both produce the CLI
tools. The FUSE and Dokan daemons build separately.

**Linux / WSL** — needs `gcc`, system `libzstd`, `zlib`:

```sh
bash build_linux.sh       # -> build_linux/invf-{mkfs,verify,fsck,cp,cat,ls,sweep,stat,zip}
```

**Windows** — needs MSVC:

```bat
build_with_env.bat        :: sets up the MSVC environment, then runs build.bat
```

zstd and zlib are vendored under `src/`.

## Tools

| Tool | Purpose |
|---|---|
| `invf-mkfs` | Format a volume |
| `invf-cp` | Copy a file into the image |
| `invf-cat` | Read a file out (use `cmd /c`, not a PowerShell redirect — see below) |
| `invf-ls` | List files |
| `invf-stat` | Space inspector; per-zone usage and per-file policy labels |
| `invf-sweep` | Run the sweep worker (RAW → Shadow, transcodes) |
| `invf-verify` | Integrity check; `--deep` verifies every segment CRC |
| `invf-fsck` | Check and repair: bitmap, orphaned blocks, journal compaction |
| `invf-zip` | Container inspection |
| `invf-sizes`, `invf-range-test` | Development helpers |

## Testing

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests.ps1        # core + transcodes
powershell -NoProfile -ExecutionPolicy Bypass -File tests_dokan.ps1  # Windows/Dokan
powershell -NoProfile -ExecutionPolicy Bypass -File tests_fsck.ps1   # fsck/repair
powershell -NoProfile -ExecutionPolicy Bypass -File tests_fuse.ps1   # Linux/FUSE
```

| Suite | Assertions |
|---|---|
| `tests.ps1` | 72 |
| `tests_dokan.ps1` | 19 |
| `tests_fsck.ps1` | 14 |
| `tests_fuse.ps1` | **0 — it exercises the code and prints, but asserts nothing and cannot fail** |

The FUSE suite is listed honestly rather than counted as coverage. Fixing it is
tracked under limitations below.

> Do not use PowerShell's `>` redirect for binary output from `invf-cat` — it
> mangles bytes. Use `cmd /c` or `[IO.File]::ReadAllBytes`.

---

## Known limitations

Stated plainly, because several of these are load-bearing if you were thinking of
using this as a real filesystem:

- **No permissions.** No `uid`, `gid`, or `mode` is stored anywhere. The FUSE
  layer reports a hardcoded `0555` for directories and `0444` for files.
- **No extended attributes**, so no SELinux labels and no file capabilities.
- **No symlinks**, and no hardlinks — a directory entry and an inode are currently
  the same object, so a name cannot be separated from its inode.
- **FUSE implements 11 operations.** Missing: `rename`, `truncate`, `fsync`,
  `symlink`, `readlink`, `link`, `chmod`, `chown`, `utimens`, `statfs`, and all
  xattr operations.
- **4 GB per-file cap** — `file_size` in the AST recipe header is a `uint32_t`,
  while the intended bound is 2^40.
- **Name lookup is a linear scan** over the whole inode area, so directory-heavy
  workloads degrade quadratically.
- **The JXL and APE transcodes shell out to external binaries at absolute paths**
  (`D:\VFS\tools\jxl\...`, `D:\bin\MAC.exe`) and will not work on another machine
  without editing `src/volume.c`.
- **No kernel module.** Access is via FUSE (Linux) or Dokan (Windows).
- **Sweep replacement is not crash-atomic** — a crash in the replace window can
  lose a file. `invf-fsck` is the recovery path.
- Untested: concurrent writers to one inode, power-loss recovery, hostile input
  fuzzing.

---

## Documentation

`doc/01` through `doc/18` cover the design in depth — on-disk format, AST recipe,
compression matrix, classification, sweep, read/write paths, crash recovery,
dedup, permissions, ENOSPC policy, Linux rootfs, caching, benchmarks, and test
coverage. Start with `doc/01-overview.md`.

These are working design documents, written primarily in Russian, and some
describe intended rather than implemented behaviour — `doc/18-test-coverage.md`
is the most reliable guide to what is actually built.

---

## License

GPL-2.0-only. See [LICENSE](LICENSE).

Contributions are welcome — please read [CONTRIBUTING.md](CONTRIBUTING.md) first,
as it covers licensing terms for contributed code.
