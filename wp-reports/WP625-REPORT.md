WP: wp/625-dlopen-loader

## Status summary (read this first)

The WP625 implementation was already **complete and committed** on this branch
when I picked it up (legs 1-6, `212eb8a..51ac1d0`), and `main` already carries
it as **`487b4b0` "lane: in-process codec plugins (dlopen loader, jxl pilot,
default IPC)"** — the squash-merge of this branch plus your conflict resolution.
Verified, not assumed: for every WP625 file the branch content is
**byte-identical to `main`** (`git diff HEAD main -- <loader/test/docs>` is
empty for `src/core/vol_plugin_inproc.c/h`, `src/cli/dlopen_trust_test.c`,
`tools/test-dlopen-trust.sh`, `docs/SECURITY.md`). `main`'s remaining
`fuse_fs.c` / `codec.c` deltas are later, unrelated WPs (WP623 shutdown, `@`
mark, etc.), not WP625 drift.

So this session was **verification + one real fix**, not a from-scratch
implementation. One genuine defect was found and fixed (leg 7), then all the
gates were run and pass.

Branch base: `5d28168`. Local `main` has since advanced ~20 commits
(`d6b0245` wp633 … `c4a9727` wp642); branch-to-main merge is yours, untouched.

## Files changed

Legs 1-6 (pre-existing on this branch, unchanged by me):

```
src/core/vol_plugin_inproc.c        (new) three-gate loader: pilot allowlist, anchored origin
                                          (realpath both sides), per-dispatch ed25519 re-verify;
                                          fixed soname libjxlest.so (never manifest-controlled);
                                          forked probe child (crash/hang -> IPC, daemon survives);
                                          RTLD_NOW|RTLD_LOCAL without DEEPBIND; dev+ino cached
                                          handle; dlclose on unload/reset; 5 s bounded estimate
                                          (est_call_bounded, pthread_timedjoin_np)
src/core/vol_plugin_inproc.h        (new) versioned ABI (INVFS_CODEC_ABI_MAJOR 2), gate API
src/cli/dlopen_trust_test.c         (new) 61-cell unit trust matrix (+ embedded hostile .so fixtures)
tools/test-dlopen-trust.sh          (new) e2e trust matrix: routing per cell, twin-volume
                                          default-vs-opt-in byte identity, informational perf leg
docs/SECURITY.md                    (new section) "In-process plugins (WP625)": threat model,
                                          why each gate is load-bearing, what stays untrusted
src/cli/fuse_fs.c                   -o allow_inprocess_plugins= (default OFF) +
                                          inprocess_{allow,hit,fallback,route} in user.invfs.stats
src/cli/pack.c                      invf-pack inproc <pack>: read-only three-gate report
src/codecs/codec.c/.h               pack reg-dir / is-volume loader inputs; unload_all on reset
src/core/vol_cpack.c                jxl estimate tries in-process first, else pool/exec (unchanged)
Makefile                            CORE_O += vol_plugin_inproc + pack_sig, -ldl, unit run line
tools/test-migrate.sh, test-seal.sh, test-seal-heal.sh, test-unlink-rollback.sh,
src/cli/cpack_map_san_test.c        (leg5) link fallout for minimal-link harnesses
tools/check-vol-find.sh             (leg4) vol-find ledger re-pointed after line shifts
```

Changed by me this session (leg 7, commit `7cb61d9`):

```
Makefile                            1 line: add $(OUT)/invf-dlopen_trust_test to
                                    TEST_SHARD_DEPS. The trust-matrix run line (leg 3)
                                    shipped without its build prerequisite; dlopen_trust_test
                                    is declared by a bare TOOL_RULE, so it is in neither
                                    CLI_MAINS (TEST_BINS) nor TOOLS (all). A clean-tree
                                    `make test` therefore executed a binary nothing built.
                                    Same one-line fix you applied in the squash (487b4b0).
                                    NOT inline-commented: a '#' inside a backslash-continued
                                    assignment truncates the rest of the list (verified:
                                    "missing separator").
```

## Tests run

```
$ make -j2 all
BUILD_EXIT=0
```

```
$ make -j2 test
...
INVFS_CODECPACKS_SYS=0 PATH="..." bash tools/run-unit-isolated.sh bin/invf-dlopen_trust_test /tmp
ok t_spec: opt-in parsing
ok t_origin: origin gate
ok t_sig_gate: signature gate
ok t_probe: probe matrix (daemon survived every hostile leg)
ok t_estimate: estimate routing
bin/invf-dlopen_trust_test: 61 checks, PASS (hit=2 fallback=4 last=ipc: not a pilot pack (or null args))
...
layer A ran and passed. Set INVFS_CODECPACK_STRICT=1 to make this a failure.
TEST_EXIT=0
```

`make test` includes `tools/check-repo-hygiene.sh`, `check-codecpack-sync.sh`,
`tools/lint-test-heredocs.sh`, `tools/plugin-daemon-smoke.sh` and the
`check-test-shards` drift gate — all green in the same run.

```
$ INVFS_E2E_AGENT=wp625-dlopen-loader bash tools/run-e2e.sh tools/test-dlopen-trust.sh
ok   unsigned local pack REFUSED in-process
ok   pack outside admin roots not even found (no silent trust)
ok   signed local pack ELIGIBLE (modulo -o flag)
ok   origin gate re-verified (scratch/symlink/volume refusals)
ok   twin volumes built (text+binary+2 JPEGs, signed jxl pack installed)
ok   default sweep stayed IPC/exec (no opt-in, no in-process)
ok   opt-in sweep served estimates in-process (log proves routing)
ok   a.bin / numbers.txt / hello.txt / p1.jpg / tiny.jpg byte-identical (corpus == default == opt-in)
ok   fsck clean (default)
ok   fsck clean (opt-in)
inprocess:   200 calls wall 0.028s mean 142.1us/call p50 139.6us p99 181.0us
exec-helper:  50 calls wall 0.017s mean 338.7us/call p50 329.2us p99 565.0us
== result: pass=18 fail=0 ==
E2E_TRUST_EXIT=0
```

```
$ INVFS_E2E_AGENT=wp625-dlopen-loader bash tools/run-e2e.sh tools/test-writepath.sh
  mmap read bit-exact: swept.txt / swept.tar / swept.bin / dup1.bin / dup2.bin / big.bin (300MB)
WRITEPATH E2E: PASS
E2E_WRITEPATH_EXIT=0
```

```
$ INVFS_E2E_AGENT=wp625-dlopen-loader bash tools/run-e2e.sh tools/test-jxl.sh   # optional photo lane
JXL E2E (codecpack lane): PASS
E2E_JXL_EXIT=0
```

Perf (informational, not a gate): in-process estimate ≈142 µs/call
(p50 139.6, p99 181.0) vs one fork+exec of the CLI helper ≈339 µs/call
(p50 329.2, p99 565.0) — ~2.4× on the estimate alone, and this is the *floor*
of the win (the pool IPC hop is heavier than fork+exec). Numbers are from a
single mixed corpus of 5 files; not worth quoting in `docs/benchmarks/`.

Two failures were seen and both were **my invocation, not the code** — recorded
so they are not mistaken for WP625 defects on re-run:

1. `make test` with `TMPDIR=/dev/shm/wp625-tmp`: `TESTISO`
   (`tools/run-unit-isolated.sh`) gives each test a private `/dev/shm`, so my
   `TMPDIR` vanished inside the namespace and paths collapsed to `/src`,
   `/7zz.log`. Re-ran with no `TMPDIR`: green.
2. `tools/test-writepath.sh` leg [C]: `FAIL: swept.txt not TEXT post-sweep` —
   `bin/meta_probe` had never been built (`make e2e` builds it; `all`/`test` do
   not), so `class_of` printed nothing. After `make -j2 bin/meta_probe`: PASS.

One genuine environment flake, unrelated to this WP and untouched by it:
`invf-helper_exec_test` ("fork-bomb children are gone after the group kill",
`survivors=31`) failed once while 3 other agents were building on this box, and
passed 2/2 standalone runs at low load immediately after.

Result: PASS

Remaining TODOs:

- **Perf leg at corpus scale — not run (box: 7.7 GB RAM, `/tmp` is a 3.8 GB
  tmpfs at 92-100 % during the session).** The informational perf numbers above
  come from a 5-file corpus. For the real comparison:
  `INVFS_ALLOW_INPROCESS_PLUGINS=jxl bash tools/run-e2e.sh tools/test-jxl.sh`
  on a ≥500-photo corpus, and `bash tools/test-dlopen-trust.sh` leg 5 re-read
  from the log, then compare wall + p50/p99 per-file-op latency IPC vs
  in-process. Only quote it in `docs/benchmarks/` if the delta survives the
  larger corpus.
- **Branch is based on `5d28168`; local `main` is ~20 commits ahead.** The
  WP625 files are byte-identical to `main`'s `487b4b0`, so this is a
  merge/ancestry decision, not a content conflict. Your call; I did not rebase,
  merge, or push.
- **Trust-gate review is yours by spec** ("line-by-line before merge, no
  exceptions"). Reviewer's map: gate order and reasons in
  `invfs_inproc_eligible` (`src/core/vol_plugin_inproc.c`); origin roots and
  `real_under` realpath-both-sides in `invfs_inproc_origin_anchored`; probe
  child + `wait_ceiling` SIGKILL in `probe_child`/`wait_ceiling`; the fixed
  soname `INPROC_JXL_SONAME`; and the new leg-6 bound, `est_call_bounded`.
  Two deliberate, documented widenings worth a second pair of eyes:
  `$INVFS_ALLOW_INPROCESS_PLUGINS` as the offline-tool opt-in
  (explicit `-o` wins; daemon env is operator-controlled), and the
  cache-full path in `invfs_inproc_load`, which returns an uncached handle and
  leaks one mapping (bounded by call count, documented).
- **Disk discipline, disclosed:** `/tmp` (3.8 GB tmpfs) was 100 % full and my
  first build died with `No space left on device`. I reclaimed two abandoned
  WP640 NTFS scratch trees — `/tmp/ntfs-hand-3743737` and
  `/tmp/ntfs-hand2-3743775`, ~311 MB, no file touched in >24 h, no open fds, no
  loop mounts, WP640 already merged. I did **not** touch `/tmp/wp641t` (2.1 GB,
  another WP's images) or any other worktree. No residue from me: the trust
  suite cleans its own scratch and no `/tmp/invfs-wp625-*` remains.

Result: PASS
