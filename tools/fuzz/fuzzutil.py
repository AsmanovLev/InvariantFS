"""
fuzzutil.py — shared helpers for the InvariantFS fuzz wave (tools/fuzz/).

Everything here is deterministic: callers pass a seed, all randomness comes
from random.Random(seed) instances created per iteration/case, so a rerun
with the same --seed reproduces every byte and every mutation.

Common ground:
  * CRC32C (Castagnoli, reflected poly 0x82F63B78) matching crc32c.c.
  * Superblock parsing + on-disk region math (see invarifs.h / mkfs.c).
  * The run() wrapper: subprocess with a timeout, crash detection
    (signal deaths / rc >= 128 are FAILURES, clean nonzero exits are not).
  * The Mutator: byte flips / zero-4K-block / truncate / crafted descriptor
    writes, all with undo info so a mutant file can be reused.
"""

import os
import random
import struct
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(REPO, "bin")

# ---------------------------------------------------------------- CRC32C --

_CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ (0x82F63B78 if (_c & 1) else 0)
    _CRC_TABLE.append(_c)


def crc32c(data, crc=0):
    crc ^= 0xFFFFFFFF
    for b in data:
        crc = (crc >> 8) ^ _CRC_TABLE[(crc ^ b) & 0xFF]
    return crc ^ 0xFFFFFFFF


# ------------------------------------------------------------ superblock --

INVFS_JOURNAL_BLOCKS = 8192
BLOCK = 4096

# descriptor slots inside block 0 (invarifs.h)
RDP0_OFF = 0x100
RSZ0_OFF = 0x140
CKP0_OFF = 0x220
DESC_REGION = (0x100, 0x400)     # the reserved descriptor area of block 0


class Layout:
    """On-disk region map parsed from a live image's superblock."""

    def __init__(self, img_path):
        with open(img_path, "rb") as f:
            sb = f.read(BLOCK)
        if len(sb) < 0x90 or sb[0:8] != b"InvariFS":
            raise ValueError("not an InvariantFS image")
        (self.total_blocks,
         self.metadata_zone_start, self.metadata_zone_blocks,
         self.raw_zone_start, self.raw_zone_blocks,
         self.shadow_zone_start, self.shadow_zone_blocks) = \
            struct.unpack_from("<7Q", sb, 0x20)
        self.sb_checksum = struct.unpack_from("<I", sb, 0x7C)[0]
        self.bitmap_blocks = (self.total_blocks // 8 + BLOCK - 1) // BLOCK
        self.journal_start = self.metadata_zone_start + self.bitmap_blocks
        self.inode_area_start = self.journal_start + INVFS_JOURNAL_BLOCKS
        self.metadata_end = self.metadata_zone_start + self.metadata_zone_blocks
        self.size = self.total_blocks * BLOCK

    def regions(self):
        """name -> (byte_off, byte_len) for every fuzz target zone."""
        B = BLOCK
        regs = {
            # (d) the checksummed superblock struct itself
            "superblock": (0, 0x90),
            # (e) the descriptor slot area of block 0 (RDP0/RSZ0/CKP0)
            "descriptors": (DESC_REGION[0], DESC_REGION[1] - DESC_REGION[0]),
            # block bitmap
            "bitmap": (self.metadata_zone_start * B,
                       self.bitmap_blocks * B),
            # (b) L2P journal (append-only, CRC'd entries)
            "journal": (self.journal_start * B,
                        INVFS_JOURNAL_BLOCKS * B),
            # (c) inode/AST area (append-only, CRC'd records)
            "inode": (self.inode_area_start * B,
                      (self.metadata_end - self.inode_area_start) * B),
            # (a) data zones (raw + shadow)
            "data": (self.raw_zone_start * B,
                     (self.total_blocks - self.raw_zone_start) * B),
        }
        return regs


# --------------------------------------------------------------- runner --

class Crash(Exception):
    """A tool died by signal / exceeded its timeout / returned >= 128."""

    def __init__(self, what, detail):
        super().__init__(what)
        self.what = what
        self.detail = detail


def run(argv, timeout=30, cwd=None, env=None, input_data=None):
    """Run argv; return (rc, stdout_bytes, stderr_bytes).

    Raises Crash on signal death, timeout or rc >= 128 -- those are the
    fuzz FAILURES. Clean nonzero exits come back normally.
    """
    try:
        p = subprocess.run(
            argv, timeout=timeout, cwd=cwd, env=env, input=input_data,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except subprocess.TimeoutExpired:
        raise Crash("timeout", "%s did not finish within %ds"
                    % (" ".join(argv), timeout))
    rc = p.returncode
    if rc < 0:
        raise Crash("signal", "%s killed by signal %d (%s)"
                    % (" ".join(argv), -rc, _signame(-rc)))
    if rc >= 128:
        raise Crash("rc>=128", "%s exit code %d" % (" ".join(argv), rc))
    return rc, p.stdout, p.stderr


def _signame(sig):
    import signal as _s
    try:
        return _s.Signals(sig).name
    except ValueError:
        return "SIG%d" % sig


# -------------------------------------------------------------- mutator --

class Mutator:
    """Random adversarial mutations over a file, with undo.

    undo() restores flip/zero mutations exactly; truncate is undone by the
    caller re-copying the pristine fixture (undo info records old size so
    the caller knows it must).
    """

    def __init__(self, path, rng):
        self.path = path
        self.rng = rng
        self.size = os.path.getsize(path)
        self._undo = []          # list of (off, bytes) to write back
        self.truncated = None    # old size when truncated

    def _pick_span(self, regions, weights):
        names = [r for r, _ in weights]
        ws = [w for _, w in weights]
        name = self.rng.choices(names, ws)[0]
        off, ln = regions[name]
        return name, off, ln

    def flip_bytes(self, region_span, count):
        """XOR `count` random bytes inside (off, len) with nonzero masks."""
        off, ln = region_span
        if ln <= 0:
            return "flip(noop)"
        desc = []
        with open(self.path, "r+b") as f:
            for _ in range(count):
                o = off + self.rng.randrange(ln)
                mask = self.rng.randrange(1, 256)
                f.seek(o)
                old = f.read(1)
                if not old:
                    continue
                self._undo.append((o, old))
                f.seek(o)
                f.write(bytes([old[0] ^ mask]))
                desc.append("%s@%d^0x%02x" % (old[0:1].hex(), o, mask))
        return "flip[%s]" % ",".join(desc)

    def zero_block(self, region_span):
        """Zero a random 4K block intersecting the region."""
        off, ln = region_span
        if ln <= 0:
            return "zero4k(noop)"
        o = off + self.rng.randrange(ln)
        o -= o % BLOCK
        n = min(BLOCK, self.size - o)
        if n <= 0:
            return "zero4k(noop)"
        with open(self.path, "r+b") as f:
            f.seek(o)
            old = f.read(n)
            self._undo.append((o, old))
            f.seek(o)
            f.write(b"\0" * n)
        return "zero4k@%d" % o

    def truncate(self, region_span):
        """Truncate the image to a random size (biased inside the region)."""
        off, ln = region_span
        if ln <= 0:
            return "trunc(noop)"
        cut = off + self.rng.randrange(ln)
        cut -= cut % BLOCK
        if cut >= self.size:
            cut = self.size - BLOCK
        self.truncated = self.size
        with open(self.path, "r+b") as f:
            f.truncate(cut)
        self.size = cut
        return "trunc@%d" % cut

    def write_bytes(self, off, data):
        """Raw write (used by the crafted-descriptor mutations)."""
        with open(self.path, "r+b") as f:
            f.seek(off)
            old = f.read(len(data))
            self._undo.append((off, old))
            f.seek(off)
            f.write(data)

    def undo(self):
        if self._undo:
            with open(self.path, "r+b") as f:
                for off, old in self._undo:
                    f.seek(off)
                    f.write(old)
            self._undo = []
        if self.truncated is not None:
            # caller must have restored content past the cut already
            self.truncated = None


# -------------------------------------------- crafted descriptor writes --

def craft_rdp0(rng):
    """A syntactically-placed RDP0 descriptor with random fields and a
    VALID crc32c -- exercises the descriptor validation, not the CRC."""
    body = bytearray(24)
    body[0:4] = b"RDP0"
    body[4] = rng.randrange(256)                    # l1_algo
    body[5] = rng.randrange(256)                    # l2_algo
    struct.pack_into("<H", body, 6, rng.randrange(65536))   # k1
    struct.pack_into("<H", body, 8, rng.randrange(65536))   # k2
    body[10] = rng.randrange(256)                   # m2
    body[11] = rng.randrange(256)                   # pad
    struct.pack_into("<Q", body, 12, rng.randrange(1 << 40))
    struct.pack_into("<I", body, 20, 0)
    struct.pack_into("<I", body, 20, crc32c(bytes(body)))
    return bytes(body)


def craft_ckp0(rng, img_blocks):
    """CKP0 with random (image-bounded) append pointers + valid CRC.
    Real 56-byte layout (invarifs.h): magic, crc32c over the struct with
    the field zeroed, then inode_area_pos / journal_pos / stage_pba /
    stage_blocks / sweep_seq / time_unix."""
    body = bytearray(56)
    body[0:4] = b"CKP0"
    struct.pack_into("<I", body, 4, 0)                    # crc placeholder
    struct.pack_into("<Q", body, 8, rng.randrange(img_blocks) * BLOCK)
    struct.pack_into("<Q", body, 16, rng.randrange(img_blocks) * BLOCK)
    struct.pack_into("<Q", body, 24, rng.randrange(img_blocks))
    struct.pack_into("<Q", body, 32, rng.randrange(1, 64))
    struct.pack_into("<Q", body, 40, rng.randrange(1 << 32))
    struct.pack_into("<Q", body, 48, rng.randrange(1 << 31))
    struct.pack_into("<I", body, 4, crc32c(bytes(body)))
    return bytes(body)


def craft_rsz0(rng, img_blocks):
    """RSZ0 with random fields + valid CRC. rsz0_sane() cross-checks the
    embedded superblock against itself, so a random one must be IGNORED --
    that check is exactly what this exercises. 524-byte struct, CRC over
    the struct with the crc field zeroed."""
    body = bytearray(0x20C)
    body[0:4] = b"RSZ0"
    struct.pack_into("<I", body, 4, rng.randrange(1 << 32))     # version
    for off in (8, 16, 24, 32, 40, 48):   # stage_start..old_total
        struct.pack_into("<Q", body, off, rng.randrange(img_blocks + 1))
    for i in range(0x178 - 0x38, 0x208):  # embedded superblock: random
        body[i] = rng.randrange(256)
    struct.pack_into("<I", body, 0x208, 0)
    struct.pack_into("<I", body, 0x208, crc32c(bytes(body)))
    return bytes(body)


def descriptor_bodies(rng, img_blocks):
    """Yield (offset, payload) crafted-descriptor writes."""
    yield RDP0_OFF, craft_rdp0(rng)
    yield RSZ0_OFF, craft_rsz0(rng, img_blocks)
    yield CKP0_OFF, craft_ckp0(rng, img_blocks)


# --------------------------------------------------------- misc helpers --

def sha256(b):
    import hashlib
    return hashlib.sha256(b).hexdigest()


def ensure_dir(d):
    os.makedirs(d, exist_ok=True)
    return d
