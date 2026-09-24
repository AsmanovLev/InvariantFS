# WP74 — e2e helper link lists + test-p7z skip guard

**Branch:** `wp/74-test-link-and-p7z`
**Worktree:** `/home/user/invfs-wp74`
**Severity:** MEDIUM (test infrastructure; blocks ~20 e2e suites)
**Source:** `make e2e` recon (30/33 suites red on `main`)
**Estimated effort:** hours

---

## Scope

Make the e2e suites that compile a C helper against the engine build
reliably again, and stop `test-p7z.sh` from hard-FAILing when its optional
`7zz` helper is absent. No product code changes.

---

## Bug A — stale hardcoded object lists in test helpers

~24 `tools/test-*.sh` compile a probe helper with a literal object list:

```
$REPO/build/obj/{volume,vol_cpack,helper_exec,...,vol_dirs,vol_tier,arc,...,rs}.o
```

That list predates Meta-v3 and the WP71 plugin client, so the link fails:

```
vol_dirs.c: undefined reference to `vol_v3_inode_get'
vol_dirs.c: undefined reference to `vol_v3_dirent_delta_put'
vol_cpack.c: undefined reference to `invfs_plugin_pool_container_cmd'
collect2: error: ld returned 1 exit status
```

Only `test-fuzz.sh` and `test-rollback.sh` include the v3 objects
(`vol_metabuf,vol_btree,vol_delta,vol_fold,vol_reclaim,vol_spt0`), and even
they miss `vol_plugin_client.o`.

**Fix (preferred):** stop hand-maintaining the list. Have the Makefile emit
the canonical core object list to a file at build time, e.g.
`build/core_objs.txt`, and change every helper `gcc` line to read it:

```sh
gcc ... -o "$WORK/tools/probe" "$WORK/tools/probe.c" \
    $(cat "$REPO/build/core_objs.txt") -Wl,-l:libzstd.so.1 -lz -lpthread
```

Keep the object list as objects (NOT a `.a` archive) so link order does not
matter. Make the emission target part of `all` so `make` always refreshes it.

**Do not** change the product link rules; only the test helpers.

Affected suites (at minimum): `test-seal, test-jxl, test-pngflac, test-rawimg,
test-exercarve, test-containerpack, test-sandbox, test-rawdisk, test-ext4fs,
test-fatfs, test-xfs, test-ntfs, test-vdi, test-resize, test-rollback,
test-dynzone, test-rocp, test-fuzz, test-flushfail, test-p7z`.

---

## Bug B — `test-p7z.sh` hard-FAILs when `bin/7zz` is missing

`tools/test-p7z.sh:77` does `[ -x "$B/7zz" ] || { echo FAIL; exit 1; }`.
Nothing in the repo builds `bin/7zz`; the vendored `tools/7-Zip-zstd/` tree is
gitignored and CI never checks it out.

**Fix:** environment-dependent helper ⇒ SKIP (exit 0), matching repo
convention (`test-qcow2.sh:66`, `test-ivpacks.sh:285-293`). Optionally resolve
a real `7zz` first (vendored `tools/7-Zip-zstd/CPP/7zip/Bundles/Alone2/_o/7zz`
or `command -v 7zz`), export the resolved path as `P7Z_7ZZ`, and only SKIP if
none exists. Note `p7z.c:770-792` uses `$P7Z_7ZZ` unconditionally, so the
export must be the resolved path.

---

## Validation

1. `make test` — must pass unchanged.
2. For each previously link-broken suite, run:
   `INVFS_E2E_AGENT=wp74-test-link bash tools/run-e2e.sh tools/<suite>.sh`
   The suite must at least get **past the helper link** (a later, unrelated
   red is acceptable and expected for the suites owned by WP75–WP78).
3. `bash tools/run-e2e.sh tools/test-p7z.sh` must SKIP cleanly (exit 0) with
   no `7zz`, and run when one is resolvable.
4. `bash tools/check-repo-hygiene.sh` — must stay OK.

---

## Out of scope (do NOT touch)

- Any `src/**` product code.
- The assertion/content bugs owned by WP75–WP78.
- `INCIDENTS.md` / `AUDIT.md` status fields.

---

## Coordination notes

- Subagent ID: `wp74-test-link`
- E2E gates: the link-affected suites above + `test-p7z.sh`.
- Dependencies: none. This WP unblocks e2e verification for WP75–WP78.
