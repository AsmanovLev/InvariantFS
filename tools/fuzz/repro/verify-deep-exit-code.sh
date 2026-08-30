#!/bin/bash
# repro: invf-verify --deep exits 0 despite structural errors
# (found by tools/fuzz/bitflip.py iteration 22, seed 0x1A2B3C4D --
# "verify --deep rc=0 yet cat <file> rc=1" anomalies on images truncated
# inside the journal area).
#
# verify.c: the shallow path ends `return errors ? 1 : 0;`, but the
# --deep branch ends `return (bad || parity_bad) ? 1 : 0;` -- the
# structural `errors` counter (bad magic, short backing store, zone
# overlap, bitmap inconsistencies) never reaches the exit code when
# --deep is given. The error text IS printed; only the exit code lies.
#
# Observed: "backing store 3408 blocks vs superblock total_blocks 31457"
#           printed, exit code 0, "deep: 0 files ok, 0 corrupt".
# Expected: any structural err() forces exit code 1, with or without
#           --deep (fsck -q exits 3 on the same image).
# Layer:    tools/verify.c exit-code plumbing (not the storage engine).
set -e
B=/home/user/InvariantFS/bin
cd /dev/shm
rm -f repro-verify-deep.img
IMG=repro-verify-deep.img

$B/invf-mkfs "$IMG" 0.12 >/dev/null
printf 'hello world\n' > /tmp/repro-a.txt
$B/invf-cp "$IMG" /tmp/repro-a.txt a.txt >/dev/null
$B/invf-sweep "$IMG" >/dev/null 2>&1

echo "--- healthy: verify --deep"
$B/invf-verify "$IMG" --deep | tail -1
echo "rc=$? (want 0)"

# cut the image inside the L2P journal area (journal = blocks 2..8194,
# i.e. bytes 8192..33558528 on this 0.12 GB image)
truncate -s 13959168 "$IMG"

echo "--- truncated mid-journal: fsck"
$B/invf-fsck -q "$IMG" 2>&1 || echo "fsck rc=$? (3 = issues reported, good)"

echo "--- truncated mid-journal: verify --deep"
rc=0
$B/invf-verify "$IMG" --deep 2>&1 | grep -E "backing store|deep:" || true
$B/invf-verify "$IMG" --deep >/dev/null 2>&1 || rc=$?
echo "verify --deep rc=$rc  <-- BUG: 0 despite the 'backing store' error"
[ "$rc" -ne 0 ] && echo "OK (fixed)" || echo "REPRODUCED"
rm -f "$IMG" /tmp/repro-a.txt
