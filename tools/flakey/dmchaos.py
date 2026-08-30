#!/usr/bin/env python3
"""
dmchaos.py — seeded dm-flakey table toggler for WP22b (tools/test-flakey.sh).

Alternates a dm device between three tables on a seeded schedule:

  up    — flakey with a huge up interval / no down interval (pass-through);
  drop  — flakey with drop_writes (1s up / 1s down autonomous windows):
          write bios complete with SUCCESS but never reach the device
          (a lying write cache / power loss at a random writeback point);
  error — the pure error target: every bio (read or write) fails with EIO
          (device off the bus).

Usage:
  dmchaos.py <dmname> <loopdev> <sectors> <seed> <max_seconds> <stopfile>
             [bias]

<bias> "even" (default): up/drop/error ≈ 45/40/15 with 0.5..2.5s modes.
<bias> "drop": drop-heavy for the sweep/seal legs — 15/75/10 with
0.3..1.2s modes, and the FIRST mode is a drop window so a fast sweep
cannot outrun the chaos. "droponly": same but never error windows (for
legs that assert a specific process rc).

Runs until <max_seconds> elapse, <stopfile> appears, or SIGTERM arrives;
ALWAYS restores the up table on the way out (finally + signal handler).
Every transition is logged with a monotonic timestamp, so a failure log
replays which windows an operation straddled. The schedule (mode sequence
and durations) is fully determined by <seed>; wall-clock jitter remains.

Needs passwordless sudo for dmsetup(8).
"""

import os
import random
import signal
import subprocess
import sys
import time


def main():
    if len(sys.argv) not in (7, 8):
        sys.exit("usage: dmchaos.py <dmname> <loopdev> <sectors> <seed> "
                 "<max_seconds> <stopfile> [even|drop]")
    name, loop, sec, seed = sys.argv[1], sys.argv[2], int(sys.argv[3]), \
        int(sys.argv[4])
    max_s, stopfile = float(sys.argv[5]), sys.argv[6]
    bias = sys.argv[7] if len(sys.argv) == 8 else "even"

    def table(mode):
        if mode == "up":
            return "0 %d flakey %s 0 3600 0" % (sec, loop)
        if mode == "drop":
            return "0 %d flakey %s 0 1 1 1 drop_writes" % (sec, loop)
        return "0 %d error" % sec          # error

    def dm_set(mode):
        """Swap the live table. suspend --noflush: a flush cannot complete
        against an error target, and we do not want one anyway (dirty pages
        must survive the swap so the NEW table decides their fate)."""
        for _ in range(20):
            r = subprocess.run(["sudo", "-n", "dmsetup", "suspend",
                                "--noflush", name],
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
            if r.returncode == 0:
                break
            time.sleep(0.2)
        else:
            raise RuntimeError("dm_set: suspend never succeeded")
        try:
            subprocess.run(["sudo", "-n", "dmsetup", "reload", name,
                            "--table", table(mode)], check=True,
                           stdout=subprocess.DEVNULL)
        finally:
            subprocess.run(["sudo", "-n", "dmsetup", "resume", name],
                           check=True, stdout=subprocess.DEVNULL)

    rng = random.Random(seed)
    t0 = time.monotonic()
    stop = {"flag": False}

    def on_term(signo, frame):
        stop["flag"] = True
    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)

    mode = "up"
    dm_set(mode)                        # known starting point
    print("t=+0.00 up (start)", flush=True)
    first = True
    try:
        while not stop["flag"] and not os.path.exists(stopfile):
            if time.monotonic() - t0 >= max_s:
                break
            if bias in ("drop", "droponly"):
                weights = [15, 85, 0] if bias == "droponly" else [15, 75, 10]
                nxt = "drop" if first else rng.choices(
                    ["up", "drop", "error"], weights)[0]
                dur = rng.uniform(0.3, 1.2)
            else:
                nxt = rng.choices(["up", "drop", "error"],
                                  [45, 40, 15])[0]
                dur = rng.uniform(0.5, 2.5)
            first = False
            if nxt != mode:
                dm_set(nxt)
                mode = nxt
                print("t=%+.2f %s" % (time.monotonic() - t0, nxt),
                      flush=True)
            # sleep in ticks so the stopfile / SIGTERM react fast
            end = time.monotonic() + dur
            while time.monotonic() < end and not stop["flag"] \
                    and not os.path.exists(stopfile):
                time.sleep(0.05)
    finally:
        dm_set("up")
        print("t=%+.2f up (end)" % (time.monotonic() - t0), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
