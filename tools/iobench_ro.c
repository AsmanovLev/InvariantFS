/*
 * iobench_ro.c — Pure read-only CrystalDiskMark-style benchmark for InvariantFS and Squashfs.
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

static bench_result run_ro_test(int fd, uint64_t file_size, size_t block_size,
                                int is_random, uint64_t target_bytes)
{
    uint8_t *buf = NULL;
    posix_memalign((void **)&buf, 4096, block_size);
    if (!buf) {
        bench_result empty = {0};
        return empty;
    }

    uint64_t max_blocks = file_size / block_size;
    if (max_blocks == 0) {
        free(buf);
        bench_result empty = {0};
        return empty;
    }
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
        ssize_t ret = pread(fd, buf, block_size, offset);
        uint64_t op_end = get_time_ns();
        if (ret != (ssize_t)block_size)
            break;

        uint64_t lat = op_end - op_start;
        if (lat > max_lat_ns) max_lat_ns = lat;
        ops_done++;
    }
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
        printf("Usage: %s <path_to_existing_file>\n", argv[0]);
        return 1;
    }
    const char *test_path = argv[1];
    int fd = open(test_path, O_RDONLY);
    if (fd < 0) {
        perror("open failed");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        perror("fstat failed");
        close(fd);
        return 1;
    }
    uint64_t file_size = (uint64_t)st.st_size;
    uint64_t test_volume = (file_size < (256ULL << 20)) ? file_size : (256ULL << 20);

    printf("======================================================================\n");
    printf("  CrystalDiskMark READ Benchmark\n");
    printf("  Target: %s (Size: %llu MiB, Test Volume: %llu MiB)\n",
           test_path, (unsigned long long)(file_size / (1024*1024)),
           (unsigned long long)(test_volume / (1024*1024)));
    printf("======================================================================\n");
    printf("%-20s | %12s | %10s | %12s | %12s\n",
           "Test Name", "MB/s", "IOPS", "Avg Lat (us)", "Max Lat (ms)");
    printf("----------------------------------------------------------------------\n");

    /* 1. SEQ 1M */
    {
        bench_result r = run_ro_test(fd, file_size, 1024 * 1024, 0, test_volume);
        printf("%-20s | %12.2f | %10.1f | %12.1f | %12.2f\n",
               "SEQ1M Read", r.mb_per_sec, r.iops, r.avg_lat_us, r.max_lat_us / 1000.0);
    }
    /* 2. RND 64K */
    {
        bench_result r = run_ro_test(fd, file_size, 64 * 1024, 1, test_volume / 2);
        printf("%-20s | %12.2f | %10.1f | %12.1f | %12.2f\n",
               "RND64K Read", r.mb_per_sec, r.iops, r.avg_lat_us, r.max_lat_us / 1000.0);
    }
    /* 3. RND 4K */
    {
        bench_result r = run_ro_test(fd, file_size, 4 * 1024, 1, test_volume / 4);
        printf("%-20s | %12.2f | %10.1f | %12.1f | %12.2f\n",
               "RND4K Read", r.mb_per_sec, r.iops, r.avg_lat_us, r.max_lat_us / 1000.0);
    }
    printf("======================================================================\n");
    close(fd);
    return 0;
}
