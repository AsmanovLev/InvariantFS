#!/usr/bin/env python3
"""WP116 instrumentation: block census of an InvariantFS image.

Classifies every allocated 4 KiB block by its on-disk magic, and counts
v3 COW base pages (BPG3) by generation so we can tell the live tree from
abandoned generations. Read-only; touches no repo code.
"""
import struct, sys, collections

BS = 4096
PAGE_MAGIC = b'BPG3'
DELTA_MAGIC = None  # discovered below


def load_super(path):
    with open(path, 'rb') as f:
        hdr = f.read(BS)
    # total_blocks / geometry: locate by scanning for the volume magic
    return hdr


def main(path):
    data = open(path, 'rb').read()
    total = len(data) // BS
    census = collections.Counter()
    gens = collections.Counter()
    page_gens = []
    for b in range(total):
        blk = data[b * BS:(b + 1) * BS]
        if blk[:4] == PAGE_MAGIC:
            gen, level, nent, csum = struct.unpack('<QHHI', blk[4:20])
            census['v3_base_page'] += 1
            page_gens.append((b, gen, level, nent))
            gens[gen] += 1
        else:
            # classify by the first recognizable magic
            if blk[:4] == b'RT30':
                census['rt30'] += 1
            elif blk[:4] in (b'\x00\x00\x00\x00',):
                census['zero'] += 1
            else:
                census['other'] += 1
    print(f'file={path} total_blocks={total}')
    print(f'BPG3 base pages total = {census["v3_base_page"]}')
    print(f'  distinct generations = {len(gens)}')
    print('  top generations (gen: count):')
    for g, c in gens.most_common(20):
        print(f'    gen {g}: {c}')
    print('  other block classes:', dict(census))


if __name__ == '__main__':
    main(sys.argv[1])
