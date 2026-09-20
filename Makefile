# InvariantFS — native Linux build (incremental).
# Mirrors tools/build_native.sh flags; `make` builds everything, `make invf-fuse` one target.
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
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3)
FUSE_LIBS   := $(shell pkg-config --libs fuse3)

CORE    := volume vol_cpack helper_exec vol_png vol_seal vol_repair vol_rollback \
           vol_resize vol_fsck vol_crash vol_exer vol_dedupe vol_textzone \
           vol_heat vol_sweep vol_read vol_write vol_records vol_ast \
           vol_dirs vol_tier vol_meta_merge vol_metabuf vol_btree vol_delta \
           vol_fold vol_reclaim vol_spt0 \
           arc crc32c lz4 flacx tarx pngx blkio miniz blake3 blake3_dispatch blake3_portable ppmd8 ppmd8enc ppmd8dec ppmd_codec codec bcj_x86 rs
CORE_O  := $(addprefix $(OBJ)/,$(addsuffix .o,$(CORE)))
B3      := blake3 blake3_dispatch blake3_portable

TOOLS   := invf-mkfs invf-verify invf-fsck invf-cp invf-cat invf-ls invf-stat \
           invf-zip invf-arctest invf-blkio_test invf-fuse invf-import invf-sweep meta_probe \
           invf-stats invf-resize invf-rollback invf-l2ptest invf-migrate-v2 invfs-pack \
           invf-v3inode

all: $(TOOLS:%=$(OUT)/%)

$(OBJ):
	mkdir -p $@

$(OBJ)/%.o: %.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

# tools that embed their own miniz copy must not double-link ours:
# zip.c includes miniz internally, so it links without $(OBJ)/miniz.o
MINIZLESS := $(filter-out miniz.o,$(notdir $(CORE_O)))

define TOOL_RULE
$(OUT)/invf-$(1): $$(OBJ)/$(1).o $(CORE_O)
	$$(CC) $$(CFLAGS) -o $$@ $$< $(CORE_O) $$(LDLIBS) $$(2)
endef

# CLI tools (main in src/cli/<name>.c)
CLI_MAINS := mkfs verify fsck cp cat ls stat arctest blkio_test resize migrate-v2 \
             metabuf_test btree_test v3inode overlay_test fold_test
$(foreach t,$(CLI_MAINS),$(eval $(call TOOL_RULE,$(t),)))

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

$(OUT)/meta_probe: $(OBJ)/meta_probe.o $(CORE_O)
	$(CC) $(CFLAGS) -Itools -o $@ $< $(CORE_O) $(LDLIBS)
$(OBJ)/meta_probe.o: tools/meta_probe.c | $(OBJ)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJ) $(TOOLS:%=$(OUT)/%) $(OUT)/invf-codec_test \
	       $(OUT)/invf-helper_exec_test $(OUT)/invf-metabuf_test \
	       $(OUT)/invf-btree_test $(OUT)/invf-delta_test \
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
      $(OUT)/invf-delta_test
	$(OUT)/invf-arctest
	$(OUT)/invf-blkio_test
	$(OUT)/invf-codec_test
	$(OUT)/invf-helper_exec_test
	$(OUT)/invf-metabuf_test
	$(OUT)/invf-btree_test
	$(OUT)/invf-delta_test

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
	bash tools/run-e2e.sh tools/test-fuzz.sh
	bash tools/run-e2e.sh tools/test-writepath.sh
	bash tools/run-e2e.sh tools/test-acl.sh
	bash tools/run-e2e.sh tools/test-astv2.sh
	bash tools/run-e2e.sh tools/test-flushfail.sh
	bash tools/run-e2e.sh tools/test-compact.sh
	bash tools/run-e2e.sh tools/test-multidev.sh
	bash tools/run-e2e.sh tools/test-migrate-v2.sh
	bash tools/run-e2e.sh tools/test-mkstemp.sh
	bash tools/run-e2e.sh tools/test-meta-extent.sh
	bash tools/run-e2e.sh tools/test-meta-extent-walk.sh
	bash tools/run-e2e.sh tools/test-meta-overflow.sh

# WP22b flakey tier: power-loss / unstable-device soak on dm-flakey over a
# loop device. Standalone on purpose (needs passwordless sudo + dm-flakey,
# takes minutes) — NOT part of `make e2e`. The script is sudo-aware; run
# `sudo -v` first if the credential cache may be cold.
#   knobs: FLAKEY_SEED=20260831 FLAKEY_SOAK_S=210 FLAKEY_ONLY=<leg>
flakey:
	bash tools/test-flakey.sh

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
