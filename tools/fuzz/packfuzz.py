#!/usr/bin/env python3
"""
packfuzz.py — containerpack parser fuzz for InvariantFS (fuzz wave 3/4).

For each pack in tools/codecpacks/ with a C helper (rawdisk, ext4fs,
fatfs, xfs, ntfs, vdi, qcow2): build a tiny VALID fixture image (hand-built
for rawdisk/vdi, mkfs tools + debugfs/mcopy/loop-mount for the fs packs,
qemu-img/qemu-io for qcow2), then feed mutated copies to the helper's
enumerate / map (and, sampled, estimate) commands.

  default: 500 mutants per pack, deterministic from --seed.

INVARIANTS per mutant (a violation is a bug report, mutant kept):
  * helper exits 0 (valid answer), 1 (error) or 3 (decline) -- anything
    else, a signal, or a >10s hang is a FAILURE;
  * a successful enumerate prints a sane member table: numeric idx in
    [0, 65535], usize <= 10x the mutant's file size;
  * a successful map prints a well-formed MRMP blob partitioning
    [0, mutant_size) exactly (FS-owned format, checked with a local
    oracle mirroring volume.c's cpack_map_parse);
  * a successful estimate prints a u64 <= 10x size + 64 MiB.

Repro: the failing mutant image is preserved under
<workdir>/failures/<pack>-NNNNN.img; rerun the printed command.
"""

import argparse
import os
import random
import shutil
import struct
import subprocess
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzutil as fz

CPACK_MAP_MAX_ENTS = 4 * 65535 + 4
EST_MARGIN = 64 << 20


# ------------------------------------------------------- fixture builders --

def build_rawdisk(path):
    """Deterministic GPT image: 2 partitions with text-ish content."""
    rng = random.Random(0x5D00D15C)
    sec = 512
    nsec = 8192                       # 4 MiB
    img = bytearray(rng.randbytes(nsec * sec))
    for i in range(0, 34):            # keep LBA0..33 area controlled
        img[i * sec:(i + 1) * sec] = b"\0" * sec
    img[nsec * sec - 33 * sec:] = b"\0" * (33 * sec)
    # protective MBR
    img[510] = 0x55
    img[511] = 0xAA
    img[446 + 4] = 0xEE
    struct.pack_into("<II", img, 446 + 8, 1, nsec - 1)
    # two partitions: [2048, 4095] and [4096, 6143]
    parts = [(2048, 4095, b"alpha"), (4096, 6143, b"beta")]
    entries = bytearray()
    for i, (first, last, name) in enumerate(parts):
        e = bytearray(128)
        e[0:16] = bytes(range(16))    # nonzero type GUID
        e[16:32] = bytes([i + 1] * 16)
        struct.pack_into("<QQQ", e, 32, first, last, 0)
        utf = ("part%d" % i).encode("utf-16-le")
        e[56:56 + len(utf)] = utf
        entries += e
    entries += b"\0" * (128 * 128 - len(entries))
    img[2 * sec:2 * sec + len(entries)] = entries
    ent_crc = zlib.crc32(bytes(entries))
    # GPT header at LBA1
    hdr = bytearray(92)
    hdr[0:8] = b"EFI PART"
    struct.pack_into("<I", hdr, 8, 0x00010000)
    struct.pack_into("<I", hdr, 12, 92)
    struct.pack_into("<I", hdr, 16, 0)          # crc placeholder
    struct.pack_into("<I", hdr, 20, 0)
    struct.pack_into("<Q", hdr, 24, 1)          # current lba
    struct.pack_into("<Q", hdr, 32, nsec - 1)   # backup lba
    struct.pack_into("<Q", hdr, 40, 34)         # first usable
    struct.pack_into("<Q", hdr, 48, nsec - 34)  # last usable
    hdr[56:72] = rng.randbytes(16)
    struct.pack_into("<Q", hdr, 72, 2)          # entries lba
    struct.pack_into("<I", hdr, 80, 128)        # num entries
    struct.pack_into("<I", hdr, 84, 128)        # entry size
    struct.pack_into("<I", hdr, 88, ent_crc)
    struct.pack_into("<I", hdr, 16, zlib.crc32(bytes(hdr)))
    img[sec:sec + 92] = hdr
    # partition content
    for first, last, name in parts:
        body = (name * 2000)[: (last - first + 1) * sec]
        img[first * sec:first * sec + len(body)] = body
    with open(path, "wb") as f:
        f.write(bytes(img))


def build_vdi(path):
    """Deterministic dynamic VDI v1.1: 4 blocks of 1 MiB, 2 allocated."""
    cb_block = 1 << 20
    c_blocks = 4
    off_blocks = 0x400
    off_data = 0x200000
    cb_disk = c_blocks * cb_block
    rng = random.Random(0xBD10)
    size = off_data + 2 * cb_block
    img = bytearray(b"\0" * size)
    comment = b"<<< InvariantFS fuzz fixture >>>\n"
    img[0:len(comment)] = comment
    struct.pack_into("<IIIIII", img, 0x40, 0xBEDA107F, 0x00010001,
                     0x180, 1, 0, 0)
    img[0x154:0x15C] = struct.pack("<II", off_blocks, off_data)
    struct.pack_into("<II", img, 0x168, 512, 0)
    struct.pack_into("<Q", img, 0x170, cb_disk)
    struct.pack_into("<II", img, 0x178, cb_block, 0)
    struct.pack_into("<II", img, 0x180, c_blocks, 2)
    # bmap: block 0 -> slot 0, block 1 unallocated, block 2 -> slot 1,
    # block 3 unallocated
    struct.pack_into("<IIII", img, off_blocks, 0, 0xFFFFFFFF, 1,
                     0xFFFFFFFF)
    img[off_data:off_data + 4096] = b"FUZZVDI0" * 512
    img[off_data + cb_block: off_data + cb_block + 4096] = b"FUZZVDI2" * 512
    with open(path, "wb") as f:
        f.write(bytes(img))


def sh(cmd, **kw):
    subprocess.run(cmd, shell=True, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, **kw)


def build_ext4fs(path):
    with open(path, "wb") as f:
        f.truncate(16 << 20)
    sh("mkfs.ext4 -q -F -b 4096 %s" % path)
    stage = path + ".stage"
    os.makedirs(stage, exist_ok=True)
    with open(os.path.join(stage, "hello.c"), "w") as f:
        f.write("int main(void){return 0;}\n" * 200)
    with open(os.path.join(stage, "data.bin"), "wb") as f:
        f.write(random.Random(7).randbytes(30000))
    sh('printf "write %s/hello.c hello.c\\nwrite %s/data.bin data.bin\\n" '
       '| debugfs -w %s' % (stage, stage, path))
    shutil.rmtree(stage)


def build_fatfs(path):
    with open(path, "wb") as f:
        f.truncate(16 << 20)
    sh("mkfs.vfat -F 16 %s" % path)
    stage = path + ".stage"
    os.makedirs(stage, exist_ok=True)
    with open(os.path.join(stage, "README.TXT"), "w") as f:
        f.write("fat fuzz fixture\n" * 100)
    with open(os.path.join(stage, "BLOB.BIN"), "wb") as f:
        f.write(random.Random(8).randbytes(20000))
    sh("mcopy -i %s %s/README.TXT %s/BLOB.BIN ::" % (path, stage, stage))
    shutil.rmtree(stage)


def _loop_populate(path, files):
    mnt = path + ".mnt"
    os.makedirs(mnt, exist_ok=True)
    fstype = "ntfs-3g" if path.endswith(".ntfs") else "xfs"
    subprocess.run("sudo -n mount -o loop -t %s %s %s"
                   % (fstype, path, mnt), shell=True, check=True)
    subprocess.run("sudo -n chown %d:%d %s"
                   % (os.getuid(), os.getgid(), mnt), shell=True, check=True)
    try:
        for name, data in files:
            with open(os.path.join(mnt, name), "wb") as f:
                f.write(data)
    finally:
        subprocess.run("sudo -n umount %s" % mnt, shell=True, check=True)
        os.rmdir(mnt)


def build_xfs(path):
    with open(path, "wb") as f:
        f.truncate(300 << 20)
    sh("mkfs.xfs -f -m crc=1,finobt=1,rmapbt=0,reflink=0,inobtcount=0,"
       "bigtime=1 -i size=512,sparse=0,nrext64=0 -n ftype=1 %s" % path)
    _loop_populate(path, [("afile.c", b"/* xfs fixture */\n" * 500),
                          ("blob.bin", random.Random(9).randbytes(50000))])


def build_ntfs(path):
    with open(path, "wb") as f:
        f.truncate(16 << 20)
    sh("mkfs.ntfs -Q -F %s" % path)
    _loop_populate(path, [("note.txt", b"ntfs fixture\n" * 500),
                          ("blob.bin", random.Random(10).randbytes(50000))])


def build_qcow2(path):
    sh("qemu-img create -f qcow2 %s 16M" % path)
    payload = b"qcow2 fixture cluster\n" * 300
    with open(path + ".pay", "wb") as f:
        f.write(payload)
    sh("qemu-io %s -c 'write -P 0x41 0 128k' -c 'write -P 0x42 1M 64k'"
       % path)
    os.unlink(path + ".pay")


PACKS = {                       # name -> (source, fixture builder)
    "rawdisk": ("rawdisk.codecpack/rawdisk.c", build_rawdisk),
    "ext4fs": ("ext4fs.codecpack/ext4fs.c", build_ext4fs),
    "fatfs": ("fatfs.codecpack/fatfs.c", build_fatfs),
    "xfs": ("xfs.codecpack/xfs.c", build_xfs),
    "ntfs": ("ntfs.codecpack/ntfs.c", build_ntfs),
    "vdi": ("vdi.codecpack/vdi.c", build_vdi),
    "qcow2": ("qcow2.codecpack/qcow2.c", build_qcow2),
}
FIXT_EXT = {"rawdisk": ".img", "ext4fs": ".ext4", "fatfs": ".fat",
            "xfs": ".xfs", "ntfs": ".ntfs", "vdi": ".vdi",
            "qcow2": ".qcow2"}


# ------------------------------------------------------------- checking --

class PackFailure(Exception):
    pass


def check_table(data, size, pack, mid):
    """enumerate rc==0 -> the member table must be sane."""
    n = 0
    for line in data.split(b"\n"):
        if not line:
            continue
        n += 1
        parts = line.split(b"\t")
        if len(parts) != 3:
            raise PackFailure("%s #%d: table line not idx\\tsname\\tusize: "
                              "%r" % (pack, mid, line[:120]))
        try:
            idx = int(parts[0])
            usize = int(parts[2])
        except ValueError:
            raise PackFailure("%s #%d: non-numeric idx/usize: %r"
                              % (pack, mid, line[:120]))
        if not (0 <= idx <= 65535):
            raise PackFailure("%s #%d: idx %d out of range"
                              % (pack, mid, idx))
        if usize < 0 or usize > 10 * size:
            raise PackFailure("%s #%d: NONSENSE usize %d (mutant is %d "
                              "bytes)" % (pack, mid, usize, size))
    if n > 65536:
        raise PackFailure("%s #%d: %d members (> 65536)" % (pack, mid, n))


def check_map(data, size, pack, mid):
    """map rc==0 -> MRMP must partition [0, size) exactly."""
    if len(data) < 8 or data[:4] != b"MRMP":
        raise PackFailure("%s #%d: map rc=0 but bad MRMP header"
                          % (pack, mid))
    count = struct.unpack_from("<I", data, 4)[0]
    if not count or count > CPACK_MAP_MAX_ENTS:
        raise PackFailure("%s #%d: map count %d" % (pack, mid, count))
    if len(data) != 8 + count * 29:
        raise PackFailure("%s #%d: map blob len %d != 8 + %d*29"
                          % (pack, mid, len(data), count))
    pos = 0
    for i in range(count):
        orig_off, ln, kind, idx, src_off = \
            struct.unpack_from("<QQBIQ", data, 8 + i * 29)
        if kind > 1:
            raise PackFailure("%s #%d: map ent %d kind %d"
                              % (pack, mid, i, kind))
        if ln == 0:
            raise PackFailure("%s #%d: map ent %d len 0" % (pack, mid, i))
        if orig_off != pos:
            raise PackFailure("%s #%d: map ent %d orig_off %d, want %d "
                              "(gap/overlap)" % (pack, mid, i, orig_off,
                                                 pos))
        pos += ln
    if pos != size:
        raise PackFailure("%s #%d: map covers %d bytes, mutant is %d"
                          % (pack, mid, pos, size))


def run_helper(helper, cmd, args, timeout=10):
    """Returns (rc, out-bytes-or-None); raises PackFailure on crash."""
    try:
        p = subprocess.run([helper, cmd] + args, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except subprocess.TimeoutExpired:
        raise PackFailure("HANG: %s %s did not finish in %ds"
                          % (helper, cmd, timeout))
    rc = p.returncode
    if rc < 0:
        raise PackFailure("CRASH: %s %s killed by signal %d (%s)"
                          % (helper, cmd, -rc, fz._signame(-rc)))
    if rc >= 128:
        raise PackFailure("CRASH: %s %s exit code %d" % (helper, cmd, rc))
    return rc, p.stdout, p.stderr


def fuzz_pack(args, pack, src, builder):
    rng = random.Random((args.seed << 8) ^ (zlib.crc32(pack.encode())
                                            & 0xFFFFFF))
    work = args.workdir
    helper = os.path.join(work, "bin", pack)
    fixture = os.path.join(work, "fixtures", pack + FIXT_EXT[pack])
    mutant = os.path.join(work, "mutant" + FIXT_EXT[pack] + "." + pack)

    subprocess.run(["cc", "-std=c11", "-O2", "-o", helper,
                    os.path.join(fz.REPO, "tools/codecpacks", src)],
                   check=True)
    if not os.path.exists(fixture) or args.refixtures:
        builder(fixture)
    # the base must enumerate: a fuzz run over a declined base is empty
    rc, out, err = run_helper(helper, "enumerate",
                              [fixture, os.path.join(work, "base.tab")])
    if rc != 0:
        raise PackFailure("%s: BASE FIXTURE does not enumerate (rc=%d): %s"
                          % (pack, rc, err.decode(errors="replace")[:400]))

    fails = []
    shutil.copyfile(fixture, mutant)
    for mid in range(args.mutants):
        size = os.path.getsize(mutant)
        mut = fz.Mutator(mutant, rng)
        kind = rng.choices(["flip", "zero4k", "trunc"], [70, 15, 15])[0]
        # 60% of flips aimed at the first 4 MiB (headers/tables live there)
        head = (0, min(size, 4 << 20))
        anywhere = (0, size)
        if kind == "flip":
            span = head if rng.random() < 0.6 else anywhere
            desc = mut.flip_bytes(span, rng.randrange(1, 17))
        elif kind == "zero4k":
            span = head if rng.random() < 0.3 else anywhere
            desc = mut.zero_block(span)
        else:
            desc = mut.truncate(anywhere)
        try:
            rc, _o, _e = run_helper(helper, "enumerate",
                                    [mutant, os.path.join(work, "m.tab")])
            if rc not in (0, 1, 3):
                raise PackFailure("enumerate rc=%d" % rc)
            if rc == 0:
                with open(os.path.join(work, "m.tab"), "rb") as f:
                    check_table(f.read(), os.path.getsize(mutant), pack, mid)
            rc, _o, _e = run_helper(helper, "map",
                                    [mutant, os.path.join(work, "m.map")])
            if rc not in (0, 1, 3):
                raise PackFailure("map rc=%d" % rc)
            if rc == 0:
                with open(os.path.join(work, "m.map"), "rb") as f:
                    check_map(f.read(), os.path.getsize(mutant), pack, mid)
            if mid % 10 == 0:
                rc, out, _e = run_helper(helper, "estimate", [mutant])
                if rc == 0:
                    try:
                        est = int(out.strip())
                    except ValueError:
                        raise PackFailure("estimate printed %r" % out[:80])
                    if est < 0 or est > 10 * os.path.getsize(mutant) + \
                            EST_MARGIN:
                        raise PackFailure("NONSENSE estimate %d (mutant %d "
                                          "bytes)" % (est, os.path.getsize(
                                              mutant)))
        except PackFailure as f:
            fdir = fz.ensure_dir(os.path.join(work, "failures"))
            keep = os.path.join(fdir, "%s-%05d%s" % (pack, mid,
                                                     FIXT_EXT[pack]))
            shutil.copyfile(mutant, keep)
            fails.append("mutant %d (%s): %s -- kept at %s ; repro: %s "
                         "enumerate %s OUT" % (mid, desc, f, keep,
                                               helper, keep))
        if mut.truncated is not None:
            shutil.copyfile(fixture, mutant)     # restore the tail
        else:
            mut.undo()
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mutants", type=int, default=500)
    ap.add_argument("--seed", type=lambda s: int(s, 0), default=0x9AC4)
    ap.add_argument("--packs", default=",".join(PACKS))
    ap.add_argument("--refixtures", action="store_true")
    ap.add_argument("--workdir", default="/dev/shm/invf-fuzz-packs")
    args = ap.parse_args()

    os.makedirs(os.path.join(args.workdir, "bin"), exist_ok=True)
    os.makedirs(os.path.join(args.workdir, "fixtures"), exist_ok=True)
    t0 = time.time()
    all_fails = []
    for pack in args.packs.split(","):
        src, builder = PACKS[pack]
        try:
            fails = fuzz_pack(args, pack, src, builder)
        except PackFailure as f:
            fails = ["HARNESS/BASE: %s" % f]
        all_fails.extend(fails)
        print("[pack %s] %d mutants, %d failures (%.0fs)"
              % (pack, args.mutants, len(fails), time.time() - t0))
        for f in fails:
            print("   FAIL %s" % f)
    print("== packfuzz: %d packs x %d mutants in %.1fs, seed %#x =="
          % (len(args.packs.split(",")), args.mutants, time.time() - t0,
             args.seed))
    print("   failures: %d" % len(all_fails))
    return 1 if all_fails else 0


if __name__ == "__main__":
    sys.exit(main())
