# Doc-to-Code Map (Linux-relevant audit lanes)

| doc | audit lane | primary sources |
|---|---|---|
| 02-on-disk-format | A1 format | volume.h, volume.c, mkfs.c, fsck.c |
| 03-ast-recipe, 04-compression-matrix, 05-data-classification, 06-sweep-worker | A2 semantics/transcode | sweep.c, arc.c/h, flacx.c, pngx.c/h, tarx.c, zip.c, gzrepro.c |
| 07-read-write-path, 15-caching | A3 IO/cache | volume.c, blkio.c/h, fuse_fs.c |
| 08-crash-recovery, 12-enospc-strategy | A4 recovery | volume.c, fsck.c, enospctest.c |
| 13-linux-rootfs + ChangeLog + build_linux.sh + f6*.log + tests_fuse.ps1 | A5 port-critical | fuse_fs.c vs doc recipe |
| 10-deduplication, 11-security-and-permissions, 18-test-coverage | A6 dedup/security/tests | volume.c, fuse_fs.c, dokan_fs.c, tests.ps1, devtest*.c |

Skipped per scope: 09-windows-port, 14-windows-io-deep, 16-benchmarks, 17-template-zone
