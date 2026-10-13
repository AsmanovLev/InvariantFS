/* leg1-rwpdf.c — WP622 leg 1 CO-RE probe (userspace loader).
 *
 * Loads leg1-rwpdf.bpf.o with libbpf, attaches every program
 * (kprobe section names + tracepoints self-attach), samples the
 * histogram maps once a second for -t SECONDS (default 30), then
 * prints per-class vfs latencies and per-opcode FUSE roundtrips:
 * count, mean us, and the log2 bucket distribution.
 *
 * Usage: sudo ./leg1-rwpdf [-t seconds] [-o out.txt]
 * Needs: the .bpf.o beside the binary (tools/ebpf/.build/ after
 * `make -C tools/ebpf`), BTF at /sys/kernel/btf/vmlinux, CAP_BPF.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#define SLOTS 20

struct hist {
	unsigned long long count;
	unsigned long long sum_ns;
	unsigned long long slots[SLOTS];
};

static volatile int stop;

static void on_sig(int s)
{
	(void)s;
	stop = 1;
}

static const char *vfs_class_name(unsigned key)
{
	/* key = cls*2+is_read */
	static const char *cls[] = { "other", "fuse", "ext4", "tmpfs" };
	static char buf[32];
	unsigned c = key / 2, r = key % 2;

	if (c > 3)
		c = 0;
	snprintf(buf, sizeof(buf), "%s/%s", cls[c], r ? "read" : "write");
	return buf;
}

static const char *fuse_op_name(unsigned op)
{
	switch (op) {
	case 1: return "LOOKUP";
	case 3: return "GETATTR";
	case 4: return "SETATTR";
	case 9: return "MKDIR";
	case 10: return "UNLINK";
	case 14: return "OPEN";
	case 15: return "READ";
	case 16: return "WRITE";
	case 20: return "FSYNC";
	case 25: return "FLUSH";
	case 27: return "OPENDIR";
	case 28: return "READDIR";
	case 35: return "CREATE";
	default: return "OTHER";
	}
}

static void print_hist(FILE *f, const char *label, struct hist *h)
{
	int i;

	if (!h->count) {
		fprintf(f, "%-14s n=0\n", label);
		return;
	}
	fprintf(f, "%-14s n=%llu mean_us=%.1f buckets[16ns<<i]:",
		label, h->count, (double)h->sum_ns / h->count / 1000.0);
	for (i = 0; i < SLOTS; i++) {
		if (h->slots[i])
			fprintf(f, " %d:%llu", i, h->slots[i]);
	}
	fprintf(f, "\n");
}

static int dump_map(FILE *f, int fd, const char *title, int is_fuse)
{
	unsigned key, next;
	struct hist h;
	char label[32];

	int first = 1, empty = 1;

	fprintf(f, "=== %s ===\n", title);
	while (bpf_map_get_next_key(fd, first ? NULL : &key, &next) == 0) {
		first = 0;
		key = next;
		if (bpf_map_lookup_elem(fd, &key, &h) == 0) {
			empty = 0;
			if (is_fuse)
				snprintf(label, sizeof(label), "op_%s",
					 fuse_op_name(key));
			else
				snprintf(label, sizeof(label), "%s",
					 vfs_class_name(key));
			print_hist(f, label, &h);
		}
	}
	if (empty)
		fprintf(f, "(empty)\n");
	return 0;
}

int main(int argc, char **argv)
{
	char objpath[1024], *self;
	struct bpf_object *obj = NULL;
	struct bpf_program *prog;
	struct bpf_link *links[32];
	int nlinks = 0, i, opt, vfs_fd = -1, fuse_fd = -1;
	int seconds = 30;
	const char *outpath = NULL;
	FILE *out = stdout;

	while ((opt = getopt(argc, argv, "t:o:")) != -1) {
		if (opt == 't')
			seconds = atoi(optarg);
		else if (opt == 'o')
			outpath = optarg;
	}
	if (outpath) {
		out = fopen(outpath, "w");
		if (!out) {
			perror("fopen");
			return 1;
		}
	}

	self = strrchr(argv[0], '/');
	snprintf(objpath, sizeof(objpath), "%.*sleg1-rwpdf.bpf.o",
		 (int)(self ? self - argv[0] + 1 : 0), argv[0]);
	obj = bpf_object__open_file(objpath, NULL);
	if (libbpf_get_error(obj)) {
		fprintf(stderr, "open %s: %s\n", objpath,
			strerror(-libbpf_get_error(obj)));
		return 1;
	}
	if (bpf_object__load(obj)) {
		fprintf(stderr, "load: %s\n", strerror(errno));
		return 1;
	}
	bpf_object__for_each_program(prog, obj) {
		struct bpf_link *l = bpf_program__attach(prog);

		if (libbpf_get_error(l)) {
			fprintf(stderr, "attach %s: %s\n",
				bpf_program__name(prog),
				strerror(-libbpf_get_error(l)));
			return 1;
		}
		if (nlinks < 32)
			links[nlinks++] = l;
	}
	vfs_fd = bpf_object__find_map_fd_by_name(obj, "vfs_hist");
	fuse_fd = bpf_object__find_map_fd_by_name(obj, "fuse_hist");
	if (vfs_fd < 0 || fuse_fd < 0) {
		fprintf(stderr, "maps missing\n");
		return 1;
	}
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	fprintf(out, "# leg1-rwpdf sampling %d s (Ctrl-C to stop early)\n",
		seconds);
	for (i = 0; i < seconds && !stop; i++)
		sleep(1);
	dump_map(out, vfs_fd, "vfs latency by class", 0);
	dump_map(out, fuse_fd, "FUSE daemon roundtrip by opcode", 1);
	if (out != stdout)
		fclose(out);
	for (i = 0; i < nlinks; i++)
		bpf_link__destroy(links[i]);
	bpf_object__close(obj);
	return 0;
}
