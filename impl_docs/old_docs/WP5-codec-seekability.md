# WP5 design note: seekable vs non-seekable codecs

User directive (2025-08-25): decoders get a `seek` capability parameter.

## Idea
Each codec/decoder is classified:
- **seekable** — can decode an arbitrary byte/frame range without
  processing everything before it (ZIP stored/deflate windows already do
  this; FLAC frame headers allow sample-accurate seek; ZSTD skippable
  frames; PNG per-IDAT chunk).
- **non-seekable** — must decode from stream start (gzip streams, MP3
  without Xing/frames index, APE as currently implemented).

## Why it matters
1. Ranged reads (`vol_read_range`) today rebuild WHOLE files through the
   ARC cache for whole-file algos. Seekable codecs could serve 64K
   windows directly -> no RAM blowup, no O(filesize) per read.
2. Sweep verification cost differs: non-seekable requires full decode
   for invariant check anyway; seekable can verify per-chunk.
3. The recipe (AST) gains per-block granularity for these formats:
   one AST block entry per frame/chunk instead of one monolithic blob.

## Design sketch
- `codec_caps` bitfield in recipe/algo metadata: SEEK=1.
- Decoder interface: `decode_range(blob, offset, len, out)` +
  `decode_full()` fallback. Tools wired via WP5 Linux port advertise
  caps at integration time (version-pinned, see version-pinning note).
- Non-seekable formats keep today's behavior (ARC rebuild).
