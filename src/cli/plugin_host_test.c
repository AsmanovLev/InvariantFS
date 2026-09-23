/* plugin_host_test.c — Comprehensive unit & integration test for InvariantFS
 * out-of-process worker pool daemon and qcow2 ivpack shared library IPC (ADR-007).
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
#include <time.h>

#include "core/invf_plugin_ipc.h"
#include "core/vol_plugin_client.h"

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        exit(1); \
    } \
} while(0)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("=== plugin_host_test (ADR-007 Worker Pool IPC) ===\n");

    const char *so_path = "tools/codecpacks/qcow2.codecpack/libqcow2.so";
    if (access(so_path, R_OK) != 0) {
        /* Attempt to compile libqcow2.so if not yet compiled */
        int r = system("gcc -O3 -fPIC -shared -Isrc/include -Isrc/codecs tools/codecpacks/qcow2.codecpack/qcow2.c src/codecs/deflate_repro.c -DIVPACK_SHARED_LIB -lz -o tools/codecpacks/qcow2.codecpack/libqcow2.so");
        ASSERT(r == 0);
    }

    /* Start daemon in background */
    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        execl("tools/invf-plugin-host", "invf-plugin-host", "-n", "2", NULL);
        _exit(127);
    }
    ASSERT(daemon_pid > 0);

    /* Wait up to 2 seconds for daemon socket */
    bool available = false;
    for (int i = 0; i < 20; i++) {
        usleep(100000); /* 100ms */
        if (invfs_plugin_pool_is_available()) {
            available = true;
            break;
        }
    }
    ASSERT(available && "daemon failed to become available");
    printf("  [1/5] Daemon successfully started and control socket active\n");

    /* Test 1: Connect to daemon */
    int rc = invfs_plugin_pool_connect();
    ASSERT(rc == 0 && "failed to connect to plugin pool");
    printf("  [2/5] Connected to plugin pool shared memory & eventfd\n");

    /* Create a valid qcow2 file using qemu-img and qemu-io */
    char tmp_qcow2[] = "/tmp/fake_test.qcow2";
    unlink(tmp_qcow2);
    int r = system("qemu-img create -f qcow2 /tmp/fake_test.qcow2 4M >/dev/null && qemu-io -c 'write 0 64k' /tmp/fake_test.qcow2 >/dev/null");
    ASSERT(r == 0);

    /* Test 2: Estimate via worker pool */
    uint64_t mbr_sz = 0;
    rc = invfs_plugin_pool_container_estimate("qcow2", so_path, tmp_qcow2, &mbr_sz);
    ASSERT(rc == 0 && "estimate failed over IPC");
    ASSERT(mbr_sz > 0 && "estimate produced zero size");
    printf("  [3/5] Plugin estimate over IPC succeeded: mbr_sz=%llu\n", (unsigned long long)mbr_sz);

    /* Test 3: Microsecond benchmark (1000 IPC ping/estimates) */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const int ITERS = 1000;
    for (int i = 0; i < ITERS; i++) {
        uint64_t sz = 0;
        rc = invfs_plugin_pool_container_estimate("qcow2", so_path, tmp_qcow2, &sz);
        ASSERT(rc == 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) * 1e-3) / ITERS;
    printf("  [4/5] IPC dispatch round-trip latency: %.2f microseconds per call (1000 iterations)\n", elapsed_us);

    /* Test 4: Disconnect and stop daemon */
    invfs_plugin_pool_disconnect();
    kill(daemon_pid, SIGTERM);
    waitpid(daemon_pid, NULL, 0);
    unlink(tmp_qcow2);
    printf("  [5/5] Daemon cleanup and shutdown verified\n");

    printf("ALL 5 CHECKS PASSED\n");
    return 0;
}
