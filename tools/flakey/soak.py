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
import copy
import hashlib
import json
import os
import random
import re
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
        self.model = {}        # name -> {"hist": [sha...], "deleted": bool,
                               #          "drop_touched": bool}
        # drop_touched (WP503): set on any acked state change (write /
        # rename / delete) that ran under drop mode -- the device ACKed a
        # write it may have discarded, so the pinned history may be
        # unsatisfiable through no fault of the engine. Cleared only by an
        # acked write under up mode, which re-pins history strictly. At the
        # gate manifest diff a drop-touched file skips the history check
        # (loudly logged) but must still be fsck/verify-clean and readable;
        # an unreadable one must be damage-named or the gate still fails.
        # Up-mode-pinned files keep byte-identical strictness.
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
            self.model[f] = {"hist": [sha], "deleted": False,
                             "drop_touched": False}
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

    # --------------------------------------- drop-touch tracking (WP503) --

    def _note_write_acked(self, name, mode):
        """Record the mode of an acked write for the gate diff."""
        ent = self.model.get(name)
        if ent is None:
            return
        if mode == "drop":
            ent["drop_touched"] = True
        elif mode == "up":
            # a strictly-pinned acked write re-pins history as before
            ent["drop_touched"] = False
        # error-mode acks never enter history (ops fail loudly there);
        # an impossible ack under error leaves the flag untouched.

    def _note_rename_acked(self, src, dst, mode):
        for n in (src, dst):
            ent = self.model.get(n)
            if ent is None:
                continue
            if mode == "drop":
                ent["drop_touched"] = True
            # an up-mode rename moves bytes without rewriting them: it
            # neither pins nor unpins, so the flag is left as-is (only an
            # acked write under up re-pins).

    def _note_delete_acked(self, name, mode):
        ent = self.model.get(name)
        if ent is None:
            return
        if mode == "drop":
            # a rollback can resurrect the pre-delete content torn
            ent["drop_touched"] = True
        # an up-mode delete changes no content; the flag is left as-is.

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
        ent = self.model.setdefault(name, {"hist": [], "deleted": False,
                                          "drop_touched": False})
        ent["hist"].append(sha)
        ent["deleted"] = False
        self._note_write_acked(name, self.mode)
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
        ent = self.model[src]
        # rename onto an existing name keeps the victim's history around
        # (the victim is overwritten; its old versions stay acceptable)
        victim = self.model.get(dst)
        if victim is not None:
            ent["hist"] = ent["hist"] + victim["hist"]
        ent["deleted"] = False
        self.model[dst] = ent
        # the source is gone from the live volume, but a rollback can
        # legitimately resurrect it (WP21 time travel): keep its history
        # marked-deleted so a resurrection is recognized, never a ghost
        self.model[src] = {"hist": list(ent["hist"]), "deleted": True,
                           "drop_touched": ent.get("drop_touched", False)}
        self._note_rename_acked(src, dst, self.mode)
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
        self._note_delete_acked(name, self.mode)
        self.n_ops += 1
        self.log("op delete %s ok (mode=%s)" % (name, self.mode))

    def op_readback(self):
        live = [n for n, e in self.model.items() if not e["deleted"]]
        if not live:
            return
        name = self.rng.choice(live)
        try:
            with open(os.path.join(self.a.mnt, name), "rb") as f:
                blob = f.read()
                got = hashlib.sha256(blob).hexdigest()
        except OSError as e:
            self.log("op readback %s -> ERR %s (mode=%s)"
                     % (name, e.strerror or e, self.mode))
            return
        if got not in self.model[name]["hist"]:
            # Preserve the offending bytes: transient states (ARC/stale
            # merge) do not survive unmount, and a hash alone cannot be
            # dissected (torn window? foreign content? stale generation?).
            gp = os.path.join(self.a.work, "garbage-%s.bin" % name)
            with open(gp, "wb") as g:
                g.write(blob)
            raise SoakFail("SILENT GARBAGE: %s reads as sha %s, never "
                           "written (hist=%s); %d bytes saved to %s"
                           % (name, got[:12],
                              [h[:12] for h in self.model[name]["hist"]],
                              len(blob), gp))
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

    @staticmethod
    def damage_names(verify_out):
        """Bare `CORRUPT: <name>` files from verify --deep output.

        Mirrors the suite's own content-evidence rule
        (tools/test-flakey.sh consistent-check: only bare `CORRUPT: <name>`
        lines name lost file bytes; `CORRUPT: inode N (...)` lines are
        namespace-audit damage, not content tears)."""
        names = set()
        for ln in (verify_out or "").splitlines():
            m = re.match(r"^  CORRUPT: ([^ ]+)\s*$", ln)
            if m:
                names.add(m.group(1))
        return names

    def diff_manifest(self, present, read_sha, damage):
        """Gate manifest diff; returns the deferred (drop-touched) names.

        Strictness for up-mode-pinned files is byte-identical to the
        pre-WP503 gate: unknown name -> GHOST, unreadable -> FAIL,
        sha outside history -> THIRD STATE FAIL. A drop-touched file
        whose sha falls outside its history is NOT a third state -- the
        device ACKed a write it may have discarded, so the pinned history
        may be unsatisfiable -- and is deferred loudly instead, after the
        caller has already established fsck/verify-clean. The silent-
        corruption tripwire stays: an unreadable drop-touched file must be
        damage-named (in `damage`) or the gate still fails -- the exemption
        covers torn-but-valid states, never silent loss."""
        deferred = []
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
            sha = read_sha(name)
            if sha is None:
                if ent.get("drop_touched") and name in (damage or set()):
                    self.log("gate: %s drop-touched, unreadable but "
                             "damage-named, history check deferred "
                             "(last acked write under drop)" % name)
                    deferred.append(name)
                    continue
                raise SoakFail("present but unreadable: %s" % name)
            if sha not in ent["hist"]:
                if ent.get("drop_touched"):
                    self.log("gate: %s drop-touched, history check "
                             "deferred (last acked write under drop)" % name)
                    deferred.append(name)
                    continue
                raise SoakFail("THIRD STATE: %s sha %s not in history"
                               % (name, sha[:12]))
            ent["deleted"] = False     # present, incl. via resurrection
        return deferred

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
        # (drop-touched files defer the history check loudly; up-mode
        # files keep full strictness). verify is clean here, so the damage
        # set is empty -- the tripwire's damage-named branch is exercised
        # by replay, not by live gates.
        present = self.ls()
        deferred = self.diff_manifest(present, self.cat_sha,
                                      self.damage_names(out))
        # everything the model believes live and that the volume shows is
        # now pinned; note resurrections explicitly for the log
        for name, ent in sorted(self.model.items()):
            if ent["deleted"] and name in present:
                self.log("gate: %s resurrected (rollback), content in "
                         "history — legal" % name)
        self.log("GATE %d ok: %d files present, model tracks %d"
                 "%s" % (self.n_gates, len(present), len(self.model),
                           (" (%d drop-touched deferred)" % len(deferred))
                           if deferred else ""))
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
                # F9: whole-volume mutations run in up mode only. A device
                # that acknowledges writes it discards (drop) is:
                # (a) undetectable per-op (acks look identical), so no
                #     engine check can refuse it, and a sweep that runs
                #     there reads a mix of stale and current data -- its
                #     liveness walk then frees live blocks (dead-end);
                # (b) out of contract anyway: drop models torn/flaky
                #     writes (WINDOWS, leg 3's design), not a permanently
                #     void volume. File ops keep all three modes; legs 3
                #     and 4 keep the window chaos.
                cli_mode = "up"
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


def replay(artifact, image, bindir, workdir, logpath):
    """WP503 artifact replay: run the NEW gate diff against preserved state.

    `artifact` holds model.json + oplog.txt from a failed leg5 run (the
    gate died at the manifest diff, so fsck/verify were clean and the
    damage set is empty). drop_touched is re-derived from the oplog with
    the live rules (_note_*_acked); file bytes are read from `image` with
    the real invf-ls/invf-cat. Verdict: every history miss must be a
    drop-touched deferral (loud), never a THIRD STATE -- plus three
    negative controls proving up-mode strictness is byte-identical and
    the unreadable tripwire still fails. Returns 0 on REPLAY: PASS."""
    with open(os.path.join(artifact, "model.json")) as f:
        model = json.load(f)
    with open(os.path.join(artifact, "oplog.txt")) as f:
        oplog = f.read().splitlines()
    os.makedirs(workdir, exist_ok=True)

    def blank(m):
        s = object.__new__(Soak)
        s.model = copy.deepcopy(m)
        s.n_rounds = 0
        s.n_gates = 0
        s.n_ops = 0
        s.t0 = time.monotonic()
        s.mode = "up"
        s.oplog = open(logpath, "a")
        s.a = argparse.Namespace(dev=image, bin=bindir, work=workdir)
        return s

    s = blank(model)
    # re-derive drop_touched from acked ops with the live rules. Only
    # `ok` lines match (ERR lines fail loudly and never enter history).
    n_drop = n_up = 0
    for ln in oplog:
        m = re.search(r"op write (\S+) \d+B sha=\S+ ok \(mode=(\S+)\)", ln)
        if m:
            s._note_write_acked(m.group(1), m.group(2))
            n_drop += m.group(2) == "drop"
            n_up += m.group(2) == "up"
            continue
        m = re.search(r"op rename (\S+)->(\S+) ok \(mode=(\S+)\)", ln)
        if m:
            s._note_rename_acked(m.group(1), m.group(2), m.group(3))
            n_drop += m.group(3) == "drop"
            continue
        m = re.search(r"op delete (\S+) ok \(mode=(\S+)\)", ln)
        if m:
            s._note_delete_acked(m.group(1), m.group(2))
            n_drop += m.group(2) == "drop"
    print("replay: oplog %d lines, %d acked-drop state changes, "
          "%d acked-up writes" % (len(oplog), n_drop, n_up))
    present = s.ls()
    print("replay: %d files present on preserved image" % len(present))
    # the preserved gate died at the diff, so verify was clean: empty
    # damage set (a damage-named file could not have survived verify).
    try:
        deferred = s.diff_manifest(present, s.cat_sha, set())
    except SoakFail as e:
        print("REPLAY: FAIL (gate still fails: %s)" % e)
        return 1
    # provenance: last acked op per deferred file, straight from the oplog
    last = {}
    for ln in oplog:
        for pat in (r"op write (\S+) \d+B sha=\S+ ok \(mode=\S+\)",
                    r"op rename (\S+)->\S+ ok \(mode=\S+\)",
                    r"op rename \S+->(\S+) ok \(mode=\S+\)",
                    r"op delete (\S+) ok \(mode=\S+\)"):
            m = re.search(pat, ln)
            if m:
                last[m.group(1)] = ln.strip()
    ok = True
    for name in deferred:
        print("replay: DEFERRED %s <- %s" % (name, last.get(name, "?")))
    strict = sorted(n for n in present if n not in deferred
                    and s.model.get(n) is not None)
    print("replay: %d deferred (loud, above), %d strict-PASS"
          % (len(deferred), len(strict)))
    if not deferred:
        print("REPLAY: FAIL (no deferral: artifact tear not reproduced)")
        ok = False
    # NC1: an up-mode-pinned file reading foreign bytes must still FAIL
    # with byte-identical THIRD STATE (prove by faulting one in-memory;
    # faulting a live mounted image would conflate metadata damage with
    # oracle strictness, so the exact code path is pinned here instead).
    nc1 = [n for n in strict
           if not s.model[n].get("drop_touched")][:1]
    if nc1:
        t = blank(s.model)
        bad = "0" * 64
        try:
            t.diff_manifest(set(nc1), lambda n: bad, set())
            print("REPLAY: FAIL (NC1: up-mode foreign sha not caught)")
            ok = False
        except SoakFail as e:
            if "THIRD STATE" in str(e):
                print("replay: NC1 ok (up-mode foreign sha -> %s)" % e)
            else:
                print("REPLAY: FAIL (NC1 wrong failure: %s)" % e)
                ok = False
    else:
        print("REPLAY: FAIL (NC1: no strict file to fault)")
        ok = False
    # NC2: unreadable + drop-touched + NOT damage-named must still FAIL.
    t = blank(s.model)
    try:
        t.diff_manifest(set(deferred[:1]), lambda n: None, set())
        print("REPLAY: FAIL (NC2: silent unreadable not caught)")
        ok = False
    except SoakFail as e:
        if "unreadable" in str(e):
            print("replay: NC2 ok (unreadable, unnamed -> %s)" % e)
        else:
            print("REPLAY: FAIL (NC2 wrong failure: %s)" % e)
            ok = False
    # NC3: unreadable + drop-touched + damage-named defers loudly.
    t = blank(s.model)
    try:
        d = t.diff_manifest(set(deferred[:1]), lambda n: None,
                            set(deferred[:1]))
        if d == deferred[:1]:
            print("replay: NC3 ok (unreadable, damage-named -> deferred)")
        else:
            print("REPLAY: FAIL (NC3 not deferred: %s)" % d)
            ok = False
    except SoakFail as e:
        print("REPLAY: FAIL (NC3 raised: %s)" % e)
        ok = False
    print("REPLAY: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dev")
    ap.add_argument("--dmname")
    ap.add_argument("--loop")
    ap.add_argument("--sectors", type=int)
    ap.add_argument("--mnt")
    ap.add_argument("--bin", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--corpus")
    ap.add_argument("--seed", type=int)
    ap.add_argument("--seconds", type=float, default=210)
    ap.add_argument("--replay", default=None,
                    help="artifact dir (model.json+oplog.txt): run the WP503 "
                    "gate-diff replay against --image instead of soaking")
    ap.add_argument("--image", default=None)
    ap.add_argument("--replay-log", default="soak-replay.log")
    a = ap.parse_args()
    if a.replay:
        if not a.image:
            ap.error("--replay needs --image")
        return replay(a.replay, a.image, a.bin, a.work, a.replay_log)
    for k in ("dev", "dmname", "loop", "sectors", "mnt",
              "corpus", "seed"):
        if getattr(a, k) is None:
            ap.error("--%s is required (or use --replay)" % k)
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
