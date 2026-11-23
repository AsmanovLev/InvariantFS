# ADR-015: a pack reads ranges or a stream, never a materialized file

## Status

Accepted. Phase 1 (ABI v3 `host_io`) is `WP-CPACK-HOSTIO-V3`; phase 2 (stream for
external binaries) is its own WP; the rejected options below are recorded so
they are not re-proposed.

## Context

A containerpack is handed `in_path` and writes the recipe and its members to
paths. The sweep therefore materializes the entire container into a scratch
directory before the pack can read a byte, and the core then reads every output
file back to write it into the volume. Measured consequences on sizes the
project exists for:

- scratch peak ~2x the container (`src/core/tool_scratch.c` grew a
  size-aware root chooser precisely because of this);
- the container is read three times (pin, strip, guard) and written twice, so a
  100 GiB image costs ~500 GiB of traffic per sweep;
- any single member larger than RAM aborts the entire decomposition, because the
  commit slurps it whole (`src/core/vol_cpack.c:5218`).

ABI v2 offers a path or a whole in-memory buffer (`src/include/ivpack_api.h:43`),
i.e. the two extremes, and neither survives a 100 GiB guest image. Meanwhile the
C packs already read through `pread` loops and the core already has
`vol_read_range`/`vol_write_range`: the file in the middle is an interface
artifact, not a requirement.

## Decision

1. **ABI v3 carries host I/O callbacks.** `ivpack_host_io { user, size, pread,
   pwrite }`, appended to `ivpack_container_args`, filled by the host only for
   `desc->api_version >= 3`. `IVPACK_API_VERSION_MIN` stays 1: a v1 or v2 pack
   keeps working unchanged, on paths, exactly as today.

2. **The core provides those callbacks over the volume itself**
   (`vol_read_range` / `vol_write_range` on the container's write session). No
   kernel module, no device node, no mount, no privilege: the pack reads the same
   bytes the core would.

3. **A pack uses `io` when present, the path when not.** The CLI form
   (`bin/<pack> <cmd> <in> <out>`) is unchanged, so tools, tests and the
   `argc-argv` world keep working.

4. **External binaries get a stream, not a file**: `ffmpeg -i pipe:0` with an
   explicit `-f`, `7zz -so`/`-si`, `cjxl`/`djxl` where they accept it. Builtin
   lanes (tar via `src/recipes/tarx.c`, iso9660) need none of this — they read
   the volume through the core already.

5. **NBD and FUSE block views are rejected.** They need a kernel module,
   `/dev/nbdN`, `CAP_SYS_ADMIN` or per-job mounts inside a sandbox built on
   Landlock and `helper_exec` with no privileges; `src/cli/fuse_fs.c:250`
   already records the deadlock risk. The security posture is worth more than the
   convenience.

6. **A tool that demands a seekable path gets a sparse temp file**, one member at
   a time, deleted immediately: holes instead of zeros, so a 100 GiB virtual disk
   with 5 GiB of data costs 5 GiB. That is the last resort, not a policy.

7. **Solid archives are refused, not decomposed.** A solid block has no
   addressable members, so per-member decomposition cannot pay: stamp
   `INVFS_CLASS_GENERIC_SOLID`, count it (`refused (solid): N files, M MiB`), and
   keep `INVFS_CPACK_SOLID=1` as the explicit opt-in.

## Consequences

- The pin disappears for C packs; scratch becomes O(largest member), and zero for
  a pack that only ever reads ranges.
- The guard can be folded into the write stream (compare while committing)
  instead of re-reading the container: ~5 passes become ~2.
- Members larger than RAM become decomposable, which is the case the project is
  for.
- The ABI grows one gated step, in the same style as the map's declaring magic
  (ADR-014): one version bump per "an older peer cannot do this", never a
  capability table.
- Packs must not assume `io != NULL`; every conversion keeps the path branch
  alive and tested, because the CLI and third-party invocations still use it.

## Alternatives rejected

- **Whole-image in-memory buffers for big images** (the v2 buffer fields): a
  100 GiB image in RAM, and the 5.2 GiB RSS measurement on a 3 GiB partition is
  what happens when that habit meets a real image.
- **NBD/FUSE virtual block devices**: see decision 5.
- **Decomposing solid archives anyway**: no addressable members; the output would
  be one member the size of the archive, i.e. a copy, with the CPU cost of a full
  unpack.

## See also

- [ADR-014-container-map-is-the-abi](ADR-014-container-map-is-the-abi.md) — the map whose bytes a pack reads
- [ADR-009-durability-contract](ADR-009-durability-contract.md) — what the writes must survive
- [ADR-007-plugin-architecture-ivpack](ADR-007-plugin-architecture-ivpack.md) — where the callbacks are handed in
- [all ADRs](README.md)
