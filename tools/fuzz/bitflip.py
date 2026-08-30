#!/usr/bin/env python3
"""
bitflip.py — adversarial image-mutation fuzz for InvariantFS (fuzz wave 1/4).

Pipeline per iteration (deterministic from --seed and the iteration index):
  mkfs on /dev/shm -> import a small mixed corpus (text/binary/empty/large)
  -> invf-sweep -> [every --seal-every: invf-sweep --seal]
  -> baseline invf-fsck + invf-verify --deep (must be clean)
  -> ONE mutation in a chosen region:
       (a) data zones  (b) journal area  (c) inode area  (d) superblock
       (e) descriptor slot region (incl. crafted valid-CRC RDP0/RSZ0/CKP0)
       (+) block bitmap
     mutation kinds: flip N random bytes / zero a random 4K block /
     truncate / crafted descriptor write
  -> invf-fsck, invf-verify --deep, invf-cat of every corpus file.

INVARIANTS (a violation is a bug report, logged with a repro):
  * no crashes: any signal death / timeout / rc >= 128 is a FAILURE
    (clean nonzero exits are the contract for corruption);
  * no silent garbage: a file that reads OK must be BIT-EXACT vs the
    original; a corrupt file must fail loudly;
  * fsck stays structurally consistent (no crash; issues may be reported).

Repro:  bitflip.py --seed S --only I   re-runs iteration I byte-for-byte.

Run from the repo root:  python3 tools/fuzz/bitflip.py --iterations 200
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

WORDS = ("the quick brown fox jumps over lazy dogs int static return if "
         "while for struct char void NULL size_t uint64_t\n").split()

REGION_WEIGHTS = [("data", 30), ("journal", 15), ("inode", 20),
                  ("superblock", 10), ("descriptors", 15), ("bitmap", 10)]

COVERAGE = None                      # collections.Counter, set in main()


def gen_corpus(rng, d):
    """Small mixed corpus; returns {name: bytes}."""
    corpus = {}

    def text(n):
        out = []
        tot = 0
        while tot < n:
            w = rng.choice(WORDS)
            out.append(w)
            tot += len(w) + 1
        return (" ".join(out))[:n].encode()

    corpus["a.txt"] = text(rng.randrange(1000, 4000))
    corpus["b.py"] = text(rng.randrange(20000, 60000))
    corpus["c.log"] = text(rng.randrange(100000, 300000))
    corpus["d.bin"] = bytes(rng.randrange(256)
                            for _ in range(rng.randrange(10000, 80000)))
    if rng.random() < 0.5:                       # e.big: compressible
        corpus["e.big"] = text(rng.randrange(500000, 1200000))
    else:                                        # e.big: incompressible
        corpus["e.big"] = rng.randbytes(rng.randrange(400000, 900000))
    corpus["f.empty"] = b""
    os.makedirs(d, exist_ok=True)
    for name, data in corpus.items():
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)
    return corpus


class Failure:
    def __init__(self, iteration, kind, detail):
        self.iteration = iteration
        self.kind = kind
        self.detail = detail

    def __str__(self):
        return "iter %d: %s: %s" % (self.iteration, self.kind, self.detail)


def one_iteration(args, it, keep_dir):
    """Run one fuzz iteration. Returns (failures, anomalies, mutation_desc)."""
    rng = random.Random((args.seed << 32) ^ (it * 0x9E3779B9))
    img = "%s-w%d.img" % (args.imgbase, it % max(1, args.workers))
    shmimg = os.path.join("/dev/shm", img)
    corpus = gen_corpus(rng, os.path.join(keep_dir, "orig"))
    fails, anoms = [], []
    mutation = "none"

    def tool(name, *a, timeout=30):
        return fz.run([os.path.join(fz.BIN, name), img, *a],
                      timeout=timeout, cwd="/dev/shm")

    def setup_tool(name, *a, timeout=30):
        rc, out, err = tool(name, *a, timeout=timeout)
        if rc != 0:
            fails.append(Failure(it, "setup",
                                 "%s rc=%d on a fresh image: %s%s"
                                 % (name, rc, out.decode(errors="replace"),
                                    err.decode(errors="replace"))))
        return rc

    try:
        if os.path.exists(shmimg):
            os.unlink(shmimg)
        if setup_tool("invf-mkfs", args.size_gb):
            return fails, anoms, mutation
        for name in corpus:
            if setup_tool("invf-cp", os.path.join(keep_dir, "orig", name),
                          name):
                return fails, anoms, mutation
        if setup_tool("invf-sweep", timeout=180):
            return fails, anoms, mutation
        if args.seal_every and it % args.seal_every == args.seal_every - 1:
            if setup_tool("invf-sweep", "--seal", timeout=180):
                return fails, anoms, mutation
            mutation = "sealed;"

        # ---- baseline: a fresh swept image must be squeaky clean ----
        rc, out, err = tool("invf-fsck", "-q", timeout=120)
        if rc != 0:
            fails.append(Failure(it, "baseline-fsck",
                                 "rc=%d on a fresh image: %s%s"
                                 % (rc, out.decode(errors="replace"),
                                    err.decode(errors="replace"))))
            return fails, anoms, mutation
        rc, out, err = tool("invf-verify", "--deep", timeout=180)
        if rc != 0:
            fails.append(Failure(it, "baseline-verify",
                                 "rc=%d on a fresh image: %s%s"
                                 % (rc, out.decode(errors="replace"),
                                    err.decode(errors="replace"))))
            return fails, anoms, mutation

        # ---- mutate ----
        lay = fz.Layout(shmimg)
        regs = lay.regions()
        mut = fz.Mutator(shmimg, rng)
        rname, roff, rlen = mut._pick_span(regs, REGION_WEIGHTS)
        if rname == "inode" and rlen > (1 << 20) and rng.random() < 0.5:
            rlen = 1 << 20          # bias: the live records sit at the head
        kind = rng.choices(["flip", "zero4k", "trunc", "craft"],
                           [55, 20, 10, 15])[0]
        if kind == "craft" and rname != "descriptors":
            kind = "flip"
        if kind == "flip":
            mutation = mut.flip_bytes((roff, rlen), rng.randrange(1, 17))
        elif kind == "zero4k":
            mutation = mut.zero_block((roff, rlen))
        elif kind == "trunc":
            mutation = mut.truncate((roff, rlen))
        else:
            for off, payload in fz.descriptor_bodies(rng, lay.total_blocks):
                mut.write_bytes(off, payload)
            mutation = "craft:RDP0+RSZ0+CKP0(valid CRCs, random fields)"
        COVERAGE["%s/%s" % (rname, kind)] += 1
        if args.seal_every and it % args.seal_every == args.seal_every - 1:
            COVERAGE["sealed"] += 1
        mutation = "%s region=%s %s" % (mutation, rname, mutation)

        # ---- post-mutation: the tools must fail cleanly, never crash ----
        tool("invf-fsck", "-q", timeout=120)
        vrc, vout, verr = tool("invf-verify", "--deep", timeout=180)
        for name, orig in sorted(corpus.items()):
            outp = os.path.join(keep_dir, "out", name)
            crc_, cout, cerr = tool("invf-cat", name, outp, timeout=60)
            if crc_ == 0:
                with open(outp, "rb") as f:
                    got = f.read()
                if got != orig:
                    fails.append(Failure(
                        it, "SILENT-GARBAGE",
                        "%s: invf-cat rc=0 but bytes differ "
                        "(orig %dB sha=%s, got %dB sha=%s); mutation: %s"
                        % (name, len(orig), fz.sha256(orig),
                           len(got), fz.sha256(got), mutation)))
            else:
                # verify --deep rc=0 yet a file unreadable is only
                # anomalous when the mutation could not have erased the
                # inode area wholesale (truncations legitimately shrink
                # the live set to zero; see repro/verify-deep-exit-code.sh
                # for the verify exit-code aspect).
                if vrc == 0 and kind != "trunc":
                    anoms.append(
                        "iter %d: verify --deep rc=0 yet cat %s rc=%d (%s)"
                        % (it, name, crc_, mutation))
    except fz.Crash as c:
        fails.append(Failure(it, "CRASH", "%s | %s | mutation: %s"
                             % (c.what, c.detail, mutation)))
    finally:
        if fails and os.path.exists(shmimg):
            # keep the mutated image: --only replays the mutation
            # deterministically, but the on-disk bytes are ground truth
            dst = os.path.join(args.workdir, "failure-%06d" % it)
            shutil.rmtree(dst, ignore_errors=True)
            os.makedirs(dst, exist_ok=True)
            subprocess.run(["cp", "--sparse=always", shmimg,
                            os.path.join(dst, "image.img")], check=False)
        if os.path.exists(shmimg):
            os.unlink(shmimg)
    return fails, anoms, mutation


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=200)
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x1A2B3C4D)
    ap.add_argument("--size-gb", default="0.12")
    ap.add_argument("--workers", type=int, default=1,
                    help="image-name rotation width (cases are "
                         "seed-deterministic regardless)")
    ap.add_argument("--seal-every", type=int, default=8,
                    help="seal every Nth image before mutating (0=never)")
    ap.add_argument("--only", type=int, default=-1,
                    help="re-run a single iteration (repro mode)")
    ap.add_argument("--workdir", default="/dev/shm/invf-fuzz-bitflip")
    args = ap.parse_args()

    args.imgbase = "bfz"
    global COVERAGE
    import collections
    COVERAGE = collections.Counter()
    if args.only >= 0:
        args.iterations = args.only + 1

    t0 = time.time()
    all_fails, all_anoms = [], []
    its = [args.only] if args.only >= 0 else range(args.iterations)
    for it in its:
        keep = fz.ensure_dir(os.path.join(args.workdir, "case"))
        shutil.rmtree(keep, ignore_errors=True)
        os.makedirs(os.path.join(keep, "out"))
        fails, anoms, mutation = one_iteration(args, it, keep)
        all_fails.extend(fails)
        all_anoms.extend(anoms)
        if fails or args.only >= 0:
            print("[iter %d] mutation: %s" % (it, mutation))
            for f in fails:
                print("  FAILURE %s" % f)
        if fails:
            # one_iteration() already stashed the mutated image here
            dst = os.path.join(args.workdir, "failure-%06d" % it)
            os.makedirs(dst, exist_ok=True)
            shutil.copytree(keep, dst, dirs_exist_ok=True)
        if (it + 1) % 25 == 0:
            print("[iter %d/%d] %.0fs elapsed, %d failures so far"
                  % (it + 1, args.iterations, time.time() - t0,
                     len(all_fails)))

    dt = time.time() - t0
    n = 1 if args.only >= 0 else args.iterations
    print("== bitflip: %d iterations in %.1fs (%.2fs/iter), seed %#x =="
          % (n, dt, dt / max(1, n), args.seed))
    print("   failures: %d, anomalies: %d" % (len(all_fails), len(all_anoms)))
    if COVERAGE:
        print("   coverage: %s" % ", ".join(
            "%s=%d" % kv for kv in sorted(COVERAGE.items())))
    for f in all_fails:
        print("   FAIL %s" % f)
    for a in all_anoms[:20]:
        print("   ANOMALY %s" % a)
    if all_fails:
        print("   repro: python3 tools/fuzz/bitflip.py --seed %#x "
              "--only ITER" % args.seed)
    return 1 if all_fails else 0


if __name__ == "__main__":
    sys.exit(main())
