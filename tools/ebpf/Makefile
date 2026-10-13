# tools/ebpf/Makefile — build the WP622 CO-RE userspace/BPF pair.
#
# The bpftrace legs (leg1-rw.bt .. leg4-pressure.bt) need no build:
# bpftrace resolves BTF on the host at attach time. Only the
# standalone leg1 CO-RE tool is compiled here:
#
#   make -C tools/ebpf          # vmlinux.h + leg1-rwpdf.bpf.o + leg1-rwpdf
#   make -C tools/ebpf check    # smoke: BTF present, objects built
#   make -C tools/ebpf clean
#
# Requires (Debian): bpftool, clang, llvm, libbpf-dev.
# See README.md for the one-line install.
#
# Build outputs live in .build/ (git-ignored); nothing compiled is
# ever committed.

B ?= .build
BPFTOOL ?= $(shell command -v bpftool 2>/dev/null || echo /sbin/bpftool)
CLANG ?= clang
CC ?= cc

BPF_OBJ = $(B)/leg1-rwpdf.bpf.o
BIN = $(B)/leg1-rwpdf
VMLINUX = $(B)/vmlinux.h

all: $(BIN)

$(B):
	mkdir -p $(B)

$(VMLINUX): | $(B)
	$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@.tmp
	mv $@.tmp $@

$(BPF_OBJ): leg1-rwpdf.bpf.c $(VMLINUX) | $(B)
	$(CLANG) -target bpf -O2 -g -D__TARGET_ARCH_x86 \
		-I$(B) -I. -c $< -o $@

# the loader finds leg1-rwpdf.bpf.o beside argv[0], i.e. in $(B)/
$(BIN): leg1-rwpdf.c $(BPF_OBJ)
	$(CC) -O2 -Wall -o $@ leg1-rwpdf.c -lbpf -lelf -lz

check: all
	test -f /sys/kernel/btf/vmlinux
	file $(BPF_OBJ) | grep -q BPF
	test -x $(BIN)

clean:
	rm -rf $(B)

.PHONY: all check clean
