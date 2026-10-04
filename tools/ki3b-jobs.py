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
    # The exec path is proven fine as root: real ELF, 0755 root:root, the
    # /lib64 -> usr/lib64 interpreter symlink resolves, and ldd binds every
    # library. The one thing dbus-exec-props adds that the rest do not is
    # User=messagebus -- systemd drops to that uid BEFORE execve. So exec the
    # binary AS that uid from a shell. If it works here and fails under systemd,
    # the fault is systemd's own setup (mount namespace, capability bounding,
    # the PrivateTmp/MountPrivate defaults it applies to unprivileged units),
    # not the filesystem.
    ("dbus-asuser", "getent passwd messagebus; getent group messagebus; "
                    "id messagebus 2>&1; "
                    "runuser -u messagebus -- /usr/bin/dbus-daemon --version 2>&1 "
                    "| head -3; "
                    "setpriv --reuid=messagebus --regid=messagebus --clear-groups "
                    "/usr/bin/dbus-daemon --version 2>&1 | head -3"),
    # What systemd changes about the unit's namespace by default for an
    # unprivileged User=, and whether the uid can traverse the exec path at all.
    # 203/EXEC localised: `runuser -u messagebus -- dbus-daemon --version` and
    # `setpriv --reuid=messagebus -- dbus-daemon --version` BOTH fail with
    # "Permission denied" from a plain shell, while the same binary as root
    # works, and dbus-ns-props shows every hardening knob = no. So this is not
    # systemd's per-unit sandbox and not dbus's config: a non-root uid cannot
    # exec a world-executable file. The binary is -rwxr-xr-x root:root and uid
    # 996 is neither owner nor group, so the OTHER r-x must grant it.
    #
    # These probes separate the three things that could produce EACCES here,
    # because "Permission denied" from execve collapses all of them:
    #   (a) the DAC check itself is wrong  -> test -x disagrees with ls
    #   (b) only exec is broken            -> read works, exec does not
    #   (c) it is systemic, not dbus       -> /bin/true and a fresh 0755 file
    #                                           behave the same way
    # plus the mode-storage control, since a mode that never reaches the kernel
    # would make every one of the others a lie.
    ("dac-asuser", "runuser -u messagebus -- sh -c '"
                   "echo -n \"  test -x dbus-daemon : \"; test -x /usr/bin/dbus-daemon && echo PASS || echo FAIL; "
                   "echo -n \"  test -r dbus-daemon : \"; test -r /usr/bin/dbus-daemon && echo PASS || echo FAIL; "
                   "echo -n \"  read first byte    : \"; head -c1 /usr/bin/dbus-daemon >/dev/null 2>&1 && echo PASS || echo FAIL; "
                   "echo -n \"  ls -l as messagebus: \"; ls -l /usr/bin/dbus-daemon 2>&1; "
                   "echo -n \"  id                 : \"; id' 2>&1"),
    ("dac-modestore", "echo '--- are mode bits even reaching the kernel?'; "
                      "umask 022; rm -f /tmp/dacmod; touch /tmp/dacmod; chmod 0755 /tmp/dacmod; chmod 0700 /tmp/dacmod; "
                      "for m in 0755 0700 0755 0077 0700; do chmod $m /tmp/dacmod; "
                      "printf '  requested %s -> kernel reports %s\\n' \"$m\" \"$(stat -c %a /tmp/dacmod)\"; done; "
                      "echo '  --- as messagebus, per-bit test on a 0755 file:'; "
                      "chmod 0755 /tmp/dacmod; runuser -u messagebus -- sh -c "
                      "'test -r /tmp/dacmod && echo \"    read ok\" || echo \"    read DENIED\"; "
                      "test -w /tmp/dacmod && echo \"    write ok\" || echo \"    write DENIED (expected)\"; "
                      "test -x /tmp/dacmod && echo \"    exec ok\" || echo \"    exec DENIED\"' 2>&1"),
    # Every non-root exec has failed so far (`sh` and `dbus-daemon` both, via
    # runuser AND setpriv, from a plain shell with no systemd involved), which
    # means no non-root program can run AT ALL -- so I cannot test a non-root
    # READ, because reading requires executing cat, which requires the thing
    # under test. The control has to come from a filesystem that is not
    # InvariantFS. /dev/shm is devtmpfs/tmpfs, never FUSE, so it separates
    # "non-root cannot exec on InvariantFS" from "non-root cannot exec here".
    #
    # dac-tmpfs-control also dumps the InvariantFS mount options verbatim. If
    # noexec is among them that explains the symptom outright (and would also
    # have to explain how root succeeded, which is the interesting part).
    ("dac-tmpfs-control", "echo '--- InvariantFS mount options, verbatim:'; "
                         "grep -E ' / |invfs|fuse' /proc/mounts 2>&1; "
                         "echo '--- same binary, non-root uid, on a NON-FUSE filesystem (/dev/shm):'; "
                         "cp /usr/bin/dbus-daemon /dev/shm/dbus-daemon 2>&1 && chmod 0755 /dev/shm/dbus-daemon && ls -l /dev/shm/dbus-daemon; "
                         "setpriv --reuid=messagebus --regid=messagebus --clear-groups /dev/shm/dbus-daemon --version 2>&1 && echo '  tmpfs exec as messagebus : PASS' || echo '  tmpfs exec as messagebus : FAIL'; "
                         "echo '--- and the SAME binary on InvariantFS for comparison:'; "
                         "setpriv --reuid=messagebus --regid=messagebus --clear-groups /usr/bin/dbus-daemon --version 2>&1 && echo '  invfs exec as messagebus : PASS' || echo '  invfs exec as messagebus : FAIL'; "
                         "echo '--- is /dev itself FUSE-backed? (if so /dev/shm proves nothing):'; "
                         "df -T /dev/shm /usr/bin/dbus-daemon /tmp 2>&1 | sed 's/^/  /'"),
    # A non-root OPEN without a non-root exec: root opens the file and hands
    # the descriptor over via a pipe, so the only thing being varied is the uid
    # that read()s from an already-open fd. If that works while exec does not,
    # the fault is in the exec path specifically (or in the kernel's per-file
    # open-for-exec), not in InvariantFS's read path.
    ("dac-open-test", "echo '--- non-root READ of an already-open fd (no exec involved):'; "
                      "cat /usr/bin/dbus-daemon | setpriv --reuid=messagebus --regid=messagebus --clear-groups /usr/bin/wc -c 2>&1; "
                      "echo '--- non-root read of a 0755 file it did not open (cat the file):'; "
                      "setpriv --reuid=messagebus --regid=messagebus --clear-groups /bin/cat /etc/hostname 2>&1; "
                      "echo '--- non-root read of a WORLD-READABLE file:'; "
                      "setpriv --reuid=messagebus --regid=messagebus --clear-groups /usr/bin/head -c 20 /etc/hostname 2>&1; echo; "
                      "echo '--- dirs: can non-root even traverse into /usr/bin?'; "
                      "setpriv --reuid=messagebus --regid=messagebus --clear-groups /usr/bin/ls /usr/bin 2>&1 | head -3"),
    # /dev/shm was NOT a valid control. It failed too, which looks like proof the
    # filesystem is innocent -- but /dev/shm is mounted by systemd with
    # noexec,nosuid,nodev by DEFAULT, so I had tested "a non-FUSE filesystem"
    # while actually testing "a non-executable filesystem". I had filtered
    # /proc/mounts down to invfs|fuse and discarded the line that would have
    # said so. Third wrong control in this investigation; the pattern is that
    # every cheap control I reach for inherits a default that is not what I
    # think it is.
    #
    # So: mount a tmpfs with exec stated EXPLICITLY, rather than inheriting
    # anyone's default, and dump every mount option unfiltered.
    ("dac-ctl3", "echo '=== ALL mounts, unfiltered (options matter):'; "
                 "cat /proc/mounts; "
                 "echo; echo '=== root sanity: exec the control binary as ROOT'; "
                 "mount -t tmpfs -o exec,mode=0755 tmpfs /mnt 2>&1 && echo '  mounted /mnt exec' || echo '  MOUNT FAILED'; "
                 "cp /usr/bin/dbus-daemon /mnt/dbus-daemon && chmod 0755 /mnt/dbus-daemon && ls -l /mnt/dbus-daemon; "
                 "/mnt/dbus-daemon --version 2>&1 && echo '  root exec on exec-tmpfs : PASS' || echo '  root exec on exec-tmpfs : FAIL'; "
                 "echo; echo '=== THE CONTROL: same binary, same explicitly-exec tmpfs, as messagebus'; "
                 "setpriv --reuid=messagebus --regid=messagebus --clear-groups /mnt/dbus-daemon --version 2>&1 && echo '  messagebus exec on exec-tmpfs : PASS' || echo '  messagebus exec on exec-tmpfs : FAIL'; "
                 "echo; echo '=== and on InvariantFS, same binary, same uid'; "
                 "setpriv --reuid=messagebus --regid=messagebus --clear-groups /usr/bin/dbus-daemon --version 2>&1 && echo '  messagebus exec on invfs : PASS' || echo '  messagebus exec on invfs : FAIL'; "
                 "echo; echo '=== another non-root uid, to rule out something odd about 996'; "
                 "setpriv --reuid=1000 --regid=1000 --clear-groups /mnt/dbus-daemon --version 2>&1 && echo '  uid1000 exec on exec-tmpfs : PASS' || echo '  uid1000 exec on exec-tmpfs : FAIL'; "
                 "echo; echo '=== PID1 confinement (would explain a guest-wide exec block):'; "
                 "grep -E '^(NoNewPrivs|Seccomp|Seccomp_filters|CapEff|CapBnd):' /proc/1/status; "
                 "echo -n '  our own: '; grep -E '^(NoNewPrivs|Seccomp|CapEff):' /proc/self/status | tr '\\n' ' '; echo; "
                 "echo '=== kernel cmdline:'; cat /proc/cmdline; "
                 "umount /mnt 2>&1"),
    ("dbus-ns-props", "SYSTEMD_COLORS=0 systemctl show dbus.service "
                      "-p ProtectSystem -p ProtectHome -p PrivateTmp "
                      "-p PrivateDevices -p ProtectKernelTunables "
                      "-p ProtectKernelModules -p ProtectControlGroups "
                      "-p NoNewPrivileges -p DynamicUser -p RestrictNamespaces "
                      "-p SystemCallFilter -p MemoryDenyWriteExecute "
                      "-p RestrictAddressFamilies -p KeyringMode "
                      "-p UMask -p DynamicUser --no-pager; "
                      "ls -ld / /usr /usr/bin /lib64 /usr/lib64 2>&1"),
    ("dbus-manual-start", "systemctl reset-failed dbus.service dbus.socket 2>&1; "
                          "systemctl start dbus.socket 2>&1; "
                          "systemctl start dbus.service 2>&1; "
                          "echo ---; systemctl is-active dbus.socket dbus.service 2>&1; "
                          "SYSTEMD_COLORS=0 systemctl status dbus.service --no-pager -l 2>&1 | head -12"),
    ("dbus-unit", "systemctl cat dbus.service 2>&1 | grep -vE '^#|^$' | head -25"),
    ("target-deps", "SYSTEMD_COLORS=0 systemctl show multi-user.target "
                    "-p Wants -p Requires -p After --no-pager"),

    # THE REGRESSION THIS TREE COULD NOT SEE. Every boot harness in this repo
    # asserts as root, so "the guest booted" and "the guest is usable by a
    # normal user" looked identical for the entire life of the test suite --
    # right up to a guest where invf-fuse mounted the root WITHOUT allow_other,
    # which makes the mount private to the mounting uid (root). Every other uid
    # then gets EACCES on every file, and dbus.service fails every boot with
    # 203/EXEC purely because systemd runs it as User=messagebus.
    #
    # So assert what root cannot see. A green boot here says nothing about this;
    # a red one means the guest is unusable to anyone but root.
    #
    # Mount options are dumped unfiltered on purpose (WP225): filtering
    # /proc/mounts down to the lines you expect is how the missing `noexec` /
    # missing `allow_other` distinction got missed twice.
    ("nonroot-access", "echo '--- mount options for / (unfiltered):'; "
                       "awk '$2==\"/\"{print \"    \"$0}' /proc/mounts; "
                       "grep -c 'allow_other' /proc/mounts >/dev/null 2>&1; "
                       "awk '$2==\"/\"' /proc/mounts | grep -q 'allow_other' "
                       "&& echo '    allow_other on / : PRESENT' || echo '    allow_other on / : ABSENT'; "
                       "echo '--- can an unprivileged uid READ a root-owned file on / ?'; "
                       "setpriv --reuid=messagebus --regid=messagebus --clear-groups "
                       "/usr/bin/head -c 8 /etc/hostname >/dev/null 2>&1 "
                       "&& echo '    messagebus read /etc/hostname : PASS' || echo '    messagebus read /etc/hostname : FAIL'; "
                       "echo '--- can it EXEC a root-owned world-executable file on / ?'; "
                       "setpriv --reuid=messagebus --regid=messagebus --clear-groups "
                       "/usr/bin/dbus-daemon --version >/dev/null 2>&1 "
                       "&& echo '    messagebus exec dbus-daemon : PASS' || echo '    messagebus exec dbus-daemon : FAIL'; "
                       "echo '--- can it traverse and list a directory on / ?'; "
                       "setpriv --reuid=messagebus --regid=messagebus --clear-groups "
                       "/usr/bin/ls /usr/bin >/dev/null 2>&1 "
                       "&& echo '    messagebus ls /usr/bin : PASS' || echo '    messagebus ls /usr/bin : FAIL'; "
                       "echo '--- and the same binary on a NON-InvFS filesystem, as the control:'; "
                       "cp /usr/bin/dbus-daemon /dev/shm/nbd 2>/dev/null && chmod 0755 /dev/shm/nbd; "
                       "setpriv --reuid=messagebus --regid=messagebus --clear-groups "
                       "/dev/shm/nbd --version >/dev/null 2>&1 "
                       "&& echo '    messagebus exec on /dev/shm (tmpfs) : PASS' || echo '    messagebus exec on /dev/shm (tmpfs) : FAIL'; "
                       "rm -f /dev/shm/nbd"),
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
    # A probe result is sometimes a str and sometimes a dict -- the dict form
    # carries `__echo_only__` when the console only ever saw its own typed line
    # back and never any output. Calling .splitlines() on the dict raised
    # AttributeError and killed the run AFTER run01.json was written, so the
    # data survived but the exit status lied and the traceback buried the
    # summary. Normalise to text instead.
    for r in results:
        if not r["reached_multi_user"]:
            log(f"  FAILING run {r['run']}: last_target={r.get('last_target')}")
            for name, out in (r.get("probes") or {}).items():
                if not out:
                    continue
                text = out if isinstance(out, str) else "\n".join(
                    f"{k}: {v}" for k, v in out.items())
                for line in text.splitlines()[:14]:
                    log(f"      {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
