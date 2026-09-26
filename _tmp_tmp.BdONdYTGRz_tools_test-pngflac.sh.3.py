import glob, os, struct, sys, zlib

for path in sorted(glob.glob(os.path.join(sys.argv[1], "*.png"))):
    d = open(path, "rb").read()
    out = bytearray(d[:8])
    i, changed = 8, False
    while i < len(d):
        ln = struct.unpack(">I", d[i:i+4])[0]
        typ = d[i+4:i+8]
        body = d[i+8:i+8+ln]
        if typ == b"IDAT":
            # coalesce the whole run of IDAT chunks, then emit it as one
            idat = bytearray(body)
            i += 12 + ln
            while i + 8 <= len(d) and d[i+4:i+8] == b"IDAT":
                n2 = struct.unpack(">I", d[i:i+4])[0]
                idat += d[i+8:i+8+n2]
                i += 12 + n2
            new = zlib.compress(zlib.decompress(bytes(idat)), 6)
            if new != body:
                changed = True
            out += struct.pack(">I", len(new)) + b"IDAT"
            out += new + struct.pack(">I", zlib.crc32(b"IDAT" + new))
            i += 12 + ln
            continue
        out += d[i:i+12+ln]
        i += 12 + ln
    if changed:
        open(path, "wb").write(bytes(out))
print("re-deflated IDATs with the host zlib")
