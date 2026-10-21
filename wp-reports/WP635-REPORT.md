WP: wp/635-recipe-tree
Files changed:
  src/core/invarifs.h        (the format authority, and the only file that defines the new bytes)
                             INVFS_ALGO_CHILD 27 -- a recipe entry that is a REFERENCE to a child recipe, not a payload
                             (pba 0, block_id indexes the CHLD table). 27 was the first free id (builtins 0..15, packs
                             16..23, WINDOW_SRC 24, NBLI 25, holes 26, tests 40/41). CHLD trailing section
                             [4B "CHLD"][u32 count][count x 32B BLAKE3], appended exactly like WINW so every
                             pre-WP635 recipe parses unchanged (zero trailing bytes) and an old reader that meets one
                             sees pba == 0 and refuses loudly instead of misreading. INVFS_CHILD_SEGS_DEFAULT 65536
                             (2^16 segments = 4 GiB coverage, 2 MiB child blob), _MAX 2^21, DEPTH_MAX 1.
                             invfs_ast_child_ref (32B, _Static_assert'd against INVFS_RECIPE_ADDR_LEN) and
                             invfs_ast_tail (one version-agnostic view of "whichever trailing section is present").
                             MAX_FILE_SIZE 2^40 -> 2^48, with the reason written down: the tree is what makes the
                             larger bound true (2^24 children x 4 GiB), and a FLAT recipe still meets
                             MAX_SEGMENTS_V2 / INVFS_RECIPE_STREAM_MAX directly -- the two numbers are not the same
                             number and the comment says so.
  src/core/vol_ast.c         (vol_ast_recipe_serialize_tail: ONE serializer for both tail forms, wins and chld
                             mutually exclusive by construction, `n_wins==0 && n_chld==0` byte-identical to the flat
                             blob; serialize_win and the new serialize_tree are thin wrappers so no caller can pass
                             both. vol_ast_recipe_tail: the stride-aware (v3) scanner; vol_ast_recipe_windows now
                             REFUSES a CHLD recipe rather than answering "no windows" to a caller that would then
                             walk reference entries as payloads and free pba 0; vol_ast_recipe_children, same
                             discipline. vol_free_recipe_blocks: the block-freeing loop split out as
                             free_recipe_level so the child recursion runs the IDENTICAL loop (WP78 TEXT-batch skip,
                             intra-recipe pba sharing, seg_extent_checked) instead of a second copy that can
                             diverge; a child that will not load/parse, or nesting past DEPTH_MAX, sets bad_out and
                             the call returns -1 -- "space is not back" rather than the WP201 success-having-freed-
                             nothing shape. vol_ast_recipe_parsed_free added on both levels, so a forced-v3 (WP639)
                             heap entry array is released.)
  src/core/volume_internal.h (four declarations: serialize_tail, serialize_tree, recipe_tail, recipe_children)
  src/cli/recipe_tree_test.c (NEW, 297 lines, 40 checks. Real volume, real allocated blocks. 1 a flat recipe is
                             byte-identical through serialize/serialize_win(NUL)/serialize_tail(NUL) and windows+
                             children together is refused; 2 a CHLD blob round-trips through children() while
                             windows() REFUSES it; 3 THE LEG -- a tree free returns exactly what a flat free of an
                             identically shaped second file returns (both files written before either is freed, so
                             the pbas are distinct and each has one live reference; the control is what makes the
                             delta a claim about the RECURSION instead of a constant that happens to be right
                             today); 4 an unresolvable child fails (-1) and frees nothing; 5 a truncated CHLD
                             section is malformed, not "zero children"; 6 a two-level tree is refused by the depth
                             guard with the pool untouched.)
  Makefile                   (recipe_tree_test in CLI_MAINS, in the shard-1 prerequisite list, and one run line
                             after invf-window_test -- no e2e-engine workflow edit: it is a unit binary)
Tests run:
  $ git -C /tmp/invfs-wp635 merge main
  Auto-merging src/core/invarifs.h / vol_ast.c / volume_internal.h
  CONFLICT (content): src/core/vol_ast.c   -- 3 hunks, all trivial, all resolved by keeping BOTH sides:
    main's WP639 v3 work (INVFS_AST_HDR_V3_LEN, invfs_ast_entry_stride, the INVFS_RECIPE_FORCE_V3 emission path
    inside the serializer, the parsed_free note) is carried into serialize_tail and into the tail scanner's
    `stride`; my wlen/entry-offset writes were repointed at main's `esz` (40 for forced v3) instead of
    sizeof(*ents). No hunk required a design decision.
  (uncommitted WIP was committed first, 60151dd, because the merge refused to run over a dirty tree)
  $ make -j2 test                                    # log /tmp/wp635-maketest2.log, MAKE_EXIT=0
  test-shard-check: OK (65 prerequisite binaries invoked across shards)
  invf-recipe_tree_test: PASS: 40 checks, 0 failures   (of 1708 assertion lines; no FAIL/not ok outside the
                                                        expected-word hits that name failure modes)
  bin/invf-dlopen_trust_test: 61 checks, PASS
  check-repo-hygiene: OK (714 tracked files) + citation check: OK (36 docs)
  check-codecpack-sync: SKIPPED layers B and C -- no registry checkout
  MAKE_EXIT=0
  $ INVFS_E2E_AGENT=wp635 bash tools/run-e2e.sh tools/test-meta-recipe.sh        -> EXIT=0, ALL META RECIPE LEGS PASS
    (8 files bit-exact through FUSE, remount, offline invf-cat, 6 recipe values corrupted+resealed and every one
     refused by the BLAKE3 gate, fsck OK, state CLEAN -- i.e. the added trailing-section code did not move any
     existing recipe's bytes)
  $ INVFS_E2E_AGENT=wp635 bash tools/run-e2e.sh tools/test-entry-v3.sh           -> EXIT=0, all legs green
  $ INVFS_E2E_AGENT=wp635 bash tools/run-e2e.sh tools/test-sweep-lane-retire.sh  -> EXIT=0, free 9262 -> 9130 -> 9455
  $ INVFS_E2E_AGENT=wp635 bash tools/run-e2e.sh tools/test-orphan-reclaim.sh      -> EXIT=0, ALL V3 ORPHAN RECLAIM LEGS PASS
  $ INVFS_E2E_AGENT=wp635 bash tools/run-e2e.sh tools/test-writepath.sh
  run 1 EXIT=1 "FAIL: swept.txt not TEXT post-sweep"; run 2 EXIT=1 identical; run 3 EXIT=0 PASS
  and once with INVFS_E2E_NO_NS=1 -> EXIT=0 PASS ("text batches flushed (4 deferred)", lane ran, class=7)
  Re-run verdict (the load-flake rule): environment, not the tree. Evidence, not assertion: (a) the same three
  failures are the ISOLATED path only, and the host was out of memory the whole time -- /tmp (a 3.8G tmpfs) sat
  at 100% because a sibling WP left a 1.7G corpus in /tmp/wp637-corpus, `free -g` showed 5G used / 1G available
  with 3G of it tmpfs pages, and a private tmpfs in the isolated namespace is charged against exactly that;
  (b) my own reproduction of the suite's leg (invf-mkfs + invf-cp a 3 MB text file + invf-sweep + meta_probe
  --heat) gives "swept.txt: text -> PPMd batch" and "class=7 algo=2" on this tree, and the text lane is untouched
  by this WP; (c) run 3, same tree, same binaries, PASS. Reported as an OPEN environment finding, not a green
  (see the note at the end -- it is the kind of thing that reads as a regression against the branch).
Result: PASS
Remaining TODOs:
  - NO PRODUCER AND NO READER, deliberately. Nothing in the tree emits INVFS_ALGO_CHILD yet, and vol_read.c does
    not dispatch it: a CHLD entry (pba == 0) hits the existing `pba invalid` refusal at vol_read.c:738-743, which
    is the loud failure the header promises, not silent zeros. This WP lands the bytes, the scanner, and the
    lifecycle leg that a producer would otherwise leak -- a file whose blocks only the child names cannot be
    reclaimed by anything else. The write path (one open child per session, INVFS_CHILD_SEGS / -o child_segs=N,
    ~4 GiB coverage per child) is a separate WP and would be the first caller of vol_ast_recipe_serialize_tree.
  - The child recipe blob is retired with the top one (both are btree values behind 0x04||BLAKE3); nothing in this
    WP changes blob retirement, and the free path frees DATA blocks only, by design.
  - MAX_FILE_SIZE 2^48 is a bound the format can express, not a bound this build can reach: no writer produces a
    tree yet, so a 1 TiB+ file still meets MAX_SEGMENTS_V2 first. Said in the header, repeated here so nobody
    reads the new constant as a capability.
Note for the orchestrator:
  /tmp is a 3.8G tmpfs and it was 100% full for the whole run (sibling worktrees ~1.9G + /tmp/wp637-corpus 1.7G).
  The first `make -j2 test` died on ENOSPC writing the log, not in a test; the log above was written to
  /var/tmp/wp635-maketest2.log and copied to /tmp/wp635-maketest2.log once space allowed. Nothing belonging to
  another WP was touched or deleted. Any suite run in ISOLATED mode on this host right now can fail for the same
  reason -- test-writepath.sh did, twice, before passing.
