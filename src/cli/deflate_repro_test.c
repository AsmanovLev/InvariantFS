/* deflate_repro_test.c — unit test for universal deflate reproduction helper.
 *
 * Verifies:
 *   1. Decompression of raw and qcow2 (-12) deflate streams.
 *   2. Parameter discovery across standard zlib levels (1, 6, 9) and memLevels.
 *   3. Discovery of QCOW2-style (-12 windowBits) cluster compression parameters.
 *   4. Re-encoding matches target stream bit-for-bit.
 *   5. Robust rejection of non-matching or arbitrary streams.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#include "deflate_repro.h"

static int checks = 0;
static int failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        fprintf(stderr, "  FAIL  %s\n", what);
    } else {
        printf("  OK    %s\n", what);
    }
}

/* Helper to generate synthetic test data with mixed patterns */
static void make_test_data(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t s = seed ? seed : 0x12345678;
    for (size_t i = 0; i < len; i++) {
        s = s * 1103515245 + 12345;
        if ((i % 64) < 32)
            buf[i] = (uint8_t)((i / 64) & 0xFF);  /* repeated runs */
        else
            buf[i] = (uint8_t)(s >> 16);          /* pseudo-random */
    }
}

int main(void)
{
    printf("deflate_repro_test: universal deflate reproduction & parameter search\n");

    const size_t raw_len = 65536; /* standard 64 KiB cluster */
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return 2;
    make_test_data(raw, raw_len, 42);

    /* 1. Test raw deflate (-15) with standard levels: 6, 1, 9 */
    static const int test_levels[] = { 6, 1, 9, 4 };
    for (size_t i = 0; i < sizeof(test_levels)/sizeof(test_levels[0]); i++) {
        int lv = test_levels[i];
        invfs_deflate_params synth_params = {
            .engine = INVFS_DEFLATE_ENGINE_ZLIB,
            .level = (int8_t)lv,
            .mem_level = 8,
            .strategy = Z_DEFAULT_STRATEGY,
            .window_bits = -15
        };
        uint8_t *synth_stream = NULL;
        size_t synth_len = 0;
        int rc = invfs_deflate_repro_encode(raw, raw_len, &synth_params, &synth_stream, &synth_len);
        ok(rc == 0 && synth_stream != NULL && synth_len > 0, "synthesize deflate stream");

        /* Decompress and verify content match */
        uint8_t *dec = NULL;
        size_t dec_len = 0;
        rc = invfs_deflate_decompress(synth_stream, synth_len, -15, &dec, &dec_len);
        ok(rc == 0 && dec_len == raw_len && memcmp(dec, raw, raw_len) == 0,
           "invfs_deflate_decompress recovers original payload bit-exact");
        free(dec);

        /* Parameter search */
        invfs_deflate_params found_params;
        memset(&found_params, 0, sizeof found_params);
        rc = invfs_deflate_repro_find(raw, raw_len, synth_stream, synth_len, -15, &found_params);
        ok(rc == 0, "invfs_deflate_repro_find discovers matching parameters");
        ok(found_params.level == lv && found_params.mem_level == 8,
           "discovered level and mem_level match expected");

        /* Re-encode with discovered parameters and verify byte-for-byte identity */
        uint8_t *re_stream = NULL;
        size_t re_len = 0;
        rc = invfs_deflate_repro_encode(raw, raw_len, &found_params, &re_stream, &re_len);
        ok(rc == 0 && re_len == synth_len && memcmp(re_stream, synth_stream, synth_len) == 0,
           "re-encoded stream matches original target stream bit-for-bit");

        free(synth_stream);
        free(re_stream);
    }

    /* 2. Test QCOW2-style (-12 window_bits) compressed cluster */
    {
        invfs_deflate_params qcow_params = {
            .engine = INVFS_DEFLATE_ENGINE_ZLIB,
            .level = 6,
            .mem_level = 8,
            .strategy = Z_DEFAULT_STRATEGY,
            .window_bits = -12
        };
        uint8_t *qcow_stream = NULL;
        size_t qcow_len = 0;
        int rc = invfs_deflate_repro_encode(raw, raw_len, &qcow_params, &qcow_stream, &qcow_len);
        ok(rc == 0 && qcow_stream != NULL, "encode QCOW2-style (-12) cluster");

        uint8_t *dec = NULL;
        size_t dec_len = 0;
        rc = invfs_deflate_decompress(qcow_stream, qcow_len, -12, &dec, &dec_len);
        ok(rc == 0 && dec_len == raw_len && memcmp(dec, raw, raw_len) == 0,
           "decompress QCOW2 cluster bit-exact");
        free(dec);

        invfs_deflate_params found_qcow;
        memset(&found_qcow, 0, sizeof found_qcow);
        rc = invfs_deflate_repro_find(raw, raw_len, qcow_stream, qcow_len, -12, &found_qcow);
        ok(rc == 0 && found_qcow.window_bits == -12,
           "discover QCOW2 (-12) cluster parameters");

        uint8_t *re_qcow = NULL;
        size_t re_qcow_len = 0;
        rc = invfs_deflate_repro_encode(raw, raw_len, &found_qcow, &re_qcow, &re_qcow_len);
        ok(rc == 0 && re_qcow_len == qcow_len && memcmp(re_qcow, qcow_stream, qcow_len) == 0,
           "re-encode QCOW2 cluster is 100% bit-exact");

        free(qcow_stream);
        free(re_qcow);
    }

    /* 3. Bundled stock zlib backend is selectable and detectable. */
    {
        invfs_deflate_params stock_params = {
            .engine = INVFS_DEFLATE_ENGINE_ZLIB_STOCK,
            .level = 6,
            .mem_level = 9,
            .strategy = Z_DEFAULT_STRATEGY,
            .window_bits = -12
        };
        invfs_deflate_params found_stock;
        uint8_t *stock_stream = NULL;
        uint8_t *stock_again = NULL;
        size_t stock_len = 0, stock_again_len = 0;
        int rc = invfs_deflate_repro_encode(raw, raw_len, &stock_params,
                                           &stock_stream, &stock_len);
        ok(rc == 0 && stock_stream != NULL,
           "encode with bundled stock zlib backend");
        memset(&found_stock, 0, sizeof found_stock);
        rc = invfs_deflate_repro_find(raw, raw_len, stock_stream, stock_len,
                                      -12, &found_stock);
        ok(rc == 0 && found_stock.engine == INVFS_DEFLATE_ENGINE_ZLIB_STOCK,
           "detect bundled stock zlib encoder");
        rc = invfs_deflate_repro_encode(raw, raw_len, &found_stock,
                                        &stock_again, &stock_again_len);
        ok(rc == 0 && stock_again_len == stock_len &&
           memcmp(stock_again, stock_stream, stock_len) == 0,
           "re-encode with detected stock backend");
        free(stock_stream);
        free(stock_again);
    }

    /* 4. Negative test: arbitrary stream rejection */
    {
        uint8_t garbage[128];
        memset(garbage, 0xA5, sizeof garbage);
        invfs_deflate_params rejected_params;
        int rc = invfs_deflate_repro_find(raw, raw_len, garbage, sizeof garbage, -15, &rejected_params);
        ok(rc == -1, "invfs_deflate_repro_find gracefully rejects non-matching stream");
    }

    free(raw);
    printf("\ndeflate_repro_test summary: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
