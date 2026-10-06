/* read_named_test.c -- WP141: vol_read_named must not answer with another
 * file's bytes and status 0.
 *
 * THE DEFECT (src/core/vol_read.c:1373, vol_read_named)
 * ====================================================
 *
 * The function splits the SUPPLIED name on '!' and walks the pieces as a
 * container path. It resolves the FIRST piece with vol_find + vol_read_file as
 * an ORDINARY inode, with no check that it is a container, and only then hands
 * those bytes to vol_zip_parse_children. So for a supplied name "a!b":
 *
 *   - no inode named "a!b", 'a' is a ZIP with a member 'b'
 *         -> b's bytes, status 0.   THIS IS THE FEATURE (see below).
 *   - an inode named "a!b" EXISTS, and 'a' is a ZIP with a member 'b'
 *         -> b's bytes, status 0.   THIS IS THE DEFECT.
 *
 * The second is the wrong one: the exact name resolves (vol_find returns it),
 * and the function then throws that answer away in favour of a member of a
 * different file. It is the same shape as the one accepted for vol_find -- an
 * inode id carries no in-band error value, and neither does a byte buffer
 * returned with status 0, so the caller cannot tell "here is the file you
 * asked for" from "here is some other file's member".
 *
 * invf-cat is not wrong today only because cat.c:83-97 does the exact-name
 * lookup ITSELF first and falls back to vol_read_named only when vol_find
 * misses. The precedence lives in the caller instead of in the function, so
 * every caller that does not re-implement cat.c's guard inherits the bug.
 *
 * WHAT THIS TEST ASSERTS, AND WHY EVERY LEG IS HERE
 * =================================================
 *
 * LEG 1 is the bug. LEG 1b is the same shadowing at a second shape, so a fix
 * that special-cases one exact spelling does not pass.
 *
 * LEG 2 is the CONTROL ARM, and it is not optional: an exact-name result that
 * matches a container member's bytes would satisfy LEG 1 while being a
 * completely different (and wrong) implementation. So LEG 2 removes the exact
 * inode and asserts that "a!b" then answers with the MEMBER's bytes,
 * byte-exact, with status 0 -- i.e. the feature still works. A fix that made
 * vol_read_named return -1 for everything fails here. That is the arm that
 * keeps this a fix and not a deletion.
 *
 * LEG 3 is the nested form of the feature ("a!b!c"), which walks two member
 * levels and is the shape the docstring at vol_read.c:1368 promises.
 *
 * LEG 4 is the plain-name path the two current test callers use.
 *
 * LEG 5 REFUTES a premise that was carried into this WP by the person who
 * filed it. src/cli/orphan_test.c:480 and src/cli/anchor_test.c:188 were
 * believed to "report a miss for a file that is present and intact" on a
 * volume holding a '!' name. They do not, and cannot: fname() formats
 * "wp121_file_%04d.txt" / "wp_anchor_file_%04d.txt" -- no '!' -- so the split
 * yields one component and the member loop never runs. This leg holds a '!'
 * name on the volume and asserts those exact generated names still read
 * byte-exact, so the refutation is pinned by the test rather than asserted in
 * prose.
 *
 * LEG 6 is the adjacent instance of the same defect in the same function: a
 * trailing bang, "plain!", splits to a single component and used to return
 * `plain`'s WHOLE bytes with status 0 -- a different name's answer.
 *
 * LEG 7 is the unreachable-in-practice arms, pinned so a later change cannot
 * quietly make them answer.
 *
 * THE ORACLE IS THE BYTES AND THE STATUS, NEVER THE RETURN ALONE. A control
 * that only asserted "non-zero" would pass against a vol_read_named that
 * returns -1 for everything -- which is exactly the over-fix LEG 2 exists to
 * catch. Every "must succeed" leg asserts BOTH rc == 0 and memcmp, and prints
 * the bytes it got.
 *
 * STALE BINARY. `make -j4` does not relink the CLI/test binaries -- they are
 * prerequisites of `make test`, not of `all`. The last check compares
 * /proc/self/exe's mtime against vol_read.c and exits 3 if the binary is older.
 * (Deliberately an mtime comparison and NOT a grep for a literal from the fix:
 * the binary being read IS this binary, so such a literal would be present
 * whether or not the fix was in the build.)
 *
 * exit 0 = pass, 1 = failure, 2 = setup error, 3 = stale binary.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "volume_internal.h"

static int checks, failures;

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs(cond ? "  ok   " : "  FAIL ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
    checks++;
    if (!cond) failures++;
}

static void info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("       ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
}

static invfs_volume *g_v;

/* ---- a minimal STORED (method 0) ZIP ------------------------------------ */

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i; int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

static void put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff); }

static void put32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v&0xff); p[1]=(uint8_t)((v>>8)&0xff);
  p[2]=(uint8_t)((v>>16)&0xff); p[3]=(uint8_t)((v>>24)&0xff); }

/* One stored member. vol_zip_extract_member refuses data_off == 0 and
 * usize == 0, so every payload here is non-empty. */
static size_t build_zip1(const char *mname, const uint8_t *pay, size_t plen,
                         uint8_t **out)
{
    size_t nlen = strlen(mname);
    size_t lh = 30 + nlen, cd = 46 + nlen, tot = lh + plen + cd + 22;
    uint8_t *b = calloc(1, tot), *p = b;
    uint32_t crc = crc32_of(pay, plen);

    if (!b) return 0;
    /* local file header */
    put32(p, 0x04034b50); put16(p+4, 20); put16(p+6, 0);  put16(p+8, 0);
    put16(p+10, 0);        put16(p+12, 0); put32(p+14, crc);
    put32(p+18, (uint32_t)plen); put32(p+22, (uint32_t)plen);
    put16(p+26, (uint16_t)nlen); put16(p+28, 0);
    memcpy(p+30, mname, nlen);
    memcpy(p+lh, pay, plen);
    p += lh + plen;
    /* central directory */
    put32(p, 0x02014b50); put16(p+4, 20); put16(p+6, 20); put16(p+8, 0);
    put16(p+10, 0); put16(p+12, 0); put16(p+14, 0);
    put32(p+16, crc); put32(p+20, (uint32_t)plen); put32(p+24, (uint32_t)plen);
    put16(p+28, (uint16_t)nlen); put16(p+30, 0); put16(p+32, 0);
    put16(p+34, 0); put16(p+36, 0); put32(p+38, 0); put32(p+42, 0);
    memcpy(p+46, mname, nlen);
    p += cd;
    /* EOCD -- vol_zip_parse_children scans backwards from zlen-22 for this */
    put32(p, 0x06054b50); put16(p+4, 0); put16(p+6, 0);
    put16(p+8, 1); put16(p+10, 1);
    put32(p+12, (uint32_t)cd); put32(p+16, (uint32_t)(lh + plen));
    put16(p+20, 0);
    *out = b;
    return tot;
}

/* ---- assertions --------------------------------------------------------- */

/* The whole oracle in one place: BOTH the status and the bytes. A caller that
 * gets the wrong bytes must not be able to pass by returning 0, and a caller
 * that refuses to answer must not pass by returning -1 -- the caller asserts
 * rc separately. */
static void expect_bytes(const char *tag, const char *name,
                         const uint8_t *want, size_t wlen, int want_rc)
{
    uint8_t *got = NULL;
    size_t glen = 0;
    int rc = vol_read_named(g_v, name, &got, &glen);

    if (rc != want_rc) {
        ok(0, "%s: vol_read_named(\"%s\") rc=%d, expected rc=%d",
           tag, name, rc, want_rc);
        free(got);
        return;
    }
    if (want_rc != 0) {
        ok(1, "%s: vol_read_named(\"%s\") rc=%d (absent)", tag, name, rc);
        free(got);
        return;
    }
    if (glen != wlen || !got || memcmp(got, want, wlen) != 0) {
        ok(0, "%s: vol_read_named(\"%s\") rc=0 but %zu bytes, wanted %zu "
           "-- WRONG BYTES WITH SUCCESS STATUS", tag, name, glen, wlen);
        free(got);
        return;
    }
    ok(1, "%s: vol_read_named(\"%s\") rc=0, %zu bytes, byte-exact",
       tag, name, glen);
    free(got);
}

/* ---- the fixture -------------------------------------------------------- */

#define PAY_MEMBER_B "PAYLOAD-OF-ZIP-MEMBER-B\n"
#define PAY_INODE_1  "REAL-INODE-a!b-PAYLOAD\n"
#define PAY_INODE_2  "REAL-INODE-a!p-PAYLOAD-LONGER-THAN-THE-MEMBER\n"
#define PAY_NEST_C   "PAYLOAD-OF-NESTED-MEMBER-C\n"
#define PAY_PLAIN    "PLAIN\n"
#define LEN(x) (sizeof(x) - 1)

static uint64_t put_blob(const char *name, const char *pay)
{
    /* vol_create_blob_file is the lane creator (bang_name_test.c:137). After
     * WP135 the user paths refuse a '!', so this is how a legacy '!' name
     * gets onto a volume at all -- which is the state this WP is about. */
    uint64_t id = vol_create_blob_file(g_v, name,
                                       (const uint8_t *)pay, strlen(pay),
                                       (uint64_t)strlen(pay), INVFS_ALGO_NONE);
    if (!id) info("create of '%s' returned 0", name);
    return id;
}

/* ---- LEG 1: the defect -------------------------------------------------- */

static void leg_a_exact_name_beats_a_member(void)
{
    info("LEG A (THE DEFECT): an inode named 'a!b' must not be shadowed by a "
         "member 'b' of the ZIP 'a'");

    /* 'a' is a genuine ZIP whose member 'b' carries DIFFERENT bytes from the
     * inode 'a!b', and a different length too -- so a control that only
     * compared a hash of the two could not tell them apart either. */
    {
        uint8_t *zip = NULL;
        size_t zlen = build_zip1("b", (const uint8_t *)PAY_MEMBER_B,
                                 LEN(PAY_MEMBER_B), &zip);
        if (!zlen || !vol_create_file(g_v, "a", zip, zlen)) {
            ok(0, "setup: could not create the ZIP 'a'");
            free(zip);
            return;
        }
        free(zip);
    }
    ok(vol_find(g_v, "a") != 0, "setup: 'a' is on the volume");

    /* The exact name now exists, and it is NOT the member. */
    ok(put_blob("a!b", PAY_INODE_1) != 0, "setup: 'a!b' inode created");
    ok(vol_find(g_v, "a!b") != 0,
       "setup: the EXACT name 'a!b' resolves (vol_find) -- so it is not an "
       "absent name and the member fallback must not be reached");

    /* THE RED ASSERTION: status AND bytes. Before the fix this returns the
     * ZIP member's 20 bytes with status 0 while 'a!b' holds 23 other bytes. */
    expect_bytes("LEG A", "a!b", (const uint8_t *)PAY_INODE_1,
                 LEN(PAY_INODE_1), 0);
}

/* LEG 1b -- the same shadowing at a second shape (different member name,
 * different prefix, different lengths), so a fix hard-coded to one spelling
 * does not pass LEG A and fail here. */
static void leg_a2_second_shape(void)
{
    uint8_t *zip = NULL;
    size_t zlen;

    info("LEG A2: the same shadowing under a different prefix and member");

    zlen = build_zip1("p", (const uint8_t *)PAY_MEMBER_B, LEN(PAY_MEMBER_B),
                      &zip);
    if (!zlen || !vol_create_file(g_v, "z", zip, zlen)) {
        ok(0, "setup: could not create the ZIP 'z'");
        free(zip);
        return;
    }
    free(zip);
    ok(put_blob("z!p", PAY_INODE_2) != 0, "setup: 'z!p' inode created");
    expect_bytes("LEG A2", "z!p", (const uint8_t *)PAY_INODE_2,
                 LEN(PAY_INODE_2), 0);
}

/* ---- LEG 2: THE CONTROL ARM -------------------------------------------- */

/* Remove every exact '!'-bearing inode, so the member reading is the only
 * possible answer -- and then demand it, byte-exact, with status 0.
 *
 * This is the arm that makes LEG A a fix rather than a deletion: a
 * vol_read_named that returned -1 for every '!' name satisfies every LEG A
 * assertion and fails here. */
static void leg_b_control_member_still_reads(void)
{
    info("LEG B (CONTROL): with no exact inode of that name, a genuine member "
         "of a genuine container must STILL read back byte-exact");

    ok(vol_unlink(g_v, "a!b") == 0, "setup: unlink('a!b')");
    ok(vol_find(g_v, "a!b") == 0, "setup: 'a!b' no longer resolves exactly");

    expect_bytes("LEG B", "a!b", (const uint8_t *)PAY_MEMBER_B,
                 LEN(PAY_MEMBER_B), 0);

    /* and the second shape, whose member is the same payload under 'z' */
    ok(vol_unlink(g_v, "z!p") == 0, "setup: unlink('z!p')");
    expect_bytes("LEG B", "z!p", (const uint8_t *)PAY_MEMBER_B,
                 LEN(PAY_MEMBER_B), 0);
}

/* ---- LEG 3: nested member read (the documented "a!inner.zip!x.txt") ------ */

static void leg_c_nested_member(void)
{
    uint8_t *inner = NULL, *outer = NULL;
    size_t ilen, olen;

    info("LEG C (CONTROL): a NESTED member of a genuine container still reads "
         "byte-exact");

    ilen = build_zip1("c", (const uint8_t *)PAY_NEST_C, LEN(PAY_NEST_C),
                      &inner);
    if (!ilen) { ok(0, "setup: inner zip"); return; }
    olen = build_zip1("inner.zip", inner, ilen, &outer);
    free(inner);
    if (!olen || !vol_create_file(g_v, "n", outer, olen)) {
        ok(0, "setup: could not create the outer ZIP 'n'");
        free(outer);
        return;
    }
    free(outer);

    /* "n!inner.zip" is the inner ZIP's own bytes; "n!inner.zip!c" is the
     * payload two levels down. Both are the documented nested walk. */
    expect_bytes("LEG C", "n!inner.zip!c", (const uint8_t *)PAY_NEST_C,
                 LEN(PAY_NEST_C), 0);

    /* and an exact 'n!inner.zip' inode still wins over that member */
    ok(put_blob("n!inner.zip", PAY_INODE_1) != 0,
       "setup: exact inode 'n!inner.zip' created");
    expect_bytes("LEG C", "n!inner.zip", (const uint8_t *)PAY_INODE_1,
                 LEN(PAY_INODE_1), 0);
    ok(vol_unlink(g_v, "n!inner.zip") == 0, "setup: unlink('n!inner.zip')");
}

/* ---- LEG 4: the plain-name path ---------------------------------------- */

static void leg_d_plain_names(void)
{
    info("LEG D (CONTROL): a bang-free name reads its own whole file");
    ok(vol_create_file(g_v, "plain", (const uint8_t *)PAY_PLAIN,
                       LEN(PAY_PLAIN)) != 0, "setup: 'plain' created");
    expect_bytes("LEG D", "plain", (const uint8_t *)PAY_PLAIN, LEN(PAY_PLAIN), 0);
}

/* ---- LEG 5: the refuted premise, pinned -------------------------------- */

/* The filing said orphan_test.c:480 and anchor_test.c:188 "report a miss for a
 * file that is present and intact" on a volume holding a '!' name. Hold one on
 * the volume and read those exact generated names. */
static void leg_e_bang_name_does_not_disturb_plain_names(void)
{
    static const char *NAMES[] = {
        "wp121_file_0000.txt", "wp121_file_0001.txt",
        "wp_anchor_file_0000.txt", "wp_anchor_file_0001.txt",
    };
    size_t i;

    info("LEG E: a '!'-bearing name on the volume does not disturb the "
         "bang-free names orphan_test/anchor_test actually ask for");

    ok(put_blob("a!b", PAY_INODE_1) != 0, "setup: 'a!b' is back on the volume");
    ok(vol_find(g_v, "a!b") != 0, "setup: 'a!b' is live");

    for (i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        const char *n = NAMES[i];
        uint8_t pay[32];
        int k;
        for (k = 0; k < 16; k++) pay[k] = (uint8_t)('A' + (int)i);
        pay[16] = (uint8_t)i;
        pay[17] = 0;
        if (!vol_create_file(g_v, n, pay, 18)) {
            ok(0, "setup: could not create '%s'", n);
            continue;
        }
        /* 'a!b' is live and 'a' is a live ZIP with a member 'b'. A member
         * interpretation of these names is impossible; if the presence of the
         * bang name could perturb them, this is where it would show. */
        expect_bytes("LEG E", n, pay, 18, 0);
    }
}

/* ---- LEG 6: the trailing bang ------------------------------------------ */

static void leg_f_trailing_bang(void)
{
    info("LEG F: a trailing '!' names nothing -- it must not answer with the "
         "prefix's whole bytes");
    /* "plain!" splits to the single component "plain", so before the fix it
     * returned `plain`'s entire contents with status 0: a different name. */
    expect_bytes("LEG F", "plain!", NULL, 0, -1);
}

/* ---- LEG 7: absent / not-a-container ------------------------------------ */

static void leg_g_unreachable_arms(void)
{
    uint8_t *zip = NULL;
    size_t zlen;

    info("LEG G: a member that does not exist, and a prefix that is not a "
         "container");

    expect_bytes("LEG G", "a!nosuchmember", NULL, 0, -1);
    expect_bytes("LEG G", "nosuchprefix!b", NULL, 0, -1);
    expect_bytes("LEG G", "plain!b", NULL, 0, -1);   /* 'plain' is not a zip */

    zlen = build_zip1("b", (const uint8_t *)PAY_MEMBER_B, LEN(PAY_MEMBER_B),
                      &zip);
    if (zlen) {
        free(zip);
        expect_bytes("LEG G", "z!nosuchmember", NULL, 0, -1);
    }
}

/* ---- stale-binary guard (mtimes, NOT a grep for the fix) --------------- */

static int mtime_of(const char *path, time_t *out)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    *out = st.st_mtime;
    return 0;
}

static int stale_binary(const char *self)
{
    static const char *deps[] = {
        "src/core/vol_read.c", "src/core/volume_internal.h",
    };
    const char *root = getenv("PWD");
    time_t me, d;
    char p[1024];
    size_t i;

    if (!root || !*root) root = ".";
    if (mtime_of(self, &me) != 0) {
        fprintf(stderr, "\nread_named_test: STALE BINARY -- cannot stat %s\n",
                self);
        return 1;
    }
    for (i = 0; i < sizeof deps / sizeof deps[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", root, deps[i]);
        if (mtime_of(p, &d) != 0) continue;
        if (d > me) {
            fprintf(stderr,
                "\nread_named_test: STALE BINARY -- %s is older than %s.\n"
                "    This binary was built before the sources it measures. The\n"
                "    test binaries are prerequisites of `make test`, not of\n"
                "    `all`, so `make -j4` does not relink them:\n"
                "        rm -f bin/invf-read_named_test && make bin/invf-read_named_test\n",
                self, p);
            return 1;
        }
    }
    return 0;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    char img[512];
    int err = 0;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("read_named_test: vol_read_named must not hand back another file's "
           "bytes with status 0 -- and a genuine container member must still "
           "read byte-exact\n");

    if (stale_binary("/proc/self/exe")) return 3;

    snprintf(img, sizeof img, "%s/invf-read-named-test.img", dir);
    unlink(img);
    {
        char cmd[1024];
        const char *root = getenv("PWD") ? getenv("PWD") : ".";
        snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 48 >/dev/null 2>&1",
                 root, img);
        if (system(cmd) != 0) {
            fprintf(stderr, "read_named_test: cannot create volume with "
                            "invf-mkfs\n");
            return 2;
        }
    }
    g_v = vol_open(img, &err);
    if (!g_v) {
        fprintf(stderr, "read_named_test: vol_open failed: err=%d\n", err);
        return 2;
    }

    leg_a_exact_name_beats_a_member();
    leg_a2_second_shape();
    leg_b_control_member_still_reads();
    leg_c_nested_member();
    leg_d_plain_names();
    leg_e_bang_name_does_not_disturb_plain_names();
    leg_f_trailing_bang();
    leg_g_unreachable_arms();

    vol_close(g_v);
    unlink(img);

    printf("\nread_named_test: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
