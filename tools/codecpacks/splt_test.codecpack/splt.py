#!/usr/bin/env python3
"""
splt.py — splt_test containerpack helper (InvariantFS WP16a).

SPLT wire format (the fixture "container"):

    0   4      "SPLT"
    4   4      u32 LE  n (member count)
    8   n*8    u64 LE  member lengths
    ..  ..     member payloads, concatenated in index order

strip emits the header verbatim (the recipe = original minus payloads);
rebuild splices the member files "<dir>/<idx>" back under the recipe.

The helper deliberately trusts the length table on enumerate/strip (a
table whose lengths do not cover the payload is NOT refused here) — that
is what makes the WP16a negative fixture a *rebuild mismatch* caught by
the FS-side bit-exact guard instead of a pack-side refusal. Structural
parse failures (short table, bad magic, idx out of range) refuse.

Commands (fixed argv, no shell; exit 0 = ok, anything else = refuse/fail):
    enumerate <in> <out>           member table: "idx<TAB>name<TAB>usize"
    extract   <in> <idx> <out>     member idx bytes
    strip     <in> <out>           recipe (the header)
    rebuild   <recipe> <dir> <out> original, bit-exact
"""
import os
import struct
import sys


def parse(buf):
    """-> (n, [lens], data_off); raises ValueError on structural garbage."""
    if len(buf) < 8 or buf[:4] != b"SPLT":
        raise ValueError("not SPLT")
    (n,) = struct.unpack_from("<I", buf, 4)
    if len(buf) < 8 + n * 8:
        raise ValueError("short member table")
    lens = list(struct.unpack_from("<%dQ" % n, buf, 8)) if n else []
    return n, lens, 8 + n * 8


def cmd_enumerate(inp, out):
    buf = open(inp, "rb").read()
    n, lens, _ = parse(buf)
    with open(out, "w") as f:
        for i in range(n):
            f.write("%d\tchunk%d\t%d\n" % (i, i, lens[i]))


def cmd_extract(inp, idx_s, out):
    buf = open(inp, "rb").read()
    n, lens, data_off = parse(buf)
    idx = int(idx_s, 10)
    if idx < 0 or idx >= n:
        raise ValueError("idx out of range")
    off = data_off + sum(lens[:idx])
    with open(out, "wb") as f:
        f.write(buf[off:off + lens[idx]])


def cmd_strip(inp, out):
    buf = open(inp, "rb").read()
    _n, _lens, data_off = parse(buf)
    with open(out, "wb") as f:
        f.write(buf[:data_off])


def cmd_rebuild(recipe, mdir, out):
    hdr = open(recipe, "rb").read()
    n, lens, data_off = parse(hdr)
    if len(hdr) != data_off:
        raise ValueError("trailing garbage in recipe")
    body = bytearray()
    for i in range(n):
        with open(os.path.join(mdir, "%d" % i), "rb") as f:
            b = f.read()
        if len(b) != lens[i]:
            raise ValueError("member %d: got %d bytes, want %d"
                             % (i, len(b), lens[i]))
        body += b
    with open(out, "wb") as f:
        f.write(hdr)
        f.write(bytes(body))


def main(argv):
    if len(argv) < 3:
        return 2
    cmd = argv[1]
    try:
        if cmd == "enumerate" and len(argv) == 4:
            cmd_enumerate(argv[2], argv[3])
        elif cmd == "extract" and len(argv) == 5:
            cmd_extract(argv[2], argv[3], argv[4])
        elif cmd == "strip" and len(argv) == 4:
            cmd_strip(argv[2], argv[3])
        elif cmd == "rebuild" and len(argv) == 5:
            cmd_rebuild(argv[2], argv[3], argv[4])
        else:
            return 2
    except (ValueError, KeyError, IndexError, OSError, struct.error):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
