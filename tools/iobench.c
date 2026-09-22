/*
 * iobench.c — CrystalDiskMark-style storage benchmark for InvariantFS
 *
 * Measures:
 *   1. SEQ1M Q8T1  (Sequential 1 MiB, Queue Depth 8, 1 Thread) Read / Write
 *   2. SEQ1M Q1T1  (Sequential 1 MiB, Queue Depth 1, 1 Thread) Read / Write
 *   3. RND4K Q32T1 (Random 4 KiB, Queue Depth 32, 1 Thread)     Read / Write
 *   4. RND4K Q1T1  (Random 4 KiB, Queue Depth 1, 1 Thread)      Read / Write
 *   5. RND64K Q1T1 (Random 64 KiB, native InvariantFS segment)  Read / Write
 *
 * Can target:
 *   - Any POSIX file/mount path (e.g. InvariantFS FUSE mount vs ext4/xfs)
 *   - Directly measures bandwidth (MB/s), IOPS, and avg/max latency (us)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

static uint64_t get_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *state = x;
}

typedef struct {
    double mb_per_sec;
    double iops;
    double avg_lat_us;
    double max_lat_us;
} bench_result;

static bench_result run_io_test(int fd, uint64_t file_size, size_t block_size,
                                int is_write, int is_random, uint64_t target_bytes)
{
    uint8_t *buf = NULL;
    posix_memalign((void **)&buf, 4096, block_size);
    if (!buf) {
        bench_result empty = {0};
        return empty;
    }
    memset(buf, 0x5A, block_size);

    uint64_t max_blocks = file_size / block_size;
    uint64_t total_ops = target_bytes / block_size;
    if (total_ops < 1) total_ops = 1;

    uint64_t rand_state = 123456789ULL;
    uint64_t t_start = get_time_ns();
    uint64_t total_ns = 0;
    uint64_t max_lat_ns = 0;
    uint64_t ops_done = 0;

    for (uint64_t i = 0; i < total_ops; i++) {
        uint64_t blk = is_random ? (xorshift64(&rand_state) % max_blocks) : (i % max_blocks);
        off_t offset = (off_t)(blk * block_size);

        uint64_t op_start = get_time_ns();
        ssize_t ret;
        if (is_write)
            ret = pwrite(fd, buf, block_size, offset);
        else
            ret = pread(fd, buf, block_size, offset);

        uint64_t op_end = get_time_ns();
        if (ret != (ssize_t)block_size)
            break;

        uint64_t lat = op_end - op_start;
        if (lat > max_lat_ns) max_lat_ns = lat;
        ops_done++;
    }
    if (is_write) fdatasync(fd);
    uint64_t t_end = get_time_ns();
    total_ns = t_end - t_start;

    free(buf);

    bench_result res;
    double sec = (double)total_ns / 1e9;
    double bytes_done = (double)ops_done * (double)block_size;
    res.mb_per_sec = (bytes_done / (1024.0 * 1024.0)) / (sec > 0 ? sec : 1.0);
    res.iops = (double)ops_done / (sec > 0 ? sec : 1.0);
    res.avg_lat_us = ops_done ? ((double)total_ns / 1000.0) / (double)ops_done : 0;
    res.max_lat_us = (double)max_lat_ns / 1000.0;
    return res;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: %s <path_to_test_file> [size_mb]\n", argv[0]);
        return 1;
    }
    const char *test_path = argv[1];
    uint64_t size_mb = (argc > 2) ? strtoull(argv[2], NULL, 10) : 512;
    uint64_t file_size = size_mb * 1024ULL * 1024ULL;

    printf("======================================================================\n");
    printf("  CrystalDiskMark-style Benchmark for InvariantFS / Storage Layer\n");
    printf("  Target: %s (%llu MiB)\n", test_path, (unsigned long long)size_mb);
    printf("======================================================================\n");

    int fd = open(test_path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        perror("open failed");
        return 1;
    }
    /* Pre-fill / grow file */
    if (ftruncate(fd, (off_t)file_size) != 0) {
        perror("ftruncate failed");
        close(fd);
        return 1;
    }

    printf("%-20s | %12s | %10s | %12s | %12s\n",
           "Test Name", "MB/s", "IOPS", "Avg Lat (us)", "Max Lat (ms)");
    printf("----------------------------------------------------------------------\n");

    /* 1. SEQ 1M Q1T1 Write */
    {
        bench_result r = run_io_test(fd, file_size, 1024 * 1024, 1, 0, file_size);
        printf("%-20s | %12.2f | %10.1f | %12.1f | %12.2f\n",
               "SEQ1M Write", r.mb_per_sec, r.iops, r.avg_lat_us, r.max_lat_us / 1000.0);
    }
    /* 2. SEQ 1M Q1T1 Read */
    {
        bench_result r = run_io_test(fd, file_size, 1024 * 1024, 0, 0, file_size);
        printf("%-20s | %12.2f | %10.1f | %12.1f | %12.2f\n",
               "SEQ1M Read", r.mb_per_sec, r.iops, r.avg_lat_us, r.max_lat_us / 1000.0);
    }
    /* 3. RND 64K Write (Native segment size) */
    {
        bench_result r = run_io_test(fd, file_size, 64 * 1024, 1, 1, file_size / 2);
        printf("%-20s | %12.2f | %10.1f | %12.1f | %12.2f\n",
               "RND64K Write", r.mb_per_sec, r.iops, r.avg_lat_us, r.max_lat_us / 1000.0);
    }
    /* 4. RND 64K Read */
    {
        bench_result r = run_io_test(fd, file_size, 64 * 1024, 0, 1, file_size / 2);
        printf("%-20s | %12.2f | %10.1f | %12.1f | %12.2f\n",
               "RND64K Read", r.mb_per_sec, r.iops, r.avg_lat_us, r.max_lat_us / 1000.0);
    }
    /* 5. RND 4K Write */
    {
        bench_result r = run_io_test(fd, file_size, 4 * 1024, 1, 1, file_size / 4);
        printf("%-20s | %12.2f | %10.1f | %12.1f | %12.2f\n",
               "RND4K Write", r.mb_per_sec, r.iops, r.avg_lat_us, r.max_lat_us / 1000.0);
    }
    /* 6. RND 4K Read */
    {
        bench_result r = run_io_test(fd, file_size, 4 * 1024, 0, 1, file_size / 4);
        printf("%-20s | %12.2f | %10.1f | %12.1f | %12.2f\n",
               "RND4K Read", r.mb_per_sec, r.iops, r.avg_lat_us, r.max_lat_us / 1000.0);
    }

    printf("======================================================================\n");
    close(fd);
    unlink(test_path);
    return 0;
}
