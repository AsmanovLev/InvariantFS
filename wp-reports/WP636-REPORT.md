WP: wp/636-iso9660-lane  (worktree /mnt/invfs-wt/invfs-wp636, 3 commits on top of c4a9727)

Files changed:
  tools/codecpacks/iso9660.codecpack/iso9660.c       (NEW 1234 lines: containerpack helper — ISO9660 + Joliet + UDF walk, I9R1 recipe, MRMP map)
  tools/codecpacks/iso9660.codecpack/manifest        (NEW   68 lines: algo 26, container, sniff CD001|BEA01 @32769, enumerate/extract/strip/rebuild/map/estimate)
  tools/codecpacks/iso9660.codecpack/mk-fixtures.py  (NEW  586 lines: byte-level fixture writer + independent ground-truth walker)
  tools/test-iso9660.sh                              (NEW  347 lines: pack-contract + FS-leg e2e)
  tools/codecpacks/registry-sync                     (  6 lines: iso9660 `fork`, engine-originated, no registry counterpart yet)

Commits: 8705606 (pack + manifest + registry record), f4180dc (ISO fallback skips the UDF VRS + contract test),
7ba0dbb (UDF partition REFERENCE is a map index, not the PD's number). No src/, docs/, INCIDENTS.md or AUDIT.md touched.

Tests run:

  $ make -j2 test                     # log /tmp/wp636-maketest.log (6943 lines)
  ... repo hygiene: OK (717 tracked files, no binaries/oversize outside allowlist)
  ... citation check: OK (line numbers in range across 36 doc(s))
  ... check-codecpack-sync: layer A passed (all 12 vendored packs in the record; iso9660 generation 1 consistent)
  ... 32 PASS suites, no FAIL
  MAKE_EXIT=0

  $ INVFS_E2E_AGENT=wp636 bash tools/run-e2e.sh tools/test-iso9660.sh      # ISOLATED (private /dev/shm)
  -- pack: iso.iso               members: 7, bytes: 414616   rebuild byte-exact
       MRMP ok: 10 ranges (3 recipe / 7 member), exact partition / estimate: 67523480
  -- pack: joliet.iso            7 / 414616, byte-exact; primary 8.3 names did NOT win (Joliet names present)
  -- pack: udf.iso               5 / 259264, byte-exact; MRMP 10 ranges (5 recipe / 5 member)
  -- pack: bridge.iso            5 / 259264 (UDF extents win over the 7-member ISO tree), byte-exact
  -- pack: vrs-iso.iso           7 / 414616 — recovers the SAME tree across the UDF VRS (tree equality, not a count)
  -- pack: ground truth        iso/joliet/vrs-iso: walker 7 extents | pack 7 members → MATCH
  -- pack: declines            decline-nomembers / -vss / -blocksize / -notcd all exit 3 (enumerate AND strip)
  == sweep  5/5 containers: "<name>.iso: iso9660 (codecpack)"; declines NOT decomposed
  == siblings  iso 7, joliet 7, udf 5, bridge 5, vrs-iso 7 member siblings + !mbrt + !mbrmap; declines clean
  == stamps   every container cls=3 algo=26 gen=1 (CONTAINER{ISO9660,1}); declines carry no stamp
  == verify --deep     deep: 50 files ok, 0 corrupt, 6306929 bytes verified
  == bit-exact   containers bit-exact (sha256 vs original); every member read == the pack's own extract
  == sweep 2     idle (the stamp holds, no re-decomposition)
  == delete      containers + all members + tables + maps deleted (cascade); fsck: OK
  == survivors   declined images bit-exact after the deletes
  ISO9660 CONTAINERPACK E2E: PASS        E2E_EXIT=0

  Corpus leg (WP Validation 2), real media from /mnt/invfs-scratch/iso — pack level, complete file:
  $ iso9660 enumerate "Windows 7 SP1 26in1 …/….iso"      # 4,183,465,984 B, ISO9660 PVD + Joliet SVD, no UDF
  rc=0  members=600  bytes=4,178,630,229   (recipe = 4,857,071 B)
  $ iso9660 strip / extract-all(600) / rebuild / cmp
  rebuild == original  → BYTE-EXACT (4,183,465,984 B; 61 s rebuild)
  $ iso9660 map   → MRMP ok: 1181 ranges (581 recipe / 600 member), exact partition of [0,4183465984)
  $ iso9660 estimate → 4,245,739,093 = sum(member usize) + 64 MiB
  (PVD volumeSpaceSize 2042708 sectors × 2048 == file size exactly; Joliet long names used, e.g. install7.esd)

  Corpus leg, torn media: 12 of 13 Windows ISOs are INCOMPLETE downloads (13–47% of the blocks allocated;
  7z reports "Errors: 1"). All 12 partial >4 GiB images: 11 × enumerate exit 3 (decline, no output written),
  1 × falls back to its intact ISO9660 stub tree (1 member, README.TXT — a UDF bridge disc's ISO side).
  Nothing was half-enumerated and nothing was corrupted; the rebuild guard is a full recipe+member memcmp,
  so a wrong extent table is a refusal, never corruption.

Bug found by the corpus leg, and fixed (7ba0dbb): a long_ad's partition REFERENCE NUMBER is the INDEX of the
partition map in the LVD array (ECMA-167 3/7.3, 3/10.5; Linux resolves it as s_partmaps[partRefNum]), not the
Partition Descriptor's partitionNumber. udf_volume compared the two, as did the root/FID ICB checks, so every
real disc declined: measured on ru-ru_windows_11_business_editions_version_26h2_arm64_dvd_3ad00693.iso
(UDF 2.01 "OSTA UDF Compliant") the PD numbers the partition 2989 (start 304, len 4092060) and the LVD's FSD
long_ad says lb=0 part=0. The pack refused and fell back to the ISO stub tree. Now u->part_index records the
matching map's index; on that disc the walk reaches the root directory and parses 9 FIDs (4 dirs, 4 files, all
part=0) and declines there because the 4 file ICB blocks (partition lb 1086–1089) are HOLES in the partially
downloaded file (SEEK_DATA says unallocated) — correct for a torn image. mk-fixtures.py wrote partition number 0,
which hid this; it now writes PART_NUM = 4352 with all references 0 (real media's shape), and with the previous
helper that fixture declines (udf.iso rc=3, bridge.iso 7 members instead of 5), so the contract test guards it.

Result: PASS

Remaining TODOs:
1. FS leg on REAL media did not run — host RAM, not the pack. Both attempts were OOM-killed (SIGKILL 137) on a
   7.7 GB box shared with four other agents (~1.6 GB available): `invf-cp` mallocs st_size (tools/invf-import.c:195),
   so the 3.9 GB Win7 import died at 9 s; the 1.02 GB Gentoo install ISO imported fine (29 s) but the sweep died at
   234 s inside the container lane, which holds the container in RAM (`full`, src/core/vol_cpack.c:3095, :3977).
   Exact command to run on a box with ≥6 GB free (INVFS_TOOL_SCRATCH must be a REAL filesystem: the tmpfs ceiling
   in AGENTS 2.8b refuses a multi-GB job, /dev/shm was 96% full here):
     INVFS_CODECPACKS=<copy of pack dir> INVFS_TOOL_SCRATCH=/srv/scratch invf-sweep v.img   # after invf-cp of the ISO
     invf-cat v.img x.iso out.iso && cmp <original> out.iso
   Note this is pre-existing cpack behaviour, identical for a 1 GB TAR; it is not specific to this pack.
2. Real UDF media at full depth (member enumeration → sweep → bit-exact) is UNPROVEN. The corpus has no complete
   UDF ISO: every UDF-capable Win10/Win11 download is partial, so the UDF walker's real-media coverage stops at the
   root directory (9 FIDs, item above). UDF is proven end-to-end only on mk-fixtures.py's byte-written images.
   Re-run when the aria2 downloads finish: no code change expected, but this is the leg that found the last bug.
3. Non-ASCII Joliet/UDF names are transliterated to '_' in the sibling name (measured on the real Win7 disc:
   "_______ ______.txt"). Bytes and rebuild are unaffected (members are addressed by index, !mbrNNNN- prefix keeps
   names unique), but the sibling name is lossy for Cyrillic/CJK media. Deliberate; list as a known limitation.
4. UDF edge cases not present in any available image, declined by design (exit 3) and listed for a follow-up WP:
   partition maps other than a single type-1 (metadata/virtual/sparable partitions, UDF 2.0+ second indirection),
   extended_ad (AD type 2) and embedded-in-FE data, Allocation Extent chains, Extended File Entries beyond what
   the fixture exercises, multi-partition volumes.
5. WD: the WP doc impl_docs/WP636-iso9660-lane.md is the orchestrator's to delete in the merging commit (AGENTS 1.2);
   this subagent did not touch it.
