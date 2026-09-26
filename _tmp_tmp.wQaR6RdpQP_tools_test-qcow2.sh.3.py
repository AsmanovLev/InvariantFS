import json
import subprocess
import sys
w = sys.argv[1]
raw = open(w + "/out/qgen-vdisk.raw", "rb").read()
ref = open(w + "/orig/member-qgen.ref", "rb").read()
mp = json.loads(subprocess.run(
    ["qemu-img", "map", "--output=json", w + "/orig/qgen.qcow2"],
    capture_output=True, check=True).stdout)
expect = b"".join(raw[int(e["start"]):int(e["start"]) + int(e["length"])]
                  for e in mp if e["data"])
assert expect == ref, "python walk disagrees with qemu-img map's allocation"
print("  qemu-img map/convert cross-check: python walk == qemu's allocation")
