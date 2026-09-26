import os, sys
WORK = sys.argv[1]
for img in ('fs-a.xfs', 'fs-b.xfs'):
    orig = os.path.getsize(f'{WORK}/orig/{img}')
    rec, musz, msz, tsz = (int(x) for x in
                           open(f'{WORK}/out/{img}.sizes').read().split())
    nm = sum(1 for _ in open(f'{WORK}/out/{img}.tab'))
    print(f'  {img}: {orig} B ({orig >> 20} MiB) decomposes into:')
    print(f'    recipe blob  {rec:>12} B  (every non-member byte verbatim:'
          f' superblocks, AG headers, B+trees, inode chunks, dir blocks,')
    print(f'                 {"":>12}    the log, free space -- mostly zeros'
          f' for a fresh image)')
    print(f'    {nm} members    {musz:>12} B  (the file contents, now flowing'
          f' through PPMd/ZSTD batching + dedupe as first-class files)')
    print(f'    table + map  {tsz + msz:>12} B')
    print(f'    => the pack itself stores {rec + musz + tsz + msz} B'
          f' ({(rec + musz + tsz + msz) / orig:.3f}x) before the pipeline;'
          f' the win is member-level compression/dedupe, not the recipe')
