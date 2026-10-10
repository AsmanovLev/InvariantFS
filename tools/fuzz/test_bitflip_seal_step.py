#!/usr/bin/env python3
"""test_bitflip_seal_step.py — WP301 regression probe for tools/fuzz/bitflip.py.

Guards the setup-path defect behind INCIDENTS.md "test-fuzz bitflip +
test-usr1-savepoint": the seal step used to invoke `invf-sweep --seal`
TWICE per --seal-every iteration (a raw tool() probe, then the same sweep
AGAIN via setup_tool() in the elif). Once native v3 seal landed (WP201),
the re-run could exit nonzero ("swept=0 skipped=6" + a seal-stage failure)
and was misrecorded as `setup: invf-sweep rc=1 on a fresh image` -- with
the argv hidden, so it read as the plain setup sweep declining every file,
exactly on the --seal-every iters (7, 15, 23, ...).

Method: drive bitflip.one_iteration() for a seal iter (it=7, seal_every=8)
with fz.BIN pointed at counting shims (no real binaries, no real image --
the mkfs shim writes just enough superblock for fuzzutil.Layout, the cat
shim copies back the byte-identical corpus). Two scenarios:

  A (seal ok): --seal must run EXACTLY ONCE, no failure, "sealed;" kept in
     the mutation string.
  B (seal fails rc=1, Oct-9 shape): --seal must run EXACTLY ONCE and the
     single failure must name the seal step (`invf-sweep --seal`), never a
     fresh-image `setup` failure, and must not be recorded twice.

Deterministic (fixed seed/iter), stdlib only, ~1 s. Exit 0 iff all
assertions hold; prints PASS/FAIL per assertion.
"""
import collections
import os
import stat
import subprocess
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fuzzutil as fz
import bitflip

SEED = 0x1A2B3C4D
IT = 7  # 7 % 8 == 7: a --seal-every iter with the default seal_every=8

SHIM_SWEEP = """#!/bin/bash
# args: <img> [--seal]
echo "sweep $*" >> "$SHIM_LOG"
if [ "${2:-}" = "--seal" ]; then
  n=$(cat "$SHIM_COUNT" 2>/dev/null || echo 0)
  echo $((n + 1)) > "$SHIM_COUNT"
  if [ "$SHIM_SEAL_MODE" = "fail" ]; then
    echo "sweep done: swept=0 skipped=6 failed=0" >&2
    echo "sweep: 0 swept" >&2
    echo "seal failed" >&2
    exit 1
  fi
fi
exit 0
"""

SHIM_MKFS = """#!/bin/bash
# args: <img> <size_gb> -- write a minimal parseable superblock, sparse rest
python3 - "$1" <<'EOF'
import struct, sys
p = sys.argv[1]
sb = bytearray(4096)
sb[0:8] = b"InvariFS"
# total, meta_start, meta_blocks, raw_start, raw_blocks, shadow_start, shadow_blocks
struct.pack_into("<7Q", sb, 0x20, 30720, 16, 12288, 20000, 1000, 20000, 10720)
with open(p, "wb") as f:
    f.write(bytes(sb))
    f.truncate(30720 * 4096)
EOF
"""

SHIM_CP = "#!/bin/bash\nexit 0\n"
SHIM_OK = "#!/bin/bash\nexit 0\n"
SHIM_CAT = """#!/bin/bash
# args: <img> <name> <outp> -- serve the corpus back bit-exact
cp "$SHIM_STATE/$2" "$3"
"""


def make_fakedir(state_dir):
    fakedir = tempfile.mkdtemp(prefix="wp301-fakebin-")
    shims = {
        "invf-mkfs": SHIM_MKFS,
        "invf-cp": SHIM_CP,
        "invf-sweep": SHIM_SWEEP,
        "invf-fsck": SHIM_OK,
        "invf-verify": SHIM_OK,
        "invf-cat": SHIM_CAT,
    }
    for name, body in shims.items():
        p = os.path.join(fakedir, name)
        with open(p, "w") as f:
            f.write(body)
        os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR | stat.S_IXGRP)
    return fakedir


def run_iter(seal_mode):
    """Run bitflip.one_iteration(IT) against shims. Returns (fails, mutation, seal_count)."""
    workdir = tempfile.mkdtemp(prefix="wp301-probe-wd-")
    state = os.path.join(workdir, "case", "orig")
    os.makedirs(os.path.join(workdir, "case", "out"))
    log = os.path.join(workdir, "calls.log")
    count = os.path.join(workdir, "sealcount")
    fakedir = make_fakedir(state)
    env = dict(os.environ, SHIM_STATE=state, SHIM_LOG=log,
               SHIM_COUNT=count, SHIM_SEAL_MODE=seal_mode)
    # fz.run inherits os.environ; point shims at per-run state via env.
    old_env = dict(os.environ)
    os.environ.update(SHIM_STATE=state, SHIM_LOG=log,
                      SHIM_COUNT=count, SHIM_SEAL_MODE=seal_mode)
    old_bin = fz.BIN
    fz.BIN = fakedir
    old_cov = bitflip.COVERAGE
    bitflip.COVERAGE = collections.Counter()
    try:
        args = types.SimpleNamespace(seed=SEED, imgbase="wp301probe-%d" % os.getpid(),
                                     workers=1, size_gb="0.12",
                                     seal_every=8, workdir=workdir)
        keep = os.path.join(workdir, "case")
        fails, anoms, mutation = bitflip.one_iteration(args, IT, keep)
    finally:
        fz.BIN = old_bin
        bitflip.COVERAGE = old_cov
        os.environ.clear()
        os.environ.update(old_env)
    try:
        with open(count) as f:
            seal_count = int(f.read().strip())
    except (OSError, ValueError):
        seal_count = 0
    # one_iteration unlinks the sham image itself; sweep the leftovers.
    for p in (os.path.join("/dev/shm", "wp301probe-%d-w0.img" % os.getpid()),):
        try:
            os.unlink(p)
        except OSError:
            pass
    import shutil
    shutil.rmtree(workdir, ignore_errors=True)
    shutil.rmtree(fakedir, ignore_errors=True)
    return fails, mutation, seal_count


def check(name, cond, extra=""):
    print("%s: %s %s" % ("PASS" if cond else "FAIL", name, extra))
    return cond


def main():
    ok = True
    fails, mutation, n = run_iter("ok")
    ok &= check("A.seal-runs-once", n == 1, "(--seal invocations=%d)" % n)
    ok &= check("A.no-failure", len(fails) == 0,
                "(failures=%d %s)" % (len(fails), [str(f) for f in fails]))
    ok &= check("A.sealed-marker", "sealed;" in mutation,
                "(mutation=%r)" % mutation)
    fails, mutation, n = run_iter("fail")
    ok &= check("B.seal-runs-once", n == 1, "(--seal invocations=%d)" % n)
    ok &= check("B.single-failure", len(fails) == 1,
                "(failures=%d %s)" % (len(fails), [str(f) for f in fails]))
    if len(fails) == 1:
        ok &= check("B.names-seal-step", "--seal" in str(fails[0]),
                    "(detail=%r)" % str(fails[0]))
        ok &= check("B.not-fresh-image-setup", "on a fresh image" not in str(fails[0]),
                    "(detail=%r)" % str(fails[0]))
    print("PROBE %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
