import os
import struct
import subprocess
import sys

d = sys.argv[1]
base = open(os.path.join(d, "diska.qcow2"), "rb").read()


def w(name, mut):
    b = bytearray(base)
    mut(b)
    open(os.path.join(d, name), "wb").write(bytes(b))


w("badmagic.qcow2", lambda b: b.__setitem__(0, 0x51 ^ 0xFF))
w("enc.qcow2", lambda b: struct.pack_into(">I", b, 32, 1))      # crypt AES
w("cbits.qcow2", lambda b: struct.pack_into(">I", b, 20, 8))    # 256B clusters
w("incompat.qcow2", lambda b: struct.pack_into(">Q", b, 72, 2))  # corrupt bit

# duplicate cluster reference: point a second L2 entry at the first data
# cluster's offset
b = bytearray(base)
l1 = struct.unpack(">Q", b[40:48])[0]
l2 = struct.unpack(">Q", b[l1:l1 + 8])[0] & 0x3FFFFFFFFFFFFFFF
e0 = struct.unpack(">Q", b[l2:l2 + 8])[0]
struct.pack_into(">Q", b, l2 + 8, e0)    # guest cluster 1 -> same extent as 0
open(os.path.join(d, "dupcl.qcow2"), "wb").write(bytes(b))

# data cluster past EOF
b = bytearray(base)
struct.pack_into(">Q", b, l2 + 8, 0x8000000000400000)
open(os.path.join(d, "pasteof.qcow2"), "wb").write(bytes(b))

# qemu-generated: backing file / internal snapshot / compressed / extended
# L2 / refcount_bits=32 / empty (no writes)
subprocess.run(["qemu-img", "create", "-f", "qcow2", d + "/bb.qcow2", "4M"],
               capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", "-b", d + "/bb.qcow2",
                "-F", "qcow2", d + "/backed.qcow2"], capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", d + "/snap.qcow2", "4M"],
               capture_output=True)
subprocess.run(["qemu-io", "-c", "write -P 1 0 64k", d + "/snap.qcow2"],
               capture_output=True)
subprocess.run(["qemu-img", "snapshot", "-c", "s1", d + "/snap.qcow2"],
               capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", d + "/comp.qcow2", "4M"],
               capture_output=True)
subprocess.run(["qemu-io", "-c", "write -P 0x7f 0 64k", d + "/comp.qcow2"],
               capture_output=True)
subprocess.run(["qemu-img", "convert", "-f", "qcow2", "-O", "qcow2", "-c",
                d + "/comp.qcow2", d + "/compc.qcow2"], capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", "-o", "extended_l2=on",
                d + "/extl2.qcow2", "4M"], capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", "-o", "refcount_bits=32",
                d + "/rb32.qcow2", "4M"], capture_output=True)
subprocess.run(["qemu-img", "create", "-f", "qcow2", d + "/empty.qcow2", "1M"],
               capture_output=True)
os.unlink(d + "/bb.qcow2")
os.unlink(d + "/comp.qcow2")
print("  decline fixtures written")
