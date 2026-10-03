#!/usr/bin/env python3
"""corpus.py -- the C-source corpus every container/suite fixture wants.

    import sys, os
    sys.path.insert(0, os.environ["REPO"] + "/tools")
    from corpus import c_sources
    for p in c_sources(20000, n=5):
        ...

WHY THIS EXISTS. Eight gate suites build a fixture out of C sources, and for
a long time each of them found those sources its own way. Six of them walked
`tools/busybox-src`, which is a git SUBMODULE: `actions/checkout@v7` does not
recurse submodules, so on CI and on every fresh clone it is an EMPTY directory
and the suite died on its own fixture --

    tar: Cowardly refusing to create an empty archive
    AssertionError: no busybox .c fixture found
    AssertionError: no big busybox .c found

-- before it had asserted anything about the product. Two of the eight also
hardcoded the author's absolute checkout path, in scripts that had already
derived $REPO correctly.

The submodule is a PREFERENCE, not a requirement: this module falls back to
the tree's own `src/`, which is the same kind of input (hundreds of .c files,
many of them large) and is present on every checkout by definition. Selection
is by SIZE rather than by a hardcoded relative path, because "walk a named
busybox directory for one specific file" is what made it brittle.

If there is no corpus at all, `c_sources` yields nothing and the caller is
expected to say so and skip -- not to build a fixture out of nothing and
report the resulting failure as if the product were at fault. `require()`
below does that skipping for you.

POSIX-ish python3, no dependencies, no side effects on import.
"""

import os
import sys

__all__ = ["c_sources", "corpus_root", "require"]


def corpus_root(repo=None):
    """Echo a directory that contains .c files: the submodule if it has any,
    else the tree's own src/. Returns None when neither does."""
    repo = repo or os.environ.get("REPO") or os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))
    for cand in (os.path.join(repo, "tools", "busybox-src"),
                 os.path.join(repo, "src")):
        if not os.path.isdir(cand):
            continue
        for _r, _d, fs in os.walk(cand):
            if any(f.endswith(".c") for f in fs):
                return cand
    return None


def c_sources(min_bytes=0, n=None, repo=None):
    """Yield .c files of at least `min_bytes`, largest first, up to `n`.

    Largest-first rather than directory order, so a corpus chosen here does
    not depend on how the filesystem happens to list a directory -- which is
    what made the previous "pick a named busybox path" approach produce
    different fixtures on different machines (and the suite's own comment
    records a result that "differed with" the worktree).
    """
    root = corpus_root(repo)
    if root is None:
        return
    found = []
    for r, ds, fs in os.walk(root):
        ds.sort()
        for f in fs:
            if not f.endswith(".c"):
                continue
            p = os.path.join(r, f)
            try:
                sz = os.path.getsize(p)
            except OSError:
                continue
            if sz >= min_bytes:
                found.append((sz, p))
    found.sort(key=lambda t: (-t[0], t[1]))
    for _sz, p in found[:n] if n else found:
        yield p


def require(min_bytes=0, n=None, repo=None):
    """c_sources, or print why there is no corpus and exit 0 (a skip).

    A skip is not a pass and not a failure: it is the suite saying out loud
    that it cannot measure here. The old behaviour -- assert on an empty walk
    and let tar/python report the consequence -- is what made a missing
    fixture look like a product defect.
    """
    paths = list(c_sources(min_bytes, n, repo))
    if not paths:
        sys.stderr.write(
            "SKIP: no C sources at or above %d bytes under %s\n"
            % (min_bytes, corpus_root(repo) or os.environ.get("REPO", "?")))
        raise SystemExit(0)
    return paths
