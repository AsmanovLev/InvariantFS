# InvariantFS — native Linux build (incremental).
# This Makefile is the ONLY build system: every object list derives from
# $(CORE), so a new core module cannot silently go missing. `make` builds
# everything, `make invf-fuse` one target.
CC      ?= gcc
SRC     := src
OUT     := bin
OBJ     := build/obj
VERSION_GIT := $(shell git describe --always --tags 2>/dev/null | sed 's/-.*//')
# A build from a SOURCE TARBALL has no .git -- and a tarball build is exactly
# what the Debian/Arch/Gentoo/Void recipes do, so `git describe` answers
# nothing. The old `|| echo unknown` never fired (the pipeline's status is
# sed's, which is 0), so VERSION was empty, the -D below defined
# INVFS_VERSION_STRING as "", and that DEFEATED the #ifndef fallback in
# src/core/invarifs.h:18 -- producing a binary that reports
# "version  (build ...)" with no version at all. Fall back to that header's
# own value instead, which is the canonical one.
VERSION := $(if $(VERSION_GIT),$(VERSION_GIT),$(shell sed -n 's/^#define INVFS_VERSION_STRING "\(.*\)"/\1/p' $(SRC)/core/invarifs.h | head -1))
BUILD_DATE := $(shell date '+%Y-%m-%d')
AUTHOR_NAME := Lev_Asmanov
AUTHOR_EMAIL := asmanovlev@gmail.com
LICENSE := GPL-2.0-only

# Sources live in per-role subdirs under $(SRC); objects stay flat in $(OBJ)
# (basenames are unique across subdirs, and tools/test-*.sh link lines use
# $(OBJ)/<basename>.o).
SRCDIRS := $(SRC)/core $(SRC)/codecs $(SRC)/recipes $(SRC)/cli $(SRC)/vendor7z $(SRC)/legacy
vpath %.c $(SRCDIRS)

CFLAGS  := -std=gnu11 -O2 -Wall -MMD -MP -I$(SRC) $(addprefix -I,$(SRCDIRS)) -pthread \
           -DINVFS_EMBED_FLACX -DMINIZ_NO_ZLIB_APIS \
           -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
           -DINVFS_VERSION_STRING=\"$(VERSION)\" \
           -DINVFS_BUILD_DATE=\"$(BUILD_DATE)\" \
           -DINVFS_AUTHOR_NAME=\"$(AUTHOR_NAME)\" \
           -DINVFS_LICENSE=\"$(LICENSE)\"
LDLIBS  := -Wl,-l:libzstd.so.1 -lz -lpthread
STOCK_ZLIB_SRC := $(wildcard $(SRC)/zlib/*.c)
STOCK_ZLIB_O := $(patsubst $(SRC)/zlib/%.c,$(OBJ)/zlib_stock_%.o,$(STOCK_ZLIB_SRC))
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3)
FUSE_LIBS   := $(shell pkg-config --libs fuse3)

CORE    := volume vol_cpack helper_exec tool_scratch vol_plugin_client vol_png vol_seal vol_repair \
           vol_resize vol_fsck vol_crash vol_exer vol_dedupe vol_textzone \
            vol_heat vol_sweep vol_read vol_write vol_records vol_ast \
            vol_dirs vol_tier vol_meta_merge vol_metabuf vol_btree vol_delta \
            vol_fold vol_reclaim vol_spt0 vol_anchor vol_walk \
            arc crc32c lz4 flacx tarx pngx blkio miniz blake3 blake3_dispatch blake3_portable ppmd8 ppmd8enc ppmd8dec ppmd_codec codec bcj_x86 rs deflate_repro \
            deflate_backend_system deflate_backend_stock
CORE_O  := $(addprefix $(OBJ)/,$(addsuffix .o,$(CORE))) $(STOCK_ZLIB_O)
B3      := blake3 blake3_dispatch blake3_portable

# Canonical core object list for the e2e helper link lines in tools/test-*.sh.
# Emitted by `all` so it can never go stale against $(CORE). Kept as a list of
# individual .o files (NOT a .a archive) so link order does not matter.
CORE_OBJS_FILE := build/core_objs.txt

# WP101: hermetic unit-suite env, applied per recipe line (see `test:`).
# Unset in production: pack_scan_all() scanning /usr/lib/invfs/codecpacks
# is the deployed behaviour.
TESTENV := INVFS_CODECPACKS_SYS=0

# WP104: run every unit-suite command in a private /tmp + /dev/shm (mount
# namespace, no root required, same shape as run-e2e.sh's ISOLATED mode).
# The unit binaries write to FIXED /tmp names and take their scratch dir
# from argv, and the recipe hands them /tmp -- which is shared with every
# worktree and every concurrent `make test`, and is a tmpfs on most hosts.
# A killed run therefore leaves residue the next run trips over. See
# tools/run-unit-isolated.sh for what this does and does NOT fix.
TESTISO := bash tools/run-unit-isolated.sh
# wp/dirs-free-before-publish needs cross-process state, and $(TESTISO) gives
# every command a private /tmp. build/ is gitignored and per-worktree.
FRB_T := $(CURDIR)/build/frbtest

TOOLS   := invf-mkfs invf-verify invf-fsck invf-cp invf-cat invf-ls invf-stat \
           invf-zip invf-arctest invf-blkio_test invf-fuse invf-import invf-sweep meta_probe \
           invf-stats invf-resize invf-rollback invfs-pack \
           invf-v3inode invf-plugin-host

all: $(TOOLS:%=$(OUT)/%) $(CORE_OBJS_FILE)

$(CORE_OBJS_FILE): $(CORE_O) | $(OBJ)
	@printf '%s\n' $(CORE_O) > $@

$(OBJ):
	mkdir -p $@

# WP114: $(OUT) is where every binary is LINKED, and unlike $(OBJ) it had
# no rule at all -- so a build in a tree that had no bin/ yet died with
# "ld: cannot open output file bin/invf-mkfs". It only ever worked because
# every developer's tree already had a bin/ in it. That is why
# `dpkg-buildpackage` (which builds from a pristine source package, so
# there is no bin/ yet) could not build at all.
$(OUT):
	mkdir -p $@

$(OBJ)/%.o: %.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/deflate_repro.o: $(SRC)/codecs/deflate_repro.c $(SRC)/codecs/deflate_backend.h $(SRC)/codecs/deflate_repro.h | $(OBJ)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(OBJ)/zlib_stock_%.o: $(SRC)/zlib/%.c | $(OBJ)
	$(CC) $(CFLAGS) -I$(SRC)/zlib -DZ_PREFIX -fPIC -c -o $@ $<

$(OBJ)/deflate_backend_system.o: $(SRC)/codecs/deflate_backend_zlib.c $(SRC)/codecs/deflate_backend.h $(SRC)/codecs/deflate_repro.h | $(OBJ)
	$(CC) $(CFLAGS) -DINVFS_BACKEND_ENGINE=INVFS_DEFLATE_ENGINE_ZLIB_SYSTEM -DINVFS_BACKEND_SYM=invfs_deflate_backend_system -fPIC -c -o $@ $<

$(OBJ)/deflate_backend_stock.o: $(SRC)/codecs/deflate_backend_zlib.c $(SRC)/codecs/deflate_backend.h $(SRC)/codecs/deflate_repro.h | $(OBJ)
	$(CC) $(CFLAGS) -I$(SRC)/zlib -DZ_PREFIX -DINVFS_BACKEND_ENGINE=INVFS_DEFLATE_ENGINE_ZLIB_STOCK -DINVFS_BACKEND_SYM=invfs_deflate_backend_stock -fPIC -c -o $@ $<

# tools that embed their own miniz copy must not double-link ours:
# zip.c includes miniz internally, so it links without $(OBJ)/miniz.o
MINIZLESS := $(filter-out miniz.o,$(notdir $(CORE_O)))

define TOOL_RULE
$(OUT)/invf-$(1): $$(OBJ)/$(1).o $(CORE_O) | $(OUT)
	$$(CC) $$(CFLAGS) -o $$@ $$< $(CORE_O) $$(LDLIBS) $$(2)
endef

# CLI tools (main in src/cli/<name>.c)
CLI_MAINS := mkfs verify fsck cp cat ls stat arctest blkio_test resize \
             metabuf_test btree_test btree_repair_test v3inode overlay_test fold_test concurrency_test \
             sweep_v3_test sweep_collect_test symlink_v3_test large_file_v3_test dedupe_v3_test deflate_repro_test window_test \
             read_parallel_bitexact_test arc_concurrency_test \
             nlink_v3_test recipe_fsck_test cpack_guard_test orphan_test rt30_slot_test anchor_test \
             fsck_rootslot_test batch_owner_test plugin_host_test plugin_mt_test rs_stability_test \
             fsck_liveness_test scratch_policy_test v2rb_rollback_test keycmp_test \
             lane_release_test pbaref_v3_test v2_open_test \
             sweep_publish_rollback_test \
             rollback_symlink_test \
             sibling_retire_v3_test tar_cap_test fold_delta_read_test \
             reclaim_reader_epoch_test readdir_error_test dedupe_symlink_test dirs_free_before_publish_test \
             stat_v3_counts_test acl_eio_test acl_inherit_test meta_clobber_test spn_skip_recipe_test \
             walk_status_test walk_status_fuse_test no_v2_surface_test \
             heat_walk_test no_ckp0_surface_test \
             table_sync_evict_test write_create_path_test tz_registry_test
$(foreach t,$(CLI_MAINS),$(eval $(call TOOL_RULE,$(t),)))

# reclaim_reader_epoch_test was, for one commit, a red control that built but
# did not run: it is the deterministic control for the base-reclaim-vs-reader
# race, and vol_reclaim_drain waited on a count nothing incremented. That
# defect is fixed (wp/reclaim-blocking-drain), so it is in the run recipe
# below, next to fold_delta_read_test, which is the same interleave on the
# delta side.

# WP71: loads every containerpack .so through dlmopen/dlopen -> needs -ldl,
# and resolves tools/codecpacks/... relative to the repo root.
$(eval $(call TOOL_RULE,ivpack_packs_test,-ldl))

# WP60: invfs-pack is named differently (invfs- not invf-)
$(OUT)/invfs-pack: $(OBJ)/pack.o $(CORE_O) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/pack.o: $(SRC)/cli/pack.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ)/invf-zip.o: $(SRC)/recipes/zip.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OUT)/invf-zip: $(OBJ)/invf-zip.o $(filter-out $(OBJ)/miniz.o,$(CORE_O))
	$(CC) $(CFLAGS) -o $@ $< $(filter-out $(OBJ)/miniz.o,$(CORE_O)) $(LDLIBS)

$(OUT)/invf-fuse: $(OBJ)/fuse_fs.o $(OBJ)/tmpstore.o $(CORE_O)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $< $(OBJ)/tmpstore.o $(CORE_O) $(LDLIBS) $(FUSE_LIBS)
$(OBJ)/fuse_fs.o: $(SRC)/cli/fuse_fs.c | $(OBJ)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c -o $@ $<
$(OBJ)/tmpstore.o: $(SRC)/cli/tmpstore.c | $(OBJ)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -c -o $@ $<

$(OUT)/invf-import: $(OBJ)/invf-import.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/invf-import.o: tools/invf-import.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/invf-sweep: $(OBJ)/invf-sweep.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/invf-sweep.o: tools/invf-sweep.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/invf-rollback: $(OBJ)/invf-rollback.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/invf-rollback.o: tools/invf-rollback.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT)/invf-plugin-host: $(OBJ)/invf-plugin-host.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS) -ldl
$(OBJ)/invf-plugin-host.o: tools/invf-plugin-host.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- ADR-007 containerpack plugins (.so) ----------------------------------
# Every C containerpack ships as BOTH a CLI helper (bin/<name>, built by its
# tools/test-<pack>.sh with cc -std=c11 -Wall -Wextra -Werror) and a shared
# object (lib<name>.so, -DIVPACK_SHARED_LIB) that invf-plugin-host dlmopen()s.
# Both come from the same .c, and the plugin glue is compiled into the CLI
# build too (unused there), so the two can never drift apart.
CPACKS      := qcow2 ext4fs fatfs ntfs rawdisk vdi xfs p7z
PACKDIR      = tools/codecpacks/$(1).codecpack
PLUGIN_CFLAGS := -std=gnu11 -O2 -fPIC -shared -Wall -Wextra -Werror \
                 -I$(SRC)/include -I$(SRC)/codecs -DIVPACK_SHARED_LIB
# qcow2 links both repro backends; its own zlib calls use the bundled stock copy.
PLUGIN_CFLAGS_qcow2 := $(PLUGIN_CFLAGS) -I$(SRC)/zlib -DZ_PREFIX
PLUGIN_EXTRA_qcow2 := $(OBJ)/deflate_repro.o $(OBJ)/deflate_backend_system.o \
                      $(OBJ)/deflate_backend_stock.o $(STOCK_ZLIB_O)

# WP113: a pack's LINK LIBRARIES come from its own manifest, `libs = -lz`,
# exactly as the registry's build-helpers.sh pack_libs() reads them
# (registry tools/build-helpers.sh:47). Before this, the .so rule hardcoded
# `-lz` for all eight packs, so the manifest field was dead here and a pack
# that gained a dependency was invisible to this repo's build -- the same
# class of bug as WP109, one layer up. The filter (flag-shaped tokens only,
# everything else reported and dropped) lives in tools/pack-libs.sh, which is
# also what `make print-libs-<pack>` runs, so there is exactly one reader.
PACK_LIBS = $(shell bash tools/pack-libs.sh $(call PACKDIR,$(1)))

define PLUGIN_SO_RULE
$(call PACKDIR,$(1))/lib$(1).so: $(call PACKDIR,$(1))/$(1).c $(call PACKDIR,$(1))/manifest $(SRC)/include/ivpack_api.h $(SRC)/include/ivpack_impl.h $(PLUGIN_EXTRA_$(1))
	$(CC) $(if $(PLUGIN_CFLAGS_$(1)),$(PLUGIN_CFLAGS_$(1)),$(PLUGIN_CFLAGS)) -o $$@ $$< $(PLUGIN_EXTRA_$(1)) -ldl $(call PACK_LIBS,$(1))
endef
$(foreach p,$(CPACKS),$(eval $(call PLUGIN_SO_RULE,$(p))))

PLUGIN_SO := $(foreach p,$(CPACKS),$(call PACKDIR,$(p))/lib$(p).so)
plugin-so: $(PLUGIN_SO)

# WP113: the CLI half of every containerpack -- bin/<name>, the helper the
# pack's own manifest argv names and that each tools/test-<pack>.sh builds
# for itself with a hand-written `cc -std=c11 -O2 -Wall -Wextra -Werror` line.
# Built here too, from the same manifest `libs`, so a pack's link needs are
# declared once and the harnesses can ask for them (`print-libs-%`).
HELPER_CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror
HELPER_CFLAGS_qcow2 := $(HELPER_CFLAGS) -I$(SRC)/codecs -I$(SRC)/zlib -DZ_PREFIX
define HELPER_RULE
$(call PACKDIR,$(1))/bin/$(1): $(call PACKDIR,$(1))/$(1).c $(call PACKDIR,$(1))/manifest $(PLUGIN_EXTRA_$(1))
	@mkdir -p $$(dir $$@)
	$(CC) $(if $(HELPER_CFLAGS_$(1)),$(HELPER_CFLAGS_$(1)),$(HELPER_CFLAGS)) -o $$@ $$< $(PLUGIN_EXTRA_$(1)) $(call PACK_LIBS,$(1))
endef
$(foreach p,$(CPACKS),$(eval $(call HELPER_RULE,$(p))))

HELPERS := $(foreach p,$(CPACKS),$(call PACKDIR,$(p))/bin/$(p))

# WP114: a codecpack may ship a C helper WITHOUT being a containerpack, in
# which case it is in neither CPACKS nor any *_EXTRA_* list and so got no
# build rule at all. jxlest (the SOF marker-walk estimator the jxl pack's
# manifest names as `estimate = jxlest estimate {in}`) is the case in point:
# plain C11 + libc, not an ivpack plugin, built ad-hoc by tools/test-jxl.sh
# and previously by packaging/install.sh's hand-rolled per-pack compile.
# install.sh now SHIPS what the Makefile builds rather than re-deriving the
# recipe, so a helper with no rule here is a helper that silently stops
# being installed. Declared explicitly below; same flags as the other helpers.
STANDALONE_HELPER_FILES := tools/codecpacks/jxl.codecpack/bin/jxlest

tools/codecpacks/jxl.codecpack/bin/jxlest: \
		tools/codecpacks/jxl.codecpack/jxlest.c \
		tools/codecpacks/jxl.codecpack/manifest
	@mkdir -p $(dir $@)
	$(CC) $(HELPER_CFLAGS) -o $@ $< $(call PACK_LIBS,jxl)

helpers: $(HELPERS) $(STANDALONE_HELPER_FILES)


# The e2e harnesses that compile a pack's CLI themselves (tools/test-ivpacks.sh)
# must link the same extras as the .so rule above, or they go stale the moment a
# CORE member or a src/zlib/*.c file is added -- which is exactly how qcow2 broke
# when Q2R3 made it call invfs_deflate_repro_*. `make print-obj-<pack>` emits the
# list (repo-relative), so those harnesses never hand-maintain it again.
# An EMPTY print means the list itself is empty (e.g. $(wildcard src/zlib/*.c)
# matched nothing) -- the harnesses are required to fail loudly on that, not to
# fall through to a silently under-linked binary.
print-obj-%:
	@printf '%s\n' $(PLUGIN_EXTRA_$*)

# WP113: the same service for a pack's `libs`, so a harness compiling bin/<p>
# itself never hand-maintains the link flags either. Prints the accepted
# flags, one line of space-separated tokens (empty for the seven packs that
# link libc only); non-link tokens in the manifest are reported on stderr and
# dropped by the same PACK_LIBS filter the build uses.
print-libs-%:
	@bash tools/pack-libs.sh $(call PACKDIR,$*)

# Same service for the harnesses that compile a small main() against the
# engine: hand-written `-I` lists go stale the moment a header moves into a
# new $(SRCDIRS) entry, and they had already lost src/legacy and src/cli.
# Emits the directories (one per line, like print-obj-%), not the flags:
# the harness turns each into -I$REPO/<dir> once it has anchored the path.
print-incdirs:
	@printf '%s\n' $(SRC) $(SRCDIRS)

# WP110: the hostile-recipe fuzzer for the pack trust boundary.
# tools/fuzz/recipefuzz.c #includes src/core/vol_cpack.c so it calls the
# REAL static cpack_map_parse / cpack_map_validate / cpack_map_serve --
# the functions that consume the MRMP/MRM2 blob a codecpack hands back
# (vol_cpack.c:2668 at sweep time, 2384-2401 again at read time). A mirror
# of the parser (as src/legacy/fuzz_invfs.c "leg 6" is) can agree with the
# code while the code is wrong, and cannot see an overflow at all; this
# links the production code and lets ASan judge it.
#
# vol_cpack.o is dropped from the link because the harness's own
# translation unit already provides those statics -- linking both is a
# duplicate-symbol error, not a stronger test.
#
# Sanitizers go on the harness TU only; the engine objects stay
# uninstrumented, which is enough: ASan replaces the allocator and
# intercepts memcpy process-wide, so an out-of-bounds read of an
# ASan-allocated recipe buffer inside uninstrumented engine code is still
# caught. -fsanitize and `ulimit -v` cannot be combined (ASan reserves
# ~16 TB of shadow), so bound this with ASAN_OPTIONS=hard_rss_limit_mb.
RECIPEFUZZ_CFLAGS := $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -g
RECIPEFUZZ_OBJS  := $(filter-out $(OBJ)/vol_cpack.o,$(CORE_O))
$(OUT)/recipefuzz: tools/fuzz/recipefuzz.c $(CORE_O)
	$(CC) $(RECIPEFUZZ_CFLAGS) -o $@ $< $(RECIPEFUZZ_OBJS) \
	      -fsanitize=address,undefined $(LDLIBS)

recipefuzz: $(OUT)/recipefuzz

# WP129: the gzip header parse, fuzzed against the REAL engine function.
# Same shape as recipefuzz above, and for the same reason: the harness
# #includes src/core/vol_cpack.c, so it runs the shipped gz_header_len()
# static. A mirror of the parser cannot find an overflow -- it is compiled
# with whatever bounds its author gave it, which is the bug.
#
# vol_cpack.o is dropped for the same reason as recipefuzz: the harness TU
# already provides those statics.
#
# Two forms from one source:
#   gzhdrfuzz-libfuzzer  libFuzzer, for the long soak (needs clang+libFuzzer,
#                        so it is NOT wired into `make test`)
#   gzhdrfuzz            the standalone driver: the seed corpus plus a PRNG
#                        sweep, ordinary CFLAGS, and IS in `make test`. The
#                        gate has to run wherever the tree is built.
GZHDRFUZZ_OBJS := $(filter-out $(OBJ)/vol_cpack.o,$(CORE_O))
GZHDR_SEEDS    := $(sort $(wildcard tools/fuzz/seeds/gzhdr/*))

# Sanitizers go on the harness TU only; the engine objects stay
# uninstrumented, which is enough: ASan replaces the allocator and
# intercepts memcpy process-wide, so an out-of-bounds read of an
# ASan-allocated buffer inside uninstrumented engine code is still caught.
# -fsanitize and `ulimit -v` cannot be combined (ASan reserves ~16 TB of
# shadow), so bound the soak with ASAN_OPTIONS=hard_rss_limit_mb.
GZHDR_SAN_CFLAGS := -std=gnu11 -O1 -g -fsanitize=address,undefined \
                    -fno-omit-frame-pointer $(addprefix -I,$(SRC) $(SRCDIRS)) \
                    -DINVFS_EMBED_FLACX -DMINIZ_NO_ZLIB_APIS

# WP129: the regression test. Sanitizers are not optional here -- on main
# the malformed cases are a memory error, not a wrong return value, so an
# uninstrumented build of this test would go GREEN on the bug.
# detect_leaks=0: vol_open allocates v->meta_type_bitmap (1 MiB at this
# geometry, src/core/volume.c:4594) and vol_close does not free it. That is a
# PRE-EXISTING volume-lifecycle leak, nothing to do with the header walk --
# and it is invisible to every other unit binary because none of them are
# built with a sanitizer. Fixing it belongs to whoever owns volume.c's
# lifecycle, not here: LeakSanitizer is not what this gate is for, and
# silencing it here is not a claim that the leak does not exist.
GZHDR_TEST_ASAN := hard_rss_limit_mb=4096:detect_leaks=0
# WP-arc-concurrent-safe: the RED CONTROL for the ARC concurrency defect, and
# the one gate in this file that a broken arc.c cannot pass by luck.
#
# It is standalone on purpose: src/core/arc.c has no dependency outside libc,
# so these link arc.c ALONE. No volume, no image, no codecpack -- which is
# what makes a sanitizer build of it cost about a second instead of
# rebuilding the engine twice.
#
#   arc-conc-tsan  -> arc.c's hash chains, lists and byte accounting are
#                     plain racy writes. TSAN does not need two accesses to
#                     overlap in TIME, only to be unordered, and with no lock
#                     at all in the file unorderedness is guaranteed: this
#                     reports on the first run, every run.
#   arc-conc-asan  -> the borrow. arc_get handed out a pointer "valid until
#                     the next arc_put"; the read path memcpy'd out of it; a
#                     concurrent arc_replace does free(victim->data). The
#                     test PLANS that free with a barrier instead of racing
#                     for it, and checks the bytes -- a use-after-free that
#                     returned success is a bit-exactness failure, not a
#                     crash, so the byte check is the point.
#
# The source declares arc_get_copy WEAK, so the same binary compiles and runs
# against a tree that predates the fix (the symbol resolves to NULL and the
# test falls back to the pre-fix borrow). That is what lets one command be
# both the red control and the green gate.
# -fsanitize and `ulimit -v` cannot be combined (ASan reserves ~16 TB of
# shadow), so bound these with ASAN_OPTIONS=hard_rss_limit_mb.
ARC_SAN_CFLAGS := -std=gnu11 -O1 -g -fno-omit-frame-pointer -I$(SRC)/core
ARC_SAN_TSAN    := -fsanitize=thread
ARC_SAN_ASAN    := -fsanitize=address

$(OUT)/invf-arc-conc-tsan: src/cli/arc_san_test.c src/core/arc.c src/core/arc.h | $(OUT)
	$(CC) $(ARC_SAN_CFLAGS) $(ARC_SAN_TSAN) -o $@ $< src/core/arc.c -lpthread

$(OUT)/invf-arc-conc-asan: src/cli/arc_san_test.c src/core/arc.c src/core/arc.h | $(OUT)
	$(CC) $(ARC_SAN_CFLAGS) $(ARC_SAN_ASAN) -o $@ $< src/core/arc.c -lpthread

# WP-heat-table-concurrent-safe: the RED CONTROL for the read-heat table, the
# sibling defect the ARC control found on the same call site.
#
# It is standalone for the same reason arc.c's is: src/core/vol_heat.c needs no
# object outside libc + libzstd. It does need the real volume_internal.h for
# the struct, and it calls 25 project symbols (vol_find, vol_get_xattr,
# vol_v3_inode_get, meta_locate_ext, ...), so src/cli/heat_san_test.c defines
# all 25 as stubs at the bottom of itself. No volume, no image, no codecpack,
# so a sanitizer build still costs about a second.
#
#   heat-conc-tsan -> the structure. heat_tab_touch() had no lock and no
#                     atomic anywhere, and the read path reaches it with
#                     g_io_lock RELEASED (src/cli/fuse_fs.c:1407 then :1414,
#                     under fuse_loop_mt at :3353). TSAN needs the accesses to
#                     be UNORDERED, not to overlap, and with no edge anywhere
#                     unorderedness is guaranteed: reports first run, every run.
#   heat-conc-asan -> the lifetime, and the leg that is PLANNED rather than
#                     raced. heat_tab_touch() grows by free()-ing the old
#                     array (src/core/vol_heat.c:68); -Wl,--wrap=free performs
#                     that free and then parks before :69 republishes it, and
#                     the test starts a reader only after observing the park.
#                     INVFS_HEAT_SAN_LEG=planned selects that leg alone.
#   heat_table_concurrency_test -> the same source with no sanitizer: the
#                     glibc heap checker aborts on the unfixed table's
#                     free()-and-republish storm, and on the fixed one it
#                     asserts the table's own invariants.
#
# The source declares heat_locks_init WEAK, so all three compile and run
# against a tree that predates the fix (the symbol resolves to NULL and the
# table is then touched with no lock at all -- the defect). Same source, same
# commands, red before and green after. -Wl,--wrap=free goes on all three so
# the interposer's __real_free resolves even when the planned leg is not run.
# As with the ARC binaries, ASan is bounded with ASAN_OPTIONS=hard_rss_limit_mb.
HEAT_SAN_CFLAGS := -std=gnu11 -O1 -g -fno-omit-frame-pointer $(addprefix -I,$(SRC) $(SRCDIRS)) \
                   -pthread -DINVFS_EMBED_FLACX -DMINIZ_NO_ZLIB_APIS \
                   -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
                   -Wl,--wrap=free
HEAT_SAN_LIBS   := -lpthread -Wl,-l:libzstd.so.1 -lz
HEAT_SAN_SRC    := src/cli/heat_san_test.c src/core/vol_heat.c

$(OUT)/invf-heat-conc-tsan: $(HEAT_SAN_SRC) src/core/volume_internal.h | $(OUT)
	$(CC) $(HEAT_SAN_CFLAGS) -fsanitize=thread -o $@ $(HEAT_SAN_SRC) $(HEAT_SAN_LIBS)

$(OUT)/invf-heat-conc-asan: $(HEAT_SAN_SRC) src/core/volume_internal.h | $(OUT)
	$(CC) $(HEAT_SAN_CFLAGS) -fsanitize=address -o $@ $(HEAT_SAN_SRC) $(HEAT_SAN_LIBS)

$(OUT)/invf-heat_table_concurrency_test: $(HEAT_SAN_SRC) src/core/volume_internal.h | $(OUT)
	$(CC) $(HEAT_SAN_CFLAGS) -O2 -o $@ $(HEAT_SAN_SRC) $(HEAT_SAN_LIBS)

# WP-cpack-map-copy-out: the RED CONTROL for the parsed !mbrmap cache, the
# THIRD instance of the ARC shape on the same lock-free read call site.
#
# One source, three binaries, linked against src/core/vol_cpack.c ALONE.
# vol_cpack.c needs the real volume_internal.h for the struct and calls ~45
# project symbols, so src/cli/cpack_map_san_test.c defines the ones off the
# path under test as stubs at the bottom of itself. The stubs that ARE on the
# path (vol_find, vol_read_file, vol_read_range, seg_read_checked,
# vol_ast_recipe_parse) serve synthetic containers, so the code under test
# runs its REAL load, validate and serve paths -- the test observes the cache
# only through the shipped accessors, never through a mirror of them. No
# volume, no image, no codecpack, so a sanitizer build costs about a second.
#
#   cpack_map_conc_tsan  -> the STRUCTURE. vol_cpack.c had no lock and no
#                           atomic anywhere, and the read path reaches it
#                           with g_io_lock RELEASED (src/cli/fuse_fs.c:1407
#                           then :1423, under fuse_loop_mt at :3362).
#                           TSAN needs the accesses to be UNORDERED, not
#                           overlapping, and with no edge anywhere the
#                           unorderedness is guaranteed: reports first run.
#   cpack_map_conc_asan  -> the LIFETIME, and both legs are PLANNED rather
#                           than raced. planned_realloc: -Wl,--wrap=realloc
#                           performs the grow's real realloc -- which frees
#                           the old array -- and parks before
#                           `v->maps = nm`, so a reader is guaranteed to
#                           resolve against a freed, still-published array.
#                           planned_free: -Wl,--wrap=free performs a retire's
#                           real free of an entry's `ents` and parks with the
#                           slot still naming a live container. Neither needs
#                           a production hook.
#   cpack_map_conc_test  -> the same source, no sanitizer. The glibc heap
#                           checker aborts on the unfixed cache and the fixed
#                           one asserts every read returned its OWN
#                           container's bytes.
#
# The source declares cpack_locks_init WEAK, so all three compile and run
# against a tree that predates the fix (the symbol resolves to NULL and the
# cache is then used with no lock at all -- the defect). Same source, same
# commands, red before and green after. -Wl,--wrap=free and
# -Wl,--wrap=realloc go on all three so the interposers' __real_* resolve
# even when the planned legs are not selected.
#
# Unlike the ARC and heat ASAN binaries this one keeps LeakSanitizer ON: the
# entries are individually malloc'd, and a free that drops the payloads but
# not the descriptor leaks a struct per cached container for the life of the
# mount -- which is exactly the kind of thing that only shows up here.
CPACK_SAN_CFLAGS := -std=gnu11 -O1 -g -fno-omit-frame-pointer $(addprefix -I,$(SRC) $(SRCDIRS)) \
                    -pthread -DINVFS_EMBED_FLACX -DMINIZ_NO_ZLIB_APIS \
                    -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 \
                    -Wl,--wrap=free -Wl,--wrap=realloc
CPACK_SAN_LIBS   := -lpthread -Wl,-l:libzstd.so.1 -lz
CPACK_SAN_SRC    := src/cli/cpack_map_san_test.c src/core/vol_cpack.c
CPACK_SAN_ASAN   := hard_rss_limit_mb=4096

$(OUT)/invf-cpack_map_conc_tsan: $(CPACK_SAN_SRC) src/core/volume_internal.h | $(OUT)
	$(CC) $(CPACK_SAN_CFLAGS) -fsanitize=thread -o $@ $(CPACK_SAN_SRC) $(CPACK_SAN_LIBS)

$(OUT)/invf-cpack_map_conc_asan: $(CPACK_SAN_SRC) src/core/volume_internal.h | $(OUT)
	$(CC) $(CPACK_SAN_CFLAGS) -fsanitize=address -o $@ $(CPACK_SAN_SRC) $(CPACK_SAN_LIBS)

$(OUT)/invf-cpack_map_conc_test: $(CPACK_SAN_SRC) src/core/volume_internal.h | $(OUT)
	$(CC) $(CPACK_SAN_CFLAGS) -O2 -o $@ $(CPACK_SAN_SRC) $(CPACK_SAN_LIBS)

$(OUT)/invf-gz_header_test: src/cli/gz_header_test.c $(CORE_O)
	$(CC) $(GZHDR_SAN_CFLAGS) -o $@ $< $(CORE_O) \
	      -fsanitize=address,undefined $(LDLIBS)

$(OUT)/gzhdrfuzz: tools/fuzz/gzhdrfuzz.c $(CORE_O)
	$(CC) $(GZHDR_SAN_CFLAGS) -o $@ $< $(GZHDRFUZZ_OBJS) \
	      -fsanitize=address,undefined -Wl,-l:libzstd.so.1 -lz -lpthread

# The soak build. libFuzzer needs clang, so it is a separate target and is
# NOT in `make test` -- a tree that builds with gcc must still be gated.
#   make FUZZ_CC=/usr/lib/llvm-19/bin/clang gzhdrfuzz-soak
FUZZ_CC ?= clang
$(OUT)/gzhdrfuzz-libfuzzer: tools/fuzz/gzhdrfuzz.c $(CORE_O)
	$(FUZZ_CC) $(GZHDR_SAN_CFLAGS) -DGZHDR_LIBFUZZER -fsanitize=fuzzer -o $@ $< \
	      $(GZHDRFUZZ_OBJS) -Wl,-l:libzstd.so.1 -lz -lpthread

# The seed corpus is GENERATED, not checked in: every file in it is either
# 18 bytes of hand-written header or a member zlib can rebuild, and a
# binary blob in git is a blob nobody can review. `make gzhdr-seeds`
# regenerates it; the test rule depends on it.
gzhdr-seeds:
	@python3 tools/mk-gzhdr-seeds.py tools/fuzz/seeds/gzhdr >/dev/null

# The gate itself: corpus + 200k PRNG cases under ASan+UBSan. Fails on any
# over-read, on any P2 disagreement (a well-formed member the old walk
# accepted and the new one refuses), and on any P3 disagreement.
test-gzhdr: $(OUT)/gzhdrfuzz gzhdr-seeds
	$(TESTENV) $(TESTISO) $(OUT)/gzhdrfuzz 200000 0x9E3779B97F4A7C15 \
	    $(GZHDR_SEEDS)

# The long soak. Run in the background; report the run count AND the wall
# clock, because "no crash" without a count is not evidence.
#   make FUZZ_CC=/usr/lib/llvm-19/bin/clang gzhdrfuzz-soak SECS=240
gzhdrfuzz-soak: $(OUT)/gzhdrfuzz-libfuzzer gzhdr-seeds
	@# libFuzzer takes ONE corpus directory, not a directory plus a file
	@# list -- "Not a directory: <file>; exiting". So the seeds are seeded
	@# INTO the corpus dir rather than appended to the argument list.
	@mkdir -p $(OBJ)/gzhdrfuzz-corpus
	@cp -n $(GZHDR_SEEDS) $(OBJ)/gzhdrfuzz-corpus/ 2>/dev/null || true
	ASAN_OPTIONS=hard_rss_limit_mb=4096 \
	$(OUT)/gzhdrfuzz-libfuzzer -max_total_time=$(or $(SECS),240) \
	    -rss_limit_mb=4096 -max_len=1024 -print_final_stats=1 \
	    $(OBJ)/gzhdrfuzz-corpus

# .ivpack bundles (ADR-007 §3: uncompressed ZIP-0, manifest + sha256 +
# lib/<name>.so + bin/<name> CLI fallback). Artifacts land in dist/ivpack/.
IVPACKS := $(foreach p,$(CPACKS),dist/ivpack/$(p).ivpack)
ivpacks: plugin-so $(IVPACKS)
dist/ivpack/%.ivpack: $(PLUGIN_SO) | dist/ivpack
	$(TESTENV) bash tools/pack-ivpack.sh tools/codecpacks/$*.codecpack $@
dist/ivpack:
	mkdir -p $@

$(OUT)/meta_probe: $(OBJ)/meta_probe.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/meta_probe.o: tools/meta_probe.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJ) $(CORE_OBJS_FILE) $(TOOLS:%=$(OUT)/%) $(OUT)/invf-codec_test \
	       $(OUT)/invf-helper_exec_test $(OUT)/invf-metabuf_test \
	       $(OUT)/invf-btree_test $(OUT)/invf-delta_test \
	       $(OUT)/invf-plugin_host_test $(OUT)/invf-plugin_mt_test \
	       $(OUT)/invf-ivpack_packs_test $(PLUGIN_SO) $(IVPACKS) \
	       tools/invf-plugin-host \
	       $(OUT)/invf-fuzz $(OUT)/gzhdrfuzz $(OUT)/gzhdrfuzz-libfuzzer \
	       $(OUT)/invf-gz_header_test \
	       $(OBJ)/gzhdrfuzz-corpus tools/fuzz/seeds/gzhdr

# ---- tests ---------------------------------------------------------------
# unit tier: fast, no I/O images
# codec.o reaches tool_tmpdir (the scratch decision), so every link that
# pulls codec.o in without CORE_O has to pull tool_scratch.o in too.
$(OUT)/invf-codec_test: $(OBJ)/codec_test.o $(OBJ)/codec.o $(OBJ)/ppmd8.o $(OBJ)/ppmd8enc.o $(OBJ)/ppmd8dec.o $(OBJ)/ppmd_codec.o $(OBJ)/lz4.o $(OBJ)/bcj_x86.o $(OBJ)/tool_scratch.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# WP61: unit coverage for the shared helper containment launcher.
$(OUT)/invf-helper_exec_test: $(OBJ)/helper_exec_test.o $(OBJ)/helper_exec.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# WP-M10: delta-log unit harness + offline e2e driver (tools/, not src/cli).
$(OUT)/invf-delta_test: $(OBJ)/delta_test.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/delta_test.o: tools/delta_test.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

# Group-commit durability + flush-count regression. This one DEFINES fsync()
# so it can count physical flushes in-process: blkio.o's reference to fsync
# is an undefined symbol in this same executable, and a definition in the
# executable wins over libc.so. That is what lets T1 assert an EXACT count
# rather than inferring one from wall time. -ldl is for RTLD_NEXT.
$(OUT)/invf-groupcommit_test: $(OBJ)/groupcommit_test.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS) -ldl
$(OBJ)/groupcommit_test.o: tools/groupcommit_test.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

# fuzz tier: on-demand property/fuzz harness for the pure/parsing layers.
# NOT part of `make test` -- `make fuzz` only builds it, run it by hand:
#   bin/invf-fuzz [iterations] [seed]
FUZZ_O := $(OBJ)/codec.o $(OBJ)/tool_scratch.o $(OBJ)/ppmd8.o $(OBJ)/ppmd8enc.o $(OBJ)/ppmd8dec.o \
          $(OBJ)/ppmd_codec.o $(OBJ)/lz4.o $(OBJ)/bcj_x86.o
$(OUT)/invf-fuzz: $(OBJ)/fuzz_invfs.o $(FUZZ_O)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

fuzz: $(OUT)/invf-fuzz

# tools/fuzz/fuzz_manifest.c links exactly this set; hand-maintaining it in
# tools/test-fuzz.sh made a second source of truth for the fuzz link.
print-fuzz-objs:
	@printf '%s\n' $(FUZZ_O)

# CI target: 10k iterations (faster than fuzz's default 100k)
fuzz-ci: $(OUT)/invf-fuzz
	$(TESTENV) $(OUT)/invf-fuzz 10000 0x1CF51EE5

# Every *_test in CLI_MAINS must be a prerequisite of test: too. The list was
# EXPLICIT, which meant a WP that added a test binary to CLI_MAINS alone left
# make test running whatever stale binary was on disk -- observed twice today,
# once reporting the buggy behaviour AFTER the fix was already committed. The
# explicit list is generated from the test binaries below it instead, so the
# two cannot drift.
TEST_BINS := $(foreach t,$(filter %_test,$(CLI_MAINS)),$(OUT)/invf-$(t))
test: $(TEST_BINS) $(OUT)/invf-arctest $(OUT)/invf-blkio_test $(OUT)/invf-codec_test \
      $(OUT)/invf-helper_exec_test $(OUT)/invf-metabuf_test $(OUT)/invf-btree_test \
      $(OUT)/invf-delta_test $(OUT)/invf-groupcommit_test $(OUT)/invf-concurrency_test $(OUT)/invf-sweep_v3_test \
      $(OUT)/invf-sweep_collect_test $(OUT)/invf-verify \
      $(OUT)/invf-fold_delta_read_test \
      $(OUT)/invf-btree_repair_test \
      $(OUT)/invf-symlink_v3_test $(OUT)/invf-large_file_v3_test $(OUT)/invf-dedupe_v3_test \
      $(OUT)/invf-read_parallel_bitexact_test \
      $(OUT)/invf-arc-conc-tsan $(OUT)/invf-arc-conc-asan \
      $(OUT)/invf-arc_concurrency_test \
      $(OUT)/invf-heat-conc-tsan $(OUT)/invf-heat-conc-asan \
      $(OUT)/invf-heat_table_concurrency_test \
      $(OUT)/invf-cpack_map_conc_tsan $(OUT)/invf-cpack_map_conc_asan \
      $(OUT)/invf-cpack_map_conc_test \
      $(OUT)/invf-deflate_repro_test $(OUT)/invf-plugin_host_test $(OUT)/invf-plugin_mt_test \
      $(OUT)/invf-window_test $(OUT)/invf-nlink_v3_test \
      $(OUT)/invf-recipe_fsck_test $(OUT)/invf-fsck_liveness_test \
      $(OUT)/invf-cpack_guard_test $(OUT)/invf-orphan_test \
      $(OUT)/invf-scratch_policy_test $(OUT)/invf-v2rb_rollback_test \
      $(OUT)/invf-keycmp_test $(OUT)/invf-tar_cap_test \
      $(OUT)/invf-lane_release_test \
      $(OUT)/invf-pbaref_v3_test \
      $(OUT)/invf-sweep_publish_rollback_test \
      $(OUT)/invf-rollback_symlink_test \
      $(OUT)/invf-sibling_retire_v3_test \
      $(OUT)/invf-rt30_slot_test $(OUT)/invf-anchor_test $(OUT)/gzhdrfuzz \
      $(OUT)/invf-fsck_rootslot_test \
      $(OUT)/invf-batch_owner_test \
      $(OUT)/invf-rs_stability_test \
      $(OUT)/invf-arc_concurrency_test \
      $(OUT)/invf-arc-conc-tsan $(OUT)/invf-arc-conc-asan \
      $(OUT)/invf-gz_header_test \
      $(OUT)/invf-ivpack_packs_test $(OUT)/invf-mkfs $(OUT)/invf-cp \
      $(OUT)/invf-sweep $(OUT)/invf-fsck $(OUT)/invf-plugin-host \
      plugin-so helpers $(CORE_OBJS_FILE) all

	@# The explicit tool list above was the only set of prerequisites, and it
	@# is incomplete: the suites also invoke invf-ls, invf-cat, invf-verify,
	@# invf-stat, invf-import, invf-stats, invf-resize and invf-rollback,
	@# none of which were in it. So those binaries were whatever was last
	@# built, and adding a new source to CORE left them stale -- observed
	@# directly: after the ANC0 merge, invf-fsck had the anchor code and
	@# invf-ls did not, so a suite asserted on an old binary and failed for a
	@# reason that had nothing to do with the tree. A test that runs against a
	@# stale binary is worse than no test, because it reports a result. `all`
	@# is the one prerequisite that cannot drift from the source list.
	@# WP101: run the unit suite with the system codecpack directory off.
	@# invf-codec_test's registry-shape assertions count the STATIC
	@# codecs, but pack_scan_all() also scans /usr/lib/invfs/codecpacks,
	@# so on a host that has codecpacks installed `make test` went red for
	@# a reason that has nothing to do with the tree. Unset is what a
	@# deployed binary gets, so this is not a behaviour change.
	@# (Note: `export` in a recipe does not survive to the next line here,
	@# so the variable is applied per command as $(TESTENV).)
	@#
	@# WP104: every command below runs through $(TESTISO), which gives it
	@# a private /tmp and /dev/shm (mount namespace, no root). Two
	@# exceptions, both deliberate:
	@#   invf-helper_exec_test -- it branches on getuid(): unprivileged it
	@#     SKIPs the privilege-drop checks, and inside a user namespace
	@#     (fake root) those checks run and FAIL. Changing what a test
	@#     asserts is not isolation, it is sabotage; it already uses a
	@#     per-pid scratch dir, so it has no residue to collide with.
	@#   check-repo-hygiene.sh -- it reads git, which must see the real
	@#     worktree; nothing in it writes outside it.
	$(TESTENV) $(TESTISO) $(OUT)/invf-arctest
	$(TESTENV) $(TESTISO) $(OUT)/invf-blkio_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-codec_test
	$(TESTENV) $(OUT)/invf-helper_exec_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-metabuf_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-btree_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-delta_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-groupcommit_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-concurrency_test /tmp
	@# WP-inode-get-fold-race: the fold frees the retired delta chain, and a
	@# reader that had already resolved a ref was reading those blocks
	@# afterwards. The interleave is PLANNED (a weak seam inside the value
	@# read), not raced for: pre-fix the fold completes and the reader comes
	@# back with ANOTHER inode's row; post-fix the fold cannot get past the
	@# reader. It fails if the red control does not arm, so it cannot go
	@# green by the allocator quietly changing.
	$(TESTENV) $(TESTISO) $(OUT)/invf-fold_delta_read_test /tmp
	@# The base-tree half of the same question (wp/reclaim-blocking-drain):
	@# the fold's reachability diff frees a retired generation, and a reader
	@# that had already captured its root walked pages that were gone --
	@# measured at ~1e-6 of reads and reported by mbuf_read_ptr's
	@# allocation check as a plain -1, with no error having happened.
	@# vol_reclaim_drain waited on g_readers_in_flight and nothing in the
	@# tree ever incremented it. The interleave is PLANNED (a weak seam in
	@# v3_base_root, fired once the root is captured and before one page of
	@# it is read), and the assertion is the read-path outcome -- the row,
	@# field for field -- not a counter. Pre-fix the folds complete and the
	@# read fails; post-fix the fold cannot get past the reader. It also
	@# fails if the interleave is not decided at all, so it cannot go green
	@# by the setup quietly changing. It is in CLI_MAINS, so $(TEST_BINS)
	@# above already has it as a prerequisite.
	$(TESTENV) $(TESTISO) $(OUT)/invf-reclaim_reader_epoch_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-sweep_v3_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-sweep_collect_test /tmp
	@# WP135: the walk-receipt controls. invf-verify and invf-sweep are
	@# invoked as SUBPROCESSES by walk_status_test (they are separate mains,
	@# and a fresh process is also a fresh fault countdown), so they have to
	@# be built before it runs. Neither was in the prerequisite list above,
	@# which is the same staleness trap the comment on `all` describes.
	$(TESTENV) $(TESTISO) $(OUT)/invf-walk_status_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-walk_status_fuse_test /tmp
	@# WP145: the sweep's HEAT stage, which is the same class one stage
	@# away from the collect -- on the in-FUSE path the decay runs BEFORE
	@# the collect (src/cli/fuse_fs.c:2008 then :2037), so the collect's
	@# receipt cannot cover a walk that already stopped. Needs invf-mkfs on
	@# disk (the `all` prerequisite), and greps /proc/self/exe for its own
	@# refusal diagnostic so a `make -j4` that did not relink this binary
	@# fails loudly instead of proving nothing.
	$(TESTENV) $(TESTISO) $(OUT)/invf-heat_walk_test /tmp
	@# WP140: the name table must not evict a LIVE name because a lookup could
	@# not be completed. Runs under $(TESTISO): fuse_get_context() is stubbed
	@# to NULL, so every permission check takes the documented uid-0 bypass and
	@# no leg here depends on a denial the one-entry fake-root uid map would
	@# swallow. Its failure is INVFS_FAULT, armed through
	@# invfs_vol_btree_fault_reload() (the site is in vol_btree.c and the
	@# countdown is per-translation-unit), and it is DISARMED after every
	@# probe, so the unset path stays inert here too.
	$(TESTENV) $(TESTISO) $(OUT)/invf-table_sync_evict_test /tmp
	@# WP142: a bulk write whose name lookup did not COMPLETE must not take
	@# the create branch -- on v3 that empties the name's inode and frees its
	@# blocks before a byte is written. Three legs: `create` and `replace` are
	@# the green controls (a refuse-on-failure fix is indistinguishable from a
	@# refuse-always fix without them), `red` is the control, and it asserts
	@# BYTES: the file's content read back before and after. Its failure is
	@# INVFS_FAULT armed through invfs_vol_btree_fault_reload() (the site is in
	@# vol_btree.c and the countdown is per-translation-unit), DISARMED after
	@# every probe, so the unset path stays inert here too.
	$(TESTENV) $(TESTISO) $(OUT)/invf-write_create_path_test create /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-write_create_path_test replace /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-write_create_path_test red /tmp
	@# WP143: a batch-registry lookup that did not COMPLETE is not "the
	@# registry is empty". Reading it that way makes the flush publish a blob
	@# holding THIS RUN'S entries only, and those rows are the only record
	@# that a batch segment exists -- irreversible, and the segments become
	@# orphans no GC can reclaim. Two legs: `ctl` is the green control (a flush
	@# with a READABLE registry preserves the earlier run's entries AND adds
	@# its own -- a fix that froze the registry would pass the first half), `red`
	@# is the control. It parses the registry blob off the volume with
	@# vol_find + vol_read_file rather than asking the engine, so it does not
	@# only build when the fix is present, and it compares the WHOLE row rather
	@# than its seq -- the overwriting flush mints a new row that REUSES the
	@# lost row's seq, which is exactly how the loss stays hidden. Fault
	@# injection is INVFS_FAULT through invfs_vol_btree_fault_reload(), DISARMED
	@# after every probe.
	$(TESTENV) $(TESTISO) $(OUT)/invf-tz_registry_test ctl /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-tz_registry_test red /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-btree_repair_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-symlink_v3_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-large_file_v3_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-read_parallel_bitexact_test /tmp
	@# A failed listing must not look like an empty directory (readdir, and
	@# the same shape in listxattr). It runs under $(TESTISO) normally: it
	@# stubs fuse_get_context() to NULL, so every permission check takes the
	@# documented uid-0 bypass and no case here depends on a denial that a
	@# fake-root uid map would swallow. Its errno injection is INVFS_FAULT,
	@# which is unset here -- the unset path is the one that has to stay
	@# inert in production, so this also asserts it stays inert.
	$(TESTENV) $(TESTISO) $(OUT)/invf-readdir_error_test /tmp
	@# WP stat-counts-v3: invf-stat's file counts against invf-ls's, on one
	@# image. Two oracles on purpose -- the defect is that one of them read an
	@# empty v2 inode area on a v3 volume and printed a confident zero, so an
	@# expectation pinned to a literal would only encode today's count. Needs
	@# invf-mkfs, invf-ls and invf-stat on disk (the `all` prerequisite).
	$(TESTENV) $(TESTISO) $(OUT)/invf-stat_v3_counts_test /tmp
	@# WP xattr-enodata-vs-eio: vol_get_xattr returned -1 for BOTH "no such
	@# xattr" and "the row could not be read", and perm_check_cred used that
	@# to decide "this inode has no ACL" -- so a read error on the inode row
	@# degraded ACL enforcement to the plain mode triad and the mount started
	@# ALLOWING what the ACL denied. Runs under $(TESTISO) because it never
	@# asks the kernel for a permission: it builds struct acreds by hand and
	@# calls the evaluator directly, so there is no real uid denial in it for
	@# the one-entry fake-root uid map to swallow. The row-read failure is
	@# INVFS_FAULT, unset here, so this leg also asserts the unset path stays
	@# inert.
	$(TESTENV) $(TESTISO) $(OUT)/invf-acl_eio_test /tmp
	@# WP acl-inherit-on-failed-lookup: the CREATE side of the same class. The
	@# mkdir and create paths read the parent's POSIX ACL fail-closed, fold it
	@# against the create mode, and then had to resolve the object's OWN name a
	@# second time to stamp the result on -- with vol_find, whose uint64_t makes
	@# "no such name" and "the lookup could not be completed" both 0. One
	@# unreadable dirent row therefore produced an object that EXISTS, carries
	@# the ACL-masked mode triad, and carries NO ACL: on a mount that does not
	@# negotiate default_permissions (AGENTS.md 2.9) that is the widening, and
	@# the mode triad cannot express the per-identity decision the named ACL
	@# entries carried. Asserts the permission DECISION (perm_check_cred by
	@# hand, uid 2000/gid 0: denied by the ACL, allowed by the bare triad), and
	@# SEARCHES the seam ordinals because the site is consulted once per path
	@# component and the ordinal is not a constant -- requiring at-least-one AND
	@# not-all, so a table in which every position behaves alike cannot pass.
	@# Runs under $(TESTISO): no real uid denial anywhere in it.
	$(TESTENV) $(TESTISO) $(OUT)/invf-acl_inherit_test /tmp
	@# WP meta-clobber-on-unreadable-row: vol_get_meta returned -1 for BOTH
	@# "this inode has no meta row" and "the row could not be read", and
	@# meta_for_path answered the merged value with meta_defaults() -- 0644,
	@# owner root -- and meta_apply_patch then WROTE that back. So a chmod /
	@# utimens / chown on an inode whose row would not read silently reset its
	@# mode and owner and returned success: the volume was changed by a call
	@# that said it worked. The test reads mode and owner back OFF THE VOLUME,
	@# because an errno-only control would pass against a fix that renamed the
	@# return value and left the write-back in place.
	@# Runs under $(TESTISO): it calls the entry points directly with
	@# fuse_get_context() stubbed to NULL, so every permission check takes the
	@# documented uid-0 bypass and the only failure injected is the row read
	@# under test. No real uid denial, so nothing for the one-entry fake-root
	@# uid map to swallow. INVFS_FAULT is unset on the healthy legs, so those
	@# also assert the unset path stays inert.
	$(TESTENV) $(TESTISO) $(OUT)/invf-meta_clobber_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-spn_skip_recipe_test /srv/bench/scratch skipped
	$(TESTENV) $(TESTISO) $(OUT)/invf-spn_skip_recipe_test /srv/bench/scratch skippedctl
	@# WP-arc-concurrent-safe: the content cache, under concurrency. TSAN is
	@# the structure (arc.c had no lock at all), ASan is the borrow (an
	@# arc_get pointer freed underneath the reader's memcpy). Both must be
	@# silent: either one reporting is this gate going red.
	$(TESTENV) $(TESTISO) $(OUT)/invf-arc-conc-tsan
	ASAN_OPTIONS=$(GZHDR_TEST_ASAN) $(TESTENV) $(TESTISO) $(OUT)/invf-arc-conc-asan
	$(TESTENV) $(TESTISO) $(OUT)/invf-arc_concurrency_test /tmp
	@# WP-heat-table-concurrent-safe: the read-heat table, under concurrency.
	@# TSAN is the structure (vol_heat.c had no lock and the read path reaches
	@# it with g_io_lock released); ASan is the lifetime, and its planned leg
	@# schedules the grow's free() with -Wl,--wrap=free instead of racing for
	@# it; the plain build asserts the table's own invariants. Any one of the
	@# three reporting is this gate going red.
	$(TESTENV) $(TESTISO) $(OUT)/invf-heat-conc-tsan
	ASAN_OPTIONS=$(GZHDR_TEST_ASAN) INVFS_HEAT_SAN_LEG=all \
	    $(TESTENV) $(TESTISO) $(OUT)/invf-heat-conc-asan
	$(TESTENV) $(TESTISO) $(OUT)/invf-heat_table_concurrency_test
	@# WP-cpack-map-copy-out: the parsed !mbrmap cache, under concurrency. It
	@# is the THIRD instance of the ARC shape on the same lock-free call site:
	@# no lock at all, a realloc that invalidates every pointer into the
	@# array, a borrowed pointer handed to a reader that copies bytes out of
	@# it, and a serve that reports success. TSAN is the structure; ASan's two
	@# planned legs schedule the grow's realloc and a retire's free with
	@# -Wl,--wrap=realloc / -Wl,--wrap=free instead of racing for them; the
	@# plain build asserts every read got its own container's bytes. Any one
	@# of the three reporting is this gate going red.
	$(TESTENV) $(TESTISO) $(OUT)/invf-cpack_map_conc_tsan
	ASAN_OPTIONS=$(CPACK_SAN_ASAN) INVFS_CPACK_SAN_LEG=all \
	    $(TESTENV) $(TESTISO) $(OUT)/invf-cpack_map_conc_asan
	INVFS_CPACK_SAN_LEG=all $(TESTENV) $(TESTISO) $(OUT)/invf-cpack_map_conc_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-dedupe_v3_test /tmp
	$(TESTENV) $(OUT)/invf-dedupe_symlink_test /srv/bench/scratch
	$(TESTENV) $(TESTISO) $(OUT)/invf-window_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-nlink_v3_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-recipe_fsck_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-fsck_liveness_test /tmp
	$(TESTENV) $(OUT)/invf-cpack_guard_test
	$(TESTENV) $(OUT)/invf-scratch_policy_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-tar_cap_test /tmp
	@# WP201: the two v2-era paths that kept running on Meta-v3. The
	@# containerpack MAP branch's rollback reached v3, where the commit it
	@# rolls back superseded the row IN PLACE -- so it cannot undo anything,
	@# and its v2 retire writes a TOMBSTONE into the shared metadata extent,
	@# which on v3 is the base-page/data pool. Measured: that block was
	@# ALLOCATED, so this is a v2-shaped record written OVER A LIVE v3 BLOCK.
	@# It does NOT destroy the fresh blob -- the v2 records are invisible to
	@# v3 resolution and the row still reads back bit-exact. And vol_v3_free_recipe_blocks
	@# reported success on a recipe it could not parse, so vol_v3_unlink
	@# reported success over blocks that were orphaned forever. Both legs
	@# assert on the DISK EFFECT, not on a return code alone.
	$(TESTENV) $(TESTISO) $(OUT)/invf-v2rb_rollback_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-no_v2_surface_test /tmp
	@# The CKP0 sweep checkpoint is gone (WP drop-ckp0-surface). Its slot
	@# is block 0 [0x220,0x258) -- a DIFFERENT region from the 32 MiB gap
	@# no_v2_surface_test scans, so it needs its own disk scan. The control
	@# cell plants a descriptor in the span and requires the scan to report
	@# it: a detector that has never gone red is not a detector.
	$(TESTENV) $(TESTISO) $(OUT)/invf-no_ckp0_surface_test /tmp
	$(TESTENV) $(TESTISO) $(OUT)/invf-v2_open_test /tmp
	@# WP202: a builtin container lane that supersedes a file on v3 gave the
	@# superseded recipe's data blocks to nobody, while the containerpack
	@# lane released them. One implementation now, and this pins the premise
	@# (the row really moved in place) beside the effect (the blocks are free
	@# AND the file is still bit-exact).
	$(TESTENV) $(TESTISO) $(OUT)/invf-lane_release_test /tmp
	@# WP sweep-rollback-test-conflict: the in-process, BY-ADDRESS form of
	@# tools/test-sweep-publish-rollback.sh's assertion. That suite measures
	@# a net over a whole sweep, which cannot say WHICH blocks went missing;
	@# this one resolves the stranded set to pbas (allocated in the data
	@# region, named by no live recipe), asserts it did not grow at all, and
	@# keeps the original defect's fingerprint as blocks == 17 * segments --
	@# the number handed BACK. Leg 1 requires the sweep to have printed the
	@# rollback at all, so it cannot go green by never entering the state.
	$(TESTENV) $(TESTISO) $(OUT)/invf-sweep_publish_rollback_test /tmp
	# WP: rollback on a volume that holds a symlink. /srv, not /tmp (RAM on
	# this host); the legs create their own dir under it. No permission
	# denial is depended on, so TESTISO's fake root is harmless here.
	$(TESTENV) $(OUT)/invf-rollback_symlink_test /srv/bench/scratch
	@# The v3 KEY ORDERING. The base B+-tree, the delta log and the
	@# fold used to carry three byte-identical private comparators and the
	@# fold's delta/base merge is correct only while they agree. They are
	@# one function now (vol_key_cmp, volume_internal.h); this asserts it
	@# still orders exactly as before, that the merge property is real
	@# (with a control that breaks it on purpose), and that no second
	@# definition has crept back into src/core. No volume, no I/O, so it
	@# cannot be flaky.
	$(TESTENV) $(TESTISO) $(OUT)/invf-keycmp_test
	@# WP pba-ref-v3-incremental: the pba reference map is the sole gate on
	@# every v3 block free, and v3 had no birth/death hook for it. The red
	@# leg (`wrongfree`) is the sequence that freed a live sharer's segment;
	@# `red`/`rednosweep` are the audit's (i)(ii)(iii) with and without the
	@# map-rebuild leg; `hookctl` publishes a second sharer through the
	@# recipe-publish path and unlinks the first.
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp wrongfree
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp hookctl
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp red
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp rednosweep
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp all
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp allnosweep
	@# WP wp/pbaref-skipped-row-is-not-exact: pba_ref_ensure SKIPPED an inode
	@# row it could not read, discarded the walk's status, and set
	@# pba_ref_stale = 0 -- the "this map is exact" flag -- on a map built
	@# from a partial view of the namespace. One unreadable row is a live
	@# reference the map does not hold, and a count that is missing one frees
	@# a block a live recipe still names. `skiprow` fails ONE inode-row read
	@# during the build (src/core/vol_btree.c, via the cross-TU
	@# invfs_vol_btree_fault_reload door) and shows the shared block freed
	@# under the surviving file; `skiprowctl` is the identical sequence with
	@# the fault off, so the damage is measured against the fault and not
	@# against the scenario. The countdown is SEARCHED, not fixed: how many
	@# row reads a build performs is the volume's business, and a hard-coded
	@# n goes stale silently and leaves the leg green on the healthy path.
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp skiprow
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp skiprowctl
	@# WP wp/unlink-takes-map-after-dirent-drop: vol_v3_unlink took the
	@# pba-ref map AFTER it had already dropped the dirent, and the map's
	@# build reaches an inode THROUGH ITS DIRENT (v3_walk_dir ->
	@# vol_v3_path_list_dir + vol_v3_path_lookup), so a rebuild taken after
	@# the delete cannot see the row whose blocks the -1 is about to
	@# subtract: count(P) comes back 1 where a correct map says 2, the -1
	@# reaches 0, and P is freed while the other sharer still names it.
	@# NO FAULT IS INJECTED -- that is the point of this leg. The rebuild
	@# is routine, because pba_ref_stale is set by any recipe publish
	@# (vol_btree.c:3881/3884), so publishing one more file arms it and an
	@# ordinary import is the whole trigger. `unlinkmapctl` is the
	@# identical sequence with nothing published in between, so the map is
	@# still exact, the ensure is a no-op, and the leg measures the
	@# ORDER rather than the scenario.
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp unlinkmap
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp unlinkmapctl
	@# WP wp/unlink-takes-map-after-dirent-drop, second site: the overwrite
	@# victim in vol_v3_rename took the map after ITS OWN dirent was dropped,
	@# with the same wrong comment ("while the row still names
	@# t_in.recipe_addr") and the same wrong free. C renamed onto B's name
	@# retires B, and the rebuild cannot reach B, so the shared segment is
	@# freed while A still names it. The control cannot be "no publish" --
	@# a rename needs its source inode, and creating it is what stales the
	@# map -- so it publishes the same file and then brings the map up to
	@# date by hand: same corpus, same rename, the only variable being
	@# whether the map is fresh or stale at retire time.
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp rename
	$(TESTENV) $(TESTISO) $(OUT)/invf-pbaref_v3_test /tmp renamectl
	@# WP wp/dirs-free-before-publish: vol_v3_create_node freed the existing
	@# inode's blocks BEFORE it republished the row, so each of the four
	@# failure returns between the free and the publish left a LIVE row naming
	@# freed, re-allocatable blocks -- reachable from an ordinary O_TRUNC on
	@# a volume that cannot append a delta record.
	@#
	@# The legs are SEPARATE PROCESSES and `setup` writes the volume each time,
	@# so the arming (setenv at process start, before vol_open) is read on the
	@# FIRST vol_v3_inode_delta_put call in the process -- which is the
	@# truncate's, because nothing else publishes a row in that process. That
	@# is what lets the injector stay `static inline` with no reload helper.
	@# `red` must follow its `setup` and precede any leg that mkfs's afresh;
	@# `hookctl` mkfs's its own volume, so it goes last. `ok` asserts the
	@# pre-existing safe path still SUCCEEDS and still reclaims, which is the
	@# leg a fix that refuses every truncate would fail.
	@#
	@# The scratch dir is $(FRB_T), NOT /tmp, and that is load-bearing:
	# $(TESTISO) mounts a FRESH private tmpfs on /tmp for every single
	# command, so a phase's state cannot survive into the next one. build/ is
	# gitignored, inside the repo (which the wrapper keeps visible), and
	# per-worktree, so two concurrent `make test` runs cannot collide.
	$(TESTENV) $(TESTISO) $(OUT)/invf-dirs_free_before_publish_test $(FRB_T) setup
	$(TESTENV) $(TESTISO) $(OUT)/invf-dirs_free_before_publish_test $(FRB_T) red
	$(TESTENV) $(TESTISO) $(OUT)/invf-dirs_free_before_publish_test $(FRB_T) setup
	$(TESTENV) $(TESTISO) $(OUT)/invf-dirs_free_before_publish_test $(FRB_T) ok
	$(TESTENV) $(TESTISO) $(OUT)/invf-dirs_free_before_publish_test $(FRB_T) hookctl
	$(TESTENV) $(TESTISO) $(OUT)/invf-sibling_retire_v3_test
	@# The container MEMBER BOUND is one number in the engine and eight
	@# mirrored copies in the container packs. Nothing noticed when they
	@# drifted -- a pack left at the old cap just declines every container
	@# over it, for that one container type, silently. Cross-checked here
	@# because a C unit test cannot see the eight sources the packs are
	@# compiled from.
	$(TESTENV) $(TESTISO) bash tools/test-cpack-max-members.sh
	@# WP140: the containerpack lane's COST SHAPE. p7z re-parses the whole
	@# 7z header on every extract, so a lane that exec'd it once per member
	@# was O(n^2) in the header size -- 13+ days at 84k members. The gate is
	@# a COUNT of header parses (the pack's own counter, plus the lane's own
	@# batch-vs-extract call count), never a wall clock: this box is shared
	@# and a timing threshold would flake with no code change. Carries its
	@# own red control -- the per-member extract arm -- and cmp's every
	@# member back against its source, because a parse-once path that
	@# spliced the wrong extents would pass a count assertion.
	$(TESTENV) $(TESTISO) bash tools/test-p7z-batch.sh

	@# The RS parity MATH, standalone: no volume, no v2, no filesystem.
	@# tools/test-seal.sh cannot do this -- the seal is v2-only and v2 is
	@# retired in 0.5.0, so that test SKIPs on every volume that can exist.
	@# Asserts determinism, bit-exact recovery from m erasures, refusal at
	@# m+1, and states the erasure-vs-error-correction boundary.
	$(TESTENV) $(OUT)/invf-rs_stability_test
	@# WP129: the GZR gzip header parse, under ASan+UBSan, against the real
	@# engine function and the generated seed corpus. This gate is what keeps
	@# the 18-byte over-read out; the libFuzzer soak (make gzhdrfuzz-soak) is
	@# the depth behind it, but it needs clang and so cannot be a build-
	@# anywhere requirement.
	$(TESTENV) $(TESTISO) bash tools/run-gzhdr-gate.sh
	@# The same parser again, through the PUBLIC engine entry point, with
	@# the malformed cases AND the well-formed ones. Built with sanitizers
	@# because on main the malformed cases are only a memory error, not a
	@# wrong return value -- an uninstrumented build would pass them. The
	@# accept leg is the one that matters commercially: a bounds check
	@# that started refusing valid gzip would cost compression silently.
	ASAN_OPTIONS=$(GZHDR_TEST_ASAN) $(TESTENV) $(TESTISO) $(OUT)/invf-gz_header_test
	@# WP121: the orphan collector is DEFAULT OFF, so the unit run proves the
	@# gate (subprocess with a clean env) and the on-disk geometry of a
	@# volume that was built, reclaimed and then damaged -- the damage leg
	@# is what a wrong liveness predicate cannot survive.
	$(TESTENV) $(TESTISO) bash tools/test-v3-orphan-reclaim.sh
	@# WP138: invf-import must never DROP a path without naming it. This is
	@# the silent-absence shape -- run as an ordinary user, /etc/shadow and
	@# every other root-only path were skipped, the tool exited 0, and
	@# invf-fsck reported OK on the volume because nothing in it references
	@# what is missing. The suite builds the fixture as the ordinary user
	@# and mode-denies it to itself, so it needs no root and no privileges:
	@# the kernel returns the same EACCES, at the same line, for the same
	@# reason. It asserts BOTH that each unreadable path is absent AND that
	@# it is named, because either half alone passes on the old code.
	@#
	@# NOT $(TESTISO), for the invf-helper_exec_test reason: unshare -r maps
	@# the caller to uid 0 in a namespace whose uid map holds exactly one
	@# uid, so the suite would see a fake root, its fixture's mode-000 files
	@# would be readable, and every leg would pass VACUOUSLY. A suite that
	@# needs to BE the ordinary user whose /etc/shadow is unreadable cannot
	@# be run as an isolation wrapper's fake root. It needs no isolation
	@# either -- a private mktemp -d under /dev/shm and a trap, no fixed
	@# path, nothing in /tmp. If it does land on a real uid 0 it re-execs
	@# itself as nobody via setpriv, and declines loudly if it cannot.
	$(TESTENV) bash tools/test-import-skip-report.sh
	@# The v3 batch REGISTRY is a block owner in its own right, not just the
	@# recipes that point into a batch. When the savepoint reclaim freed a
	@# block the registry still owned, the block went straight back to the
	@# shared free pool and this same sweep's stage-6 tz_v3_gc freed it again
	@# through the row it never dropped -- by then as a live base B+-tree page,
	@# which cost a file its recipe (fuzz seed 0x5e9, image 1, 81 ops). The
	@# window is intra-sweep, so the suite drives the sweep's own prepare and
	@# reads the bitmap; leg 3 is what keeps a veto from passing as a fix.
	$(TESTENV) $(TESTISO) bash tools/test-v3-batch-owner.sh
	@# WP123: the RT30 reader must refuse a root slot whose block the
	@# allocation bitmap reports as free. The suite carries its own red
	@# control so a no-op fix cannot pass it.
	$(TESTENV) $(TESTISO) bash tools/test-v3-rt30-slot-alloc.sh
	@# Two RT30 slots at the same gen are TWO different things: both slots
	@# naming ONE root is what an ordinary rollback produces (clean, and it
	@# used to be reported DAMAGED with exit 3), while two DISTINCT pages
	@# at one gen is a real ambiguous publish and must still be caught. The
	@# negative leg is the one that stops a fix which silences the tiebreak
	@# outright; the red control proves the false positive was real.
	$(TESTENV) $(TESTISO) bash tools/test-v3-rt30-same-root.sh
	@# The ANC0 tail anchor: a second LOCATION for the block-0 descriptors.
	@# The suite carries its own red control (a volume with no anchor must
	@# never have its tail block written), so a no-op cannot pass it.
	$(TESTENV) $(TESTISO) bash tools/test-v3-meta-anchor.sh
	@# A v3 sweep transform that cannot publish its new recipe must ROLL BACK
	@# the segments it wrote, not leave them allocated and unreferenced. The
	@# suite drives the volume to the exact state (a file whose per-segment
	@# remap runs but whose recipe publish has nowhere to go) and asserts
	@# the free-block count does not drop across the sweep; pre-fix it drops
	@# by 17 per orphaned segment and nothing ever gives them back.
	$(TESTENV) $(TESTISO) bash tools/test-sweep-publish-rollback.sh
	$(TESTENV) $(TESTISO) $(OUT)/invf-deflate_repro_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-plugin_host_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-plugin_mt_test
	$(TESTENV) $(TESTISO) $(OUT)/invf-ivpack_packs_test
	$(TESTENV) $(TESTISO) bash tools/test-sweep-ui.sh
	$(TESTENV) $(TESTISO) bash tools/lint-test-heredocs.sh
	$(TESTENV) bash tools/check-repo-hygiene.sh
	@# WP112: the vendored codecpacks (tools/codecpacks/) and the registry
	@# repo's codecpacks/<name>/<version>/ are two copies of the same packs
	@# with nothing cross-checking them; a manifest bug in the registry
	@# (qcow2 `map {in}`, no `decomp_gen`) shipped unnoticed for exactly
	@# that reason. This line is what makes the next one loud. A host with
	@# no registry checkout gets a loud SKIP on stderr, not a silent pass;
	@# `make check-codecpacks` is the strict form.
	$(TESTENV) bash tools/check-codecpack-sync.sh

# repo hygiene is also a standalone gate, for when you do not want a rebuild
check-hygiene:
	$(TESTENV) bash tools/check-repo-hygiene.sh

# WP112: vendored-codecpack / registry drift, strict (no registry = fail).
# Point it at a checkout with INVFS_CODECPACK_REGISTRY=<path>.
check-codecpacks:
	INVFS_CODECPACK_STRICT=1 bash tools/check-codecpack-sync.sh

# e2e tier: tmpfs images under /dev/shm; test-jxl needs cjxl/djxl installed
e2e: all
	$(TESTENV) bash tools/run-e2e.sh tools/test-textzone.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-dedupe.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-heat.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-seal.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-jxl.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-pngflac.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-rawimg.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-binbatch.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-conbatch.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-exercarve.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-containerpack.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-cpack-mcost-bitexact.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-sandbox.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-helper-isolation.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-rawdisk.sh
	@# WP114: the packaging gate. INVFS_PKG_DEB=0 keeps it off the full
	@# compile inside dpkg-buildpackage (that is `make test`'s job, not this
	@# one's); INVFS_PKG_DEB=1 builds the real .deb and inspects it, and is
	@# what a release should be cut with.
	$(TESTENV) INVFS_PKG_DEB=0 bash tools/run-e2e.sh tools/test-packaging.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-ext4fs.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-fatfs.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-xfs.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-ntfs.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-vdi.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-resize.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-rollback.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-watermark.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-usr1-savepoint.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-dynzone.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-imagelock.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-p7z.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-qcow2.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-qcow2-zlib.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-ivpacks.sh
	@# tools/test-sweepboot.sh was written in the WP23 era and never put in
	@# a target or a workflow, so it counted as coverage and ran nowhere.
	@# It is the only gate on `invf-sweep --extract-packs` and on the
	@# self-hosting loop it exists for (the volume carrying the codecpacks
	@# that decompose it), and it needs nothing this tier lacks: no FUSE,
	@# no sudo, no loop device, no network -- it is an offline image suite,
	@# so it belongs in the plain `e2e` list rather than beside `flakey`.
	@# Verified green on its first run under run-e2e.sh (2026-09-30); the
	@# "remains RED on main" note in INCIDENTS.md predates the WP101
	@# `--seal` migration and is stale.
	$(TESTENV) bash tools/run-e2e.sh tools/test-sweepboot.sh
	@# tools/test-gate-c.sh: the ENOSPC tier. Floor breach -> READONLY flip ->
	@# invf-sweep -> space-latch auto-release makes the volume writable again,
	@# two writers racing the reserve, and writes landing while the daemon
	@# sweeps. No external tooling (python3 + fusermount3), so it belongs in
	@# the plain list. It was in no target and no workflow; read end to end
	@# and run under run-e2e.sh before wiring (green on the first run,
	@# 2026-09-30 -- but two of its legs only became assertions in the same
	@# commit; see that commit's message).
	$(TESTENV) bash tools/run-e2e.sh tools/test-gate-c.sh
	@# tools/test-gate-d2.sh: the concurrency tier. Two writers on one file,
	@# ten writers on ten files, and a daemon sweep pass running against a
	@# live appender. Also in no target and no workflow before this; it is a
	@# LOCKED suite (generic /tmp/opencode mountpoint), which run-e2e.sh
	@# serialises on the global lock. Read end to end and run under
	@# run-e2e.sh before wiring -- green, but only after its D2c leg was
	@# given the assertion it claimed to have (see that commit).
	$(TESTENV) bash tools/run-e2e.sh tools/test-gate-d2.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-fuzz.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-writepath.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-acl.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-flushfail.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-sweep-flushfail.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-multidev.sh
	$(TESTENV) bash tools/run-e2e.sh tools/test-mkstemp.sh

# WP22b flakey tier: power-loss / unstable-device soak on dm-flakey over a
# loop device. Standalone on purpose (needs passwordless sudo + dm-flakey,
# takes minutes) — NOT part of `make e2e`. It is a LOCKED suite (sudo +
# losetup + shared /tmp), so it goes through the same runner as `make e2e`
# and serialises on the global lock instead of clobbering shared /dev/shm and
# /tmp. No `sudo -v` warm-up here on purpose: the suite uses `sudo -n`
# throughout and preflights with `sudo -n true` (exit 2 if unavailable),
# whereas `sudo -v` would demand a password and fail without a tty.
#   knobs: FLAKEY_SEED=20260831 FLAKEY_SOAK_S=210 FLAKEY_ONLY=<leg>
flakey:
	$(TESTENV) bash tools/run-e2e.sh tools/test-flakey.sh

# ---- release --------------------------------------------------------------
# Build the host-installer release artifact consumed by packaging/bootstrap.sh
# and by the Arch/Gentoo/Void recipes:
#   dist/invfs-<ver>-<arch>.tar.zst   (bin + codecpacks + packaging tree)
#   dist/SHA256SUMS
#   dist/SHA256SUMS.sig   (detached ed25519, when SIGNING_KEY is given)
# bootstrap.sh downloads all of it and verifies SHA256 before unpack/exec.
#
# WP114: `helpers` is now a prerequisite. The artifact ships the codecpack
# C helpers and packaging/install.sh ships whatever the Makefile built
# (it used to re-derive the compile itself, with a link line that could not
# resolve qcow2's deflate_repro objects, so the install aborted at exit 1 on
# any host with a compiler). Without `helpers` in the artifact there is
# nothing for install.sh to ship and every pack silently degrades.
#
# Signing is opt-in and never silently skipped: without SIGNING_KEY the
# artifact is unsigned and the build says so on stdout, because an unsigned
# release is exactly the thing the author must not publish by accident.
#   make release SIGNING_KEY=/path/to/invfs-signing.ed25519
ARCH ?= $(shell uname -m)
RELEASE_VERSION ?= $(VERSION)
RELEASE_NAME := invfs-$(RELEASE_VERSION)-$(ARCH)
DIST := dist
RELEASE_DIR := $(DIST)/$(RELEASE_NAME)
SIGNING_KEY ?=

release: all helpers
	rm -rf $(RELEASE_DIR) $(DIST)/$(RELEASE_NAME).tar.zst $(DIST)/SHA256SUMS \
	       $(DIST)/SHA256SUMS.sig
	mkdir -p $(RELEASE_DIR)/tools $(RELEASE_DIR)/bin
	@# Ship EXACTLY $(TOOLS), not the whole bin/. `all` also builds ~23 unit
	@# harnesses (invf-anchor_test, invf-delta_test, invf-ivpack_packs_test,
	@# gzhdrfuzz, ...) into bin/, and packaging/install.sh installs
	@# `bin/invf-*` -- so a `cp -a bin` shipped every one of those test
	@# binaries into the user-facing package on all four targets. $(TOOLS) is
	@# this repo's own definition of what ships; use it as the single filter
	@# rather than maintaining an exclusion list (which is how
	@# invf-codec_test/invf-fuzz came to be special-cased before).
	@for t in $(TOOLS); do \
	    install -m755 $(OUT)/$$t $(RELEASE_DIR)/bin/ || exit 1; \
	done
	cp -a packaging $(RELEASE_DIR)/packaging
	@# The target-native RECIPES are not payload -- they are inputs to build a
	@# package, and packaging/install.sh never reads them. Leaving them in
	@# would also make the release a fixed-point problem: each recipe pins
	@# the artifact's digest, and the artifact contains the recipe, so
	@# pasting a digest in would change the digest. What install.sh actually
	@# needs (install.sh, man/, systemd/, dracut/, mkinitcpio/, and the two
	@# debian/invfs.initramfs-* payload files) all stays.
	rm -f $(RELEASE_DIR)/packaging/PKGBUILD \
	      $(RELEASE_DIR)/packaging/invfs.spec
	rm -rf $(RELEASE_DIR)/packaging/gentoo $(RELEASE_DIR)/packaging/void
	rm -f $(RELEASE_DIR)/packaging/debian/rules \
	      $(RELEASE_DIR)/packaging/debian/control \
	      $(RELEASE_DIR)/packaging/debian/changelog \
	      $(RELEASE_DIR)/packaging/debian/copyright \
	      $(RELEASE_DIR)/packaging/debian/install
	cp -a tools/codecpacks $(RELEASE_DIR)/tools/codecpacks
	printf '%s\n' '$(RELEASE_VERSION)' > $(RELEASE_DIR)/VERSION
	@# owner/mtime pinned so a rebuild of the same tree is byte-identical
	tar --sort=name --owner=0 --group=0 --numeric-owner \
	    --mtime='@0' -C $(DIST) -cf - $(RELEASE_NAME) \
		| zstd -q -T0 -19 -f -o $(DIST)/$(RELEASE_NAME).tar.zst
	( cd $(DIST) && sha256sum $(RELEASE_NAME).tar.zst > SHA256SUMS )
	@# detach-sign SHA256SUMS (which pins the artifact) -- never the tarball
	@# directly, so one signature covers every file published under the tag.
	@#
	@# WP114: this block used to print "signed" after an `sq` invocation that
	@# had silently failed (wrong flag names, and `sq sign --signer-file`
	@# wants a SECRET key file, not an armored public one), so a release
	@# could be announced as signed with no .sig on disk at all. The rule now
	@# is: SIGNING_KEY means a signature is produced AND verifies, or the
	@# build fails. Verify-after-sign is the whole point.
	@if [ -z "$(SIGNING_KEY)" ]; then \
	    echo "release: NOTE: UNSIGNED (no SIGNING_KEY= given); do not publish this"; \
	elif command -v gpg >/dev/null 2>&1; then \
	    rm -f $(DIST)/SHA256SUMS.sig; \
	    gpg --batch --yes --local-user "$(SIGNING_KEY)" \
	        --output $(DIST)/SHA256SUMS.sig \
	        --detach-sign $(DIST)/SHA256SUMS || \
	        { echo "release: gpg failed to sign; aborting" >&2; exit 1; }; \
	    gpg --batch --verify $(DIST)/SHA256SUMS.sig $(DIST)/SHA256SUMS >/dev/null 2>&1 || \
	        { echo "release: signature does not verify; aborting (not publishing an unverified sig)" >&2; exit 1; }; \
	    echo "release: signed + verified SHA256SUMS -> $(DIST)/SHA256SUMS.sig"; \
	else \
	    echo "release: SIGNING_KEY set but gpg(1) not found; cannot sign" >&2; \
	    exit 1; \
	fi
	@echo "release: $(DIST)/$(RELEASE_NAME).tar.zst"
	@cat $(DIST)/SHA256SUMS

# The target-native recipes pin the artifact's digest, and pinning it by
# hand is how a release ships with a stale hash that nobody notices until a
# user gets a checksum error. This prints the exact lines to paste after
# each release:
#   make release && make release-sums
# into packaging/PKGBUILD (sha256sums), packaging/gentoo/invfs-<v>.ebuild
# (BLAKE2B, which is what portage wants) and packaging/void/files/ (the
# .sha256 xbps-src checks).
release-sums: release
	@# portage wants BLAKE2B (32-byte digest) + size, which is b2sum -l 256
	@# output -- NOT openssl's blake2b512, which is a different length.
	@s=$$(cut -d' ' -f1 $(DIST)/SHA256SUMS); n=$(RELEASE_NAME); \
	f=$(DIST)/$$n.tar.zst; \
	echo "artifact: $$n"; \
	echo; echo "# packaging/PKGBUILD"; \
	printf "sha256sums=('%s'\n            '  %s')\n" "$$s" "$$n"; \
	echo; echo "# packaging/gentoo/invfs-$(RELEASE_VERSION).ebuild (BLAKE2B)"; \
	if command -v b2sum >/dev/null 2>&1; then \
	    sz=$$(wc -c < "$$f" | tr -d ' '); \
	    printf "BLAKE2B=\"%s\" size=%s\n" "$$(b2sum -l 256 "$$f" | cut -d' ' -f1)" "$$sz"; \
	else \
	    echo "(install coreutils 'b2sum' to print the portage BLAKE2B line)"; \
	fi; \
	echo; echo "# packaging/void/files/$$n.tar.zst.sha256"; \
	echo "$$s  $$n.tar.zst"; \
	echo; echo "(printed, not written: a digest of the tarball cannot live inside"; \
	echo " the tree the tarball is built from -- writing it there would change the"; \
	echo " tarball on the next run and make every release un-reproducible)"

# ---- docs -----------------------------------------------------------------
# ctags index (impl_docs/FUNCTIONS.md, TYPES.md, functions/, types/) plus the
# doxygen HTML browser (impl_docs/doxygen/html, gitignored). Requires doxygen
# (and graphviz for the call graphs).
docs:
	$(TESTENV) bash tools/gen_impl_docs.sh
	doxygen Doxyfile

docs-clean:
	rm -rf impl_docs/doxygen

.PHONY: all clean test e2e fuzz flakey recipefuzz docs docs-clean release
-include $(wildcard $(OBJ)/*.d)

$(OUT)/invf-stats: $(OBJ)/invf-stats.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/invf-stats.o: tools/invf-stats.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
