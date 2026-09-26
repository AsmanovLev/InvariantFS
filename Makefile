# InvariantFS — native Linux build (incremental).
# This Makefile is the ONLY build system: every object list derives from
# $(CORE), so a new core module cannot silently go missing. `make` builds
# everything, `make invf-fuse` one target.
CC      ?= gcc
SRC     := src
OUT     := bin
OBJ     := build/obj
VERSION := $(shell git describe --always --tags 2>/dev/null | sed 's/-.*//' || echo "unknown")
BUILD_DATE := $(shell date '+%Y-%m-%d')
AUTHOR_NAME := Lev_Asmanov
AUTHOR_EMAIL := asmanovlev@gmail.com
LICENSE := GPL-2.0-only

# Sources live in per-role subdirs under $(SRC); objects stay flat in $(OBJ)
# (basenames are unique across subdirs, and tools/test-*.sh link lines use
# $(OBJ)/<basename>.o).
SRCDIRS := $(SRC)/core $(SRC)/codecs $(SRC)/recipes $(SRC)/cli $(SRC)/vendor7z $(SRC)/legacy
vpath %.c $(SRCDIRS)

CFLAGS  := -std=gnu11 -O2 -MMD -MP -I$(SRC) $(addprefix -I,$(SRCDIRS)) -pthread \
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

CORE    := volume vol_cpack helper_exec vol_plugin_client vol_png vol_seal vol_repair vol_rollback \
           vol_resize vol_fsck vol_crash vol_exer vol_dedupe vol_textzone \
            vol_heat vol_sweep vol_read vol_write vol_records vol_ast \
            vol_dirs vol_tier vol_meta_merge vol_metabuf vol_btree vol_delta \
            vol_fold vol_reclaim vol_spt0 \
            arc crc32c lz4 flacx tarx pngx blkio miniz blake3 blake3_dispatch blake3_portable ppmd8 ppmd8enc ppmd8dec ppmd_codec codec bcj_x86 rs deflate_repro \
            deflate_backend_system deflate_backend_stock
CORE_O  := $(addprefix $(OBJ)/,$(addsuffix .o,$(CORE))) $(STOCK_ZLIB_O)
B3      := blake3 blake3_dispatch blake3_portable

# Canonical core object list for the e2e helper link lines in tools/test-*.sh.
# Emitted by `all` so it can never go stale against $(CORE). Kept as a list of
# individual .o files (NOT a .a archive) so link order does not matter.
CORE_OBJS_FILE := build/core_objs.txt

TOOLS   := invf-mkfs invf-verify invf-fsck invf-cp invf-cat invf-ls invf-stat \
           invf-zip invf-arctest invf-blkio_test invf-fuse invf-import invf-sweep meta_probe \
           invf-stats invf-resize invf-rollback invf-l2ptest invfs-pack \
           invf-v3inode invf-plugin-host

all: $(TOOLS:%=$(OUT)/%) $(CORE_OBJS_FILE)

$(CORE_OBJS_FILE): $(CORE_O) | $(OBJ)
	@printf '%s\n' $(CORE_O) > $@

$(OBJ):
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
$(OUT)/invf-$(1): $$(OBJ)/$(1).o $(CORE_O)
	$$(CC) $$(CFLAGS) -o $$@ $$< $(CORE_O) $$(LDLIBS) $$(2)
endef

# CLI tools (main in src/cli/<name>.c)
CLI_MAINS := mkfs verify fsck cp cat ls stat arctest blkio_test resize \
             metabuf_test btree_test btree_repair_test v3inode overlay_test fold_test concurrency_test \
             sweep_v3_test symlink_v3_test large_file_v3_test dedupe_v3_test deflate_repro_test \
             plugin_host_test plugin_mt_test
$(foreach t,$(CLI_MAINS),$(eval $(call TOOL_RULE,$(t),)))

# WP71: loads every containerpack .so through dlmopen/dlopen -> needs -ldl,
# and resolves tools/codecpacks/... relative to the repo root.
$(eval $(call TOOL_RULE,ivpack_packs_test,-ldl))

# WP60: invfs-pack is named differently (invfs- not invf-)
$(OUT)/invfs-pack: $(OBJ)/pack.o $(CORE_O)
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

$(OUT)/invf-l2ptest: $(OBJ)/l2ptest.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/l2ptest.o: tools/l2ptest.c | $(OBJ)
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

define PLUGIN_SO_RULE
$(call PACKDIR,$(1))/lib$(1).so: $(call PACKDIR,$(1))/$(1).c $(SRC)/include/ivpack_api.h $(SRC)/include/ivpack_impl.h $(PLUGIN_EXTRA_$(1))
	$(CC) $(if $(PLUGIN_CFLAGS_$(1)),$(PLUGIN_CFLAGS_$(1)),$(PLUGIN_CFLAGS)) -o $$@ $$< $(PLUGIN_EXTRA_$(1)) -lz -ldl
endef
$(foreach p,$(CPACKS),$(eval $(call PLUGIN_SO_RULE,$(p))))

PLUGIN_SO := $(foreach p,$(CPACKS),$(call PACKDIR,$(p))/lib$(p).so)
plugin-so: $(PLUGIN_SO)

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

# .ivpack bundles (ADR-007 §3: uncompressed ZIP-0, manifest + sha256 +
# lib/<name>.so + bin/<name> CLI fallback). Artifacts land in dist/ivpack/.
IVPACKS := $(foreach p,$(CPACKS),dist/ivpack/$(p).ivpack)
ivpacks: plugin-so $(IVPACKS)
dist/ivpack/%.ivpack: $(PLUGIN_SO) | dist/ivpack
	bash tools/pack-ivpack.sh tools/codecpacks/$*.codecpack $@
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
	       $(OUT)/invf-fuzz

# ---- tests ---------------------------------------------------------------
# unit tier: fast, no I/O images
$(OUT)/invf-codec_test: $(OBJ)/codec_test.o $(OBJ)/codec.o $(OBJ)/ppmd8.o $(OBJ)/ppmd8enc.o $(OBJ)/ppmd8dec.o $(OBJ)/ppmd_codec.o $(OBJ)/lz4.o $(OBJ)/bcj_x86.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# WP61: unit coverage for the shared helper containment launcher.
$(OUT)/invf-helper_exec_test: $(OBJ)/helper_exec_test.o $(OBJ)/helper_exec.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# WP-M10: delta-log unit harness + offline e2e driver (tools/, not src/cli).
$(OUT)/invf-delta_test: $(OBJ)/delta_test.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/delta_test.o: tools/delta_test.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

# fuzz tier: on-demand property/fuzz harness for the pure/parsing layers.
# NOT part of `make test` -- `make fuzz` only builds it, run it by hand:
#   bin/invf-fuzz [iterations] [seed]
FUZZ_O := $(OBJ)/codec.o $(OBJ)/ppmd8.o $(OBJ)/ppmd8enc.o $(OBJ)/ppmd8dec.o \
          $(OBJ)/ppmd_codec.o $(OBJ)/lz4.o $(OBJ)/bcj_x86.o
$(OUT)/invf-fuzz: $(OBJ)/fuzz_invfs.o $(FUZZ_O)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

fuzz: $(OUT)/invf-fuzz

# CI target: 10k iterations (faster than fuzz's default 100k)
fuzz-ci: $(OUT)/invf-fuzz
	$(OUT)/invf-fuzz 10000 0x1CF51EE5

test: $(OUT)/invf-arctest $(OUT)/invf-blkio_test $(OUT)/invf-codec_test \
      $(OUT)/invf-helper_exec_test $(OUT)/invf-metabuf_test $(OUT)/invf-btree_test \
      $(OUT)/invf-delta_test $(OUT)/invf-concurrency_test $(OUT)/invf-sweep_v3_test \
      $(OUT)/invf-btree_repair_test \
      $(OUT)/invf-symlink_v3_test $(OUT)/invf-large_file_v3_test $(OUT)/invf-dedupe_v3_test \
      $(OUT)/invf-deflate_repro_test $(OUT)/invf-plugin_host_test $(OUT)/invf-plugin_mt_test \
      $(OUT)/invf-ivpack_packs_test $(OUT)/invf-mkfs $(OUT)/invf-cp \
      $(OUT)/invf-sweep plugin-so $(CORE_OBJS_FILE)
	$(OUT)/invf-arctest
	$(OUT)/invf-blkio_test
	$(OUT)/invf-codec_test
	$(OUT)/invf-helper_exec_test
	$(OUT)/invf-metabuf_test
	$(OUT)/invf-btree_test
	$(OUT)/invf-delta_test
	$(OUT)/invf-concurrency_test /tmp
	$(OUT)/invf-sweep_v3_test /tmp
	$(OUT)/invf-btree_repair_test /tmp
	$(OUT)/invf-symlink_v3_test /tmp
	$(OUT)/invf-large_file_v3_test /tmp
	$(OUT)/invf-dedupe_v3_test /tmp
	$(OUT)/invf-deflate_repro_test
	$(OUT)/invf-plugin_host_test
	$(OUT)/invf-plugin_mt_test
	$(OUT)/invf-ivpack_packs_test
	bash tools/test-sweep-ui.sh

# e2e tier: tmpfs images under /dev/shm; test-jxl needs cjxl/djxl installed
e2e: all
	bash tools/run-e2e.sh tools/test-textzone.sh
	bash tools/run-e2e.sh tools/test-dedupe.sh
	bash tools/run-e2e.sh tools/test-heat.sh
	bash tools/run-e2e.sh tools/test-seal.sh
	bash tools/run-e2e.sh tools/test-jxl.sh
	bash tools/run-e2e.sh tools/test-pngflac.sh
	bash tools/run-e2e.sh tools/test-rawimg.sh
	bash tools/run-e2e.sh tools/test-binbatch.sh
	bash tools/run-e2e.sh tools/test-conbatch.sh
	bash tools/run-e2e.sh tools/test-exercarve.sh
	bash tools/run-e2e.sh tools/test-containerpack.sh
	bash tools/run-e2e.sh tools/test-sandbox.sh
	bash tools/run-e2e.sh tools/test-helper-isolation.sh
	bash tools/run-e2e.sh tools/test-rawdisk.sh
	bash tools/run-e2e.sh tools/test-ext4fs.sh
	bash tools/run-e2e.sh tools/test-fatfs.sh
	bash tools/run-e2e.sh tools/test-xfs.sh
	bash tools/run-e2e.sh tools/test-ntfs.sh
	bash tools/run-e2e.sh tools/test-vdi.sh
	bash tools/run-e2e.sh tools/test-resize.sh
	bash tools/run-e2e.sh tools/test-rollback.sh
	bash tools/run-e2e.sh tools/test-watermark.sh
	bash tools/run-e2e.sh tools/test-dynzone.sh
	bash tools/run-e2e.sh tools/test-rocp.sh
	bash tools/run-e2e.sh tools/test-p7z.sh
	bash tools/run-e2e.sh tools/test-qcow2.sh
	bash tools/run-e2e.sh tools/test-ivpacks.sh
	bash tools/run-e2e.sh tools/test-fuzz.sh
	bash tools/run-e2e.sh tools/test-writepath.sh
	bash tools/run-e2e.sh tools/test-acl.sh
	bash tools/run-e2e.sh tools/test-flushfail.sh
	bash tools/run-e2e.sh tools/test-multidev.sh
	bash tools/run-e2e.sh tools/test-mkstemp.sh

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
	bash tools/run-e2e.sh tools/test-flakey.sh

# ---- release --------------------------------------------------------------
# Build the host-installer release artifact consumed by packaging/bootstrap.sh:
#   dist/invfs-<ver>-<arch>.tar.zst   (bin + codecpacks + packaging tree)
#   dist/SHA256SUMS
# bootstrap.sh downloads both and verifies SHA256 before unpack/exec.
ARCH ?= $(shell uname -m)
RELEASE_VERSION ?= $(VERSION)
RELEASE_NAME := invfs-$(RELEASE_VERSION)-$(ARCH)
DIST := dist
RELEASE_DIR := $(DIST)/$(RELEASE_NAME)

release: all
	rm -rf $(RELEASE_DIR) $(DIST)/$(RELEASE_NAME).tar.zst $(DIST)/SHA256SUMS
	mkdir -p $(RELEASE_DIR)/tools
	cp -a bin $(RELEASE_DIR)/bin
	rm -f $(RELEASE_DIR)/bin/invf-codec_test $(RELEASE_DIR)/bin/invf-fuzz
	cp -a packaging $(RELEASE_DIR)/packaging
	cp -a tools/codecpacks $(RELEASE_DIR)/tools/codecpacks
	printf '%s\n' '$(RELEASE_VERSION)' > $(RELEASE_DIR)/VERSION
	tar -C $(DIST) -cf - $(RELEASE_NAME) \
		| zstd -q -T0 -19 -f -o $(DIST)/$(RELEASE_NAME).tar.zst
	( cd $(DIST) && sha256sum $(RELEASE_NAME).tar.zst > SHA256SUMS )
	@echo "release: $(DIST)/$(RELEASE_NAME).tar.zst"
	@cat $(DIST)/SHA256SUMS

# ---- docs -----------------------------------------------------------------
# ctags index (impl_docs/FUNCTIONS.md, TYPES.md, functions/, types/) plus the
# doxygen HTML browser (impl_docs/doxygen/html, gitignored). Requires doxygen
# (and graphviz for the call graphs).
docs:
	bash tools/gen_impl_docs.sh
	doxygen Doxyfile

docs-clean:
	rm -rf impl_docs/doxygen

.PHONY: all clean test e2e fuzz flakey docs docs-clean release
-include $(wildcard $(OBJ)/*.d)

$(OUT)/invf-stats: $(OBJ)/invf-stats.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/invf-stats.o: tools/invf-stats.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<
