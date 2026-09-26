mkdir /src
write $WORK/stage/cat.c /src/cat.c
write $WORK/stage/cp.c /src/cp.c
write $WORK/stage/ls.c /src/ls.c
write $WORK/stage/tar.c /src/tar.c
write $WORK/stage/vi.c /src/vi.c
write $WORK/stage/ping.c /src/ping.c
mkdir /bin
write $WORK/stage/busybox.elf /bin/busybox.elf
mkdir /deep
mkdir /deep/a
mkdir /deep/a/b
mkdir /deep/a/b/c
mkdir /deep/a/b/c/d
mkdir /deep/a/b/c/d/e
write $WORK/stage/cat.c /deep/a/b/c/d/e/leaf.c
mkdir /hl
write $WORK/stage/hl.txt /hl/alpha.txt
link /hl/alpha.txt /hl/beta.txt
write $WORK/stage/empty.bin /empty.bin
write $WORK/stage/head512k.bin /falloc.bin
fallocate /falloc.bin 128 512
sif /falloc.bin size 2097152
write $WORK/stage/head1m.bin /sparse8m.bin
sif /sparse8m.bin size 8388608
