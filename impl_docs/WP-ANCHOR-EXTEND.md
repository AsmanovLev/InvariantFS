# WP-ANCHOR-EXTEND — `extend_meta_extent` must not grow over the anchor block

Status: ready to implement. Small, fail-closed, one function.

## Why

`extend_meta_extent()` (`src/core/volume.c:2639`) grows a metadata extent in
place when the adjacent run is free. Its test for "free" is the clear bits in
`v->bitmap` and nothing else:

```c
if (bit_get(v->bitmap, adj_pba) == 0) {
    for (k = 1; k < adj_blocks; k++)
        if (bit_get(v->bitmap, adj_pba + k) != 0) { can_extend = 0; break; }
```

A clear bit is NOT proof that a block is free: the tail anchor block is exactly
such a block, which is how `vol_punch_holes()` zeroed it (fixed by
`anchor_block_protected()`, `src/core/vol_anchor.c:259`, applied at
`volume.c:2521` and `volume.c:3873`). `extend_meta_extent()` is a third
allocator-shaped path and did not get the same guard, so a metadata extent that
happens to end just before the anchor can be extended across it, marking the
anchor allocated for metadata and overwriting it on the next metadata write.

Latent, not observed: no test drives a metadata extent into the anchor's
predecessor.

## Design

Treat a protected block as not-free in both loops:

```c
if (!bit_get(v->bitmap, adj_pba) && !anchor_block_protected(v, adj_pba)) {
    for (k = 1; k < adj_blocks; k++)
        if (bit_get(v->bitmap, adj_pba + k) ||
            anchor_block_protected(v, adj_pba + k)) { can_extend = 0; break; }
```

`anchor_block_protected()` is a structural test on volume geometry and takes a
const volume, so this costs a compare per candidate block and cannot fail.

Keep the function's contract (`1` = extended, `0` = not) unchanged; the caller
in `vol_meta_merge.c:170` already treats 0 as "try the next class".

## Validation

1. Unit leg in the existing anchor test (`src/cli/anchor_test.c`, same shape as
   leg 10 in `tools/test-meta-anchor.sh`): build a volume whose metadata
   extent ends at `anchor_pba - 1`, call `extend_meta_extent()` with a class
   that covers the anchor, assert `0` and that the anchor block's bytes and
   its bitmap state are untouched; then assert the SAME call succeeds when the
   same geometry is placed one block away (so the leg proves the guard, not a
   general refusal).
2. RED CONTROL, run and recorded in the WP: with the guard removed the leg
   must fail on the anchor bytes, not merely on the return code.
3. `tools/test-meta-anchor.sh` still PASS, `make -j1 -k test` unchanged.

### Measured (leg 11, `tools/test-meta-anchor.sh`)

The leg lives in `src/cli/anchor_test.c` as `extend-meta <img> <mode>`, which
injects one mapper entry in RAM (no flush, no MET0 persist, every bit restored
before close) and clears the anchor's bitmap bit first -- the world a bitmap
rebuild from replay leaves, which is the only world in which the bug is
reachable. Same class bump in all three modes, 1 -> 2 (32 -> 64 blocks):

| mode | geometry | guard test | fixed | guard removed |
|---|---|---|---|---|
| `edge` | run starts AT the anchor | first condition | `RC=0`, bit 0, META 0, bytes identical | `RC=1`, bit 1, META 1, mapper claims the anchor, record written at pba `anchor`, **bytes changed** |
| `runlast` | run ends AT the anchor | second condition | `RC=0`, bit 0, META 0, bytes identical | `RC=1`, bit 1, META 1, mapper claims the anchor |
| `shifted` | run one block further out | must succeed | `RC=1`, class 2, 32 blocks marked, free -32, anchor untouched | identical (`RC=1`) |

Red control, guard removed, suite exit 1 (`tools/test-meta-anchor.sh`):

```
MODE=edge ... RC=1 ... ANCHOR_BIT_AFTER=1 ANCHOR_METATYPE_AFTER=1
  MAPPER_CLASS_AFTER=2 MAPPER_CLAIM_BLOCKS=64 CLAIMED_ANCHOR=1
  RUN_BITS_SET_AFTER=32 RECORD_WRITE=1 RECORD_WRITE_PBA=78642
  ANCHOR_BYTES_CHANGED=1 FREE_BLOCKS_DELTA=32
FAIL: leg 11 (edge): RED CONTROL -- extend_meta_extent() grew a metadata
  extent OVER the ANC0 anchor and the metadata record write that follows
  destroyed it. The volume's only second copy of RT30/SPT0 is gone.
```

and the block itself, after that run: `invf-anchor_test probe` reports
`ANCHOR_STATE=absent`, i.e. the volume lost its second descriptor copy.

Both conditions are load-bearing, measured separately: with only the FIRST
condition fixed, `edge` passes (`RC=0`) and `runlast` still returns `RC=1`.
With only the second fixed, `edge` fails. So the leg cannot be satisfied by
half the fix, and `shifted` cannot be satisfied by refusing everything.

## Out of scope

- `vol_punch_holes()` and the sweep's zero scan (already guarded, 55998cb).
- Any other allocator; if a fourth one is found, that is a separate WP.

## Coordination notes

- Worktree `/d/Users/User/invfs_deploy/wt-anchor`, branch `wp/anchor-extend`.
- Touches `src/core/volume.c` and `src/cli/anchor_test.c` ONLY. It must not
  touch `src/core/vol_cpack.c`, `src/core/volume_internal.h` or anything under
  `tools/codecpacks/`: another worktree owns those right now.
