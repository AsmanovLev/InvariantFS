/* concurrency_test.c — WP-M20 lock-free base-read / delta-append test.
 *
 * Exercises the WP-M20 concurrency design:
 *   - Base reads (vol_v3_inode_get) are lock-free: the base is immutable
 *     between folds and the delta is published atomically.
 *   - Delta append is the only writer-critical section (g_delta_append_lock).
 *   - Multiple reader threads hammer the read path while one writer thread
 *     continuously appends delta records and triggers folds.
 *
 * exit 0 = all checks passed, 1 = a check failed, 2 = usage/open error.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

#include "volume_internal.h"
#include "vol_btree.h"
#include "vol_delta.h"

static int checks = 0;
static int failures = 0;
static volatile int g_stop = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

/* ---- key / value encodings (frozen WP-M5/M6 forms) -------------------- */

static void ino_key(uint64_t id, uint8_t k[8])
{
    int i;
    for (i = 0; i < 8; i++)
        k[i] = (uint8_t)(id >> (56 - 8 * i));
}

static uint16_t ino_row(uint8_t *buf, uint16_t mode, uint64_t size)
{
    invfs_v3_inode_row r;
    memset(&r, 0, sizeof r);
    r.row_version = INVFS_V3_INODE_ROW_VERSION;
    r.type = INVFS_ITYP_REG;
    r.mode = mode;
    r.uid = 1000;
    r.gid = 1000;
    r.nlink = 1;
    r.size = size;
    r.mtime = (int64_t)time(NULL);
    r.atime = r.mtime;
    memcpy(buf, &r, sizeof r);
    return (uint16_t)sizeof r;
}

/* ---- volume ------------------------------------------------------------ */

static invfs_volume *g_v;

static int delta_put_inode(uint64_t id, uint16_t mode, uint64_t size)
{
    uint8_t k[8], v[INVFS_V3_INODE_ROW_FIXED];
    uint16_t vl;
    ino_key(id, k);
    vl = ino_row(v, mode, size);
    return vol_delta_append(g_v, k, sizeof k, v, vl, 0);
}

static int delta_del_inode(uint64_t id)
{
    uint8_t k[8];
    ino_key(id, k);
    return vol_delta_append(g_v, k, sizeof k, NULL, 0,
                            INVFS_DELTA_FLAG_DELETE);
}

/* ---- reader thread ---------------------------------------------------- */

static void *reader_thread(void *arg)
{
    (void)arg;
    uint64_t ops = 0;
    while (!g_stop) {
        invfs_v3_inode in;
        int r = vol_v3_inode_get(g_v, 42, &in);
        (void)r;
        ops++;
    }
    printf("  reader did %llu ops\n", (unsigned long long)ops);
    return NULL;
}

/* ---- writer thread ---------------------------------------------------- */

static void *writer_thread(void *arg)
{
    (void)arg;
    uint64_t fold_iter = 0;
    uint64_t ops = 0;
    uint64_t ino_counter = 1000;
    while (!g_stop) {
        uint16_t mode = (uint16_t)(0644 | (fold_iter & 1 ? 0100 : 0));
        uint64_t size = 100 + fold_iter;
        if (delta_put_inode(42, mode, size) == 0) {
            ops++;
        }
        if (delta_put_inode(ino_counter, 0644, 50) == 0) {
            ops++;
            ino_counter++;
        }
        if (ino_counter > 1010) {
            delta_del_inode(ino_counter - 5);
            ops++;
        }
        if (fold_iter > 0 && fold_iter % 20 == 0) {
            vol_v3_fold(g_v);
            ops++;
        }
        fold_iter++;
    }
    printf("  writer did %llu ops (%llu folds)\n",
           (unsigned long long)ops, (unsigned long long)(fold_iter / 20));
    return NULL;
}

/* ---- main ------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    pthread_t readers[4];
    pthread_t writer;
    int i;

    printf("concurrency tests (WP-M20): lock-free base reads + delta append\n");

    snprintf(img, sizeof img, "%s/invf-concurrency_test.img", dir);

    /* build a minimal v3 volume using mkfs */
    {
        char cmd[1024];
        snprintf(cmd, sizeof cmd,
                 "%s/bin/invf-mkfs %s 16 2>/dev/null",
                 getenv("PWD") ? getenv("PWD") : ".", img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }

    {
        int err = 0;
        g_v = vol_open(img, &err);
        if (!g_v) {
            fprintf(stderr, "concurrency_test: vol_open(%s) failed: err=%d\n",
                    img, err);
            return 2;
        }
    }

    /* seed base inodes */
    {
        invfs_v3_inode in;
        memset(&in, 0, sizeof in);
        in.type = INVFS_ITYP_REG;
        in.mode = 0644;
        in.uid = in.gid = 1000;
        in.nlink = 1;
        in.size = 100;
        in.mtime = in.atime = (int64_t)time(NULL);
        ok(vol_v3_inode_put(g_v, 42, &in) == 0, "base put inode 42");
        memset(&in, 0, sizeof in);
        in.type = INVFS_ITYP_REG;
        in.mode = 0644;
        in.uid = in.gid = 1000;
        in.nlink = 1;
        in.size = 200;
        in.mtime = in.atime = (int64_t)time(NULL);
        ok(vol_v3_inode_put(g_v, 43, &in) == 0, "base put inode 43");
    }

    /* verify pre-state */
    {
        invfs_v3_inode in;
        ok(vol_v3_inode_get(g_v, 42, &in) == 1 && in.mode == 0644 && in.size == 100,
           "pre-state: inode 42 visible with correct mode/size");
        ok(vol_v3_inode_get(g_v, 43, &in) == 1 && in.mode == 0644 && in.size == 200,
           "pre-state: inode 43 visible with correct mode/size");
        ok(vol_v3_inode_get(g_v, 44, &in) == 0,
           "pre-state: inode 44 not visible (not yet in delta)");
    }

    /* spawn reader threads */
    for (i = 0; i < 4; i++) {
        if (pthread_create(&readers[i], NULL, reader_thread, NULL) != 0) {
            printf("  cannot create reader thread %d\n", i);
            g_stop = 1;
            break;
        }
    }

    /* spawn writer thread */
    if (pthread_create(&writer, NULL, writer_thread, NULL) != 0) {
        printf("  cannot create writer thread\n");
        g_stop = 1;
    }

    printf("  running concurrency test for 3 seconds...\n");
    sleep(3);
    g_stop = 1;

    /* join all threads */
    for (i = 0; i < 4; i++)
        pthread_join(readers[i], NULL);
    pthread_join(writer, NULL);

    /* verify post-state */
    {
        invfs_v3_inode in;
        int r42 = vol_v3_inode_get(g_v, 42, &in);
        ok(r42 == 1, "post-state: inode 42 still readable");
        if (r42 == 1) {
            ok(in.mode != 0, "post-state: inode 42 has valid mode");
            ok(in.size != 0, "post-state: inode 42 has valid size");
        }
        ok(vol_v3_inode_get(g_v, 43, &in) == 1,
           "post-state: inode 43 still readable");
    }

    vol_close(g_v);
    g_v = NULL;
    remove(img);

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
