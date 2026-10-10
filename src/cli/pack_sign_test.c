/* pack_sign_test.c — pack signatures v1, asserted on its own (WP203).
 *
 * What this pins:
 *   1. VECTORS. The vendored ed25519 signs RFC 8032 §7.1 TEST 1 (empty
 *      message) and TEST 3 (2-byte message) to the exact published
 *      signatures. If the vendored arithmetic is wrong, or a later edit
 *      touches it, this goes red — the vectors are transcribed from
 *      rfc-editor.org/rfc8032.txt, not from the implementation.
 *   2. ROUND TRIP. keygen -> sign -> verify accepts; tampered manifest,
 *      tampered helper, tampered sidecar, wrong keyring, and a signed
 *      manifest with no integrity binding all refuse (PACK_SIG_BAD).
 *   3. STRICTNESS. A corpus of malformed sidecars (bad tag, short/long,
 *      non-hex, extra line, missing newline, empty, huge) every one
 *      refuses with BAD — never UNSIGNED, never OK. Malformed .pub
 *      files in the keyring are ignored (the good key still verifies).
 *   4. GATE MATRIX. unsigned+yes/no/skip/-y/-n and bad+skip/-y, with a
 *      scripted prompt (no stdin reads in the unit tier).
 *   5. HEX. The strict decoder takes exactly 2n hex chars, nothing else.
 *
 * What this does NOT do: touch the network, a volume, or the registry
 * (install-gate only — pack_sig.h says why); fuzz the codec registration
 * path (unchanged by this WP — see Remaining TODOs in the WP report for
 * the why-not).
 *
 * Usage: bin/invf-pack_sign_test /tmp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../codecs/ed25519.h"
#include "pack_sig.h"

static int fails;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("FAIL: %s\n", what);
    } else {
        printf("ok:   %s\n", what);
    }
}

/* RFC 8032 §7.1, transcribed from rfc-editor.org/rfc8032.txt. */
static const char *T1_SEED =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
static const char *T1_PK =
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
static const char *T1_SIG =
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
    "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
static const char *T3_SEED =
    "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7";
static const char *T3_PK =
    "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025";
static const char *T3_MSG = "af82";
static const char *T3_SIG =
    "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
    "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a";

static void test_vectors(void)
{
    unsigned char seed[32], pk[32], sk[64], sig[64], esig[64], msg[2];

    ok(invfs_hex_decode(T1_SEED, seed, 32) == 0, "vec: t1 seed parses");
    ok(invfs_hex_decode(T1_PK, pk, 32) == 0, "vec: t1 pk parses");
    ok(invfs_hex_decode(T1_SIG, esig, 64) == 0, "vec: t1 sig parses");
    memcpy(sk, seed, 32);
    memcpy(sk + 32, pk, 32);
    ok(invfs_ed25519_sign(sig, NULL, 0, sk) == 0, "vec: t1 signs");
    ok(memcmp(sig, esig, 64) == 0, "vec: t1 matches RFC 8032 TEST 1");
    ok(invfs_ed25519_verify(esig, NULL, 0, pk) == 0, "vec: t1 verifies");
    sig[0] ^= 1;
    ok(invfs_ed25519_verify(sig, NULL, 0, pk) != 0,
       "vec: t1 tampered sig refuses");

    ok(invfs_hex_decode(T3_SEED, seed, 32) == 0, "vec: t3 seed parses");
    ok(invfs_hex_decode(T3_PK, pk, 32) == 0, "vec: t3 pk parses");
    ok(invfs_hex_decode(T3_SIG, esig, 64) == 0, "vec: t3 sig parses");
    ok(invfs_hex_decode(T3_MSG, msg, 2) == 0, "vec: t3 msg parses");
    memcpy(sk, seed, 32);
    memcpy(sk + 32, pk, 32);
    ok(invfs_ed25519_sign(sig, msg, 2, sk) == 0, "vec: t3 signs");
    ok(memcmp(sig, esig, 64) == 0, "vec: t3 matches RFC 8032 TEST 3");
    ok(invfs_ed25519_verify(esig, msg, 2, pk) == 0, "vec: t3 verifies");
    msg[0] ^= 1;
    ok(invfs_ed25519_verify(esig, msg, 2, pk) != 0,
       "vec: t3 tampered msg refuses");
}

static void test_hex(void)
{
    unsigned char o[4];
    ok(invfs_hex_decode("00ffAB12", o, 4) == 0 &&
       o[0] == 0 && o[1] == 0xff && o[2] == 0xab && o[3] == 0x12,
       "hex: mixed-case decodes");
    ok(invfs_hex_decode("00ffAB1", o, 4) != 0, "hex: short refuses");
    ok(invfs_hex_decode("00ffAB123", o, 4) != 0, "hex: long refuses");
    ok(invfs_hex_decode("00ffAB1x", o, 4) != 0, "hex: non-hex refuses");
    ok(invfs_hex_decode("00ffAB12\n", o, 4) != 0, "hex: newline refuses");
    ok(invfs_hex_decode("00ff AB12", o, 4) != 0, "hex: space refuses");
    ok(invfs_hex_decode("0x00ffAB12", o, 4) != 0, "hex: 0x prefix refuses");
    ok(invfs_hex_decode("", o, 0) == 0, "hex: empty/0 decodes");
}

/* ---- scratch pack fixtures ---- */

static char g_root[1024];

static void wfile(const char *rel, const char *content)
{
    char full[2048];
    FILE *f;
    snprintf(full, sizeof full, "%s/%s", g_root, rel);
    f = fopen(full, "wb");
    if (!f) {
        printf("FAIL: fixture write %s\n", full);
        exit(1);
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void wbin(const char *rel, const unsigned char *b, size_t n)
{
    char full[2048];
    FILE *f;
    snprintf(full, sizeof full, "%s/%s", g_root, rel);
    f = fopen(full, "wb");
    if (!f) {
        printf("FAIL: fixture write %s\n", full);
        exit(1);
    }
    fwrite(b, 1, n, f);
    fclose(f);
}

/* Build keys/<name>.pub+.sec and pack/{manifest,bin/enc}. cfg_keygen
 * uses the real keygen path (errors fail the test, not the fixture). */
static void fixture_pack(const char *keyname)
{
    char keys[2048], pack[2048], err[512];
    snprintf(keys, sizeof keys, "%s/keys", g_root);
    snprintf(pack, sizeof pack, "%s/pack", g_root);
    mkdir(keys, 0700);
    mkdir(pack, 0700);
    {
        char bindir[2048];
        snprintf(bindir, sizeof bindir, "%s/bin", pack);
        mkdir(bindir, 0700);
    }
    if (pack_sig_keygen(keys, keyname, err, sizeof err) != 0) {
        printf("FAIL: keygen %s\n", err);
        exit(1);
    }
    {
        char mpath[2048];
        FILE *f;
        snprintf(mpath, sizeof mpath, "%s/manifest", pack);
        f = fopen(mpath, "wb");
        if (!f) {
            printf("FAIL: fixture manifest\n");
            exit(1);
        }
        fprintf(f, "name = vxp\n");
        fprintf(f, "algo = 46\n");
        fprintf(f, "encode = bin/enc {in} {out}\n");
        fclose(f);
    }
    {
        /* helper content is fixed bytes (not a script): tamper legs flip
         * a byte and must refuse. */
        static const unsigned char enc[] = { 0x7f, 'E', 'L', 'F', 1, 2, 3 };
        char save[1024];
        snprintf(save, sizeof save, "%s", g_root);
        snprintf(g_root, sizeof g_root, "%s", pack);
        wbin("bin/enc", enc, sizeof enc);
        snprintf(g_root, sizeof g_root, "%s", save);
    }
}

static void pack_path(char *out, size_t cap) { snprintf(out, cap, "%s/pack", g_root); }
static void keys_path(char *out, size_t cap) { snprintf(out, cap, "%s/keys", g_root); }
static void sec_path(char *out, size_t cap, const char *keyname)
{
    snprintf(out, cap, "%s/keys/%s.sec", g_root, keyname);
}

static int sign_fixture(const char *keyname)
{
    char pack[2048], sec[2048], err[512];
    int rc;
    pack_path(pack, sizeof pack);
    sec_path(sec, sizeof sec, keyname);
    rc = pack_sig_sign_dir(pack, sec, err, sizeof err);
    if (rc != 0)
        printf("     (sign: %s)\n", err);
    return rc;
}

static pack_sig_status verify_fixture(char *err, size_t errcap)
{
    char pack[2048], keys[2048];
    pack_path(pack, sizeof pack);
    keys_path(keys, sizeof keys);
    return pack_sig_verify_dir(pack, keys, err, errcap);
}

static void test_round_trip(void)
{
    char err[512], pack[2048];
    pack_sig_status st;

    fixture_pack("rt");
    ok(sign_fixture("rt") == 0, "rt: sign succeeds");
    st = verify_fixture(err, sizeof err);
    ok(st == PACK_SIG_OK, "rt: signed pack verifies");
    (void)pack;

    /* tampered helper: flip one byte of bin/enc, manifest+sig untouched. */
    pack_path(pack, sizeof pack);
    {
        char save[1024];
        snprintf(save, sizeof save, "%s", g_root);
        snprintf(g_root, sizeof g_root, "%s", pack);
        wfile("bin/enc", "TAMPERED BYTES!!");
        snprintf(g_root, sizeof g_root, "%s", save);
    }
    st = verify_fixture(err, sizeof err);
    ok(st == PACK_SIG_BAD, "rt: tampered helper refuses (BAD)");

    /* restore by re-signing, then tamper the manifest instead. */
    ok(sign_fixture("rt") == 0, "rt: re-sign succeeds");
    st = verify_fixture(err, sizeof err);
    ok(st == PACK_SIG_OK, "rt: re-signed verifies");
    {
        char save[1024];
        snprintf(save, sizeof save, "%s", g_root);
        snprintf(g_root, sizeof g_root, "%s", pack);
        /* append a line to the real manifest (helpers untouched) */
        {
            char mp[2048];
            FILE *f;
            snprintf(mp, sizeof mp, "%s/manifest", pack);
            f = fopen(mp, "ab");
            fprintf(f, "evil = 1\n");
            fclose(f);
        }
        snprintf(g_root, sizeof g_root, "%s", save);
    }
    st = verify_fixture(err, sizeof err);
    ok(st == PACK_SIG_BAD, "rt: tampered manifest refuses (BAD)");

    /* restore, then tamper the sidecar (flip one hex char). */
    ok(sign_fixture("rt") == 0, "rt: re-sign succeeds (2)");
    {
        char sp[2048], *buf = NULL;
        size_t n = 0;
        FILE *f;
        snprintf(sp, sizeof sp, "%s/manifest.sig", pack);
        f = fopen(sp, "rb");
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        rewind(f);
        buf = malloc(n + 1);
        if (fread(buf, 1, n, f) != n) {
            printf("FAIL: sig read\n");
            exit(1);
        }
        fclose(f);
        buf[20] = (buf[20] == 'a') ? 'b' : 'a';
        f = fopen(sp, "wb");
        if (fwrite(buf, 1, n, f) != n) {
            printf("FAIL: sig write\n");
            exit(1);
        }
        fclose(f);
        free(buf);
    }
    st = verify_fixture(err, sizeof err);
    ok(st == PACK_SIG_BAD, "rt: tampered sidecar refuses (BAD)");
}

static void test_keyring(void)
{
    char err[512], keys[2048], pack[2048];
    pack_sig_status st;

    fixture_pack("kr");
    ok(sign_fixture("kr") == 0, "keyring: sign succeeds");
    keys_path(keys, sizeof keys);
    pack_path(pack, sizeof pack);

    /* wrong keyring (empty dir): signed-by-unknown refuses. */
    {
        char empty[2048];
        snprintf(empty, sizeof empty, "%s/empty", g_root);
        mkdir(empty, 0700);
        st = pack_sig_verify_dir(pack, empty, err, sizeof err);
        ok(st == PACK_SIG_BAD, "keyring: unknown key refuses (BAD)");
    }
    /* malformed .pub beside the good key: ignored, good key verifies. */
    {
        char bad[2048];
        FILE *f;
        snprintf(bad, sizeof bad, "%s/broken.pub", keys);
        f = fopen(bad, "wb");
        fprintf(f, "not-a-key\n");
        fclose(f);
        snprintf(bad, sizeof bad, "%s/alsobad.pub", keys);
        f = fopen(bad, "wb");
        fprintf(f, "ed25519-pub zzzz\n");
        fclose(f);
    }
    st = verify_fixture(err, sizeof err);
    ok(st == PACK_SIG_OK, "keyring: malformed .pub ignored, good key OK");

    /* signed manifest with the integrity line stripped: refuses even
     * though the ed25519 over the stripped bytes would verify — the
     * binding is mandatory, not advisory. Re-sign AFTER stripping, so
     * the signature itself is valid and only the binding is absent. */
    {
        char mp[2048], *buf = NULL;
        size_t n = 0;
        FILE *f;
        char *nl;
        snprintf(mp, sizeof mp, "%s/manifest", pack);
        f = fopen(mp, "rb");
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        rewind(f);
        buf = malloc(n + 1);
        if (fread(buf, 1, n, f) != n) {
            printf("FAIL: manifest read\n");
            exit(1);
        }
        fclose(f);
        buf[n] = 0;
        /* drop the integrity line */
        {
            char *w = buf;
            const char *p = buf;
            while (*p) {
                const char *eol = strchr(p, '\n');
                size_t ll = eol ? (size_t)(eol - p) : strlen(p);
                if (!(ll > 9 && !memcmp(p, "integrity", 9))) {
                    memmove(w, p, ll);
                    w += ll;
                    *w++ = '\n';
                }
                if (!eol)
                    break;
                p = eol + 1;
            }
            *w = 0;
        }
        (void)nl;
        f = fopen(mp, "wb");
        fwrite(buf, 1, strlen(buf), f);
        fclose(f);
        free(buf);
    }
    ok(sign_fixture("kr") == 0, "keyring: re-sign (bindingless) succeeds");
    /* sign re-adds the binding — so strip AGAIN after signing: now the
     * signature is over bound bytes but the manifest on disk is not. */
    {
        char mp[2048], *buf = NULL;
        size_t n = 0;
        FILE *f;
        snprintf(mp, sizeof mp, "%s/manifest", pack);
        f = fopen(mp, "rb");
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        rewind(f);
        buf = malloc(n + 1);
        if (fread(buf, 1, n, f) != n) {
            printf("FAIL: manifest read 2\n");
            exit(1);
        }
        fclose(f);
        buf[n] = 0;
        {
            char *w = buf;
            const char *p = buf;
            while (*p) {
                const char *eol = strchr(p, '\n');
                size_t ll = eol ? (size_t)(eol - p) : strlen(p);
                if (!(ll > 9 && !memcmp(p, "integrity", 9))) {
                    memmove(w, p, ll);
                    w += ll;
                    *w++ = '\n';
                }
                if (!eol)
                    break;
                p = eol + 1;
            }
            *w = 0;
        }
        f = fopen(mp, "wb");
        fwrite(buf, 1, strlen(buf), f);
        fclose(f);
        free(buf);
    }
    st = verify_fixture(err, sizeof err);
    ok(st == PACK_SIG_BAD, "keyring: bindingless signed manifest refuses");

    /* duplicate integrity line refuses (ambiguous binding). */
    ok(sign_fixture("kr") == 0, "keyring: re-sign (3) succeeds");
    {
        char mp[2048];
        FILE *f;
        snprintf(mp, sizeof mp, "%s/manifest", pack);
        f = fopen(mp, "ab");
        fprintf(f, "integrity = 00\n");
        fclose(f);
    }
    st = verify_fixture(err, sizeof err);
    ok(st == PACK_SIG_BAD, "keyring: duplicate integrity refuses");
}

/* Write a hand-made sidecar and require BAD (never UNSIGNED/OK). */
static void corrupt_sig(const char *body, size_t n, const char *what)
{
    char sp[2048], pack[2048], err[512];
    FILE *f;
    pack_sig_status st;
    pack_path(pack, sizeof pack);
    snprintf(sp, sizeof sp, "%s/manifest.sig", pack);
    f = fopen(sp, "wb");
    if (fwrite(body, 1, n, f) != n) {
        printf("FAIL: sig write %s\n", what);
        exit(1);
    }
    fclose(f);
    st = verify_fixture(err, sizeof err);
    ok(st == PACK_SIG_BAD, what);
}

static void test_sig_corpus(void)
{
    char pack[2048], keys[2048], good[512];
    FILE *f;
    size_t goodn = 0;

    fixture_pack("corpus");
    ok(sign_fixture("corpus") == 0, "corpus: sign succeeds");
    (void)keys;
    pack_path(pack, sizeof pack);
    {
        char sp[2048];
        snprintf(sp, sizeof sp, "%s/manifest.sig", pack);
        f = fopen(sp, "rb");
        fseek(f, 0, SEEK_END);
        goodn = (size_t)ftell(f);
        rewind(f);
        if (fread(good, 1, goodn, f) != goodn) {
            printf("FAIL: sig read\n");
            exit(1);
        }
        fclose(f);
    }
    ok(goodn == (size_t)(12 + 64 + 1 + 128 + 1), "corpus: sidecar shape sane");

    corrupt_sig("", 0, "corpus: empty refuses");
    corrupt_sig("sig-ed25519", 11, "corpus: tag-only refuses");
    corrupt_sig("md5 abc\n", 8, "corpus: wrong tag refuses");
    corrupt_sig(good, goodn - 1, "corpus: missing newline refuses");
    {
        char trunc[512];
        memcpy(trunc, good, goodn);
        trunc[goodn - 2] = '\n'; /* drop one hex char */
        corrupt_sig(trunc, goodn - 1, "corpus: short sig-hex refuses");
    }
    {
        char ext[520];
        memcpy(ext, good, goodn - 1);
        ext[goodn - 1] = '0';
        ext[goodn] = '\n';
        corrupt_sig(ext, goodn + 1, "corpus: long sig-hex refuses");
    }
    {
        char bad[512];
        memcpy(bad, good, goodn);
        bad[15] = 'Z'; /* non-hex in key field */
        corrupt_sig(bad, goodn, "corpus: non-hex key refuses");
    }
    {
        char bad[512];
        memcpy(bad, good, goodn);
        bad[80] = 'Z'; /* non-hex in sig field */
        corrupt_sig(bad, goodn, "corpus: non-hex sig refuses");
    }
    {
        char two[1024];
        memcpy(two, good, goodn);
        memcpy(two + goodn, good, goodn);
        corrupt_sig(two, goodn * 2, "corpus: extra line refuses");
    }
    {
        char no[512];
        memcpy(no, good, goodn);
        no[12 + 64] = '\t'; /* tab instead of space */
        corrupt_sig(no, goodn, "corpus: tab separator refuses");
    }
    {
        /* 600 bytes of hex: over the shape, must refuse, not truncate. */
        char big[700];
        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = '\n';
        corrupt_sig(big, sizeof big, "corpus: huge refuses");
    }
    {
        /* 600-byte file of valid-prefix bytes then garbage. */
        char big[610];
        memcpy(big, "sig-ed25519 ", 12);
        memset(big + 12, 'b', sizeof big - 13);
        big[sizeof big - 1] = '\n';
        corrupt_sig(big, sizeof big, "corpus: oversize refuses");
    }
}

/* Scripted prompt answers for the gate matrix. */
static int g_answer;
static int prompt_scripted(const char *packname, void *ctx)
{
    (void)packname;
    (void)ctx;
    return g_answer;
}

static void test_gate(void)
{
    char pack[2048], keys[2048], err[512];
    int rc;

    fixture_pack("gate");
    pack_path(pack, sizeof pack);
    keys_path(keys, sizeof keys);

    /* unsigned pack: no sidecar yet. */
    g_answer = 1;
    rc = pack_sig_gate_install(pack, "gate", keys, 0, 0, 0,
                               prompt_scripted, NULL, err, sizeof err);
    ok(rc == 0, "gate: unsigned + yes proceeds");
    g_answer = 0;
    rc = pack_sig_gate_install(pack, "gate", keys, 0, 0, 0,
                               prompt_scripted, NULL, err, sizeof err);
    ok(rc == 1, "gate: unsigned + no aborts");
    rc = pack_sig_gate_install(pack, "gate", keys, 0, 0, 1,
                               prompt_scripted, NULL, err, sizeof err);
    ok(rc == 0, "gate: unsigned + skip-flag proceeds");
    rc = pack_sig_gate_install(pack, "gate", keys, 1, 0, 0,
                               prompt_scripted, NULL, err, sizeof err);
    ok(rc == 0, "gate: unsigned + -y proceeds");
    g_answer = 0; /* must not even be asked */
    rc = pack_sig_gate_install(pack, "gate", keys, 0, 1, 0,
                               prompt_scripted, NULL, err, sizeof err);
    ok(rc == 0, "gate: unsigned + -n reports, installs nothing");

    /* signed pack: gate green. */
    ok(sign_fixture("gate") == 0, "gate: sign succeeds");
    g_answer = 0;
    rc = pack_sig_gate_install(pack, "gate", keys, 0, 0, 0,
                               prompt_scripted, NULL, err, sizeof err);
    ok(rc == 0, "gate: signed proceeds without asking");

    /* bad sig: skip AND -y both still refuse (locked decision). */
    {
        char sp[2048];
        FILE *f;
        snprintf(sp, sizeof sp, "%s/manifest.sig", pack);
        f = fopen(sp, "ab");
        fprintf(f, "x\n");
        fclose(f);
    }
    rc = pack_sig_gate_install(pack, "gate", keys, 0, 0, 1,
                               prompt_scripted, NULL, err, sizeof err);
    ok(rc == 1, "gate: bad + skip-flag STILL refuses");
    g_answer = 1;
    rc = pack_sig_gate_install(pack, "gate", keys, 1, 0, 0,
                               prompt_scripted, NULL, err, sizeof err);
    ok(rc == 1, "gate: bad + -y STILL refuses");
    rc = pack_sig_gate_install(pack, "gate", keys, 0, 1, 0,
                               prompt_scripted, NULL, err, sizeof err);
    ok(rc == 1, "gate: bad + -n STILL refuses");
}

static void test_keygen_names(void)
{
    char keys[2048], err[512];
    keys_path(keys, sizeof keys);
    ok(pack_sig_keygen(keys, "good-name.x1", err, sizeof err) == 0,
       "keygen: dotted name ok");
    ok(pack_sig_keygen(keys, "../evil", err, sizeof err) != 0,
       "keygen: slash name refuses");
    ok(pack_sig_keygen(keys, "", err, sizeof err) != 0,
       "keygen: empty name refuses");
    ok(pack_sig_keygen(keys, "sp ace", err, sizeof err) != 0,
       "keygen: space name refuses");
    {
        /* .sec must be 0600: group/other get nothing. */
        char sec[2048];
        struct stat st;
        snprintf(sec, sizeof sec, "%s/good-name.x1.sec", keys);
        ok(stat(sec, &st) == 0 && (st.st_mode & 077) == 0,
           "keygen: secret is 0600");
    }
}

int main(int argc, char **argv)
{
    char sub[2048];
    if (argc < 2) {
        fprintf(stderr, "usage: %s /tmp\n", argv[0]);
        return 2;
    }
    snprintf(g_root, sizeof g_root, "%s/packsign.%d", argv[1], (int)getpid());
    mkdir(g_root, 0700);
    snprintf(sub, sizeof sub, "%s/t", g_root);
    mkdir(sub, 0700);
    /* each leg gets its own fixture root (legs tamper shared names). */
    {
        char save[1024];
        snprintf(save, sizeof save, "%s", g_root);
#define LEG(d, fn)                                                             \
    do {                                                                       \
        snprintf(g_root, sizeof g_root, "%s/" d, save);                        \
        mkdir(g_root, 0700);                                                   \
        fn;                                                                    \
        snprintf(g_root, sizeof g_root, "%s", save);                           \
    } while (0)
        LEG("rt", test_round_trip());
        LEG("kr", test_keyring());
        LEG("corpus", test_sig_corpus());
        LEG("gate", test_gate());
        LEG("names", test_keygen_names());
#undef LEG
    }
    test_vectors();
    test_hex();
    printf("%d checks, %d failures\n", checks, fails);
    printf(fails ? "FAIL: pack_sign_test\n" : "PASS: pack_sign_test\n");
    return fails != 0;
}
