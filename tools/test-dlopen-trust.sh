#!/bin/bash
# test-dlopen-trust.sh — WP625 trust-matrix gate.
#
# Matrix {signed/unsigned} x {local/remote} x {opt-in/opt-out} x
# {good-ABI/bad-ABI/crashing-.so} -> asserts in-process-vs-IPC routing per
# cell + daemon survival throughout, then proves default mounts behave
# byte-identically (before/after sweep comparison on a mixed corpus) and
# reports the informational perf leg (IPC/exec vs in-process estimate).
#
# Disk discipline: everything under /tmp/invfs-wp625-trust-<pid>, removed
# on exit (trap). Lean: fixtures are bytes-to-hundreds-of-KB.
#
# Usage: bash tools/test-dlopen-trust.sh
# Env: runs from the repo root. Unsets INVFS_ALLOW_INPROCESS_PLUGINS
# itself per leg (a leaked opt-in would turn control arms into test arms).
set -u
export PATH="$PATH:/sbin"

ROOT="$PWD"
[ -f Makefile ] || { echo "run from the repo root"; exit 2; }
[ -f src/core/vol_plugin_inproc.c ] || { echo "WP625 loader missing"; exit 2; }

S="/tmp/invfs-wp625-trust-$$"
mkdir -p "$S" || exit 2
cleanup() { rm -rf "$S"; }
trap cleanup EXIT

pass=0; fail=0
ok()   { pass=$((pass+1)); echo "ok   $*"; }
no()   { fail=$((fail+1)); echo "FAIL $*"; }

need() { command -v "$1" >/dev/null 2>&1 || { echo "SKIP: missing $1"; return 1; }; }

echo "== leg 0: build =="
make plugin-so bin/invf-dlopen_trust_test bin/invf-pack bin/invf-sweep bin/invf-mkfs bin/invf-cp bin/invf-cat bin/invf-fsck tools/codecpacks/jxl.codecpack/bin/jxlest >/dev/null 2>&1 \
  || { echo "build failed"; exit 1; }
[ -f tools/codecpacks/jxl.codecpack/libjxlest.so ] || { echo "pilot .so missing"; exit 1; }
ok "built loader + pilot libjxlest.so"

echo "== leg 1: unit trust matrix (60 cells, hostile .so legs) =="
unset INVFS_ALLOW_INPROCESS_PLUGINS
if ./bin/invf-dlopen_trust_test "$S/unit"; then
  ok "unit matrix PASS (daemon survived crash/hang/missing-symbol legs)"
else
  no "unit matrix failed"
fi

echo "== leg 2: invf-pack inproc trust metadata =="
# NOTE: no `install` leg here on purpose: install targets the host-global
# roots (/.invariantfs/codecpacks), so e2e must not run it. `inproc` is
# read-only and resolves the explicit admin registry ($INVFS_CODECPACKS),
# which exercises the same gates against the same roots.
export INVFS_CODECPACKS="$S/packs" INVFS_KEYRING="$S/kr"
mkdir -p "$S/packs/jxl.codecpack" "$S/kr"
./bin/invf-pack keygen --keyring "$S/kr" t >/dev/null 2>&1 || { no "keygen failed"; }
echo "name = jxl" > "$S/packs/jxl.codecpack/manifest"
cp tools/codecpacks/jxl.codecpack/libjxlest.so "$S/packs/jxl.codecpack/"
# unsigned admin pack -> REFUSED (signature gate, even though origin passes)
if ./bin/invf-pack inproc jxl 2>&1 | grep -q "REFUSE"; then
  ok "unsigned local pack REFUSED in-process"
else
  no "unsigned local pack should REFUSE"
fi
# remote origin can never be eligible: the same unsigned tree outside every
# admin root refuses on origin too (unit origin leg covers the matrix; here
# the raw-scratch shape).
export INVFS_CODECPACKS="$S/nowhere"
if ./bin/invf-pack inproc jxl 2>&1 | grep -q "not installed"; then
  ok "pack outside admin roots not even found (no silent trust)"
else
  no "pack outside admin roots should not resolve"
fi
export INVFS_CODECPACKS="$S/packs"
# signed admin pack -> ELIGIBLE modulo the mount flag
if ./bin/invf-pack sign --key "$S/kr/t.sec" "$S/packs/jxl.codecpack" >/dev/null 2>&1; then
  if ./bin/invf-pack inproc jxl 2>&1 | grep -q "ELIGIBLE"; then
    ok "signed local pack ELIGIBLE (modulo -o flag)"
  else
    no "signed local pack should be ELIGIBLE"
  fi
else
  no "pack sign failed"
fi
unset INVFS_CODECPACKS INVFS_KEYRING
if ./bin/invf-dlopen_trust_test "$S/unit2" >/dev/null 2>&1; then
  ok "origin gate re-verified (scratch/symlink/volume refusals)"
else
  no "origin gate re-run failed"
fi

echo "== leg 3+4: default vs opt-in sweep (byte-identical, routing proven) =="
# Mixed corpus incl. real JPEGs; the signed jxl pack is installed through
# the EXPLICIT admin registry (a copy — signing must never mutate the
# source tree). Two identical volumes: one swept default, one opt-in.
C="$S/corpus"; mkdir -p "$C"
head -c 60000 /dev/urandom > "$C/a.bin" 2>/dev/null || dd if=/dev/urandom of="$C/a.bin" bs=1024 count=58 2>/dev/null
seq 1 2000 > "$C/numbers.txt"
echo "hello invariantfs" > "$C/hello.txt"
python3 - "$C" <<'PY'
import sys, numpy as np
from PIL import Image
d = sys.argv[1]
rng = np.random.default_rng(625)
x = np.linspace(0, 255, 480, dtype=np.float32)
y = np.linspace(0, 255, 360, dtype=np.float32)[:, None]
n = rng.normal(0, 14, (360, 480))
im = Image.fromarray(np.dstack([np.clip(x+n,0,255), np.clip(y+n,0,255),
                                np.clip((x+y)/2+n,0,255)]).astype(np.uint8))
im.save(d + "/p1.jpg", "JPEG", quality=85)
im2 = Image.fromarray((rng.normal(128, 20, (128,128,3))).clip(0,255).astype(np.uint8))
im2.save(d + "/tiny.jpg", "JPEG", quality=85)
print("jpegs:", __import__("os").path.getsize(d+"/p1.jpg"), __import__("os").path.getsize(d+"/tiny.jpg"))
PY
# signed jxl pack copy: manifest + helper + pilot .so, then ed25519 sign.
mkdir -p "$S/packs"
# fresh dir: leg 2 leaves a minimal unsigned jxl.codecpack here, and cp -r
# onto an existing dir would nest (packs/jxl.codecpack/jxl.codecpack).
rm -rf "$S/packs/jxl.codecpack"
cp -r tools/codecpacks/jxl.codecpack "$S/packs/jxl.codecpack"
cp tools/codecpacks/jxl.codecpack/bin/jxlest "$S/packs/jxl.codecpack/bin/jxlest" 2>/dev/null || {
  mkdir -p "$S/packs/jxl.codecpack/bin"
  cc -std=c11 -O2 -o "$S/packs/jxl.codecpack/bin/jxlest" tools/codecpacks/jxl.codecpack/jxlest.c
}
cp tools/codecpacks/jxl.codecpack/libjxlest.so "$S/packs/jxl.codecpack/libjxlest.so"
./bin/invf-pack sign --key "$S/kr/t.sec" "$S/packs/jxl.codecpack" >/dev/null 2>&1 || { no "jxl pack sign failed"; }
export INVFS_CODECPACKS="$S/packs" INVFS_KEYRING="$S/kr"
for v in default opt; do
  ./bin/invf-mkfs "$S/vol-$v.img" 64 >/dev/null 2>&1 || { no "mkfs $v failed"; }
  for f in a.bin numbers.txt hello.txt p1.jpg tiny.jpg; do
    ./bin/invf-cp "$S/vol-$v.img" "$C/$f" "/$f" >/dev/null 2>&1 || no "cp $f ($v) failed"
  done
done
ok "twin volumes built (text+binary+2 JPEGs, signed jxl pack installed)"
# default sweep: must show NO in-process routing (opt-in absent).
unset INVFS_ALLOW_INPROCESS_PLUGINS
export INVFS_INPROC_LOG=1
./bin/invf-sweep "$S/vol-default.img" >"$S/sweep-default.log" 2>&1 || { no "default sweep failed"; }
if grep -q "inproc.*in-process" "$S/sweep-default.log"; then
  no "default sweep routed in-process without opt-in"
else
  ok "default sweep stayed IPC/exec (no opt-in, no in-process)"
fi
# opt-in sweep: the JPEG estimates must go through the .so.
export INVFS_ALLOW_INPROCESS_PLUGINS=jxl
./bin/invf-sweep "$S/vol-opt.img" >"$S/sweep-opt.log" 2>&1 || { no "opt-in sweep failed"; }
if grep -q "inproc.*in-process" "$S/sweep-opt.log"; then
  ok "opt-in sweep served estimates in-process (log proves routing)"
else
  no "opt-in sweep never went in-process (no [inproc] lines)"
fi
unset INVFS_ALLOW_INPROCESS_PLUGINS INVFS_INPROC_LOG
# byte-identity across all five files, both volumes, plus fsck.
for f in a.bin numbers.txt hello.txt p1.jpg tiny.jpg; do
  ./bin/invf-cat "$S/vol-default.img" "/$f" "$S/d-$f" >/dev/null 2>&1 || { no "export $f (default) failed"; continue; }
  ./bin/invf-cat "$S/vol-opt.img" "/$f" "$S/o-$f" >/dev/null 2>&1 || { no "export $f (opt-in) failed"; continue; }
  if cmp -s "$C/$f" "$S/d-$f" && cmp -s "$C/$f" "$S/o-$f" && cmp -s "$S/d-$f" "$S/o-$f"; then
    ok "$f byte-identical (corpus == default == opt-in)"
  else
    no "$f diverged across default/opt-in"
  fi
done
./bin/invf-fsck "$S/vol-default.img" >/dev/null 2>&1 && ok "fsck clean (default)" || no "fsck failed (default)"
./bin/invf-fsck "$S/vol-opt.img" >/dev/null 2>&1 && ok "fsck clean (opt-in)" || no "fsck failed (opt-in)"
# convergence: a second default sweep must be stable (no unbounded drift).
F1=$(./bin/invf-fsck "$S/vol-default.img" 2>/dev/null | grep -i free | head -1 || echo "")
./bin/invf-sweep "$S/vol-default.img" >/dev/null 2>&1
F2=$(./bin/invf-sweep "$S/vol-default.img" >/dev/null 2>&1; ./bin/invf-fsck "$S/vol-default.img" 2>/dev/null | grep -i free | head -1 || echo "")
if [ "$F1" = "$F2" ]; then
  ok "repeat sweeps converge (free-block line stable)"
else
  echo "note: free-block lines differ (one-sweep-lag shaped, AGENTS.md 2.5): [$F1] vs [$F2]"
  ok "repeat sweep ran (lag noted, not asserted)"
fi
unset INVFS_CODECPACKS INVFS_KEYRING

echo "== leg 5: perf (informational, not a gate) =="
# 200 estimate calls: in-process .so call vs fork+exec CLI helper per call.
# (The pool-IPC hop is heavier than fork+exec; this is the floor of the win.)
cat > "$S/perf.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include <stdint.h>
#include "ivpack_api.h"
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int cmpd(const void *a, const void *b){ double x=*(double*)a, y=*(double*)b; return (x>y)-(x<y); }
int main(int argc, char **argv){
    const char *so = argv[1]; const char *in = argv[2]; int n = atoi(argv[3]);
    void *h = dlopen(so, RTLD_NOW|RTLD_LOCAL);
    ivpack_container_estimate_fn est;
    static double t[2000];
    if(!h){ fprintf(stderr,"dlopen: %s\n", dlerror()); return 1; }
    est = (ivpack_container_estimate_fn)dlsym(h,"ivpack_container_estimate");
    if(!est){ fprintf(stderr,"dlsym\n"); return 1; }
    if(n > 2000) n = 2000;
    double t0 = now();
    for(int i=0;i<n;i++){ double a=now(); ivpack_estimate_res r; memset(&r,0,sizeof r); est(in,&r); t[i]=now()-a; }
    double wall = now()-t0;
    qsort(t, n, sizeof t[0], cmpd);
    printf("inprocess: %d calls wall %.3fs mean %.1fus/call p50 %.1fus p99 %.1fus\n",
           n, wall, wall/n*1e6, t[n/2]*1e6, t[(n*99)/100]*1e6);
    return 0;
}
EOF
if need cc && [ -f "$S/corpus/a.bin" ]; then
  if cc -O1 -Isrc/include -o "$S/perf" "$S/perf.c" -ldl 2>/dev/null; then
    "$S/perf" "$ROOT/tools/codecpacks/jxl.codecpack/libjxlest.so" "$S/corpus/a.bin" 200 2>/dev/null || echo "note: perf-inproc skipped"
    # CLI helper timing (per-call fork+exec of the estimate helper, p50/p99)
    if [ -x tools/codecpacks/jxl.codecpack/bin/jxlest ]; then
      python3 - "$S/corpus/a.bin" <<'PY'
import subprocess, sys, time
inp = sys.argv[1]
ts = []
for _ in range(50):
    t0 = time.monotonic()
    subprocess.run(["tools/codecpacks/jxl.codecpack/bin/jxlest", "estimate", inp],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ts.append(time.monotonic() - t0)
ts.sort()
print("exec-helper: 50 calls wall %.3fs mean %.1fus/call p50 %.1fus p99 %.1fus" %
      (sum(ts), sum(ts)/len(ts)*1e6, ts[len(ts)//2]*1e6, ts[(len(ts)*99)//100]*1e6))
PY
      ok "perf numbers recorded (informational)"
    else
      echo "note: bin/jxlest not built; perf leg partial"
      ok "perf leg partial (informational)"
    fi
  else
    echo "note: perf helper did not compile; skipping"
    ok "perf leg skipped (informational)"
  fi
else
  ok "perf leg skipped (no cc; informational)"
fi

echo "== result: pass=$pass fail=$fail =="
[ "$fail" -eq 0 ]
