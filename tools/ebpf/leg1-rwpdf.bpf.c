/* leg1-rwpdf.bpf.c — WP622 leg 1 CO-RE probe (BPF side).
 *
 * Attaches with plain kprobes + tracepoints; all kernel-structure
 * access goes through CO-RE relocations (BPF_CORE_READ) against
 * vmlinux.h generated from the host BTF, so the same object runs on
 * any kernel with the probed symbols (no kernel headers needed).
 *
 * Maps:
 *   start      HASH  tid -> {ts, class, is_read} (in-flight vfs calls)
 *   fuse_start HASH  req unique -> {ts, opcode}  (in-flight FUSE reqs)
 *   vfs_hist   HASH  class*2+is_read -> {count, sum_ns, slots[20]}
 *   fuse_hist  HASH  opcode -> {count, sum_ns, slots[20]}
 * Buckets are log2 from a 16 ns floor: slot i covers
 * [16<<i, 16<<(i+1)) ns, slot 19 is the overflow.
 *
 * Classes: 0 other, 1 fuse (0x65735546), 2 ext4 (0xEF53),
 * 3 tmpfs (0x01021994).
 *
 * Build: make -C tools/ebpf leg1-rwpdf
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

#define SLOTS 20

struct inflight {
	__u64 ts;
	__u32 cls;
	__u32 is_read;
};

struct fuse_inflight {
	__u64 ts;
};

struct hist {
	__u64 count;
	__u64 sum_ns;
	__u64 slots[SLOTS];
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 8192);
	__type(key, __u32);
	__type(value, struct inflight);
} start SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 32768);
	__type(key, __u64);
	__type(value, struct fuse_inflight);
} fuse_start SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 32);
	__type(key, __u32);
	__type(value, struct hist);
} vfs_hist SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 64);
	__type(key, __u32);
	__type(value, struct hist);
} fuse_hist SEC(".maps");

static __always_inline __u32 sb_class(struct file *f)
{
	__u32 magic = 0;

	/* CO-RE chain: file -> f_inode -> i_sb -> s_magic */
	BPF_CORE_READ_INTO(&magic, f, f_inode, i_sb, s_magic);
	if (magic == 0x65735546)
		return 1;
	if (magic == 0xEF53)
		return 2;
	if (magic == 0x01021994)
		return 3;
	return 0;
}

static __always_inline int bucket(__u64 dt)
{
	int i;

#pragma unroll
	for (i = 0; i < SLOTS; i++) {
		if (dt < (16ULL << i))
			return i;
	}
	return SLOTS - 1;
}

static __always_inline void hist_add(void *map, __u32 key, __u64 dt)
{
	struct hist *h;
	int b;

	h = bpf_map_lookup_elem(map, &key);
	if (!h) {
		struct hist z = {};

		bpf_map_update_elem(map, &key, &z, BPF_NOEXIST);
		h = bpf_map_lookup_elem(map, &key);
		if (!h)
			return;
	}
	b = bucket(dt);
	__sync_fetch_and_add(&h->count, 1);
	__sync_fetch_and_add(&h->sum_ns, dt);
	__sync_fetch_and_add(&h->slots[b], 1);
}

static __always_inline int vfs_enter(struct file *f, __u32 is_read)
{
	__u32 tid = (__u32)bpf_get_current_pid_tgid();
	struct inflight e = {};

	e.ts = bpf_ktime_get_ns();
	e.cls = sb_class(f);
	e.is_read = is_read;
	bpf_map_update_elem(&start, &tid, &e, BPF_ANY);
	return 0;
}

static __always_inline int vfs_exit(void)
{
	__u32 tid = (__u32)bpf_get_current_pid_tgid();
	struct inflight *e;
	__u64 dt;
	__u32 key;

	e = bpf_map_lookup_elem(&start, &tid);
	if (!e)
		return 0;
	dt = bpf_ktime_get_ns() - e->ts;
	key = e->cls * 2 + (e->is_read & 1);
	hist_add(&vfs_hist, key, dt);
	bpf_map_delete_elem(&start, &tid);
	return 0;
}

SEC("kprobe/vfs_read")
int BPF_KPROBE(vfs_read_enter, struct file *file)
{
	return vfs_enter(file, 1);
}

SEC("kprobe/vfs_write")
int BPF_KPROBE(vfs_write_enter, struct file *file)
{
	return vfs_enter(file, 0);
}

SEC("kretprobe/vfs_read")
int BPF_KRETPROBE(vfs_read_exit)
{
	return vfs_exit();
}

SEC("kretprobe/vfs_write")
int BPF_KRETPROBE(vfs_write_exit)
{
	return vfs_exit();
}

/* FUSE join is by request pointer, not by tracepoint unique: the
 * fuse_request_send trace event fires BEFORE the unique is assigned
 * (fuse_send_one traces, then send_req assigns), so its unique field
 * is always 0 and the tracepoint pair is unjoinable. Instead:
 * kretprobe fuse_get_req (retval = new req) to kprobe
 * fuse_request_end (single arg = req). Opcode via CO-RE through
 * struct fuse_req -> in -> h -> opcode (u32, stable).
 */
SEC("kretprobe/fuse_get_req")
int BPF_KRETPROBE(fuse_get_alloc)
{
	__u64 req = (__u64)PT_REGS_RC(ctx);
	struct fuse_inflight e = {};

	if (!req)
		return 0;
	e.ts = bpf_ktime_get_ns();
	bpf_map_update_elem(&fuse_start, &req, &e, BPF_ANY);
	return 0;
}

SEC("kprobe/fuse_request_end")
int BPF_KPROBE(fuse_end, unsigned long long req_ptr)
{
	struct fuse_req *req = (struct fuse_req *)req_ptr;
	struct fuse_inflight *e;
	__u64 key = req_ptr;
	__u32 op = 0;
	__u64 dt;

	e = bpf_map_lookup_elem(&fuse_start, &key);
	if (!e)
		return 0;
	/* req->in.h crosses anonymous struct wrappers, which the
	 * multi-hop BPF_CORE_READ_INTO macro cannot chain through;
	 * a single preserve_access_index expression records the same
	 * CO-RE relocations for the whole path. */
	bpf_probe_read_kernel(&op, sizeof(op),
		(const void *)__builtin_preserve_access_index(&req->in.h.opcode));
	dt = bpf_ktime_get_ns() - e->ts;
	hist_add(&fuse_hist, op, dt);
	bpf_map_delete_elem(&fuse_start, &key);
	return 0;
}

char LICENSE[] SEC("license") = "GPL";
