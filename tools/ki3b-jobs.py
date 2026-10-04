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
    ("list-jobs", "SYSTEMD_COLORS=0 systemctl list-jobs --no-pager --no-legend"),
    ("failed-units", "SYSTEMD_COLORS=0 systemctl --failed --no-pager --no-legend"),
    # WHY does dbus fail? It Wants= and is ordered After= dbus.service, and it
    # restarts forever, so multi-user.target never gets its dependency
    # satisfied. The mechanism is established; the cause is not. These probes
    # are the ones that go after the cause.
    ("dbus-status", "SYSTEMD_COLORS=0 systemctl status dbus.service --no-pager -l | head -40"),
    ("dbus-journal", "journalctl -u dbus.service --no-pager -n 40 2>&1 | tail -40"),
    ("dbus-props", "SYSTEMD_COLORS=0 systemctl show dbus.service "
                   "-p Type -p Restart -p RestartUSec -p Result -p ExecMainStatus "
                   "-p NRestarts -p After --no-pager"),
    ("machine-id", "echo machine-id=$(cat /etc/machine-id 2>&1); ls -l /run/dbus 2>&1; ls -ld /var/lib/dbus 2>&1"),
    ("dbus-bin", "command -v dbus-daemon; dbus-daemon --version 2>&1 | head -2"),
    # Is multi-user ACTUALLY unreached, or did systemd complete and merely fail
    # to PRINT it? `list-jobs` came back empty -- no pending jobs -- which is
    # not what a boot waiting on something looks like. The serial log is the
    # only witness so far, and it has already lied once (the raw
    # `systemd[1]:` lines present only in the "stalling" logs). So ask the
    # manager for the target's state directly instead of believing the console.
    ("target-active", "for t in multi-user.target graphical.target basic.target "
                      "sysinit.target; do printf '%s ' $t; "
                      "systemctl is-active $t 2>&1; done"),
    ("target-state", "SYSTEMD_COLORS=0 systemctl show multi-user.target "
                     "-p ActiveState -p SubState -p ActiveEnterTimestamp "
                     "-p Result --no-pager"),
    ("system-state", "systemctl is-system-running --no-pager; "
                     "systemctl is-system-running --wait >/dev/null 2>&1; "
                     "echo exit=$?"),
    # Does systemd's exec fail in GENERAL on this root, or only for dbus?
    ("exec-test", "systemd-run --unit=ki3b-exec-probe --wait --collect "
                  "/bin/echo ki3b-exec-ok 2>&1 | tail -3"),
    ("dbus-hardening", "SYSTEMD_COLORS=0 systemctl show dbus.service "
                       "-p ProtectSystem -p ProtectHome -p PrivateTmp "
                       "-p PrivateDevices -p NoNewPrivileges "
                       "-p RestrictNamespaces -p CapabilityBoundingSet "
                       "-p MemoryDenyWriteExecute -p SystemCallFilter --no-pager"),
    # 203/EXEC with NO hardening set means the failure is in the path, not in
    # the sandbox. systemd answers 203/EXEC for any failure to get the program
    # running -- including a failed access() on the binary, a missing ELF
    # interpreter, or a directory systemd created itself being unusable -- so
    # the exec line alone cannot tell us which. Walk the path.
    ("dbus-path", "namei -l /usr/bin/dbus-daemon 2>&1 | tail -12; "
                  "stat -c '%n %A %U:%G %s' /usr/bin/dbus-daemon "
                  "/lib64/ld-linux-x86-64.so.2 2>&1"),
    ("dbus-interp", "head -c 20 /usr/bin/dbus-daemon 2>/dev/null | od -c | head -2; "
                    "ls -l /lib64 2>&1 | head -3; ldd /usr/bin/dbus-daemon 2>&1 | head -6"),
    ("dbus-exec-props", "SYSTEMD_COLORS=0 systemctl show dbus.service "
                        "-p ExecStart -p ExecStartPre -p ExecStartPost "
                        "-p User -p Group -p RootDirectory -p RootImage "
                        "-p WorkingDirectory -p RuntimeDirectory "
                        "-p RuntimeDirectoryPreserve -p StateDirectory "
                        "-p CacheDirectory -p LogsDirectory "
                        "-p ReadWritePaths -p InaccessiblePaths --no-pager"),
    ("dbus-manual-start", "systemctl reset-failed dbus.service dbus.socket 2>&1; "
                          "systemctl start dbus.socket 2>&1; "
                          "systemctl start dbus.service 2>&1; "
                          "echo ---; systemctl is-active dbus.socket dbus.service 2>&1; "
                          "SYSTEMD_COLORS=0 systemctl status dbus.service --no-pager -l 2>&1 | head -12"),
    ("dbus-unit", "systemctl cat dbus.service 2>&1 | grep -vE '^#|^$' | head -25"),
    ("target-deps", "SYSTEMD_COLORS=0 systemctl show multi-user.target "
                    "-p Wants -p Requires -p After --no-pager"),
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

    def read_all(self):
        with self.lock:
            return self.buf

    def marker(self, n):
        # NO '#' in this token. '#' starts a comment in sh, so a marker like
        # ###KI3B0###BEGIN turned the entire probe line into a comment:
        #   echo ###KI3B0###BEGIN; systemctl is-system-running; echo ###KI3B0###END
        # runs NOTHING. The tty still echoes the typed line, so the console
        # looked responsive and the tool reported 'captured' with the command
        # text in it -- three separate layers of plausible-looking nothing.
        # Shell-safe token, no metacharacters that bash would interpret.
        return f"__KI3B{n}__"

    def probe(self, n, cmd, timeout=90):
        """Run one command and return its OUTPUT.

        The subtlety, which cost the first version of this tool a full sweep of
        12 boots plus a validation run: the tty ECHOES every character typed
        into it. So the console stream contains the whole line I sent --

            echo __KI3B0__BEGIN; systemctl is-system-running; echo __KI3B0__END

        -- twice over in effect: once as the echo, once as real output. The
        first occurrence of the END marker is therefore inside my own echo, and
        naively waiting for it 'succeeds' instantly and returns the command
        back, having learned nothing. Every probe came back 'captured' with the
        command text in it.

        So: wait for the marker TWICE and take the second. The echo is the
        first, the real one is the second, and nothing else can contain it
        because the command cannot produce a string it does not know.
        """
        start = len(self.read_all())
        m = self.marker(n)
        self.send(f"echo {m}BEGIN; {cmd}; echo {m}END")
        end_at = self._wait_for_nth(f"{m}END", 2, timeout, since=start)
        if end_at is None:
            return None
        with self.lock:
            hay = self.buf[start:end_at]
        b = self._nth_index(hay, f"{m}BEGIN", 2)
        if b is None:
            return None
        body = hay[b + len(f"{m}BEGIN"):]
        body = body.replace("\r\n", "\n").strip()
        # If the capture degenerates back into the echo, say so rather than
        # storing it as if it were an answer. An empty result is legitimate
        # (e.g. `systemctl --failed` with nothing failed); a result that is
        # just the command text is not.
        if body and cmd.split()[0] in body and "__KI3B" not in body \
                and len(body) < len(cmd) + 4:
            return {"__echo_only__": body}
        return body

    def _nth_index(self, hay, needle, n):
        """Index of the nth occurrence of `needle`, or None."""
        idx = -1
        for _ in range(n):
            idx = hay.find(needle, idx + 1)
            if idx < 0:
                return None
        return idx

    def _wait_for_nth(self, needle, n, timeout, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                hay = self.buf[since:]
                found = self._nth_index(hay, needle, n)
            if found is not None:
                return since + found
            time.sleep(0.3)
        return None

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
        # Deliberately NOT `stty -echo`. I added that as belt-and-braces and it
        # inverted the bug: probe() counts on the marker appearing TWICE (echo,
        # then real output), so with echo disabled every probe timed out and
        # reported 'console wedged?' when the console was fine. The two
        # mechanisms are opposites; pick one. Echo stays on.

        for n, (name, cmd) in enumerate(PROBES):
            out = g.probe(n, cmd, timeout=args.probe_timeout)
            rec["probes"][name] = out
            log(f"  run {idx}: probe {name}: "
                f"{'captured' if out is not None else 'NO RESPONSE'}")
            if out is None:
                rec["error"] = f"probe {name} got no response -- console wedged?"
                break

        # Did the boot transaction FINISH? getty.target legitimately precedes
        # multi-user.target, so a shell at t=0 says nothing about whether the
        # target is EVER reached -- and reading the flag straight after the
        # shell appeared marked 4/4 runs as failures when the earlier harness
        # measured 8/15 succeeding. The guest is still booting; the answer does
        # not exist yet. So wait for it, and only a boot that never arrives
        # pays the full window.
        settle_deadline = time.time() + args.settle_multi
        while time.time() < settle_deadline:
            if "Reached target multi-user.target" in g.read_all():
                break
            time.sleep(3.0)
        with g_read(g) as full:
            rec["reached_multi_user"] = "Reached target multi-user.target" in full
            targets = re.findall(r"Reached target ([A-Za-z0-9._-]+)", full)
            rec["targets_reached"] = targets
            rec["last_target"] = targets[-1] if targets else None

        if not args.no_poweroff:
            g.send("(sleep 1; poweroff) &")
        time.sleep(8)
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
    ap.add_argument("--settle-multi", type=int, default=180,
                    help="how long to let the boot transaction FINISH before "
                         "recording whether multi-user.target was reached")
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
