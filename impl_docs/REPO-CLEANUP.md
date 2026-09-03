# REPO-CLEANUP: purging `var/` from git history (WP-RC prep, 2026-09-04)

`var/` (local staging/VM trees) is untracked at every current branch tip and
ignored since the `.gitignore` `var/` entry, but it was committed in early
history and still dominates the object store. This document is the tested
command sequence for the **later, separate** act of removing it from history.
**Do not run it casually** — it rewrites every commit after `var/` first
appeared (all commit IDs change).

Status: prepared only. Tested against a `--mirror` scratch clone of this repo
on 2026-09-04; the live repo was NOT modified.

## Measured facts (this repo, 2026-09-04)

| metric | before | after (scratch clone) |
|---|---|---|
| `.git` size | 364 MiB (350.55 MiB in one pack + 11 MiB loose) | **5.8 MiB** (5.65 MiB pack) |
| objects | 44,810 packed + 882 loose | 1,904 packed |
| commits | 111 | 111 (no commit touched only `var/`, so `--prune-empty` dropped none) |
| `main` tip tree | `63a851460aa490e233b5323e74aaee6a2b8beb30` | identical (var/ is absent from the tip) |
| branches | 15 | 15 (all preserved; `wp22e`/`wp23` untouched — they predate `var/`) |

`var/` footprint in history: 43,611 objects, 40,902 blobs,
897.8 MiB uncompressed — ~95% of all historical blob bytes. On disk the
(untracked) `var/` tree itself is ~7.1 GB and stays untouched; only its
*history* is purged.

## Tool choice

`git filter-repo` is **not installed** on this box (checked 2026-09-04) and
the box is offline, so the tested path is `git filter-branch`. If
filter-repo ever becomes available, prefer it:

```
git filter-repo --path var/ --invert-paths --force
```

(it does the refs/original + reflog + gc cleanup itself; skip to
"Post-purge" then). The rest of this document is the filter-branch path.

## Pre-checks (all must hold)

1. **Folder backup is current** — the user's folder-level backup of the repo
   is the rollback mechanism. Verify it covers the current state before
   starting.
2. **All work is merged**: every worktree branch either merged into `main`
   or deliberately abandoned. There are 12 worktrees at prep time
   (`git worktree list`): acl, cfix, dynzone, l2pq, p7zfix, pkg, repoclean,
   rocp, sandbox, w12c, wp25, wp26 (+ the main checkout).
3. **All worktrees removed**: `git worktree remove <path>` for each. A
   worktree pins a per-worktree HEAD/reflog; purging with worktrees
   attached leaves stale admin dirs and risks confusion. Re-create any
   needed worktrees after the purge.
4. **Stash empty**: `git stash list` shows nothing (the WP20-era stash was
   dropped in WP-RC after verifying its content landed in `e1be4e22`
   "WP20b"). filter-branch does not rewrite `refs/stash`; a leftover stash
   would keep the old object graph alive.
5. No uncommitted changes in the main checkout (`git status` clean).

## Command sequence (tested on a scratch mirror clone, 14 s + gc)

Run in the **main checkout** (`/home/user/InvariantFS`):

```
# 0. optional extra safety net (cheap): a bundle of all refs pre-purge
git bundle create /path/on/backup-disk/invfs-pre-purge.bundle --all

# 1. rewrite every branch, dropping var/ from every tree
git filter-branch --force --prune-empty \
    --index-filter 'git rm -r --cached --ignore-unmatch var/' \
    --tag-name-filter cat -- --all

# 2. drop the pre-rewrite refs filter-branch keeps
rm -rf .git/refs/original/

# 3. expire all reflogs, then collect garbage
git reflog expire --expire=now --all
git gc --aggressive --prune=now
```

Verification afterwards:

```
git rev-list --objects --all -- var/ | wc -l   # expect 0
git rev-parse main^{tree}                       # expect 63a851460aa490e233b5323e74aaee6a2b8beb30
du -sh .git                                     # expect ~6 MiB
make test                                       # unit tier still green
```

Scratch-clone rehearsal (exactly what was tested):

```
git clone --no-local --mirror file:///home/user/InvariantFS /tmp/invfs-purge-test
cd /tmp/invfs-purge-test
git filter-branch --force --prune-empty \
    --index-filter 'git rm -r --cached --ignore-unmatch var/' \
    --tag-name-filter cat -- --all
rm -rf refs/original/
git reflog expire --expire=now --all
git gc --aggressive --prune=now
# result: 352 MiB -> 5.8 MiB, main tip tree identical, 111 -> 111 commits,
# all 15 branches present
```

## Post-purge

- Re-create worktrees as needed (`git worktree add ...`) — old worktree
  admin dirs under `.git/worktrees/` from before the purge are stale;
  `git worktree prune` clears them.
- All pre-purge commit IDs are invalid. Update any notes/docs referencing
  them (the impl_docs WP papers reference SHAs like `5c449f80`; they are
  historical prose, acceptable to leave).
- No remotes are configured (`git remote -v` is empty), so no force-push
  step is needed.

## Rollback

The user's folder backup of the repo predates the purge and is the primary
rollback: restore the folder, done. If the optional bundle (step 0) was
made, `git clone invfs-pre-purge.bundle restored/` also reconstructs every
pre-purge ref. After `git gc --prune=now` the old objects are gone from the
live repo — there is no in-repo undo.
