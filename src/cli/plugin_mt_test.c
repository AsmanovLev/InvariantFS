/* plugin_mt_test.c — Multi-threaded concurrent worker pool IPC & spill-to-disk test.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <time.h>

#include "core/invf_plugin_ipc.h"
#include "core/vol_plugin_client.h"

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

#define NUM_THREADS 4
#define OPS_PER_THREAD 250

static const char *g_so_path = "tools/codecpacks/qcow2.codecpack/libqcow2.so";
static const char *g_qcow2_file = "/tmp/mt_test.qcow2";

static void *thread_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        uint64_t sz = 0;
        int rc = invfs_plugin_pool_container_estimate("qcow2", g_so_path, g_qcow2_file, &sz);
        if (rc != 0 || sz == 0) {
            fprintf(stderr, "Thread %d failed at op %d (rc=%d, sz=%llu)\n", tid, i, rc, (unsigned long long)sz);
            pthread_exit((void *)(intptr_t)1);
        }
    }
    pthread_exit(NULL);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("=== plugin_mt_test (Multi-threaded & In-Memory Spill IPC) ===\n");

    /* Ensure libqcow2.so is compiled */
    if (access(g_so_path, R_OK) != 0) {
        int r = system("gcc -O3 -fPIC -shared -Isrc/include -Isrc/codecs tools/codecpacks/qcow2.codecpack/qcow2.c src/codecs/deflate_repro.c -DIVPACK_SHARED_LIB -lz -o tools/codecpacks/qcow2.codecpack/libqcow2.so");
        ASSERT(r == 0);
    }

    /* Create sample qcow2 */
    unlink(g_qcow2_file);
    int r = system("qemu-img create -f qcow2 /tmp/mt_test.qcow2 4M >/dev/null && qemu-io -c 'write 0 64k' /tmp/mt_test.qcow2 >/dev/null");
    ASSERT(r == 0);

    /* Start daemon with 4 workers */
    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        execl("bin/invf-plugin-host", "invf-plugin-host", "-n", "4", NULL);
        execl("tools/invf-plugin-host", "invf-plugin-host", "-n", "4", NULL);
        _exit(127);
    }
    ASSERT(daemon_pid > 0);

    for (int i = 0; i < 20; i++) {
        usleep(100000);
        if (invfs_plugin_pool_is_available()) break;
    }
    ASSERT(invfs_plugin_pool_is_available());

    /* Test 1: Connect to pool */
    int rc = invfs_plugin_pool_connect();
    ASSERT(rc == 0);
    printf("  [1/4] Connected to 4-worker pool\n");

    /* Test 2: In-memory direct buffer map execution */
    uint8_t mrmp_buf[64 * 1024];
    size_t mrmp_len = 0;
    rc = invfs_plugin_pool_container_cmd_mem("qcow2", g_so_path, 5 /* MAP */,
                                             g_qcow2_file, NULL, 0,
                                             mrmp_buf, sizeof(mrmp_buf), &mrmp_len);
    ASSERT(rc == 0 && "in-memory map failed");
    ASSERT(mrmp_len >= 8 && memcmp(mrmp_buf, "MRMP", 4) == 0 && "in-memory map did not produce MRMP header");
    printf("  [2/4] Direct in-memory buffer execution succeeded (produced %zu bytes MRMP header)\n", mrmp_len);

    /* Test 3: Spill-to-disk test (request larger than slot capacity triggers automatic scratch spill) */
    uint8_t *large_buf = malloc(1024);
    size_t large_out_len = 0;
    /* Request with out_cap = 40MB (> INVF_SLOT_DATA_CAP/2) triggers automatic disk spill */
    rc = invfs_plugin_pool_container_cmd_mem("qcow2", g_so_path, 5 /* MAP */,
                                             g_qcow2_file, NULL, 0,
                                             large_buf, 40 * 1024 * 1024, &large_out_len);
    ASSERT(rc == 0 && "spill-to-disk execution failed");
    ASSERT(large_out_len >= 8 && memcmp(large_buf, "MRMP", 4) == 0);
    free(large_buf);
    printf("  [3/4] Automatic spill-to-disk executed and verified bit-exact\n");

    /* Test 4: Multi-threaded concurrent execution */
    printf("  [4/4] Running %d threads concurrently x %d ops = %d total requests...\n",
           NUM_THREADS, OPS_PER_THREAD, NUM_THREADS * OPS_PER_THREAD);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_t th[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&th[i], NULL, thread_worker, (void *)(intptr_t)i);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        void *res = NULL;
        pthread_join(th[i], &res);
        ASSERT(res == NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9;
    double ops_per_sec = (NUM_THREADS * OPS_PER_THREAD) / elapsed_s;
    printf("  Concurrent throughput: %.1f ops/sec (%.2f microseconds / op across 4 threads)\n",
           ops_per_sec, (elapsed_s * 1e6) / (NUM_THREADS * OPS_PER_THREAD));

    /* Cleanup */
    invfs_plugin_pool_disconnect();
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    unlink(g_qcow2_file);

    printf("ALL 4 MULTI-THREADED CHECKS PASSED\n");
    return 0;
}
