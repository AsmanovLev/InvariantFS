# WP53 — checkpoint registry write failure (landed)

Branch: wp/53-ckp-registry (commit e4ad0f6, squash-merged).

Root cause: on mapper volumes `inode_area_make_room` fell through to the
legacy compaction gate when the active extent was full; under a live sweep
checkpoint that gate refuses, so `vol_ckp_end`'s `\x01reten` owner create
failed. Fix: on a mapper volume return "a new extent can be allocated"
unless the mapper table itself is full; `tools/invf-sweep.c` now exits
nonzero on a genuine `vol_ckp_end` failure.

Verified: reproduced at N=73 fixture files (active extent 309 bytes free);
post-fix no warning, exit 0, fsck CLEAN, `--realize`/`rollback` correct.
`make test` 4722/0; `test-meta-extent-walk` 6/0. `test-rollback`/
`test-sweepboot` stayed red for WP52/pre-existing reasons.
