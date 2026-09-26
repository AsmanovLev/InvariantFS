mkdir /src
write $WORK/stage/vi.c /src/vi.c
write $WORK/stage/ping.c /src/ping.c
mkdir /bin
write $WORK/stage/busybox.elf /bin/busybox.elf
mkdir /deep
mkdir /deep/a
mkdir /deep/a/b
mkdir /deep/a/b/c
mkdir /deep/a/b/c/d
write $WORK/stage/cp.c /deep/a/b/c/d/leaf.c
mkdir /hl
write $WORK/stage/hl.txt /hl/alpha.txt
link /hl/alpha.txt /hl/beta.txt
write $WORK/stage/empty.bin /empty.bin
write $WORK/stage/head256k.bin /sparse4m.bin
sif /sparse4m.bin size 4194304
