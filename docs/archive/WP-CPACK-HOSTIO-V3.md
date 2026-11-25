# WP-CPACK-HOSTIO-V3 — packs read ranges, not files

Status: LANDED on `wp/cpack-hostio-v3` (7 commits + the ledger fix), all six
validation items answered below, `make test` rc=0 with 106 suites. Not merged
to `main` and not pushed. Phase 1 of the "container as data, never as a file"
plan; ADR-015 records the decisions.

## Why

A containerpack is invoked with `in_path` today, so the sweep must materialize
the whole container into a scratch directory before the pack can look at it, and
the pack writes the recipe and every member back out as files. Measured cost on
a 100 GiB guest image: ~2x the image in scratch (pin + extracted members, see
`src/core/tool_scratch.c`), the container read three times (pin, strip, guard)
and written twice, i.e. ~500 GiB of traffic. A single member larger than RAM
cannot be decomposed at all: the commit slurps it whole
(`src/core/vol_cpack.c:5218`) and one failure aborts the whole container.

ABI v2 offers exactly two choices — a path, or a whole in-memory buffer
(`src/include/ivpack_api.h:43-66`, "Optional in-memory buffers") — and both are
wrong at this size. The packs themselves do not need either: every C container
pack reads through a `pread` loop already.

## Design

1. **ABI v3: host I/O callbacks.** Append to `ivpack_container_args`:

   ```c
   typedef struct ivpack_host_io {
       void    *user;
       uint64_t size;       /* bytes the handle spans; 0 = handle unusable */
       /* <0 on error, short reads are legal only at EOF */
       ssize_t (*pread)(void *user, void *buf, size_t n, uint64_t off);
       ssize_t (*pwrite)(void *user, const void *buf, size_t n, uint64_t off);
   } ivpack_host_io;
   ```

   with a `const ivpack_host_io *io;` member. Gating is the existing mechanism:
   the host fills v2 fields only for `desc->api_version >= 2` and fills `io`
   only for `>= 3`, so a v1 or v2 plugin keeps working with no change at all
   (`IVPACK_API_VERSION_MIN` stays 1).

2. **Core provider.** `publish`/`host_io` backed by the existing volume paths:
   `pread` → `vol_read_range`, `pwrite` → `vol_write_range` on the container's
   write session. No new kernel surface, no mounts, no privileges — this is the
   same I/O the core already does.

3. **Pack conversion, in this order:** `ext4fs` (the proof; it is the pack that
   owns the elision work), then `qcow2` if time allows. A pack uses `io` when
   present and falls back to the path when not, so the CLI (`bin/ext4fs
   <cmd> <in> <out>`) behaves exactly as before.

4. **The commit path streams.** `cpack_member_commit` stops slurping: head
   window (`CPACK_LEAF_HEAD`, 64 KiB) for the container sniff, then a chunked
   copy (4 MiB) into the volume. Members ≤ 1 MiB keep the single-slurp fast
   path. The recipe is streamed the same way.

5. **Solid archives are refused, not decomposed.** A solid 7z/rar block has no
   addressable members, so decomposition cannot pay: add
   `INVFS_CLASS_GENERIC_SOLID`, stamp it with the reason, count it in the sweep
   report (`refused (solid): N files, M MiB`), and keep `INVFS_CPACK_SOLID=1` as
   the explicit opt-in for the operator who wants to pay a full unpack.

## Validation

Every item below was run on a native ext4 copy (`git clone`, NOT rsync --
see the trap at the end), gcc 14.2.1, f3 3.18.2.

1. **Zero-scratch proof (the acceptance):** run a decomposition with
   `INVFS_TOOL_SCRATCH` pointing at a 1 MiB tmpfs. With host_io the container is
   never materialized, so it must succeed and produce a bit-exact round trip; on
   a 100 GiB-scale case the same run must not touch scratch at all. Paste the
   command and the resulting `du` of the scratch root.
2. `tools/test-ext4fs.sh` PASS (CLI path unchanged), plus the `zl.ext4`
   adversarial leg still green.
3. `tools/test-containerpack.sh` PASS: an untouched v2 pack (ntfs/fatfs) still
   decomposes through the path, proving the gate.
4. `build/invf-cpack_map_conc_test` (plain/asan/tsan) PASS.
5. **Streaming member:** a member larger than the test's RSS budget (e.g. 512 MiB
   with the process capped) must commit successfully; today it aborts the
   container.
6. Solid: a real solid archive (or the smallest reproducible stand-in) gets
   `GENERIC_SOLID` stamped, appears in the counter, and is NOT decomposed
   without the opt-in.

## Validation: what was run, and what it said

1. **Zero-scratch proof.** `bash tools/run-e2e.sh tools/test-cpack-hostio.sh`
   -> `CPACK HOST I/O (ADR-015): PASS`. J1, the acceptance leg: an 8 MiB ext4
   container decomposed with the scratch root a 1 MiB tmpfs, and
   `du -sk` of that root afterwards is **0 KiB**. Control J2, the same sweep
   with `INVFS_CPACK_HOSTIO=0`: refused by the same scratch, no CONTAINER
   stamp, volume still bit-exact. J3, the same pack source built as descriptor
   v2: no `io` fields, the pin returns, refused again. J4, solid: stamped
   `GENERIC_SOLID`, counted, not retried, `INVFS_CPACK_SOLID=1` restores the
   plain decline. J5, a 192 MiB member committed with sweep `VmHWM` 101 MiB
   (it used to be one malloc, and one failure aborted the container).
2. **`tools/test-ext4fs.sh` -> `EXT4FS CONTAINERPACK E2E: PASS`.** The CLI path
   is unchanged, which is the point of the `__has_include` gate.
3. **`tools/test-containerpack.sh` -> `CONTAINERPACK E2E: PASS`**, rc=0, an
   untouched v2 pack (splt_test) still decomposes through `in_path`.
   This leg was RED before the change and for an unrelated reason; see the
   commit "tests: the neg.splt fixture never reached the pack (it is 26
   bytes)" -- the 26-byte negative fixture had been taken by the sub-block
   batching lane since 93b9fa6, so the map validator's own negative control
   had been testing the wrong lane.
4. **`invf-cpack_map_conc_test` / `_asan` / `_tsan` -> PASSED, 0 failures.**
   TSAN aborts before `main` on this host with
   `FATAL: ThreadSanitizer: unexpected memory mapping` (ASLR entropy, not the
   code): `setarch $(uname -m) -R bin/invf-cpack_map_conc_tsan` passes.
   Worth knowing before trusting a TSAN red in CI.
5. **Streaming member** -- measured in J5 above, not separately.
6. **Solid** -- measured in J4 above, not separately.

Gate: `make test` -> **rc=0, 106 suites, 0 failures**, `repo hygiene: OK`,
`check-codecpack-sync: OK`. Build: 0 errors, the same 11 warnings `main`
carries (nothing new; `vol_cpack.c:3648`'s `ealgo` only moved to 3728).

### Two traps found while gating, both environmental

* **A rsync copy cannot be gated.** `make test` fails `test-shard-4` at
  `check-repo-hygiene.sh` -> `fatal: not a git repository`, because that gate
  and `check-vol-find.sh` both start with `git rev-parse --show-toplevel`. The
  coordination note above recommends rsync; it is right for the *pack child*
  problem and wrong for the gate. `git clone -b <branch>` into the native fs
  gives both.
* **`check-codecpack-sync.sh` discovers `../invfs-registry` by default**, so a
  stale copy of the registry anywhere beside the clone makes `make test` red
  with "vendored codecpacks have drifted" when the truth is "your neighbour is
  old". Refresh it or point `INVFS_CODECPACK_REGISTRY` at a real checkout.

## Out of scope

- **Stream (level 2) for external binaries** — ffmpeg `pipe:0` with an explicit
  `-f`, `7zz -si`, cjxl/djxl. Separate WP; level 2 is three tools, not "archivers
  in general", and builtin lanes (tar via `src/recipes/tarx.c`, iso9660) need
  none of it.
- **NBD / FUSE mounts: rejected.** They need a kernel module, `/dev/nbdN`,
  `CAP_SYS_ADMIN` and per-job mounts inside a sandbox built on Landlock and
  `helper_exec` with no privileges; `src/cli/fuse_fs.c:250` already records the
  deadlock risk. A tool that insists on a seekable path gets a **sparse temp
  file**, one member at a time, deleted immediately — no privileges, no kernel.
- Deep sweep (queue with a depth cap). It becomes cheap only after this lands.
- The container tree (member as a node record).
- Any wire-format change to the map or the recipe.

## Coordination notes

- Worktree `/d/Users/User/invfs_deploy/wt-hostio`, branch `wp/cpack-hostio-v3`.
  Registry copy of ext4fs: only if the change is pack-side and must stay in
  step; the plugin glue is vendored-only (registry packs build standalone).
- Do not touch `src/core/vol_cpack.c`'s map wire/parse/validate logic
  (ADR-014 territory) or the anchor allocators.
- Build/test on a native WSL copy (`rsync --exclude .git --exclude bin
  --exclude build --exclude "*.so"`): on drvfs every pack child dies
  `execve EACCES` because Landlock does not apply there. Never use `/tmp` for
  images.
