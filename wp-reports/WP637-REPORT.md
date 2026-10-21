WP: wp/637-wim-lane  (worktree /mnt/invfs-wt/invfs-wp637)
Files changed (vs main):
  tools/codecpacks/wim.codecpack/manifest   (NEW: algo 26, containerpack, verbatim per-resource members)
  tools/codecpacks/wim.codecpack/wim.c      (NEW, 1063 lines: sniff/enumerate/extract[+_batch]/strip/rebuild/map/estimate)
  tools/test-wim.sh                         (NEW, 1082 lines: fixtures, pack contract, FS legs, dedupe, guard, corpus)
  tools/codecpacks/registry-sync            (wim recorded: layer A needs a line for a vendored pack)
  Makefile                                  (test-wim.sh wired into `e2e`, 848c8df/80e1a8c era)
  This round, on top of 80e1a8c:
    tools/test-wim.sh   (scratch was leaked on every run: 105-348 MB/run, six runs = 903 MB
                         on a box whose only scratch root was full; now trapped like the
                         sibling suites, INVFS_WIM_KEEP=1 keeps it)
    tools/test-wim.sh   (ABI leg: libwim.so is dlopen()ed and asked, not just compiled --
                         desc vs manifest, estimate parity with the CLI, decline-on-noise)

Tests run:
  $ git status -sb          -> ## wp/637-wim-lane (clean)
  $ make -j2 -q all         -> MAKE_Q_EXIT=0   (tree was already built)

  $ timeout 3000 make -j2 test > /tmp/wp637-maketest.log 2>&1
  MAKE_EXIT=0                                (log: /tmp/wp637-maketest.log, 7039 lines)
    test-shard-check: OK (64 prerequisite binaries invoked across shards)
    test heredocs: OK (140 python block(s) compile)
    repo hygiene: OK (716 tracked files, no binaries/oversize outside allowlist)
    check-codecpack-sync layer A: all 12 vendored packs are in the record
      (layers B and C SKIPPED -- no registry checkout on this host)

  $ INVFS_E2E_AGENT=wp637 bash tools/run-e2e.sh tools/test-wim.sh
  E2E_EXIT=0                                 (log: /tmp/wp637-wim-e2e-final.log)
    cc: CLI + libwim.so OK (one source, both builds)
    a.wim: 4 members, bit-exact, map ok, estimate 70000640
    b.wim: 4 members, bit-exact, map ok, estimate 68943872
    solid.wim: 2 members, bit-exact, map ok, estimate 69468160   (3 entries alias ONE extent)
    incomp.wim: 5 members, bit-exact, map ok, estimate 70780928
  so: desc name=wim version=1.0.0 class=containerpack; estimate rc=0 orig=2893473
      mbr=70000640 eligible=1                                  (.so == CLI, byte for byte)
  so: desc ... estimate rc=3 orig=0 mbr=0 eligible=0           (64 KiB of /dev/urandom: decline)
  soleg a.wim 0 1 / incomp.wim 0 1 / junk.bin 3 0               -- ABI leg (new this round)
    9 refuse fixtures, each exit 3 exactly (badmagic, short, wip, split, badver,
      badcb, stride, pastend, zerotbl) + stride8.wim ACCEPTED (the +8 tail tolerated)
    FS leg: mkfs -> cp -> sweep -> "a.wim: wim (codecpack)" -> !mbrNNNN siblings,
      !mbrt, !mbrmap -> verify --deep -> invf-cat bit-exact (container and direct member)
      -> pack-ABSENT reads (INVFS_CODECPACKS=/nonexistent) bit-exact through the map
      -> unlink drops every "!" sibling -> fsck clean, survivor bit-exact
    dedupe leg (the reason the lane exists; one 1048576 B resource at a.wim/1573784
      and b.wim/717336 -- both misaligned, by different amounts):
        empty baseline 120826 free; used: A 229, B 291, A+B 337, resource-alone 183 blocks
        shared by the two WIMs: 183 blocks = 749568 B of the 1048576 B resource
        AB sweeps: merged=33 cross-file=33 segments
        -> the resource is stored ONCE across the two WIMs
    size-guard leg: incomp.wim refused by the engine's containerpack size guard,
      1163 fixed + 3674112 content + 6400 member-cost (5 members) = 3681675 B vs
      3672778 B original; refusal EXPLAINED on the record, file still bit-exact
      through the generic floor (no CONTAINER{26} stamp)
    bounded-stub mechanism (the split-WIM shape) graded on a fixture: 4-entry table,
      1 member inside 1576960 B, 3-entry map, strict partition, rebuild == original
    CORPUS LEG, a real Microsoft WIM graded WHOLE:
      ISO Windows 7 SP1 26in1 (x86-x64) by Eagle123 / JINN/WinSetup/boot7x86.wim
      255690593 B, table 8316 entries, dwVersion 0x00010d00, 8316 members,
      map 8318 entries strict partition of 255688704 B, recipe 584587 B (0.23%),
      rebuild bit-exact -- every resource Microsoft wrote was parsed and returned
    scratch removed (/var/tmp/wp637wim-*); INVFS_WIM_KEEP=1 keeps it
    WIM E2E: PASS

  $ INVFS_E2E_AGENT=wp637 bash tools/run-e2e.sh tools/test-ivpacks.sh
  IVPACKS_EXIT=0  ("0 failure(s), 5 skip(s)" -- the 5 are missing host fixture
                    tooling, not wim; ivpacks' CPACKS list does not include wim,
                    which is why test-wim.sh now carries its own ABI leg)

Result: PASS

Notes on the round (what the resume actually found):
  - The headline bug was already committed (5cc214a): the lane declined EVERY real
    WIM because the 56/8/56 resource-header words were read as one field. The
    fixtures had been written from the same misreading, so no sync check could
    have caught it -- only a real WIM could, and the corpus leg grades one whole
    (8316 resources) on this host now.
  - Gates were green on arrival. Fallout fixed this round was in the suite's own
    hygiene, not in the lane:
      64ad76e  test-wim.sh leaked 105-348 MB of scratch per run (903 MB accumulated
               here, on a / with 1.6 GB free). Trapped; INVFS_WIM_KEEP=1 keeps it.
      59494dc  libwim.so was graded by `cc` alone -- it could have exported nothing
               and stayed green. Now dlopen()ed via ctypes, descriptor checked
               against the manifest, estimate required to equal the CLI's, and
               64 KiB of noise required to decline with 3 (not 1).

Remaining TODOs:
  1. CROSS-VERSION SHARED-RESOURCE MEASUREMENT (WP validation item 3, "a measurement,
     not a gate") -- NOT RUN. Two independent blockers on this host:
       a) every Windows 10 / 11 ISO under /mnt/invfs-scratch/iso is unreadable:
          sector 16 (the ISO9660 PVD) and the first 32 KiB are all zeroes, and
          7z 25.01 answers "Cannot open the file as archive" (exit 2). Only the
          Windows 7 26in1 ISO opens, so there is no second version to compare
          against. `ls -l` sizes look complete; the payload does not.
       b) /mnt/invfs-scratch is 100% full (86 G, 8 K avail) and / has 2.1 G free,
          while a 5.8 GB install.wim needs ~4.3x itself in staging (source, stub,
          rebuilt, every member) -- the corpus leg's own cap refuses that shape
          (INVFS_WIM_CORPUS_MAX_MB=320) and skips with the numbers rather than
          dying as an opaque ENOSPC.
     Exact command once a re-downloaded ISO and scratch exist:
       INVFS_WIM_ISO=<win10.iso> INVFS_WIM_MEMBER=sources/install.wim \
       INVFS_WIM_CORPUS_MAX_MB=4096 bash tools/test-wim.sh
     and then the same with a win11 ISO, comparing the two runs' member tables
     (the pack has no cross-WIM reporting leg yet; the measurement is the two
     `enumerate` tables intersected on the resource SHA-1, which is entry field
     bytes 24..44 of each 50-byte table entry).
  2. install.esd / LZMS solid payloads: only a verbatim round-trip is owed by this
     WP and only a fixture (solid.wim, 3 entries aliasing one extent -> 1 member)
     was graded. No real .esd is reachable on this host for the same reason as (1).
  3. wim is not in tools/test-ivpacks.sh's CPACKS list (out of WP637's scope: that
     file belongs to the pack-ABI tier). test-wim.sh's new ABI leg covers the same
     ground for this pack; if the orchestrator prefers one home for the check, the
     leg is a straight port into the CPACKS loop.
  4. `make check-codecpacks` (STRICT) cannot pass here: no registry checkout, so
     layers B and C have nothing to compare. Layer A is green, which is the gate
     `make test` runs.
