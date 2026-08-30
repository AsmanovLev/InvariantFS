#!/bin/bash
# repro: invf-cat (and the zip.c member-extract path) report success and
# exit 0 while the OUTPUT file silently lost bytes on a full filesystem.
# (found by tools/fuzz/opseq.py / bitflip.py when /dev/shm hit its quota
# mid-run: "SILENT-GARBAGE: cat rc=0 but bytes differ, got 0..N bytes".)
#
# cat.c:85-86 / zip.c:200-201:
#       fwrite(data, 1, len, f);
#       fclose(f);
#   printf("extracted '%s' -> %s (%zu bytes)\n", ...);
# neither fwrite nor fclose is checked, so a short/failed write (ENOSPC,
# EDQUOT, EIO on the output fs) still prints "extracted N bytes" and
# returns 0 -- a restore pipeline consuming invf-cat archives empty files
# believing they were restored.
#
# Observed: "extracted 'a.bin' -> a.out (100000 bytes)", rc=0, a.out is
#           0 bytes.
# Expected: nonzero exit + error message when any output byte is lost.
# Layer:    tools (cat.c, zip.c) output path; the storage engine itself
#           correctly fails image-side writes loudly (probe below shows
#           "[create] write fail seg 0" + rc=1 under backing-store EDQUOT).
set -e
B=/home/user/InvariantFS/bin
cd /dev/shm
rm -f repro-cat.img
$B/invf-mkfs repro-cat.img 0.12 >/dev/null
python3 -c "open('/dev/shm/repro-a.bin','wb').write(b'A'*100000)"
$B/invf-cp repro-cat.img /dev/shm/repro-a.bin a.bin >/dev/null

# a tiny full output filesystem (2 MiB tmpfs, then fill it)
MNT=/dev/shm/repro-cat-mnt
rm -rf "$MNT" && mkdir -p "$MNT"
if sudo -n mount -t tmpfs -o size=2M none "$MNT" 2>/dev/null; then
    sudo -n chown "$(id -u):$(id -g)" "$MNT"
    dd if=/dev/zero of="$MNT/fill" bs=1M count=2 status=none 2>/dev/null || true
    echo "--- cat onto a full filesystem:"
    rc=0
    $B/invf-cat repro-cat.img a.bin "$MNT/a.out" || rc=$?
    ls -l "$MNT/a.out"
    echo "invf-cat rc=$rc  <-- BUG if 0 with a short/empty output file"
    sudo -n umount "$MNT"
else
    echo "sudo mount unavailable; skipped the mount-based leg"
fi
rm -rf "$MNT" repro-cat.img /dev/shm/repro-a.bin
