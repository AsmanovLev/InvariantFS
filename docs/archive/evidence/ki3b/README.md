# KI3b evidence — the raw basis for WP224 and WP225

These are the serial logs and probe results behind two findings in
`INCIDENTS.md`. They are committed because both findings **overturned earlier
conclusions**, and in WP225's case the conclusion is that InvariantFS is *not*
implicated. A claim that exonerates a filesystem should not rest on a sentence
with no receipt next to it.

Produced by `tools/ki3b-jobs.py`, which boots the Debian trixie root in
`/mnt/invfs-scratch/debboot5/deb.img` under QEMU and drives the guest over a
**UNIX-socket serial console** (`-serial unix:…,server=on,wait=on`) rather than
a file. The transport matters — see below.

| run | what it established |
|---|---|
| `confirm/` | 8 boots. Serial printed `Reached target multi-user.target` **0/8**; in-guest `multi-user.target` and `graphical.target` were `active` **8/8**. `ActiveEnterTimestamp` present every run. **The boot always completed.** |
| `dbus/` | `dbus.service` `status=203/EXEC`, 8/8. Ruled out the binary, the ELF interpreter, the exec path, and systemd hardening — then found `User=messagebus`, the one variable nothing else varied. |
| `uid/` | `runuser`/`setpriv` as `messagebus` get `Permission denied` exec'ing `dbus-daemon`; every `Protect*`/`Private*` knob is `no`. Not a systemd sandbox. |
| `dac/`, `tmpfs/` | `runuser -u messagebus -- sh` also fails: `/bin/sh` will not exec either. Non-root exec fails **systemically**. |
| `ctl2/` | First control attempt. InvariantFS mount is `rw,nosuid,nodev` — **no `noexec`**. `/dev/shm` test was ambiguous. |
| `ctl3/` | **The decisive run.** Full unfiltered `/proc/mounts`; a tmpfs mounted by root with `-o exec` stated *explicitly*. Root execs the binary (`PASS`), `messagebus` and `uid 1000` both get `Permission denied`. PID1 reports `NoNewPrivs: 0`, `Seccomp: 0`, `CapEff`/`CapBnd` all-ones. **Non-root exec is broken on every filesystem in this guest, including one that is not FUSE.** |

## Two things to know before reading these

**1. The socket transport is load-bearing.** Under `-serial unix:` the console
emits *no* `[ OK ] Reached target ...` lines at all, so `last_target` sits at
`swap.target` even on runs where `multi-user.target` is demonstrably active.
That absence is what produced two sessions of confident nonsense — a "hang near
`sys-fs-fuse-connections.mount`", then a "multi-user transaction that never
completes". Both were inferences from a marker that was not being emitted. The
8-of-10-vs-2-of-10 split recorded in the older `boot-debian-qemu.sh` logs came
from the **file** transport, which is a different I/O path; these runs do not
claim that variance was a capture artifact, only that the boot completes.

**2. Two of these measurements were wrong before the last one was right.**
`ctl2/`'s `/dev/shm` control was discarded on the belief that systemd mounts it
`noexec` by default; `ctl3/`'s unfiltered dump shows
`tmpfs /dev/shm tmpfs rw,nosuid,nodev` — no `noexec`, so that control was valid
and its failure was real evidence. The mistake was filtering `/proc/mounts` to
`invfs|fuse` and discarding the one line that would have answered the question.
The controls are kept, including the wrong turns, because "why is this file
here" is answerable and "why did you believe this" is the part worth knowing.

## Reproducing

Stage the root first with `tools/configure-debian.sh <staging-dir> sshd`
(see `docs/DEBIAN-INSTALL.md`), then:

    python3 tools/ki3b-jobs.py --volume <deb.img> --runs 8 --outdir <dir> `docs/DEBIAN-INSTALL.md`) and exclusive use of the loop device. **One QEMU at a
time** — the volume is exclusively attached. The guest writes to `deb.img` on
loop-backed ext4, so a block-device bug lands on the backing image: use
disposable scratch only.
