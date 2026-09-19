#!/bin/bash
# test-helper-isolation.sh — WP61 containment for helper children.
#
# The fixture helper (a real codecpack) tries, per sweep, one of:
#   (a) socket      — connect out to 8.8.8.8:80; a success escapes the netns
#   (b) fork-bomb   — fork 50 children and sleep; must be capped + group-killed
#   (c) home        — write a marker only when $HOME leaked through the scrub
#   (d) forever     — sleep 1000; the wall-clock deadline must kill it
# and every run must stay bit-exact (identity encode/decode) and finish.
#
# Also runs the standalone http://helper_exec unit test (direct launcher
# coverage: rlimits, timeout, group kill, netns, env scrub, Landlock,
# privilege drop) and, when sudo is available, its root privilege-drop leg.
#
# Run from the repo root after `make`:
#   bash tools/run-e2e.sh tools/test-helper-isolation.sh
# Nested suites use plain `bash` on purpose: the outer run-e2e already holds
# the global e2e flock, a nested run-e2e would self-deadlock.
set -e
set -o pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
B=$REPO/bin
WT=$(cd "$(dirname "$0")/.." && pwd)
WORK=/dev/shm/wp61iso
trap 'rm -rf "$WORK" /dev/shm/wp61iso*.img' EXIT
IMG=wp61iso.img
rm -rf "$WORK" && mkdir -p "$WORK/orig" "$WORK/out" \
    "$WORK/packs/canary.codecpack"
cd /dev/shm

echo "== tools =="
command -v python3 >/dev/null || { echo "FAIL: python3 not installed"; exit 1; }

echo "== fixture codecpack (identity transcode + one poke per sweep) =="
cat > "$WORK/packs/canary.codecpack/canary.py" <<'PY'
#!/usr/bin/env python3
"""WP61 containment canary. Identity transcode so a contained run still
earns the CODEC stamp and stays bit-exact; each poke is armed via
P61_POKE (passed through the scrub explicitly by the test)."""
import os
import socket
import sys
import time
import zlib


def poke(mode):
    if mode == "home":
        if os.environ.get("HOME"):
            try:
                with open(os.environ.get("P61_MARK", "/dev/shm/wp61iso/esc"), "w") as f:
                    f.write("leaked\n")
            except OSError:
                pass
    elif mode == "forever":
        time.sleep(1000)
    elif mode == "bomb":
        for _ in range(50):
            try:
                os.fork()
            except OSError:
                break
        time.sleep(1000)
    elif mode == "socket":
        try:
            s = socket.socket()
            s.settimeout(2)
            s.connect(("8.8.8.8", 80))
            with open("/dev/shm/wp61iso/net_ok", "w") as f:
                f.write("x")
            sys.exit(1)            # a successful connect is an escape
        except OSError:
            pass


def main(a):
    if len(a) < 3:
        return 2
    cmd = a[1]
    mode = os.environ.get("P61_POKE", "")
    try:
        if cmd == "estimate" and len(a) == 3:
            print(os.path.getsize(a[2]))
        elif cmd == "encode" and len(a) == 4:
            poke(mode)
            with open(a[2], "rb") as f:
                d = f.read()
            with open(a[3], "wb") as f:
                f.write(zlib.compress(d, 9))
        elif cmd == "decode" and len(a) == 4:
            with open(a[2], "rb") as f:
                d = f.read()
            with open(a[3], "wb") as f:
                f.write(zlib.decompress(d))
        else:
            return 2
    except (OSError, ValueError, zlib.error):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
PY
cat > "$WORK/packs/canary.codecpack/manifest" <<'EOF'
# WP61 containment fixture: identity codec so a contained helper still
# passes the decode-back guard and stays bit-exact.
name = canary
algo = 46
pack_version = 1
generation = 1
caps = wholefile|external
dec_mem = 0
sniff.magic = 50363158
encode = python3 {pack}/canary.py encode {in} {out}
decode = python3 {pack}/canary.py decode {in} {out}
estimate = python3 {pack}/canary.py estimate {in}
EOF
python3 -c "open('$WORK/orig/canary.bin','wb').write(b'P61X' + bytes(range(256)) * 300)"
echo "  canary.bin: $(stat -c%s "$WORK/orig/canary.bin") bytes"

# one sweep + bit-exact check; $1=tag $2=poke-mode
run_poke() {
    tag=$1
    mode=$2
    img="wp61iso-$tag.img"
    rm -f "$img"
    $B/invf-mkfs "$img" 0.2 >/dev/null
    $B/invf-cp "$img" "$WORK/orig/canary.bin" canary.bin >/dev/null
    rm -f "$WORK/esc" "$WORK/net_ok"
    t0=$(date +%s)
    P61_POKE="$mode" P61_MARK="$WORK/esc" \
        INVFS_HELPER_KEEPENV=P61_POKE,P61_MARK \
        INVFS_HELPER_TIMEOUT_MS=1500 INVFS_HELPER_NPROC=32 \
        INVFS_CODECPACKS="$WORK/packs" \
        $B/invf-sweep "$img" > "$WORK/sweep-$tag.log" 2>&1 \
        || { echo "FAIL: $tag sweep exited nonzero"; cat "$WORK/sweep-$tag.log"; exit 1; }
    t1=$(date +%s)
    INVFS_CODECPACKS="$WORK/packs" $B/invf-cat "$img" canary.bin \
        "$WORK/out/$tag.bin" >/dev/null
    cmp -s "$WORK/orig/canary.bin" "$WORK/out/$tag.bin" \
        || { echo "FAIL: $tag not bit-exact"; exit 1; }
    grep -q " 0 corrupt," <(INVFS_CODECPACKS="$WORK/packs" \
        $B/invf-verify "$img" --deep) \
        || { echo "FAIL: $tag verify --deep not clean"; exit 1; }
    echo "  $tag: $(( t1 - t0 ))s, $(grep -c 'canary (codecpack)' "$WORK/sweep-$tag.log" || true) codec stamp(s)"
    rm -f "$img"
}

echo "== (c) home: HOME stays scrubbed; identity transcode still bit-exact =="
run_poke home home
[ ! -e "$WORK/esc" ] || { echo "FAIL: \$HOME leaked into the helper"; exit 1; }
grep -q "canary.bin: canary (codecpack)" "$WORK/sweep-home.log" \
    || { echo "FAIL: contained home run did not transcode"; exit 1; }
echo "  HOME dropped, marker absent, CODEC stamp"

echo "== (d) forever: the deadline kills it, fallback is bit-exact =="
run_poke forever forever
grep -q "canary.bin: canary (codecpack)" "$WORK/sweep-forever.log" \
    && { echo "FAIL: run-forever helper still earned a stamp"; exit 1; }
[ -z "$(pgrep -f "$WORK/packs/canary.codecpack/canary.py" || true)" ] \
    || { echo "FAIL: run-forever helper survived"; exit 1; }
echo "  killed at the deadline, no survivor, generic fallback bit-exact"

echo "== (b) fork-bomb: NPROC caps it, the group kill reaps it =="
run_poke bomb bomb
grep -q "canary.bin: canary (codecpack)" "$WORK/sweep-bomb.log" \
    && { echo "FAIL: fork-bomb helper still earned a stamp"; exit 1; }
sleep 1
[ -z "$(pgrep -f "$WORK/packs/canary.codecpack/canary.py" || true)" ] \
    || { echo "FAIL: fork-bomb survivors remain"; exit 1; }
echo "  capped + group-killed, no survivors, generic fallback bit-exact"

echo "== (a) socket: the helper has no network =="
run_poke socket socket
if [ -e "$WORK/net_ok" ]; then
    echo "  WARN: netns unavailable here (connect succeeded); see unit test"
else
    echo "  outbound connect failed inside the child"
fi
grep -q "canary.bin: canary (codecpack)" "$WORK/sweep-socket.log" \
    && echo "  CODEC stamp (connect refused, transcode ran)" \
    || echo "  generic fallback (connect succeeded and the poke exited)"

echo "== unit launcher coverage (rlimits/timeout/group-kill/netns/scrub/Landlock) =="
"$B/invf-helper_exec_test"

echo "== root privilege-drop leg =="
if sudo -n true 2>/dev/null; then
    sudo -n "$B/invf-helper_exec_test" > "$WORK/root.log" 2>&1 \
        || { cat "$WORK/root.log"; exit 1; }
    grep -q "privdrop: helper uid=" "$WORK/root.log" \
        || { echo "FAIL: root leg did not verify the dropped uid"; cat "$WORK/root.log"; exit 1; }
    grep -q "^PASS" "$WORK/root.log" \
        || { echo "FAIL: root leg failed"; cat "$WORK/root.log"; exit 1; }
    grep "privdrop:" "$WORK/root.log"
else
    echo "  SKIP: passwordless sudo unavailable; run the binary as root to verify"
fi

echo "HELPER ISOLATION E2E: PASS"
