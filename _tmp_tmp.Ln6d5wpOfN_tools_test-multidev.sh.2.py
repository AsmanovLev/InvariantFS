import sys
rp = int(sys.argv[1])
d0 = open("/dev/shm/wp25md0.img","rb").read()
d1 = open("/dev/shm/wp25md1.img","rb").read()
blk = d0[rp*4096:(rp+1)*4096]
sys.exit(0 if d1.find(blk) >= 0 else 1)
