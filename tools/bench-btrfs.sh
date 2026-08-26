#!/bin/bash
# bench-btrfs.sh — fairness counter-benchmark: same corpus onto a 1 GB btrfs
# image with maximum compression settings (compress-force=zstd:<max>, defrag
# recompress). Answers "what does the corpus cost on a compressed POSIX FS".
# Usage: bash tools/bench-btrfs.sh [corpus_dir]   (default: /var/tmp/bench/silesia)
set -e
set -o pipefail

CORPUS=${1:-/var/tmp/bench/silesia}
IMG=/var/tmp/bench/btrfsbench.img
MNT=/var/tmp/bench/btrfsmnt
LVL=15   # btrfs kernel max zstd level is 15 (19 is tar|zstd territory)

rm -f "$IMG"; mkdir -p "$MNT"
truncate -s 1G "$IMG"
mkfs.btrfs -q -f "$IMG" >/dev/null

sudo mount -o loop,compress-force=zstd:$LVL "$IMG" "$MNT"
trap 'sudo umount "$MNT" 2>/dev/null || true' EXIT

sudo cp -a "$CORPUS"/. "$MNT"/
sync
# belt & braces: recompress everything at max level (kills any first-write
# heuristics leftovers)
sudo btrfs filesystem defrag -r -czstd -L$LVL "$MNT" >/dev/null 2>&1 || true
sync

echo "== btrfs (zstd:$LVL, compress-force) =="
sudo btrfs filesystem usage -b "$MNT" | grep -E "Data|Metadata" | grep -v free
df -B1 "$MNT" | tail -1
echo
echo "== compsize =="
sudo compsize -x "$MNT" 2>/dev/null | tail -12 || sudo compsize "$MNT" | tail -12
sudo umount "$MNT"
trap - EXIT
echo "image kept: $IMG"
