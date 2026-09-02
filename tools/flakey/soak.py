#!/usr/bin/env python3
"""
soak.py — WP22b leg 5: seeded op-sequence soak on a dm-flakey device.

Drives a seeded random program of file ops (create/write, rewrite, rename,
delete, readback — through a FUSE mount) and engine ops (sweep, --seal,
--unseal, --realize, fsck -f, verify --deep — CLI, unmounted) while the
underlying dm device toggles between three tables on a seeded schedule:

  up    — flakey pass-through;
  drop  — flakey drop_writes (1s/1s autonomous windows): writes complete
          with success but never reach the device (lying write cache /
          power loss at a random writeback point);
  error — pure error target: every bio fails EIO (device off the bus).

A shadow model tracks every content version ever written per name
(history semantics): at every gate,

  * fsck is structurally clean (after the documented recovery ladder:
    auto-recover -> invf-rollback [-> --free-redundant first if the seal
    refuses it] -> fsck -f; torn parity is healed by one re-seal);
  * verify --deep rc==0 (no CORRUPT file, parity clean or absent);
  * every file listed by invf-ls is a known name whose content hash is
    IN ITS WRITTEN HISTORY — complete-or-absent per op, never a third
    state (rollback may legitimately resurrect an older version);
  * the FUSE daemon never dies and never has to be killed.

Any violation exits 1 with the op log + model preserved (the caller
copies the backing image alongside). Deterministic from --seed modulo
wall-clock timing jitter.
"""

import argparse
import hashlib
import json
import os
import random
import signal
import subprocess
import sys
import time

WORDS = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda "
         "mu nu xi omicron pi rho sigma tau upsilon phi chi psi omega "
         "struct return while static void\n").split()


class SoakFail(Exception):
    pass


class Soak:
    def __init__(self, a):
        self.a = a
        self.rng = random.Random(a.seed)
        self.crng = random.Random(a.seed ^ 0xC0FFEE)   # content stream
        self.t0 = time.monotonic()
        self.oplog = open(os.path.join(a.work, "oplog.txt"), "w")
        self.model = {}        # name -> {"hist": [sha...], "deleted": bool}
        self.n_rounds = 0
        self.n_ops = 0
        self.n_gates = 0
        self.mounted = False
        self.mode = "up"
        # seed corpus enters the model as pinned history
        for f in sorted(os.listdir(a.corpus)):
            p = os.path.join(a.corpus, f)
            with open(p, "rb") as fh:
                sha = hashlib.sha256(fh.read()).hexdigest()
            self.model[f] = {"hist": [sha], "deleted": False}
        signal.signal(signal.SIGTERM, self._sig)
        signal.signal(signal.SIGINT, self._sig)

    # ---------------------------------------------------------- plumbing --

    def _sig(self, signo, frame):
        self.log("signal %d -> restoring up + unmount" % signo)
        try:
            self.dm_set("up")
            if self.mounted:
                self.unmount()
        finally:
            os._exit(2)

    def log(self, s):
        self.oplog.write("t=%+.1f r%d %s\n" % (time.monotonic() - self.t0,
                                               self.n_rounds, s))
        self.oplog.flush()

    def sh(self, argv, timeout=240, sudo=False):
        """Run a command; return (rc, stdout+stderr). Never raises."""
        if sudo:
            argv = ["sudo", "-n"] + argv
        try:
            r = subprocess.run(argv, capture_output=True, text=True,
                               timeout=timeout)
            return r.returncode, (r.stdout or "") + (r.stderr or "")
        except subprocess.TimeoutExpired:
            return 124, "TIMEOUT after %ds" % timeout

    def dm_set(self, mode):
        sec, loop, name = self.a.sectors, self.a.loop, self.a.dmname
        if mode == "up":
            spec = "0 %d flakey %s 0 3600 0" % (sec, loop)
        elif mode == "drop":
            spec = "0 %d flakey %s 0 1 1 1 drop_writes" % (sec, loop)
        else:
            spec = "0 %d error" % sec
        for _ in range(20):
            rc, _ = self.sh(["dmsetup", "suspend", "--noflush", name],
                            sudo=True)
            if rc == 0:
                break
            time.sleep(0.2)
        else:
            raise SoakFail("dm suspend never succeeded")
        rc, out = self.sh(["dmsetup", "reload", name, "--table", spec],
                          sudo=True)
        rc2, out2 = self.sh(["dmsetup", "resume", name], sudo=True)
        if rc != 0 or rc2 != 0:
            raise SoakFail("dm_set %s failed: %s %s" % (mode, out, out2))
        if mode != self.mode:
            self.log("chaos -> %s" % mode)
            self.mode = mode

    # ------------------------------------------------------------ mount --

    def mount(self):
        # the daemon runs -f in the background so its stderr lands in
        # fuse.log (a daemonized run drops every post-detach diagnostic,
        # which is exactly what a soak failure needs for the postmortem)
        flog = open(os.path.join(self.a.work, "fuse.log"), "ab")
        subprocess.Popen([os.path.join(self.a.bin, "invf-fuse"),
                          self.a.dev, self.a.mnt, "-f"],
                         stdout=flog, stderr=flog,
                         start_new_session=True)
        for _ in range(50):
            if self._mounted_q():
                self.mounted = True
                return
            time.sleep(0.1)
        raise SoakFail("mount never appeared (see fuse.log)")

    def _mounted_q(self):
        with open("/proc/mounts") as f:
            return (" %s " % self.a.mnt) in f.read()

    def unmount(self, strict=True):
        self.sh(["fusermount3", "-u", self.a.mnt])
        for _ in range(300):                       # up to 60s of drain
            if not self._daemon_alive():
                self.mounted = False
                return
            time.sleep(0.2)
        self.sh(["pkill", "-9", "-f", "invf-fuse %s" % self.a.dev])
        self.mounted = False
        if strict:
            raise SoakFail("FUSE daemon wedged on unmount (kill -9 needed)")

    def _daemon_alive(self):
        rc, out = self.sh(["pgrep", "-f", "invf-fuse %s" % self.a.dev])
        return rc == 0

    # --------------------------------------------------------- engine ----

    def fsck(self, fix=False, content=False):
        a = [os.path.join(self.a.bin, "invf-fsck"), self.a.dev]
        if fix:
            a.append("-f")
        env = dict(os.environ)
        if content:
            env["INVFS_FSCK_CONTENT"] = "1"
        try:
            r = subprocess.run(a, capture_output=True, text=True,
                               timeout=300, env=env)
            return r.returncode, (r.stdout or "") + (r.stderr or "")
        except subprocess.TimeoutExpired:
            return 124, "TIMEOUT"

    def verify(self):
        return self.sh([os.path.join(self.a.bin, "invf-verify"),
                        self.a.dev, "--deep"], timeout=600)

    def sweep(self, *flags):
        return self.sh([os.path.join(self.a.bin, "invf-sweep"), self.a.dev]
                       + list(flags), timeout=600)

    def rollback(self):
        return self.sh([os.path.join(self.a.bin, "invf-rollback"),
                        self.a.dev], timeout=300)

    def ls(self):
        rc, out = self.sh([os.path.join(self.a.bin, "invf-ls"), self.a.dev])
        if rc != 0:
            raise SoakFail("invf-ls rc=%d: %s" % (rc, out[:300]))
        # invf-ls lines: "  <size> bytes  inode N  name"
        names = set()
        for ln in out.splitlines():
            p = ln.split()
            if len(p) >= 4 and p[1] == "bytes" and p[2] == "inode":
                names.add(p[-1])
        return names

    def cat_sha(self, name):
        outf = os.path.join(self.a.work, "soak-cat.out")
        rc, _ = self.sh([os.path.join(self.a.bin, "invf-cat"),
                         self.a.dev, name, outf], timeout=120)
        if rc != 0:
            return None
        with open(outf, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()

    # ----------------------------------------------------------- model ----

    def gen_content(self):
        size = self.crng.choice([self.crng.randrange(4096, 40000),
                                 self.crng.randrange(40000, 400000),
                                 self.crng.randrange(400000, 1100000)])
        if self.crng.random() < 0.5:
            out = []
            tot = 0
            while tot < size:
                w = self.crng.choice(WORDS)
                out.append(w)
                tot += len(w) + 1
            return (" ".join(out))[:size].encode()
        return self.crng.randbytes(size)

    def op_write(self, name=None):
        name = name or ("s%02d.bin" % self.rng.randrange(40))
        data = self.gen_content()
        sha = hashlib.sha256(data).hexdigest()
        try:
            fd = os.open(os.path.join(self.a.mnt, name),
                         os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
            try:
                mv = memoryview(data)
                while mv:
                    n = os.write(fd, mv)
                    mv = mv[n:]
                os.fsync(fd)
            finally:
                os.close(fd)
        except OSError as e:
            self.log("op write %s -> ERR %s (mode=%s)"
                     % (name, e.strerror or e, self.mode))
            return
        ent = self.model.setdefault(name, {"hist": [], "deleted": False})
        ent["hist"].append(sha)
        ent["deleted"] = False
        self.n_ops += 1
        self.log("op write %s %dB sha=%s ok (mode=%s)"
                 % (name, len(data), sha[:12], self.mode))

    def op_rename(self):
        live = [n for n, e in self.model.items() if not e["deleted"]]
        if not live:
            return
        src = self.rng.choice(live)
        dst = "r%02d.bin" % self.rng.randrange(20)
        try:
            os.rename(os.path.join(self.a.mnt, src),
                      os.path.join(self.a.mnt, dst))
        except OSError as e:
            self.log("op rename %s->%s -> ERR %s (mode=%s)"
                     % (src, dst, e.strerror or e, self.mode))
            return
        ent = self.model.pop(src)
        # rename onto an existing name keeps the victim's history around
        # (the victim is overwritten; its old versions stay acceptable)
        victim = self.model.get(dst)
        if victim is not None:
            ent["hist"] = ent["hist"] + victim["hist"]
        ent["deleted"] = False
        self.model[dst] = ent
        self.n_ops += 1
        self.log("op rename %s->%s ok (mode=%s)" % (src, dst, self.mode))

    def op_delete(self):
        live = [n for n, e in self.model.items() if not e["deleted"]
                and n.startswith(("s", "r"))]
        if not live:
            return
        name = self.rng.choice(live)
        try:
            os.unlink(os.path.join(self.a.mnt, name))
        except OSError as e:
            self.log("op delete %s -> ERR %s (mode=%s)"
                     % (name, e.strerror or e, self.mode))
            return
        self.model[name]["deleted"] = True
        self.n_ops += 1
        self.log("op delete %s ok (mode=%s)" % (name, self.mode))

    def op_readback(self):
        live = [n for n, e in self.model.items() if not e["deleted"]]
        if not live:
            return
        name = self.rng.choice(live)
        try:
            with open(os.path.join(self.a.mnt, name), "rb") as f:
                got = hashlib.sha256(f.read()).hexdigest()
        except OSError as e:
            self.log("op readback %s -> ERR %s (mode=%s)"
                     % (name, e.strerror or e, self.mode))
            return
        if got not in self.model[name]["hist"]:
            raise SoakFail("SILENT GARBAGE: %s reads as sha %s, never "
                           "written (hist=%s)"
                           % (name, got[:12],
                              [h[:12] for h in self.model[name]["hist"]]))
        self.log("op readback %s ok (mode=%s)" % (name, self.mode))

    # ------------------------------------------------------------ gates ---

    def recover(self):
        """The documented ladder; returns True when fsck ends OK."""
        rc, out = self.fsck()
        self.log("gate: fsck rc=%d: %s" % (rc, out[-300:].replace("\n", " | ")))
        # Bounded ladder: a live checkpoint is rolled back; a REFUSED
        # rollback (rc==3: torn journal staging -- under silent drops no
        # barrier proves the stage persisted) leaves the post-sweep state
        # untouched by construction, so the honest move is to ACCEPT it:
        # realize the unusable checkpoint (frees the retention registry,
        # clears CKP0) and re-run the ladder from the top. The re-armed
        # checkpoint from the realize is made in UP mode (the gate set it)
        # and rolls back cleanly if the next fsck needs it gone.
        for _ in range(3):
            if not ("checkpoint:" in out and "live" in out):
                break
            self.log("gate: checkpoint live -> rollback")
            rc, out = self.rollback()
            self.log("gate: rollback rc=%d: %s" % (rc, out[-300:].replace("\n", " | ")))
            if rc != 0 and "free-redundant" in out:
                self.log("gate: rollback refused (seal live) "
                         "-> --free-redundant")
                rc2, out2 = self.sweep("--free-redundant")
                self.log("gate: free-redundant rc=%d: %s" % (rc2, out2[-200:].replace("\n", " | ")))
                rc, out = self.rollback()
                self.log("gate: rollback#2 rc=%d: %s" % (rc, out[-300:].replace("\n", " | ")))
            if rc == 3:
                self.log("gate: rollback declined (torn staging); "
                         "accepting the post-sweep state via --realize")
                rc2, out2 = self.sweep("--realize")
                self.log("gate: realize rc=%d: %s" % (rc2, out2[-200:].replace("\n", " | ")))
                rc, out = self.fsck()
                self.log("gate: fsck rc=%d: %s" % (rc, out[-300:].replace("\n", " | ")))
                continue
            if rc != 0:
                self.log("gate: rollback ladder failed: %s" % out[-300:])
                return False
        else:
            return False
        rc, out = self.fsck(fix=True)
        self.log("gate: fsck -f rc=%d: %s" % (rc, out[-300:].replace("\n", " | ")))
        rc, out = self.fsck()
        self.log("gate: fsck final rc=%d: %s" % (rc, out[-300:].replace("\n", " | ")))
        return rc == 0 and "\nOK" in ("\n" + out)

    def gate(self, final=False):
        self.n_gates += 1
        self.dm_set("up")
        if self.mounted:
            self.unmount()
        if not self.recover():
            raise SoakFail("recovery ladder dead-ended at gate")
        rc, out = self.verify()
        if rc != 0 and "CORRUPT" not in out and "mismatched" in out:
            self.log("gate: torn parity -> re-seal")
            self.sweep("--seal")
            rc, out = self.verify()
        if rc != 0 and "CORRUPT" in out:
            # WP22d: a segment's data can be dropped by a window after its
            # map survived (maps are re-durabilized by compactions; data
            # is written once). The map-level cut is blind to it; the
            # segment CRC is not. Quarantine the content-torn records
            # (the losses are listed loudly; the names fall back or go
            # absent) and re-verify.
            self.log("gate: content-corrupt -> fsck -f (content cut)")
            rc2, out2 = self.fsck(fix=True, content=True)
            self.log("gate: fsck -f (content) rc=%d: %s"
                     % (rc2, out2[-300:].replace("\n", " | ")))
            rc, out = self.verify()
        if rc != 0:
            raise SoakFail("verify not clean at gate: %s" % out[-500:])
        # manifest diff: presence => known name + content in its history
        present = self.ls()
        for name in sorted(present):
            ent = self.model.get(name)
            if ent is None:
                # a container member ("archive!part") is engine-generated
                # content-addressed payload of its parent archive; it is
                # tracked through the parent's read-back, never written
                # directly. A member whose parent is PRESENT is no ghost.
                # (A member with no live parent IS one.)
                if "!" in name and name.split("!", 1)[0] in present:
                    continue
                raise SoakFail("GHOST: %s on the volume, never written"
                               % name)
            sha = self.cat_sha(name)
            if sha is None:
                raise SoakFail("present but unreadable: %s" % name)
            if sha not in ent["hist"]:
                raise SoakFail("THIRD STATE: %s sha %s not in history"
                               % (name, sha[:12]))
            ent["deleted"] = False     # present, incl. via resurrection
        # everything the model believes live and that the volume shows is
        # now pinned; note resurrections explicitly for the log
        for name, ent in sorted(self.model.items()):
            if ent["deleted"] and name in present:
                self.log("gate: %s resurrected (rollback), content in "
                         "history — legal" % name)
        self.log("GATE %d ok: %d files present, model tracks %d"
                 % (self.n_gates, len(present), len(self.model)))
        if not final:
            self.mount()

    # ------------------------------------------------------------- main ---

    def run(self):
        self.mount()
        deadline = self.t0 + self.a.seconds
        while time.monotonic() < deadline:
            self.n_rounds += 1
            mode = self.rng.choices(["up", "drop", "error"],
                                    [45, 40, 15])[0]
            self.dm_set(mode)
            # mounted phase: 2..6 file ops (under error they legally fail)
            for _ in range(self.rng.randrange(2, 7)):
                if not self._daemon_alive() or not self._mounted_q():
                    raise SoakFail("FUSE daemon died mid-soak (mode=%s)"
                                   % self.mode)
                op = self.rng.choices(["write", "rename", "delete",
                                       "readback"], [40, 10, 10, 15])[0]
                getattr(self, "op_" + op)()
            # CLI phase (engine ops) with fresh chaos under the hood
            if self.rng.random() < 0.35:
                cli_mode = self.rng.choices(["up", "drop"], [50, 50])[0]
                self.dm_set("up")          # clean unmount first
                self.unmount()
                self.dm_set(cli_mode)
                cli = self.rng.choices(
                    ["sweep", "seal", "unseal", "realize", "fsck", "verify"],
                    [30, 12, 5, 5, 20, 8])[0]
                if cli == "sweep":
                    rc, out = self.sweep()
                elif cli == "seal":
                    rc, out = self.sweep("--seal")
                elif cli == "unseal":
                    rc, out = self.sweep("--unseal")
                elif cli == "realize":
                    rc, out = self.sweep("--realize")
                elif cli == "fsck":
                    rc, out = self.fsck(fix=True)
                else:
                    rc, out = self.verify()
                self.log("cli %s (mode=%s) rc=%d: %s"
                         % (cli, cli_mode, rc,
                            out.strip().splitlines()[-1][:120]
                            if out.strip() else ""))
                self.dm_set("up")
                self.mount()
            if self.n_rounds % 4 == 0:
                self.gate()
        self.gate(final=True)
        with open(os.path.join(self.a.work, "model.json"), "w") as f:
            json.dump(self.model, f, indent=1)
        print("SOAK: PASS rounds=%d ops=%d gates=%d (mode changes in oplog)"
              % (self.n_rounds, self.n_ops, self.n_gates))
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev", required=True)
    ap.add_argument("--dmname", required=True)
    ap.add_argument("--loop", required=True)
    ap.add_argument("--sectors", type=int, required=True)
    ap.add_argument("--mnt", required=True)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--seconds", type=float, default=210)
    a = ap.parse_args()
    s = Soak(a)
    try:
        return s.run()
    except SoakFail as e:
        s.log("FAIL: %s" % e)
        with open(os.path.join(a.work, "model.json"), "w") as f:
            json.dump(s.model, f, indent=1)
        try:
            s.dm_set("up")
            if s.mounted:
                s.unmount(strict=False)
        except Exception:
            pass
        print("FAIL: %s" % e)
        return 1


if __name__ == "__main__":
    sys.exit(main())
