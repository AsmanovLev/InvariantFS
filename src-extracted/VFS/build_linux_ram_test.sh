#!/bin/bash
# Full cycle test in RAM (/dev/shm tmpfs): cp 20 JPEG -> sweep -> verify -> stat
set -e
cd /dev/shm
rm -f ram.img && /mnt/d/VFS/build_linux/invf-mkfs ram.img 2 > /dev/null
cd /dev/shm
for f in /mnt/d/VFS/build/media2/m*.jpg; do
  /mnt/d/VFS/build_linux/invf-cp ram.img "$f" "$(basename $f)" > /dev/null 2>&1 || echo "CP FAIL $f"
done
echo "=== cp done, files:"
/mnt/d/VFS/build_linux/invf-ls ram.img | tail -2
echo "=== sweep in RAM ==="
time /mnt/d/VFS/build_linux/invf-sweep ram.img 2>&1 | tail -2
echo "=== verify ==="
/mnt/d/VFS/build_linux/invf-verify ram.img | grep blocks
echo "=== stat ==="
/mnt/d/VFS/build_linux/invf-stat ram.img | sed 's/\x1b\[[0-9;]*m//g'
