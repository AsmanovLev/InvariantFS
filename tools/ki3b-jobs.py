#!/usr/bin/env python3
"""ki3b-jobs.py — WHICH JOB HOLDS multi-user.target?

WP224 says a systemd boot on an InvariantFS root is nondeterministic: 8 of 10
boots reach multi-user.target and 2 stop at getty.target, with the guest still
usable. The serial log says only that the target was never "reached" -- it
cannot say what was still pending, because systemd is not printing it.

So this stops inferring from the console and asks the guest directly:

  * the serial console is a unix SOCKET here, not `-serial file:`, because a
    file is write-only -- QEMU could not be typed into. Everything before this
    that needed to talk to the guest had to guess, and guessing produced two
    different wrong answers about the same boots (see INCIDENTS.md WP224).
  * once the auto-login shell appears, `systemctl list-jobs` names the job
    holding multi-user.target, and `systemctl --failed` names anything that
    died trying. That is the whole question, asked of the thing that knows.

Nothing here asserts, and nothing here decides what the answer means. It
collects. A run that reaches multi-user.target is still captured, because the
difference between a good run and a bad one is the data.

  tools/ki3b-jobs.py --volume /path/deb.img --runs 12
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# The probes. `systemctl list-jobs` is the one that answers the question; the
# rest are context -- a stuck job with a failed dependency needs both.
PROBES = [
    ("is-system-running", "systemctl is-system-running --no-pager"),
    ("list-jobs", "systemctl list-jobs --no-pager --no-legend"),
    ("failed-units", "systemctl --failed --no-pager --no-legend"),
    ("failed-jobs", "systemctl list-jobs --state=failed --no-pager --no-legend"),
    ("target-deps", "systemctl show multi-user.target -p Wants -p Requires "
                    "-p After --no-pager"),
    ("boot-count", "systemctl --failed --no-pager | wc -l"),
    ("kernel-tail", "dmesg 2>/dev/null | tail -25"),
]


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Guest:
    """A QEMU guest whose serial console is a socket we can both read and type
    into. Reading and writing the same fd is the whole point: the earlier
    harnesses could read the console but not answer questions from inside it."""

    def __init__(self, volume, sock_path, port, mem, kernel, initrd, accel, extra_drives):
        self.sock_path = sock_path
        self.buf = ""
        self.lock = __import__("threading").Lock()
        cmd = [
            "qemu-system-x86_64",
            "-machine", f"q35,accel={accel}",
            "-cpu", "host" if accel == "kvm" else "max",
            "-m", str(mem), "-smp", "2",
            "-kernel", kernel, "-initrd", initrd,
            "-append", "console=ttyS0,115200 invfs.init=/lib/systemd/systemd",
            "-drive", f"file={volume},format=raw,if=virtio",
            "-netdev", f"user,id=net0,hostfwd=tcp::{port}-:22",
            "-device", "virtio-net-pci,netdev=net0",
            "-display", "none", "-monitor", "none", "-no-reboot",
            # server=on,wait=on: QEMU WAITS for us before starting the guest.
            # With wait=off, output produced before we connect is DISCARDED --
            # which would silently delete the early boot we are here to read.
            "-serial", f"unix:{sock_path},server=on,wait=on",
        ]
        cmd += extra_drives
        self.err = tempfile.NamedTemporaryFile(prefix="ki3b-qemu-", suffix=".err", delete=False)
        self.proc = subprocess.Popen(cmd, stdout=self.err, stderr=subprocess.STDOUT)
        self.sock = None
        self._reader = None

    def connect(self, timeout=90):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited before the console socket appeared")
            if os.path.exists(self.sock_path):
                try:
                    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    s.connect(self.sock_path)
                    s.settimeout(0.5)
                    self.sock = s
                    return True
                except OSError:
                    pass
            time.sleep(0.25)
        return False

    def _pump(self):
        import threading
        while True:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            text = chunk.decode("utf-8", "replace")
            with self.lock:
                self.buf += text

    def start_reader(self):
        import threading
        self._reader = threading.Thread(target=self._pump, daemon=True)
        self._reader.start()

    def send(self, line):
        self.sock.sendall((line + "\n").encode())

    def wait_for(self, needle, timeout, since=0):
        """Wait for `needle` to appear in the console stream AFTER offset
        `since`. Returns the offset of the match, or None. The offset matters:
        'systemctl --failed' appears in the command I type and again in its
        output, and matching my own echo would report success having learned
        nothing -- the same shape as the 'SSH OK' bug in b7921c6."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                hay = self.buf[since:]
                if needle in hay:
                    return since + hay.index(needle)
            time.sleep(0.3)
        return None

    def read_all(self):
        with self.lock:
            return self.buf

    def marker(self, n):
        return f"###KI3B{n}###"

    def probe(self, n, cmd, timeout=90):
        """Run one command and return its output, delimited by markers."""
        start = len(self.read_all())
        m = self.marker(n)
        self.send(f"echo {m}BEGIN; {cmd}; echo {m}END")
        end_at = self.wait_for(f"{m}END", timeout, since=start)
        if end_at is None:
            return None
        with self.lock:
            hay = self.buf[start:end_at]
        b = hay.find(f"{m}BEGIN")
        if b < 0:
            return None
        body = hay[b + len(f"{m}BEGIN"):]
        return body.replace("\r\n", "\n").strip()

    def kill(self):
        try:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        except Exception:
            pass
        try:
            os.unlink(self.sock_path)
        except OSError:
            pass
        err = ""
        try:
            err = open(self.err.name).read()[-2000:]
        except OSError:
            pass
        return err


def one_run(idx, args, port):
    sock_path = os.path.join(tempfile.gettempdir(), f"ki3b-{idx}-{os.getpid()}.sock")
    extra = []
    if args.multi:
        extra += ["-drive", f"file={args.multi},format=raw,if=virtio"]
    g = Guest(args.volume, sock_path, port, args.mem, args.kernel, args.initrd,
              args.accel, extra)
    rec = {"run": idx, "reached_multi_user": False, "probes": {}, "error": None}
    try:
        if not g.connect():
            rec["error"] = "could not connect to the serial socket"
            return rec
        g.start_reader()

        # Phase 1: does it get to a usable console? getty.target legitimately
        # PRECEDES multi-user.target, so a shell is NOT evidence of success --
        # it is only evidence that we can now ask the question.
        deadline = time.time() + args.timeout
        shell_at = None
        while time.time() < deadline:
            hay = g.read_all()
            if re.search(r"root@debian:[^\n]*#", hay):
                shell_at = len(hay)
                break
            if "multi-user.target" in hay:
                rec["reached_multi_user"] = True
            if g.proc.poll() is not None:
                rec["error"] = "qemu exited during boot"
                return rec
            time.sleep(1.0)

        with g_read(g) as full:
            rec["reached_multi_user"] = "Reached target multi-user.target" in full
            targets = re.findall(r"Reached target ([A-Za-z0-9._-]+)", full)
            rec["targets_reached"] = targets
            rec["last_target"] = targets[-1] if targets else None

        if shell_at is None:
            rec["error"] = f"no console shell within {args.timeout}s"
            return rec

        # A newline first: the prompt may be sitting there having been written
        # before we attached, so nothing is echoed back until we poke it.
        g.send("")
        time.sleep(1.5)

        for n, (name, cmd) in enumerate(PROBES):
            out = g.probe(n, cmd, timeout=args.probe_timeout)
            rec["probes"][name] = out
            log(f"  run {idx}: probe {name}: "
                f"{'captured' if out is not None else 'NO RESPONSE'}")
            if out is None:
                rec["error"] = f"probe {name} got no response -- console wedged?"
                break

        if not args.no_poweroff:
            g.send("(sleep 1; poweroff) &")
        time.sleep(min(args.settle, 20))
        return rec
    finally:
        rec["qemu_stderr"] = g.kill()


class g_read:
    def __init__(self, g):
        self.g = g

    def __enter__(self):
        return self.g.read_all()

    def __exit__(self, *a):
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--volume", required=True)
    ap.add_argument("--multi", help="second device image")
    ap.add_argument("--runs", type=int, default=12)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--probe-timeout", type=int, default=90)
    ap.add_argument("--settle", type=int, default=20)
    ap.add_argument("--port", type=int, default=2426)
    ap.add_argument("--mem", type=int, default=3072)
    ap.add_argument("--kernel", default=f"/boot/vmlinuz-{os.uname().release}")
    ap.add_argument("--initrd", default=os.path.join(REPO, "vm/initramfs.cpio.gz"))
    ap.add_argument("--accel", default="auto")
    ap.add_argument("--outdir", default="/mnt/invfs-scratch/ki3b-jobs")
    ap.add_argument("--no-poweroff", action="store_true")
    args = ap.parse_args()

    if args.accel == "auto":
        args.accel = "kvm" if (os.access("/dev/kvm", os.R_OK) and
                               os.access("/dev/kvm", os.W_OK)) else "tcg"
    os.makedirs(args.outdir, exist_ok=True)
    if not os.path.exists(args.volume):
        sys.exit(f"no such volume: {args.volume}")
    if not os.path.exists(args.initrd):
        sys.exit(f"no initramfs at {args.initrd} -- run tools/mkinitramfs.sh")

    log(f"accel={args.accel}  volume={args.volume}  runs={args.runs}")
    results = []
    for i in range(1, args.runs + 1):
        log(f"=== run {i}/{args.runs}")
        rec = one_run(i, args, args.port)
        path = os.path.join(args.outdir, f"run{i:02d}.json")
        with open(path, "w") as fh:
            json.dump(rec, fh, indent=2)
        results.append(rec)
        log(f"=== run {i}: multi-user={'YES' if rec['reached_multi_user'] else 'NO'} "
            f"last_target={rec.get('last_target')} err={rec.get('error')}")
        time.sleep(3)

    summary = {
        "runs": len(results),
        "reached_multi_user": sum(1 for r in results if r["reached_multi_user"]),
        "results": results,
    }
    with open(os.path.join(args.outdir, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=2)

    good = summary["reached_multi_user"]
    log("")
    log(f"SUMMARY: {good}/{len(results)} reached multi-user.target")
    for r in results:
        if not r["reached_multi_user"]:
            log(f"  FAILING run {r['run']}: last_target={r.get('last_target')}")
            for name, out in (r.get("probes") or {}).items():
                if out:
                    for line in out.splitlines()[:14]:
                        log(f"      {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
