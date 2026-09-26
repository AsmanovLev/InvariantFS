mkdir /src
write $WORK/stage/tar.c /src/tar.c
write $WORK/stage/head256k.bin /sparse2m.bin
sif /sparse2m.bin size 2097152
