# WP71-ivpack-all-packs — every C containerpack as an `.ivpack` / `.so` plugin

**Branch:** `wp/71-ivpack-all-packs`
**Worktree:** `/home/user/wt/wp71` (AGENTS.md §1.3 says `/tmp/invfs-wp71`; this
agent's `/tmp` does not survive between sessions, so the worktree lives in the
persistent workspace instead — same rules otherwise)
**Severity:** HIGH (Bug A silently corrupts a decomposition; Bug C kills the daemon)
**Source:** user delegation ("сделай ivpack-и для других частей в виде .so-шек"),
follow-up to commit `1175fe7` (ADR-007 worker pool + qcow2 plugin)
**Estimated effort:** done — 8 packs, 4 bugs, 3 test surfaces

---

## Scope

Commit `1175fe7` proved the ADR-007 plugin path with one pack (`qcow2`). This WP
generalises it to **every C containerpack** and fixes what generalising exposed.

In scope:

- `src/include/ivpack_impl.h` (new) — the shared ABI glue every pack includes.
- ABI v2 in `src/include/ivpack_api.h` — `extract_idx` + `self_path`, appended.
- Plugin exports in all eight C containerpacks: `qcow2`, `ext4fs`, `fatfs`,
  `ntfs`, `rawdisk`, `vdi`, `xfs`, `p7z`.
- `Makefile`: `plugin-so` (builds all eight `lib<name>.so`) and `ivpacks`
  (bundles them into `dist/ivpack/<name>.ivpack`).
- `tools/pack-ivpack.sh`: emits the ADR-007 §3 layout instead of a flat one.
- `src/core/vol_cpack.c` + `src/core/vol_plugin_client.[ch]` +
  `src/core/invf_plugin_ipc.h` + `tools/invf-plugin-host.c`: operand routing,
  ABI gating, and startup failure handling.
- Tests: `src/cli/ivpack_packs_test.c` (unit, in `make test`),
  `tools/test-ivpacks.sh` (e2e, in `make e2e`),
  `tools/plugin-daemon-smoke.sh` (developer tool, needs a real daemon).

Not one line of any pack's format logic changed. The glue sits between the
existing `cmd_*` functions and the ABI, and each pack still builds as a CLI with
its own documented command (`cc -std=c11 -O2 -Wall -Wextra -Werror <pack>.c`).

---

## Bug A — EXTRACT could not name a member over the plugin path

**File:** `src/core/vol_cpack.c` (`invfs_codec_pack_cmd`),
`src/core/vol_plugin_client.c`, `src/core/invf_plugin_ipc.h`,
`tools/codecpacks/qcow2.codecpack/qcow2.c` (`ivpack_container_cmd`)
**Function:** `invfs_codec_pack_cmd` → `invfs_plugin_pool_container_cmd` →
`ivpack_container_cmd`
**CWE:** CWE-683 (function call with incorrect argument order/placement)

`invfs_codec_pack_cmd()` receives the member index as `idx` and builds
`extract {in} {idx} {out}` for the CLI. The plugin dispatch added by `1175fe7`
had nowhere to put it:

```c
/* vol_cpack.c, before */
int prc = invfs_plugin_pool_container_cmd(c->name, so_path, pcmd,
                                          in, out, recipe, dir);   /* no idx */
```

and the qcow2 export filled the gap with the wrong operands:

```c
/* qcow2.c, before */
case 2: /* EXTRACT */
    if (!args->in_path || !args->recipe_path || !args->mbr_dir) return -2;
    return cmd_extract(args->in_path, args->recipe_path, args->mbr_dir);
    /*                 image         ^ idx = the RECIPE path  ^ out = a DIRECTORY */
```

`cmd_extract(in, idx_s, out)` runs `strtoul(idx_s)`, so a recipe *path* parses
to 0 and the member is written to a *directory* path — every plugin-side EXTRACT
either failed or produced the wrong member. The failure was invisible:
`invfs_codec_pack_cmd()` falls back to the CLI whenever the pool returns a
negative code, and a *positive* code (the pack's own `3` = decline) was returned
to the caller as authoritative, so the sweep would have accepted a decomposition
whose members came from nowhere.

**Fix:** carry the index. `invf_plugin_req` gains `char extract_idx[32]`
(IPC version 1 → 2), the client API gains an `idx` parameter, and the ABI struct
gains `extract_idx` (see Bug D for the versioning rule). `vol_cpack.c` now routes
operands per command instead of passing all four positionally.

---

## Bug B — MAP was handed the recipe path instead of the image

**File:** `tools/codecpacks/qcow2.codecpack/qcow2.c`, `src/core/vol_cpack.c`
**Function:** `ivpack_container_cmd` case 5
**CWE:** CWE-683

```c
/* qcow2.c, before */
case 5: /* MAP */
    if (!args->recipe_path || !args->out_path) return -2;
    return cmd_map(args->recipe_path, args->out_path);
```

Every containerpack's `map` takes the **image**, not the recipe: it re-runs the
pack's own `analyze()` and *derives* the recipe layout (`src_off` counters in
`rawdisk.c:cmd_map`, and the same shape in the other packs). `tools/test-rawdisk.sh`
calls it that way (`"$RD" map "$WORK/orig/disk-gpt.img" ...`), and
`vol_cpack.c:2466` passes `pin` (the pinned image). Handing it `recipe_path`
means the plugin's MAP always declined — and, since a decline is a positive
return code, `invfs_codec_pack_cmd()` would have reported it as authoritative
instead of falling back to the CLI, losing `CAP_SEEK` for the whole image.

**Fix:** `map {in} {out}` → `in_path`, `out_path`, in the shared glue and in the
routing table. The comment in `vol_cpack.c` now says explicitly that `in` for
MAP is the image.

---

## Bug C — `invf-plugin-host` dereferenced `MAP_FAILED` when `/dev/shm` was small

**File:** `tools/invf-plugin-host.c`
**Function:** `main`

A slot is a flat 64 MiB, so the pool is `num_workers * 64 MiB` of `/dev/shm`.
On any host with the container-default 64 MiB tmpfs, `ftruncate()` fails with
`ENOSPC`, the code `return 1`'d **without `shm_unlink()`** (leaving a 64 MiB
turd that fills the tmpfs for everyone after it), and on a partial failure the
next line dereferenced `pool_mem == MAP_FAILED`:

```c
void *pool_mem = mmap(...);
if (pool_mem == MAP_FAILED) { perror("mmap"); return 1; }   /* shm still linked */
...
memset(hdr, 0, sizeof(*hdr));   /* SIGSEGV if mmap had failed differently */
```

Observed: `timeout 15 ./bin/invf-plugin-host -n 1` → *"the monitored command
dumped core"*, with `plugin_host_test` reporting only the downstream
`daemon failed to become available`.

**Fix:** `statvfs("/dev/shm")` first, shrink `num_workers` to what the tmpfs can
hold and *say so*; if not even one slot fits, exit 1 with an actionable message
and unlink the shm. `ftruncate`/`mmap` failures now report `errno`, close the fd
and unlink before returning.

---

## Bug D — a plugin's decline killed the worker, and its estimate vanished

**File:** `src/include/ivpack_impl.h` (new), `src/include/ivpack_api.h`
**CWE:** CWE-754 (improper check for unusual conditions)

Two properties of the existing packs make a naive `dlopen`-and-call integration
wrong:

1. **Every containerpack declines with a bare `exit(3)`** (`xfs.c:decline`,
   `fatfs.c:note`+`return RC_DECLINE`, `ntfs.c:EXIT_DECLINE`, `die()` in all of
   them). Called in-process inside a worker, the *first* declined image takes
   the worker down; `1175fe7`'s qcow2 export called `qc_parse()` directly and so
   inherited exactly that risk for anything qcow2 refuses.
2. **A pack reports its estimate by printing it.** Capturing that with a
   redirected stdout works, but the child leaves through `_exit()`, which skips
   stdio cleanup — the number dies in the child's buffer and the caller sees an
   empty string. This is not hypothetical: it is the first thing that broke when
   the glue was written (`estimate rc=-1 eligible=0 mbr_size=0` while the CLI
   printed `67338240`).

**Fix:** `ivpack_impl.h` runs every command in a `fork()`ed child of the worker:

- the child's exit status *is* the plugin's return code, so `3` still means
  decline and `1` still means error, exactly like the CLI;
- `fflush(NULL)` before `_exit()`;
- stdout is drained **concurrently** with the child (draining after `waitpid()`
  deadlocks as soon as a pack prints more than a pipe buffer);
- the child's stderr goes to `/dev/null` — a pack's diagnostics belong to a CLI
  user, not to a daemon's log;
- packs whose `main()` allocates a streaming window (`rawdisk`, `p7z`: 8 MiB
  `g_buf`) get a GLUE statement that reproduces it.

Cost: one `fork()` per call and no `execve()`, no ELF load, no dynamic linking —
which is the part ADR-007 removes. A pack that ever becomes `exit()`-free can
set `IVPACK_F_NO_FORK` in its descriptor; none does today, and the flag is
defined but not yet honoured by the glue (documented in `ivpack_api.h`).

**ABI versioning rule** (why appending was safe): `ivpack_container_args` grew
`extract_idx` + `self_path` at the *end*, `IVPACK_API_VERSION` went 1 → 2 with
`IVPACK_API_VERSION_MIN = 1`, and `invf-plugin-host` fills the v2 fields **only**
for a plugin whose `desc->api_version >= 2` and refuses to load anything below
`IVPACK_API_VERSION_MIN`. An old `.so` keeps working; a new `.so` on an old host
sees `self_path == NULL` and falls back (`p7z` then scans `/proc/self/maps`, and
still has `$P7Z_7ZZ` and PATH).

---

## Also fixed (test plumbing, no product impact)

- `src/cli/plugin_host_test.c` exec'd `tools/invf-plugin-host`, but the Makefile
  builds it to `bin/invf-plugin-host` (`TOOLS` naming), so `execl` failed with
  127 and the test reported the misleading *"daemon failed to become
  available"*. It now prefers `bin/` and keeps `tools/` as a fallback, builds the
  `.so` through `make plugin-so` instead of a hard-coded `gcc -O3` line, and
  **SKIPs** (exit 0) when `/dev/shm` cannot hold two 64 MiB slots or when
  `qemu-img`/`qemu-io` are absent — the environment-dependent legs of this repo
  degrade to SKIP, never FAIL (see `helper_exec_test.c`'s own header comment).
- `tools/pack-ivpack.sh` now emits the ADR-007 §3 layout
  (`manifest`, `sha256`, `lib/<name>.so`, `bin/<name>`), takes the pack name from
  the manifest rather than the directory, requires the `.so` to exist instead of
  silently packing a directory without one, and uses `zip -X` so the same tree
  packs to the same bytes.

---

## Validation

1. **Unit:** `make test` → `invf-ivpack_packs_test` **80 checks, 0 failures**:
   all eight `lib<name>.so` load through `dlmopen(LM_ID_NEWLM)` (with the
   `dlopen` fallback), export the three symbols, their descriptor matches the
   pack's own `manifest` (`name =`, `type = container` → `containerpack`),
   `api_version >= IVPACK_API_VERSION_MIN`, operand validation answers
   **negative** (the CLI-fallback cue) for NULL args / unknown cmd / ENUMERATE
   without `out_path`, and estimating a non-image returns **3** without taking
   the test process down (Bug D's fork guard, exercised 8×).
2. **E2E:** `bash tools/test-ivpacks.sh` → **0 failures, 4 skips**. Per pack:
   descriptor + manifest agreement, decline parity with the CLI on a non-image,
   and — for the four packs whose fixture tooling exists here (`rawdisk` GPT,
   `qcow2` via qemu-img, `vdi` via qemu-img convert, `ext4fs` via
   mkfs.ext4+debugfs) — a full **enumerate → extract every member → strip →
   rebuild → `cmp` against the original, bit-exact**, then the same five calls
   through the CLI with `table`/`recipe`/`map`/every member compared byte for
   byte, plus estimate parity (`rawdisk 67338240`, `qcow2 67305472`,
   `vdi 68157440`, `ext4fs 67430262`). Skips: `fatfs` (needs mkfs.vfat+mtools),
   `ntfs`/`xfs` (need mkfs + a sudo loop mount), `p7z` (needs `7zz`).
   This is the leg that pins Bugs A and B: a swapped operand shows up as a
   failed rebuild, not as a vague error.
3. **Daemon:** `bash tools/plugin-daemon-smoke.sh` (and `PACK=qcow2 …`) drives
   the *real* stack — `invf-plugin-host` ← `vol_plugin_client` ← `librawdisk.so`
   / `libqcow2.so`: estimate over IPC (`mbr_size=67338240`, identical to the
   CLI), ENUMERATE, STRIP, EXTRACT idx=1,2,3, REBUILD, then `cmp` of the rebuilt
   image against the original (**bit-exact**) and of every member against what
   the CLI helper extracts (**identical**). Needs 64 MiB of `/dev/shm` per
   worker; exits 0 with a SKIP line otherwise.
4. **Build gates:** all eight packs still compile as CLIs with their own
   documented command (`cc -std=c11 -O2 -Wall -Wextra -Werror <pack>.c -lz`) and
   as plugins with `-Wall -Wextra -Werror` — the glue is compiled into *both*
   builds (only `main()` is `#ifndef IVPACK_SHARED_LIB`), so the `.so` and the
   CLI cannot drift.
5. **No regressions:** the other 13 unit suites are unchanged. Two pre-existing
   failures on this machine are environment artifacts, identical before and after
   this WP (verified by running the baseline worktree at `1175fe7`):
   `helper_exec_test` "Landlock denies a read outside the whitelist" (the
   sandbox's seccomp makes `landlock_restrict_self` a no-op while the ABI probe
   still reports v6) and `deflate_repro_test` "discovered level and mem_level
   match expected" (on zlib 1.2.13 two levels produce byte-identical streams, so
   the search legitimately returns a different one). `tools/test-qcow2.sh`
   passes every pack-level leg here (6 fixtures + `compc` + 11 declines +
   fuzz-lite 30/30 bit-exact) and then fails at `sweep #1` — **the baseline
   fails at the same line with the same message**, so it is not this WP.

---

## Deliverables

- New: `src/include/ivpack_impl.h`, `src/cli/ivpack_packs_test.c`,
  `tools/test-ivpacks.sh`, `tools/ivpack_probe.c`, `tools/plugin-daemon-smoke.sh`.
- Changed: `src/include/ivpack_api.h` (ABI v2), `src/core/invf_plugin_ipc.h`
  (IPC v2 + `extract_idx`), `src/core/vol_plugin_client.[ch]`,
  `src/core/vol_cpack.c`, `tools/invf-plugin-host.c`, `tools/pack-ivpack.sh`,
  `src/cli/plugin_host_test.c`, `Makefile`, and the eight pack `.c` files
  (exports + `main()` guard only).
- `INCIDENTS.md`: entries for Bugs A–C (they were live on `main`).
- Docs: `impl_docs/WP71-ivpack-all-packs.md` (this file).

---

## Out of scope (do NOT touch)

- **Codec-pack ABI.** `ivpack_api.h` only describes containerpacks. `jxl`
  (`cjxl`/`djxl` + `jxlest`) and `raw_image` (`raw_image.py`) are `wholefile`
  codec packs with `encode`/`decode` semantics; they need their own
  `ivpack_codec_*` entry points before they can be `.so` plugins.
- **Python packs.** `splt_test` (fixture, algo 40) and `raw_image` are
  interpreted; there is nothing to `dlopen`. An embedding story (or a C port)
  is a separate WP.
- **Loading a plugin *from* an `.ivpack`.** `pack-ivpack.sh` produces the
  archive and `test-ivpacks.sh` verifies that the `.so` inside it still loads
  once unpacked, but `invf-plugin-host` still takes a filesystem `.so` path.
  ZIP-0 central-directory lookup + `memfd`/`mmap` loading (ADR-007 §3's
  zero-copy claim) is the next WP.
- **On-volume `.invariantfs/{packs,old_packs}`, sweep migration, `invfs-pack gc`**
  (ADR-007 §2) — nothing here writes to a volume.
- **In-process execution** (`IVPACK_F_NO_FORK`) and the worker-side sandboxing
  ADR-007 §4 asks for (`CLONE_NEWNET`, drop to `nobody`, Landlock per worker).
  Today the worker is unsandboxed and the fork guard is what keeps a pack's
  `exit()`/crash from taking the pool down.
- **Worker restart on crash.** ADR-007 §4 promises a fresh worker in ≈1 ms after
  a `SIGSEGV`; `invf-plugin-host` currently does not reap and respawn.

---

## Coordination notes

- Subagent ID for this WP: `wp71-ivpack-all-packs`
- Pass via `INVFS_E2E_AGENT=wp71-ivpack-all-packs` when invoking e2e.
- E2E gates to run:
  - `bash tools/run-e2e.sh tools/test-ivpacks.sh` (new, added to `make e2e`)
  - `bash tools/run-e2e.sh tools/test-qcow2.sh` (the pack's own gate)
  - `bash tools/run-e2e.sh tools/test-rawdisk.sh`, `tools/test-ext4fs.sh`,
    `tools/test-vdi.sh`, `tools/test-p7z.sh`, `tools/test-ntfs.sh`,
    `tools/test-xfs.sh`, `tools/test-fatfs.sh` — the packs whose `.c` this WP
    touched. **Not run by this agent**: this machine has no loop device, no
    `/dev/fuse` and no sudo mount, so the FS legs of those suites cannot pass
    here regardless of the change (the pack-level legs did run, inside
    `test-ivpacks.sh`). Please run them on the real box before merging.
  - `bash tools/plugin-daemon-smoke.sh` on a host with ≥128 MiB `/dev/shm`.
- Dependencies: builds on `1175fe7`; none of the open WP branches conflict
  (this WP touches no format code).
- Environment notes for whoever reproduces: the plugin `.so` files, the CLI
  helper binaries under `tools/codecpacks/*/bin/`, `dist/ivpack/` and `build/`
  are all gitignored artifacts — `make plugin-so` and `make ivpacks` regenerate
  them. `tools/busybox-src` is a submodule and must be initialised
  (`git submodule update --init --depth 1 tools/busybox-src`) before
  `test-qcow2.sh` can build its text fixture.
