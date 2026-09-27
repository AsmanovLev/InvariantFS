#!/usr/bin/env python3
"""WP129: build the gzhdrfuzz seed corpus.

Every seed is a whole file, because that is the unit the sweep hands the
GZR builtin: a heap buffer of exactly the file's length. The seeds split
into two families, and the second family is the one that matters most:

  crash-*   the shapes that over-read before WP129, kept verbatim so the
            regression can never be re-introduced unnoticed.
  valid-*   WELL-FORMED gzip, one per FLG combination, each built with
            Python's gzip/zlib so it is a real member. These are the
            anti-regression half: a bounds check that quietly starts
            rejecting valid gzip is worse than the overflow, because it
            costs compression with no error anywhere.
"""
import gzip
import os
import struct
import sys
import zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else "tools/fuzz/seeds/gzhdr"
os.makedirs(OUT, exist_ok=True)


def raw_member(flg_extra=None, payload=b"payload", fname=None, fcomment=None,
               hcrc=False, ftext=False, trailer=True, nbytes=None):
    """Assemble a gzip member by hand so every FLG bit is controllable."""
    flg = 0
    if ftext:
        flg |= 0x01
    if hcrc:
        flg |= 0x02
    if flg_extra is not None:
        flg |= 0x04
    if fname is not None:
        flg |= 0x08
    if fcomment is not None:
        flg |= 0x10

    co = zlib.compressobj(6, zlib.DEFLATED, -15)
    stream = co.compress(payload) + co.flush()

    head = bytearray(b"\x1f\x8b\x08" + bytes([flg]))
    head += struct.pack("<I", 0)          # MTIME
    head += bytes([0x00, 0x03])           # XFL, OS (unknown)
    if flg_extra is not None:
        head += struct.pack("<H", len(flg_extra)) + flg_extra
    if fname is not None:
        head += fname + b"\x00"
    if fcomment is not None:
        head += fcomment + b"\x00"
    if hcrc:
        head += struct.pack("<H", zlib.crc32(bytes(head)) & 0xFFFF)

    body = bytes(head) + stream
    if trailer:
        body += struct.pack("<II", zlib.crc32(payload) & 0xFFFFFFFF, len(payload))
    if nbytes is not None:                 # truncate, to make a header lie
        body = body[:nbytes]
    return body


def put(name, data):
    p = os.path.join(OUT, name)
    with open(p, "wb") as f:
        f.write(data)
    print(f"  {name:34s} {len(data):6d} bytes  flg=0x{data[3]:02x}" if len(data) > 3
          else f"  {name:34s} {len(data):6d} bytes")


print("crash shapes (over-read on main):")
# The reported 18-byte reproducer: FLG=0x6d (FTEXT|FHCRC|FEXTRA|FNAME|FCOMMENT),
# XLEN=0x00FF, so the old walk formed hlen=267 and read at offset 267 of an
# 18-byte buffer.
put("crash-poc18-extra-name.bin",
    bytes([0x1f, 0x8b, 0x08, 0x6d, 0, 0, 0, 0, 0, 0, 0xff, 0, 0, 0, 0, 0, 0, 0]))
# FNAME with no NUL anywhere in the tail: the old `while (gz[hlen]) hlen++`
# had no bound at all, so this one walks off the end of the heap block.
put("crash-name-nonul.bin",
    bytes([0x1f, 0x8b, 0x08, 0x08]) + b"A" * 14)
# FCOMMENT with no NUL, same shape via the other scan.
put("crash-comment-nonul.bin",
    bytes([0x1f, 0x8b, 0x08, 0x10]) + b"A" * 14)
# FEXTRA with the largest XLEN and FNAME set: hlen jumps 65k past an 18-byte
# buffer and is then dereferenced.
put("crash-extra-max-name.bin",
    bytes([0x1f, 0x8b, 0x08, 0x0c, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0]))
# Truncated FNAME: a real gzip cut in the middle of the original name.
_put_full = raw_member(None, fname=b"a-very-long-original-file-name.tar", trailer=False)
put("crash-trunc-name.bin", _put_full[:20])
_put_full = raw_member(None, fname=b"a-very-long-original-file-name.tar", trailer=False)
put("crash-trunc-comment.bin",
    raw_member(None, fcomment=b"a-very-long-comment-string", trailer=False)[:20])
# A reserved-bit set (0xE0) is unparseable per RFC 1952 s2.1.1; the old walk
# walked it as if it were a normal member.
_rb = bytearray(raw_member(None, payload=b"q" * 64))
_rb[3] = 0xE0                      # reserved bits set, member otherwise whole
put("crash-reserved-bits.bin", bytes(_rb))

print("well-formed members (must stay accepted, unchanged hlen):")
put("valid-plain.gz", gzip.compress(b"hello invfs\n" * 100))
put("valid-text.gz", raw_member(None, payload=b"x" * 4096, ftext=True))
put("valid-hcrc.gz", raw_member(None, payload=b"y" * 4096, hcrc=True))
put("valid-extra.gz", raw_member(b"\x41\x42\x43", payload=b"z" * 4096))
put("valid-name.gz", raw_member(None, payload=b"w" * 4096, fname=b"members/one.txt"))
put("valid-comment.gz",
    raw_member(None, payload=b"v" * 4096, fcomment=b"created by something"))
put("valid-extra-name.gz",
    raw_member(b"\x01\x02", payload=b"u" * 4096, fname=b"a.tar", hcrc=True))
put("valid-all-flags.gz",
    raw_member(b"\xff" * 8, payload=b"t" * 4096, fname=b"n.tar",
               fcomment=b"c", hcrc=True, ftext=True))
# The FEXTRA length that exactly fills the file, and the one that is one
# byte too long: the boundary of the new bound.
_put = raw_member(b"\x00" * 6, payload=b"s" * 64)
put("valid-extra-exact.bin", _put)
put("valid-extra-one-short.bin", _put[:-1])
# A reserved bit set but the member otherwise complete.
_e0 = bytearray(_put); _e0[3] = 0xE0
put("invalid-reserved-e0.bin", bytes(_e0))

print(f"\ncorpus at {OUT}")
