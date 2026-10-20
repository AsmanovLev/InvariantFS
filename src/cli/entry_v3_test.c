/*
 * entry_v3_test.c — WP639 recipe entry v3: layout, CRC32C, dual-stride.
 *
 * What is under test
 * ------------------
 * The 32-bit entry word is full (zone:2 + algo:6 + block_id:24), so a wider
 * entry is a new 40-byte stride, not a wider bitfield. The on-disk v3 image
 * is assembled arithmetically (shifts/masks + LE memcpy), never by memcping
 * a struct -- so the bytes do not depend on toolchain bitfield layout. This
 * test pins that contract:
 *
 *   A. stride/version table (1/2 -> 32, 3 -> 40, anything else -> 0/refuse).
 *   B. byte-exact layout of a packed entry (all fields but the CRC, which
 *      belongs to crc32c_test's pin, not this file's).
 *   C. pack -> verify -> unpack round-trip at the representable maxima, and
 *      the fail-closed unpack of wide values the 32 B in-memory shape cannot
 *      hold (zone > 3, algo > 63, block_id > 2^24-1).
 *   D. tamper in every field region (off/len/flags/boff/crc/pba) fails
 *      verify; a set reserved bit fails verify.
 *   E. serialize_win under INVFS_RECIPE_FORCE_V3=1 emits a v3 header with
 *      40 B entries; without it the writer stays v1/v2 byte-identical.
 *   F. parse of a v3 blob verifies every entry, converts to the 32 B shape,
 *      and refuses truncated/unknown-version blobs; a flipped entry byte
 *      fails the parse (loudly, never a misread).
 *   G. boundaries: 0 entries, single-blob edges, chunk-stride edges
 *      (76.8 entries per 3072-chunk must round-trip, not align).
 *   H. trailing window table behind 40 B entries parses (stride-aware).
 *
 * What is NOT under test here (e2e legs live in tools/test-entry-v3.sh):
 * mounting, sweeping, or fscking a forced-v3 volume -- this binary never
 * touches a volume. CRC32C itself is pinned by src/cli/crc32c_test.c.
 *
 * exit 0 = pass, 1 = failure, 2 = setup error.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "volume_internal.h"

static int checks = 0, failures = 0;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
    else       printf("  OK    %s\n", what);
}

int main(void)
{
    /* ---- LEG A: stride table ---- */
    ok(invfs_ast_entry_stride(INVFS_AST_VERSION_V1) == 32,
       "A: v1 stride is 32");
    ok(invfs_ast_entry_stride(INVFS_AST_VERSION_V2) == 32,
       "A: v2 stride is 32");
    ok(invfs_ast_entry_stride(INVFS_AST_VERSION_V3) == 40,
       "A: v3 stride is 40");
    ok(invfs_ast_entry_stride(0) == 0 && invfs_ast_entry_stride(4) == 0 &&
       invfs_ast_entry_stride(0xEEEEEEEEu) == 0,
       "A: unknown versions stride 0 (refuse)");
    ok(sizeof(invfs_ast_block_entry) == 32 &&
       sizeof(invfs_ast_block_entry_v3) == 40,
       "A: in-memory strides locked (32 / 40)");

    /* ---- LEG B: byte-exact packed layout (CRC field excluded) ---- */
    {
        uint8_t img[40];
        uint64_t flags;
        /* zone=1, algo=25 (NBLI), block_id=0xABCDEF:
         * flags = 1 | 25<<6 | 0xABCDEF<<16 = 0xABCDEF0641 */
        invfs_ast_entry_v3_pack(img, 0x0102030405060708ull,
                                0x1112131415161718ull, 1, 25, 0xABCDEFull,
                                0x11223344u, 0x2122232425262728ull);
        ok(img[0] == 0x08 && img[7] == 0x01,
           "B: file_offset little-endian");
        ok(img[8] == 0x18 && img[15] == 0x11,
           "B: length little-endian");
        memcpy(&flags, img + 16, 8);
        ok(flags == 0xABCDEF0641ull,
           "B: flags word is zone|algo<<6|block_id<<16, rsv zero");
        ok(img[48 / 8 + 16] == 0 && img[23] == 0,
           "B: reserved high bits are zero");
        ok(memcmp(img + 24, "\x44\x33\x22\x11", 4) == 0,
           "B: block_offset little-endian");
        ok(img[32] == 0x28 && img[39] == 0x21,
           "B: pba little-endian");
        ok(invfs_ast_entry_v3_verify(img, 40) == 0,
           "B: fresh pack verifies");
    }

    /* ---- LEG C: round-trip at representable maxima; fail closed above ---- */
    {
        uint8_t img[40];
        invfs_ast_block_entry e;
        memset(&e, 0, sizeof e);
        invfs_ast_entry_v3_pack(img, 0xFFFFFFFFFFFFFFFFull,
                                0xFFFFFFFFFFFFFFFFull, 3, 63, 0xFFFFFFu,
                                0xFFFFFFFFu, 0xFFFFFFFFFFFFFFFFull);
        ok(invfs_ast_entry_v3_verify(img, 40) == 0,
           "C: max representable pack verifies");
        ok(invfs_ast_entry_v3_unpack(img, &e) == 0 &&
           e.file_offset == 0xFFFFFFFFFFFFFFFFull &&
           e.length == 0xFFFFFFFFFFFFFFFFull &&
           e.zone == 3 && e.algo == 63 && e.block_id == 0xFFFFFFu &&
           e.block_offset == 0xFFFFFFFFu &&
           e.pba == 0xFFFFFFFFFFFFFFFFull,
           "C: max representable unpacks bit-exact");
        /* wide values verify (the image is well-formed) but do not unpack
         * into the 32 B in-memory shape: in-memory widening arrives with
         * the stream-cap raise, never before it. */
        invfs_ast_entry_v3_pack(img, 0, 0, 4, 0, 0, 0, 0);
        ok(invfs_ast_entry_v3_verify(img, 40) == 0 &&
           invfs_ast_entry_v3_unpack(img, &e) != 0,
           "C: zone 4 verifies but refuses unpack (fail closed)");
        invfs_ast_entry_v3_pack(img, 0, 0, 0, 64, 0, 0, 0);
        ok(invfs_ast_entry_v3_verify(img, 40) == 0 &&
           invfs_ast_entry_v3_unpack(img, &e) != 0,
           "C: algo 64 verifies but refuses unpack (fail closed)");
        invfs_ast_entry_v3_pack(img, 0, 0, 0, 0, 0x1000000u, 0, 0);
        ok(invfs_ast_entry_v3_verify(img, 40) == 0 &&
           invfs_ast_entry_v3_unpack(img, &e) != 0,
           "C: block_id 2^24 verifies but refuses unpack (fail closed)");
    }

    /* ---- LEG D: tamper anywhere fails verify ---- */
    {
        static const size_t spots[] = {0, 7, 8, 15, 16, 23, 24, 27, 28, 31,
                                       32, 39};
        size_t i;
        int all_fail = 1;
        for (i = 0; i < sizeof spots / sizeof spots[0]; i++) {
            uint8_t img[40];
            invfs_ast_entry_v3_pack(img, 100, 200, 1, 2, 300, 400, 500);
            img[spots[i]] ^= 0x01;
            if (invfs_ast_entry_v3_verify(img, 40) == 0)
                all_fail = 0;
        }
        ok(all_fail, "D: a 1-bit flip in any field region fails verify");
        {
            uint8_t img[40];
            invfs_ast_entry_v3_pack(img, 100, 200, 1, 2, 300, 400, 500);
            img[22] ^= 0x80;   /* rsv bit (flags bit 48+7): must be zero */
            ok(invfs_ast_entry_v3_verify(img, 40) != 0,
               "D: a set reserved bit fails verify");
        }
        ok(invfs_ast_entry_v3_verify(NULL, 40) != 0 &&
           invfs_ast_entry_v3_verify((const uint8_t *)"", 39) != 0,
           "D: NULL/truncated input fails verify");
    }

    /* ---- LEG E: forced v3 serialize; unforced stays v1/v2 ---- */
    {
        invfs_ast_block_entry ents[3];
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents_out = NULL;
        size_t nents = 0;
        uint32_t ver = 0;
        memset(ents, 0, sizeof ents);
        ents[0].file_offset = 0; ents[0].length = 65536;
        ents[0].zone = 0; ents[0].algo = 1; ents[0].block_id = 0;
        ents[0].block_offset = 0; ents[0].pba = 777;
        ents[1].file_offset = 65536; ents[1].length = 65536;
        ents[1].zone = 0; ents[1].algo = 1; ents[1].block_id = 1;
        ents[1].block_offset = 0; ents[1].pba = 778;
        ents[2].file_offset = 131072; ents[2].length = 100;
        ents[2].zone = 0; ents[2].algo = 0; ents[2].block_id = 2;
        ents[2].block_offset = 0; ents[2].pba = 779;
        setenv("INVFS_RECIPE_FORCE_V3", "1", 1);
        ok(vol_ast_recipe_serialize(196708ull, ents, 3, &blob, &blen) == 0,
           "E: forced-v3 serialize succeeds");
        memcpy(&ver, blob, 4);
        ok(ver == INVFS_AST_VERSION_V3 && blen == 24 + 3 * 40,
           "E: forced blob is v3 (24 B header + 3x40 B entries)");
        ok(vol_ast_recipe_parse(blob, blen, &ah, &ents_out, &nents) == 0 &&
           ah.version == INVFS_AST_VERSION_V3 && nents == 3 && ents_out &&
           ents_out[0].pba == 777 && ents_out[1].pba == 778 &&
           ents_out[2].length == 100 && ents_out[2].block_id == 2,
           "E: forced-v3 blob parses with verified, converted entries");
        vol_ast_recipe_parsed_free(&ah, ents_out);
        free(blob);
        blob = NULL;
        unsetenv("INVFS_RECIPE_FORCE_V3");
        ok(vol_ast_recipe_serialize(196708ull, ents, 3, &blob, &blen) == 0,
           "E: unforced serialize succeeds");
        memcpy(&ver, blob, 4);
        ok(ver != INVFS_AST_VERSION_V3 && blen == 16 + 3 * 32,
           "E: unforced output stays v1 byte-shape (no v3 without need)");
        ok(vol_ast_recipe_parse(blob, blen, &ah, &ents_out, &nents) == 0 &&
           nents == 3 && ents_out && ents_out[1].pba == 778,
           "E: unforced blob parses as before");
        vol_ast_recipe_parsed_free(&ah, ents_out);
        free(blob);
    }

    /* ---- LEG F: v3 parse refuses corruption ---- */
    {
        invfs_ast_block_entry ents[2];
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents_out = NULL;
        size_t nents = 0;
        memset(ents, 0, sizeof ents);
        ents[0].pba = 11; ents[0].length = 65536;
        ents[1].file_offset = 65536; ents[1].length = 65536;
        ents[1].block_id = 1; ents[1].pba = 12;
        setenv("INVFS_RECIPE_FORCE_V3", "1", 1);
        ok(vol_ast_recipe_serialize(131072ull, ents, 2, &blob, &blen) == 0,
           "F: forced-v3 fixture serializes");
        unsetenv("INVFS_RECIPE_FORCE_V3");
        blob[24 + 40] ^= 0x04;   /* flip a length bit of entry 1 */
        ok(vol_ast_recipe_parse(blob, blen, &ah, &ents_out, &nents) != 0,
           "F: a flipped entry byte fails the parse (no misread)");
        blob[24 + 40] ^= 0x04;   /* restore */
        ok(vol_ast_recipe_parse(blob, blen, &ah, &ents_out, &nents) == 0 &&
           nents == 2,
           "F: restored blob parses again");
        vol_ast_recipe_parsed_free(&ah, ents_out);
        ents_out = NULL;
        ok(vol_ast_recipe_parse(blob, blen - 1, &ah, &ents_out, &nents) != 0,
           "F: a truncated v3 blob fails the parse");
        ok(vol_ast_recipe_parse(blob, 10, &ah, &ents_out, &nents) != 0,
           "F: a header-truncated blob fails the parse");
        blob[0] = 0x04; blob[1] = 0x00; blob[2] = 0x00; blob[3] = 0x00;
        ok(vol_ast_recipe_parse(blob, blen, &ah, &ents_out, &nents) != 0,
           "F: unknown version 4 fails the parse");
        free(blob);
    }

    /* ---- LEG G: boundaries (0 entries; chunk-stride edges) ---- */
    {
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents_out = NULL;
        size_t nents = 99;
        setenv("INVFS_RECIPE_FORCE_V3", "1", 1);
        ok(vol_ast_recipe_serialize(0ull, NULL, 0, &blob, &blen) == 0 &&
           blen == 24,
           "G: 0-entry v3 blob is header-only");
        ok(vol_ast_recipe_parse(blob, blen, &ah, &ents_out, &nents) == 0 &&
           nents == 0,
           "G: 0-entry v3 blob parses (no entries, no failure)");
        vol_ast_recipe_parsed_free(&ah, ents_out);
        free(blob);
        unsetenv("INVFS_RECIPE_FORCE_V3");
        /* 76 full 40 B entries + a 32-byte tail: the RMC 3072-chunk splits
         * mid-entry (76.8 entries/chunk). Chunking is byte-based, so this
         * must round-trip -- and the day it does not, this leg names it. */
        {
            invfs_ast_block_entry *ents;
            const size_t n = 77;
            size_t i;
            ents = (invfs_ast_block_entry *)calloc(n, sizeof *ents);
            ok(ents != NULL, "G: 77-entry fixture allocates");
            for (i = 0; i < n && ents; i++) {
                ents[i].file_offset = (uint64_t)i * 65536ull;
                ents[i].length = 65536;
                ents[i].block_id = (uint32_t)i;
                ents[i].pba = 1000 + (uint64_t)i;
            }
            setenv("INVFS_RECIPE_FORCE_V3", "1", 1);
            ok(vol_ast_recipe_serialize((uint64_t)n * 65536ull, ents, (uint32_t)n,
                                        &blob, &blen) == 0 &&
               blen == 24 + n * 40,
               "G: 77-entry v3 blob has the exact byte size");
            unsetenv("INVFS_RECIPE_FORCE_V3");
            ok(vol_ast_recipe_parse(blob, blen, &ah, &ents_out, &nents) == 0 &&
               nents == n && ents_out && ents_out[76].pba == 1076 &&
               ents_out[76].block_id == 76,
               "G: 77-entry v3 blob parses with the tail entry intact");
            vol_ast_recipe_parsed_free(&ah, ents_out);
            free(blob);
            free(ents);
        }
    }

    /* ---- LEG H: window table behind 40 B entries ---- */
    {
        invfs_ast_block_entry ents[2];
        invfs_ast_window_entry wins[1];
        const invfs_ast_window_entry *wout = NULL;
        uint32_t nw = 99;
        uint8_t *blob = NULL;
        size_t blen = 0;
        invfs_ast_hdr ah;
        const invfs_ast_block_entry *ents_out = NULL;
        size_t nents = 0;
        memset(ents, 0, sizeof ents);
        memset(wins, 0, sizeof wins);
        ents[0].length = 65536; ents[0].block_id = 0; ents[0].pba = 21;
        ents[1].file_offset = 65536; ents[1].length = 65536;
        ents[1].algo = 24; ents[1].block_id = 1; ents[1].pba = 99;
        wins[0].src_off = 0; wins[0].src_len = 4096; wins[0].transform = 0;
        setenv("INVFS_RECIPE_FORCE_V3", "1", 1);
        ok(vol_ast_recipe_serialize_win(131072ull, ents, 2, wins, 1,
                                        &blob, &blen) == 0,
           "H: forced-v3 windowed serialize succeeds");
        unsetenv("INVFS_RECIPE_FORCE_V3");
        ok(vol_ast_recipe_parse(blob, blen, &ah, &ents_out, &nents) == 0 &&
           nents == 2 && ents_out && ents_out[1].algo == 24,
           "H: forced-v3 windowed blob parses (entries verified)");
        vol_ast_recipe_parsed_free(&ah, ents_out);
        ok(vol_ast_recipe_windows(blob, blen, &ah, &wout, &nw) == 0 &&
           nw == 1 && wout && wout[0].src_len == 4096,
           "H: window table found at the 40 B stride offset");
        free(blob);
    }

    printf("entry_v3_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
