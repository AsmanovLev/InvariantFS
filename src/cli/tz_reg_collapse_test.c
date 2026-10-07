/* tz_reg_collapse_test.c -- registry load refuses instead of reporting
 * empty (P0-2).
 *
 * WHAT THIS IS
 * ------------
 * tz_reg_load used to answer 0 ("empty") when the registry blob failed to
 * read -- and tz_reg_store rewrites the WHOLE blob from the in-memory
 * array, so the next flush sealed this run's batches onto an empty array
 * and published a blob with only them. Every earlier row vanished, their
 * segments orphaned irreversibly (unfreeable, unreclaimable). A failed
 * read is not entitled to assert a negative; the fix returns -1, which
 * every caller already treats as failure.
 *
 * WHY THIS IS NOT A PROBABILITY TEST
 * ----------------------------------
 * Deterministic fixture + deterministic fault. A crafted registry blob
 * (one row, known pba) is published under the owner name; a one-shot
 * engine fault (`tz_reg_blob_read`) fails exactly its blob read. The
 * assertions run through tz_reg_owned_blocks, the real load consumer
 * (spn_reclaim's owner set):
 *
 *   control (intact):  rc 0, one extent, the crafted pba. If the fixture
 *                      cannot even do this, nothing below measures the
 *                      fix -- FAIL, not pass.
 *   faulted read:      rc != 0. Pre-fix: silent 0 with zero extents (the
 *                      masquerade). Post-fix: -1.
 *   bad magic:         crafted blob with wrong magic, rc != 0. Pre-fix:
 *                      silent 0 (torn content reads as empty). Post-fix: -1.
 *   torn count:        crafted blob claiming 5 rows with 1 present,
 *                      rc != 0. Pre-fix: silent 0 with the 1 row clamped
 *                      (the rest silently dropped, then forgotten by the
 *                      next store). Post-fix: -1.
 *   restored:          valid blob republished; rc 0 with the row back
 *                      (fixture discipline: no leg clobbered the row).
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "invarifs.h"
#include "volume_internal.h"   /* vol_create_blob_file, TZ_OWNER_NAME, tz_* */
#include "vol_fault.h"

static int checks, failures;
static char g_img[512];
static invfs_volume *g_v;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL  %s\n", what);
    } else {
        printf("  ok   %s\n", what);
    }
}

static void arm_textzone(const char *spec)
{
    setenv("INVFS_FAULT", spec, 1);
    invfs_vol_textzone_fault_reload();
}
static void disarm(void)
{
    unsetenv("INVFS_FAULT");
    invfs_vol_textzone_fault_reload();
}

/* registry blob: magic LE + n LE + n x {seq u32, algo u32, pba u64,
 * phys u32, pad u32} (24 bytes each -- the engine struct has tail
 * padding to 8, and a short entry is exactly what the torn-count
 * check must catch). So that a row can only come from this blob,
 * the pba is a distinctive constant no allocator hands out here. */
#define VICTIM_PBA ((uint64_t)0xC011A8E0u)
#define TZ_ENT_LEN 24
static void build_blob(uint32_t magic, uint32_t n, int rows,
                       uint8_t *out, size_t *outlen)
{
    uint8_t *p = out;
    uint32_t zero = 0;
    memcpy(p, &magic, 4); p += 4;
    memcpy(p, &n, 4); p += 4;
    for (int i = 0; i < rows; i++) {
        uint32_t seq = 7, algo = 0, phys = 2;
        uint64_t pba = VICTIM_PBA;
        memcpy(p, &seq, 4); p += 4;
        memcpy(p, &algo, 4); p += 4;
        memcpy(p, &pba, 8); p += 8;
        memcpy(p, &phys, 4); p += 4;
        memcpy(p, &zero, 4); p += 4;
    }
    *outlen = (size_t)(p - out);
}

static int publish_registry(uint32_t magic, uint32_t n, int rows)
{
    uint8_t blob[8 + 8 * 24];
    size_t blen = 0;
    build_blob(magic, n, rows, blob, &blen);
    vol_unlink(g_v, TZ_OWNER_NAME);
    return vol_create_blob_file(g_v, TZ_OWNER_NAME, blob, blen, blen,
                                INVFS_ALGO_NONE) != 0;
}

static int owned_blocks(int *rc_out, uint64_t *pba_out, size_t *n_out)
{
    tz_extent *arr = NULL;
    size_t n = 0;
    int rc = tz_reg_owned_blocks(g_v, &arr, &n);
    *rc_out = rc;
    *n_out = n;
    *pba_out = (rc == 0 && n > 0 && arr) ? arr[0].pba : 0;
    free(arr);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    int err = 0, rc;
    uint64_t pba;
    size_t n;

    printf("tz_reg_collapse_test: registry load refuses, never reports empty\n");

    snprintf(g_img, sizeof g_img, "%s/invf-tz-reg-collapse-test.img", dir);
    unlink(g_img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 2>/dev/null",
                 root, g_img);
        if (system(cmd) != 0) {
            printf("  cannot create volume with invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(g_img, &err);
    if (!g_v) {
        printf("  vol_open failed: err=%d\n", err);
        return 2;
    }

    /* control: one crafted row loads fine through the real path */
    ok(publish_registry(0x33565a54u, 1, 1), "setup: valid 1-row registry");
    owned_blocks(&rc, &pba, &n);
    printf("        (control: rc=%d n=%zu pba=%llu)\n", rc, n,
           (unsigned long long)pba);
    {
        /* fixture validation (not the finding): the blob must round-trip
         * through the file layer exactly, or every leg below measures
         * the fixture instead of the load. */
        uint64_t oid = vol_find(g_v, TZ_OWNER_NAME);
        uint8_t *rb = NULL;
        size_t rlen = 0;
        uint8_t want[28];
        size_t wantlen = 0;
        build_blob(0x33565a54u, 1, 1, want, &wantlen);
        ok(oid != 0, "setup: owner row resolves");
        ok(vol_read_file(g_v, oid, &rb, &rlen) == 0 && rb != NULL,
           "setup: owner blob reads");
        ok(rb != NULL && rlen == wantlen && memcmp(rb, want, wantlen) == 0,
           "setup: owner blob round-trips byte-exact");
        free(rb);
        {
            /* second read: the load below re-reads the same blob. */
            uint8_t *rb2 = NULL;
            size_t rlen2 = 0;
            int rrc = vol_read_file(g_v, oid, &rb2, &rlen2);
            printf("        (second read: rc=%d len=%zu)\n", rrc, rlen2);
            free(rb2);
        }
        {
            /* lookup parity: the load uses vol_find_rc, not vol_find. */
            uint64_t rid = 0;
            int frc = vol_find_rc(g_v, TZ_OWNER_NAME, &rid);
            printf("        (find_rc=%d id=%llu)\n", frc,
                   (unsigned long long)rid);
        }
    }
    ok(owned_blocks(&rc, &pba, &n) == 0, "control: owned_blocks ran");
    printf("        (control: rc=%d n=%zu pba=%llu)\n", rc, n,
           (unsigned long long)pba);
    ok(rc == 0 && n == 1 && pba == VICTIM_PBA,
       "control: intact registry loads the row");

    /* red leg 1: faulted blob read must refuse, not report empty */
    arm_textzone("tz_reg_blob_read:1");
    ok(owned_blocks(&rc, &pba, &n) == 0 && rc != 0,
       "faulted read refuses (no silent empty)");
    disarm();
    ok(owned_blocks(&rc, &pba, &n) == 0 && rc == 0 && n == 1 &&
       pba == VICTIM_PBA, "row survives the refused load");

    /* red leg 2: wrong magic is torn, not empty */
    ok(publish_registry(0xDEADBEEFu, 1, 1), "setup: bad-magic blob");
    ok(owned_blocks(&rc, &pba, &n) == 0 && rc != 0,
       "bad magic refuses (no silent empty)");

    /* red leg 3: entry count overrunning the blob is a torn tail */
    ok(publish_registry(0x33565a54u, 5, 1), "setup: overcount blob");
    ok(owned_blocks(&rc, &pba, &n) == 0 && rc != 0,
       "torn count refuses (no silent clamp-and-forget)");

    /* fixture discipline: a valid registry still loads afterwards */
    ok(publish_registry(0x33565a54u, 1, 1), "setup: valid registry again");
    ok(owned_blocks(&rc, &pba, &n) == 0 && rc == 0 && n == 1 &&
       pba == VICTIM_PBA, "restored: row loads after the refusals");

    vol_close(g_v);
    printf("tz_reg_collapse_test: %d checks, %d failures\n",
           checks, failures);
    return failures ? 1 : 0;
}
