#!/usr/bin/env python3
"""
opseq.py — random op-sequence fuzz for InvariantFS (fuzz wave 2/4).

Random programs of {create, rewrite (incl. shorten/truncate), delete,
sweep, fsck, verify, stat (sync-ish open/close), spot-cat} are driven
through the real CLI tools (delete via tools/fuzz/ophelper, the tzrm
convention: no `invf-rm` exists on purpose). A shadow reference tree on
the host mirrors every surviving file's expected bytes.

  default: 5 images x 300 ops, deterministic from --seed.

INVARIANTS (a violation is a bug report, logged with the op history):
  * no crashes (signal / timeout / rc >= 128 anywhere);
  * after each sweep: invf-fsck rc==0 AND invf-verify --deep rc==0 AND
    every surviving file bit-exact vs the shadow tree;
  * every fsck/verify anywhere: rc==0 (a fresh workload has no excuse
    for orphans/missing/bad records);
  * invf-ls shows exactly the shadow set (no leaked \x01 owners, no
    ghosts, no lost files);
  * failed ops only where legal (create/rewrite/delete rc!=0 is a bug:
    the workload is sized to fit).

Repro:  opseq.py --seed S --only-image I   re-runs image I's program
byte-for-byte (the op log is also kept per failure).
"""

import argparse
import os
import random
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzutil as fz

WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda "
         "mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega\n"
         ).split()
NAME_POOL = ["f%03d.txt" % i for i in range(24)] + \
            ["f%03d.bin" % i for i in range(24)]
OPHELPER = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "ophelper")


def gen_content(rng):
    cls = rng.choices(["text", "binary", "zero", "random", "empty"],
                      [35, 25, 15, 20, 5])[0]
    size = rng.choices([rng.randrange(1, 200),
                        rng.randrange(200, 20000),
                        rng.randrange(20000, 400000)],
                       [30, 50, 20])[0]
    if cls == "empty":
        return b""
    if cls == "zero":
        return b"\0" * size
    if cls in ("binary", "random"):
        return rng.randbytes(size)
    out = []
    tot = 0
    while tot < size:
        w = rng.choice(WORDS)
        out.append(w)
        tot += len(w) + 1
    return (" ".join(out))[:size].encode()


class OpseqFailure(Exception):
    def __init__(self, op_idx, kind, detail):
        super().__init__(detail)
        self.op_idx = op_idx
        self.kind = kind
        self.detail = detail


class Run:
    """One image's random op program."""

    def __init__(self, args, image_idx):
        self.args = args
        self.idx = image_idx
        self.rng = random.Random((args.seed << 16) ^ (image_idx * 0x85EBCA6B))
        self.img = "opseq-w%d.img" % image_idx
        self.shmimg = os.path.join("/dev/shm", self.img)
        self.work = os.path.join(args.workdir, "img%d" % image_idx)
        self.shadow = {}                    # name -> bytes
        self.oplog = []

    # -- tool plumbing -------------------------------------------------
    def tool(self, name, *a, timeout=60, expect=None, op_idx=-1, what=None):
        try:
            rc, out, err = fz.run([os.path.join(fz.BIN, name), self.img, *a],
                                  timeout=timeout, cwd="/dev/shm")
        except fz.Crash as c:
            raise OpseqFailure(op_idx, "CRASH",
                               "%s: %s | %s" % (what or name, c.what,
                                                c.detail))
        if expect is not None and rc != expect:
            raise OpseqFailure(
                op_idx, "rc",
                "%s: rc=%d, want %d\nstdout: %s\nstderr: %s"
                % (what or name, rc, expect,
                   out.decode(errors="replace")[:2000],
                   err.decode(errors="replace")[:2000]))
        return rc, out, err

    def helper(self, *a, op_idx=-1):
        try:
            rc, out, err = fz.run([OPHELPER, self.img, *a], timeout=60,
                                  cwd="/dev/shm")
        except fz.Crash as c:
            raise OpseqFailure(op_idx, "CRASH", "ophelper: %s | %s"
                               % (c.what, c.detail))
        return rc, out, err

    def log(self, s):
        self.oplog.append(s)

    # -- ops -------------------------------------------------------------
    def op_create(self, i):
        free = [n for n in NAME_POOL if n not in self.shadow]
        if not free:
            return self.op_rewrite(i)
        name = self.rng.choice(free)
        data = gen_content(self.rng)
        host = os.path.join(self.work, "payload")
        with open(host, "wb") as f:
            f.write(data)
        self.log("%d create %s %dB" % (i, name, len(data)))
        self.tool("invf-cp", host, name, expect=0, op_idx=i,
                  what="create %s" % name)
        self.shadow[name] = data

    def op_rewrite(self, i):
        if not self.shadow:
            return self.op_create(i)
        name = self.rng.choice(sorted(self.shadow))
        data = gen_content(self.rng)
        if self.rng.random() < 0.3:                    # truncate-ish
            data = data[: len(data) // 3]
        host = os.path.join(self.work, "payload")
        with open(host, "wb") as f:
            f.write(data)
        self.log("%d rewrite %s %dB" % (i, name, len(data)))
        self.tool("invf-cp", host, name, expect=0, op_idx=i,
                  what="rewrite %s" % name)
        self.shadow[name] = data

    def op_delete(self, i):
        if not self.shadow:
            return self.op_create(i)
        name = self.rng.choice(sorted(self.shadow))
        self.log("%d delete %s" % (i, name))
        rc, out, err = self.helper("rm", name, op_idx=i)
        if rc != 0:
            raise OpseqFailure(i, "rc", "delete %s rc=%d: %s"
                               % (name, rc, err.decode(errors="replace")))
        del self.shadow[name]

    def op_sweep(self, i):
        self.log("%d sweep" % i)
        self.tool("invf-sweep", expect=0, op_idx=i, timeout=300,
                  what="sweep")
        self.full_check(i)

    def op_fsck(self, i):
        self.log("%d fsck" % i)
        self.tool("invf-fsck", "-q", expect=0, op_idx=i, what="fsck")

    def op_verify(self, i):
        self.log("%d verify" % i)
        self.tool("invf-verify", "--deep", expect=0, op_idx=i,
                  timeout=180, what="verify --deep")

    def op_stat(self, i):   # sync-ish open/close probe
        self.log("%d stat" % i)
        self.tool("invf-stat", expect=0, op_idx=i, what="stat")

    def op_spotcat(self, i):
        if not self.shadow:
            return
        name = self.rng.choice(sorted(self.shadow))
        self.log("%d spotcat %s" % (i, name))
        outp = os.path.join(self.work, "spot")
        self.tool("invf-cat", name, outp, expect=0, op_idx=i,
                  what="cat %s" % name)
        with open(outp, "rb") as f:
            got = f.read()
        if got != self.shadow[name]:
            raise OpseqFailure(i, "SILENT-GARBAGE",
                               "cat %s: bytes differ (want %dB sha=%s, "
                               "got %dB sha=%s)"
                               % (name, len(self.shadow[name]),
                                  fz.sha256(self.shadow[name]),
                                  len(got), fz.sha256(got)))

    # -- whole-tree check (after every sweep) ---------------------------
    def full_check(self, i):
        self.tool("invf-fsck", "-q", expect=0, op_idx=i,
                  what="post-sweep fsck")
        self.tool("invf-verify", "--deep", expect=0, op_idx=i,
                  timeout=180, what="post-sweep verify --deep")
        rc, out, err = self.tool("invf-ls", expect=0, op_idx=i,
                                 what="ls")
        listed = set()
        for line in out.decode(errors="replace").splitlines():
            parts = line.split()
            if len(parts) >= 5 and parts[1] == "bytes" and \
                    parts[2] == "inode":
                listed.add(parts[4])
        if listed != set(self.shadow):
            raise OpseqFailure(
                i, "ls-drift",
                "ls set != shadow set\nonly-ls: %s\nonly-shadow: %s"
                % (sorted(listed - set(self.shadow)),
                   sorted(set(self.shadow) - listed)))
        for name in sorted(self.shadow):
            outp = os.path.join(self.work, "chk")
            self.tool("invf-cat", name, outp, expect=0, op_idx=i,
                      what="post-sweep cat %s" % name)
            with open(outp, "rb") as f:
                got = f.read()
            if got != self.shadow[name]:
                raise OpseqFailure(i, "SILENT-GARBAGE",
                                   "post-sweep cat %s: bytes differ"
                                   % name)

    def run(self):
        if os.path.exists(self.shmimg):
            os.unlink(self.shmimg)
        shutil.rmtree(self.work, ignore_errors=True)
        os.makedirs(self.work)
        self.tool("invf-mkfs", self.args.size_gb, expect=0, what="mkfs")
        ops = [("create", 30, self.op_create),
               ("rewrite", 25, self.op_rewrite),
               ("delete", 15, self.op_delete),
               ("sweep", 10, self.op_sweep),
               ("fsck", 5, self.op_fsck),
               ("verify", 5, self.op_verify),
               ("stat", 5, self.op_stat),
               ("spotcat", 5, self.op_spotcat)]
        fns = [o[2] for o in ops]
        weights = [o[1] for o in ops]
        for i in range(self.args.ops):
            self.rng.choices(fns, weights)[0](i)
        self.log("final full check")
        self.full_check(self.args.ops)
        if os.path.exists(self.shmimg):
            os.unlink(self.shmimg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--images", type=int, default=5)
    ap.add_argument("--ops", type=int, default=300)
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x05E9)
    ap.add_argument("--size-gb", default="0.15")
    ap.add_argument("--only-image", type=int, default=-1)
    ap.add_argument("--workdir", default="/dev/shm/invf-fuzz-opseq")
    args = ap.parse_args()

    if not os.path.exists(OPHELPER):
        print("FAIL: %s missing (tools/test-fuzz.sh builds it)" % OPHELPER)
        return 2

    t0 = time.time()
    fails = []
    idxs = [args.only_image] if args.only_image >= 0 else range(args.images)
    for idx in idxs:
        r = Run(args, idx)
        try:
            r.run()
            print("[image %d] OK (%d ops, %.0fs)"
                  % (idx, args.ops, time.time() - t0))
        except OpseqFailure as f:
            fails.append(f)
            logp = os.path.join(args.workdir,
                                "oplog-img%d.txt" % idx)
            os.makedirs(args.workdir, exist_ok=True)
            with open(logp, "w") as fh:
                fh.write("\n".join(r.oplog) + "\n")
            # keep the image: --only-image replays the program, but the
            # on-disk bytes are ground truth
            if os.path.exists(r.shmimg):
                subprocess.run(["cp", "--sparse=always", r.shmimg,
                                os.path.join(args.workdir,
                                             "oplog-img%d.img" % idx)],
                               check=False)
            print("[image %d] FAILURE at op %d: %s: %s"
                  % (idx, f.op_idx, f.kind, f.detail))
            print("         op log: %s" % logp)
            print("         repro: python3 tools/fuzz/opseq.py "
                  "--seed %#x --only-image %d" % (args.seed, idx))
        finally:
            if os.path.exists(r.shmimg):
                os.unlink(r.shmimg)
    dt = time.time() - t0
    print("== opseq: %d images x %d ops in %.1fs, seed %#x =="
          % (args.images if args.only_image < 0 else 1, args.ops, dt,
             args.seed))
    print("   failures: %d" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
