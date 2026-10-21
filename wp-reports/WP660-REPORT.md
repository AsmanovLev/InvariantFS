WP: wp/660-chld-producer  (squash-merged to main as 350908d)
Files changed:
  src/core/vol_write.c                 the producer: seal a child when n_ents would cross
                                       child_segs, vol_recipe_store it, append a reference
                                       entry, serialize_tree ONLY when n_chld > 0
  src/core/vol_read.c                  the reader: CHLD dispatch in the ranged path; the full
                                       path delegates to it (no sixth decode copy)
  src/cli/chld_producer_test.c (NEW)   110 checks
  Makefile                             test binary + run line
  tools/check-vol-find.sh              ledger re-point, 9 entries +35 in vol_read.c

Measured wall this removes (and the doc I first wrote had it attributed wrong --
corrected): the recipe BLOB cap binds, not the entry count.
  INVFS_RECIPE_STREAM_MAX 64 MiB / 32 B = 2,097,152 entries x 64 KiB = 128 GiB/file
  MAX_SEGMENTS_V2 2^24 = 16,777,216 entries -> 1 TiB   (orders of magnitude away)
A 0.93 TiB image is 7.4x past the flat wall.

Tests run:
  $ ./bin/invf-chld_producer_test        PASS: 110 checks, 0 failures
      includes "PROVEN: on-disk recipe is BYTE-IDENTICAL to plain serializer",
      unlink returning +38 blocks with fsck CLEAN, truncate across a child
      boundary, and nested-CHLD refusal in BOTH read paths (EIO, not zeros)
  $ make -j4 test                        MAKE_EXIT=0, 34 PASS
  $ tools/run-e2e.sh tools/test-writepath.sh     WRITEPATH E2E: PASS
  $ tools/run-e2e.sh tools/test-meta-recipe.sh   ALL META RECIPE LEGS PASS
Result: PASS
Remaining TODOs:
  - Rewrite-in-place of a sealed child: a ranged write into a sealed span
    materialises the child (read span, apply, seal again). Correct, not optimal;
    its own WP.
  - Depth > 1 (format allows it; policy is 1).
  - Not proven at the real target: a 100 GiB single file written through a FUSE
    mount and read back with cmp. The gate proves the mechanism at small scale
    with -o child_segs=8; the 100 GiB leg needs the box free for hours. Command:
      invf-mkfs v.img 1400; invf-fuse v.img mnt
      dd if=/dev/urandom of=mnt/big.img bs=1M count=102400
      umount mnt; invf-cat v.img big.img back.img && cmp ...
  - The v3 40-byte entry stays test-only (INVFS_RECIPE_FORCE_V3=1): with CHLD
    the entry fields are not under pressure, and widening them would LOWER the
    flat wall (102 GiB) for +25% recipe bytes. No rollback needed; production
    output is unchanged.
NOTE ON PROVIDERS (measured this session): premier returned 502 for every model;
reformboss hit its daily cap; agentrouter/deepseek-v4-flash died in exploration
twice (398 and 564 bytes of output, no commits); lan-proxy/ag/gemini-3.8-flash
needed NO_PROXY for the LAN address (pi's httpProxy was routing it out) and then
produced three commits in one run. It left vol_decode_child_full declared but
unwritten (link error), which the orchestrator finished by delegation.
