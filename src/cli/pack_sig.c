/* pack_sig.c — pack manifest signatures v1 (WP203). See pack_sig.h.
 *
 * What this file does, in order: strict readers for the three new file
 * shapes (sig sidecar, key files, integrity line); a sorted BLAKE3 walk
 * binding every helper file's bytes; sign (bind then ed25519) and verify
 * (parse, bind-check, keyring-match, ed25519); keygen; and the install
 * gate that composes missing/bad with -y/-n/--skip-signature-verification.
 *
 * What it does NOT do: touch the codec registry, the sweep, or any exec
 * path (install-gate only — pack_sig.h says why); invent PKI, revocation,
 * or timestamps (filed as follow-up, not built); shell out (no fork/exec
 * anywhere in this file).
 */
#define _CRT_SECURE_NO_WARNINGS

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "blake3.h"
#include "ed25519.h"
#include "pack_sig.h"

#define SIG_FILE_NAME "manifest.sig"
#define MANIFEST_NAME "manifest"
#define INTEGRITY_KEY "integrity"

/* Caps: every new surface here reads untrusted bytes (packs arrive over
 * the network), so every read is bounded and every length comes from a
 * constant, never from the input. A manifest above 1 MiB or a sig/key
 * file above 512 bytes is refused, not truncated: silent truncation
 * would verify a prefix the signer never saw. */
#define MANIFEST_MAX  (1024u * 1024u)
#define SMALLFILE_MAX 512u
#define PATH_CAP      4096u

static void set_err(char *err, size_t cap, const char *msg,
                    const char *detail)
{
    if (!err || cap == 0)
        return;
    if (detail && detail[0])
        snprintf(err, cap, "%s: %s", msg, detail);
    else
        snprintf(err, cap, "%s", msg);
}

/* Bounded whole-file read. Returns malloc'd NUL-terminated bytes with
 * *len_out set, or NULL with err set (missing, too big, unreadable).
 * Caller frees. */
static char *read_bounded(const char *path, size_t cap, size_t *len_out,
                          char *err, size_t errcap)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;

    if (!f) {
        set_err(err, errcap, "cannot open", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        set_err(err, errcap, "cannot seek", path);
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0 || (size_t)n > cap) {
        set_err(err, errcap, "file too large, refusing (not truncating)",
                path);
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        set_err(err, errcap, "out of memory", path);
        fclose(f);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        set_err(err, errcap, "short read", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    if (len_out)
        *len_out = (size_t)n;
    return buf;
}

/* ---- strict small-file shapes ----
 *
 * The sig sidecar and the key files are single-line, fixed-field formats.
 * The parser takes exactly one shape and refuses everything else with a
 * reason: odd lengths, long/short fields, extra lines, missing newline
 * discipline — all BAD, never "best effort". Field lengths are constants
 * (64/128 hex chars); no count is derived from untrusted bytes.
 */

/* Expect `tag <hex2*want>`: split buf (NUL-terminated, one line + \n)
 * into hex field. Returns 0 with out filled, -1 with err set. */
static int parse_tagged_hex(const char *buf, const char *tag,
                            unsigned char *out, size_t want,
                            char *err, size_t errcap, const char *path)
{
    size_t taglen = strlen(tag);
    const char *p;
    char hex[256 + 1];

    if (want * 2 > sizeof hex - 1) {
        set_err(err, errcap, "internal: hex field too wide", path);
        return -1;
    }
    if (strncmp(buf, tag, taglen) != 0 || buf[taglen] != ' ') {
        set_err(err, errcap, "bad envelope tag (want 'tag <hex>')", path);
        return -1;
    }
    p = buf + taglen + 1;
    /* one line only: the hex field runs to the newline, and the newline
     * must be the last byte. */
    {
        const char *nl = strchr(p, '\n');
        if (!nl || nl[1] != 0 || (size_t)(nl - p) != want * 2) {
            set_err(err, errcap, "bad envelope shape (one line, exact hex)",
                    path);
            return -1;
        }
        memcpy(hex, p, want * 2);
        hex[want * 2] = 0;
    }
    if (invfs_hex_decode(hex, out, want) != 0) {
        set_err(err, errcap, "bad envelope hex", path);
        return -1;
    }
    return 0;
}

/* manifest.sig: `sig-ed25519 <pk-hex64> <sig-hex128>\n`, nothing else. */
static int parse_sig_file(const char *buf, size_t len,
                          unsigned char pk[32], unsigned char sig[64],
                          char *err, size_t errcap, const char *path)
{
    const char *tag = "sig-ed25519 ";
    size_t taglen = strlen(tag);
    const char *sp;
    char pkhex[64 + 1], sighex[128 + 1];

    (void)len;
    if (strncmp(buf, tag, taglen) != 0) {
        set_err(err, errcap,
                "bad signature envelope (want 'sig-ed25519 <key> <sig>')",
                path);
        return -1;
    }
    /* exactly: 64 hex, space, 128 hex, newline, end. */
    if (strlen(buf) != taglen + 64 + 1 + 128 + 1) {
        set_err(err, errcap, "bad signature envelope length", path);
        return -1;
    }
    sp = buf + taglen;
    if (sp[64] != ' ' || buf[taglen + 64 + 1 + 128] != '\n') {
        set_err(err, errcap, "bad signature envelope shape", path);
        return -1;
    }
    memcpy(pkhex, sp, 64);
    pkhex[64] = 0;
    memcpy(sighex, sp + 65, 128);
    sighex[128] = 0;
    if (invfs_hex_decode(pkhex, pk, 32) != 0 ||
        invfs_hex_decode(sighex, sig, 64) != 0) {
        set_err(err, errcap, "bad signature envelope hex", path);
        return -1;
    }
    return 0;
}

/* ---- integrity binding ----
 *
 * integrity = BLAKE3 over a sorted walk: for every entry under the pack
 * dir, feed relpath bytes + NUL, then per kind:
 *   regular file (not manifest.sig): 8-byte LE size + raw content
 *   symlink:                         "link:" + readlink target + NUL
 *   anything else (dir, fifo, socket, device): kind byte only
 * manifest.sig itself is skipped (it signs the manifest; including it
 * would make signing a fixed point). The manifest is skipped too: it is
 * covered directly by ed25519 over its exact bytes (including the
 * integrity line), and hashing it here would bind the pre-rewrite bytes
 * at sign time against the post-rewrite bytes at verify time — a
 * mismatch on every signed pack.
 */

typedef struct {
    char *rel; /* malloc'd relative path */
} entry;

static int entry_cmp(const void *a, const void *b)
{
    return strcmp(((const entry *)a)->rel, ((const entry *)b)->rel);
}

/* Collect relative paths under <dir> (rel_prefix "" at top). Grows *list.
 * Returns 0, -1 with err set. Symlink loops cannot recurse: symlinks are
 * never descended (lstat + S_ISLNK check before recursion). */
static int collect_entries(const char *dir, const char *rel_prefix,
                           entry **list, size_t *n, size_t *cap,
                           char *err, size_t errcap)
{
    DIR *d = opendir(dir);
    struct dirent *de;

    if (!d) {
        set_err(err, errcap, "cannot list pack dir", dir);
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        char full[PATH_CAP], rel[PATH_CAP];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (snprintf(rel, sizeof rel, "%s%s%s", rel_prefix,
                     rel_prefix[0] ? "/" : "", de->d_name) >= (int)sizeof rel ||
            snprintf(full, sizeof full, "%s/%s", dir,
                     de->d_name) >= (int)sizeof full) {
            set_err(err, errcap, "pack path too long", de->d_name);
            closedir(d);
            return -1;
        }
        if (*n >= *cap) {
            size_t ncap = *cap ? *cap * 2 : 64;
            entry *nl = (entry *)realloc(*list, ncap * sizeof *nl);
            if (!nl) {
                set_err(err, errcap, "out of memory", dir);
                closedir(d);
                return -1;
            }
            *list = nl;
            *cap = ncap;
        }
        (*list)[*n].rel = strdup(rel);
        if (!(*list)[*n].rel) {
            set_err(err, errcap, "out of memory", dir);
            closedir(d);
            return -1;
        }
        (*n)++;
        if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode) &&
            !S_ISLNK(st.st_mode)) {
            /* recurse with the stack-built prefix (no chdir, no escapes:
             * names come from the directory itself). */
            if (collect_entries(full, rel, list, n, cap, err, errcap) != 0) {
                closedir(d);
                return -1;
            }
        }
    }
    closedir(d);
    return 0;
}

static void feed_u64le(blake3_hasher *h, unsigned long long v)
{
    unsigned char b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (unsigned char)(v & 255);
        v >>= 8;
    }
    blake3_hasher_update(h, b, 8);
}

int pack_sig_integrity_compute(const char *packdir,
                               char hex_out[65],
                               char *err, size_t errcap)
{
    entry *list = NULL;
    size_t n = 0, cap = 0, i;
    blake3_hasher h;
    unsigned char sum[32];
    int rc = -1;

    if (collect_entries(packdir, "", &list, &n, &cap, err, errcap) != 0)
        return -1;
    qsort(list, n, sizeof *list, entry_cmp);
    blake3_hasher_init(&h);
    for (i = 0; i < n; i++) {
        char full[PATH_CAP];
        struct stat st;
        if (!strcmp(list[i].rel, SIG_FILE_NAME) ||
            !strcmp(list[i].rel, MANIFEST_NAME))
            continue; /* sidecar + manifest: covered by ed25519, not here */
        blake3_hasher_update(&h, list[i].rel, strlen(list[i].rel) + 1);
        if (snprintf(full, sizeof full, "%s/%s", packdir,
                     list[i].rel) >= (int)sizeof full) {
            set_err(err, errcap, "pack path too long", list[i].rel);
            goto out;
        }
        if (lstat(full, &st) != 0) {
            set_err(err, errcap, "pack entry vanished mid-walk",
                    list[i].rel);
            goto out;
        }
        if (S_ISLNK(st.st_mode)) {
            /* hash the link text, never the target: no following, so no
             * escape from the pack dir, no TOCTOU through replacement. */
            char tgt[PATH_CAP];
            ssize_t k = readlink(full, tgt, sizeof tgt - 1);
            if (k < 0) {
                set_err(err, errcap, "cannot read link", list[i].rel);
                goto out;
            }
            tgt[k] = 0;
            blake3_hasher_update(&h, "link:", 5);
            blake3_hasher_update(&h, tgt, (size_t)k + 1);
        } else if (S_ISREG(st.st_mode)) {
            FILE *f = fopen(full, "rb");
            unsigned char chunk[65536];
            size_t k;
            if (!f) {
                set_err(err, errcap, "cannot read pack file", list[i].rel);
                goto out;
            }
            feed_u64le(&h, (unsigned long long)st.st_size);
            while ((k = fread(chunk, 1, sizeof chunk, f)) > 0)
                blake3_hasher_update(&h, chunk, k);
            if (ferror(f)) {
                set_err(err, errcap, "short read on pack file", list[i].rel);
                fclose(f);
                goto out;
            }
            fclose(f);
        } else {
            /* dirs and odd nodes bind name + kind only (S_ISDIR etc.).
             * Presence, not content: content lives in the walked files. */
            unsigned char kind = S_ISDIR(st.st_mode) ? 'd' : 's';
            blake3_hasher_update(&h, &kind, 1);
        }
    }
    blake3_hasher_finalize(&h, sum, 32);
    if (invfs_hex_encode(sum, 32, hex_out, 65) != 0) {
        set_err(err, errcap, "internal: hex buffer too small", packdir);
        goto out;
    }
    rc = 0;
out:
    for (i = 0; i < n; i++)
        free(list[i].rel);
    free(list);
    return rc;
}

/* Find the integrity line: exactly `integrity = <hex64>` (spaces around
 * the value tolerated, like the manifest parser). Returns: 1 found (hex
 * copied), 0 absent, -1 duplicate/malformed with err set. */
static int find_integrity(const char *manifest, char hex64[65],
                          char *err, size_t errcap)
{
    int found = 0;
    const char *p = manifest;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        /* line must start with the key (no leading-space form: the
         * signer writes it, and the signer writes it flush). */
        if (linelen > strlen(INTEGRITY_KEY) &&
            !memcmp(p, INTEGRITY_KEY, strlen(INTEGRITY_KEY))) {
            const char *v = p + strlen(INTEGRITY_KEY);
            while (v < p + linelen && (*v == ' ' || *v == '\t'))
                v++;
            if (v < p + linelen && *v == '=') {
                char val[128];
                size_t vlen;
                v++;
                while (v < p + linelen && (*v == ' ' || *v == '\t'))
                    v++;
                vlen = (size_t)((p + linelen) - v);
                while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t' ||
                                    v[vlen - 1] == '\r'))
                    vlen--;
                if (found) {
                    set_err(err, errcap,
                            "duplicate integrity line (ambiguous binding)",
                            NULL);
                    return -1;
                }
                if (vlen != 64 || vlen >= sizeof val) {
                    set_err(err, errcap,
                            "malformed integrity line (want 64 hex chars)",
                            NULL);
                    return -1;
                }
                memcpy(val, v, vlen);
                val[vlen] = 0;
                {
                    unsigned char tmp[32];
                    if (invfs_hex_decode(val, tmp, 32) != 0) {
                        set_err(err, errcap,
                                "malformed integrity line (non-hex)", NULL);
                        return -1;
                    }
                }
                memcpy(hex64, val, 65);
                found = 1;
            }
        }
        if (!eol)
            break;
        p = eol + 1;
    }
    return found;
}

static int path_join(char *dst, size_t cap, const char *a, const char *b,
                     char *err, size_t errcap)
{
    if (snprintf(dst, cap, "%s/%s", a, b) >= (int)cap) {
        set_err(err, errcap, "path too long", b);
        return -1;
    }
    return 0;
}

/* Load one key file shape (`tag <hex>`). Missing file: -1 with err set
 * (callers decide whether that is fatal). */
static int load_key_file(const char *path, const char *tag,
                         unsigned char *out, size_t want,
                         char *err, size_t errcap)
{
    char *buf = read_bounded(path, SMALLFILE_MAX, NULL, err, errcap);
    int rc;
    if (!buf)
        return -1;
    rc = parse_tagged_hex(buf, tag, out, want, err, errcap, path);
    free(buf);
    return rc;
}

/* True iff <keyring_dir>/<*.pub> holds pk. Malformed .pub files are
 * IGNORED (ssh-known_hosts convention): one broken key must not veto
 * installs signed by the good ones. An unreadable keyring directory is
 * an error only when we need a key and found none — reported as "no
 * trusted key", which is the actionable form either way. */
static int keyring_has(const char *keyring_dir, const unsigned char pk[32])
{
    DIR *d = opendir(keyring_dir);
    struct dirent *de;
    int hit = 0;

    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        char full[PATH_CAP];
        struct stat st;
        char *buf;
        unsigned char k[32];
        if (l < 5 || strcmp(de->d_name + l - 4, ".pub") != 0)
            continue;
        if (snprintf(full, sizeof full, "%s/%s", keyring_dir,
                     de->d_name) >= (int)sizeof full)
            continue;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        buf = read_bounded(full, SMALLFILE_MAX, NULL, NULL, 0);
        if (!buf)
            continue;
        if (parse_tagged_hex(buf, "ed25519-pub", k, 32, NULL, 0, full) == 0 &&
            !memcmp(k, pk, 32))
            hit = 1;
        free(buf);
        if (hit)
            break;
    }
    closedir(d);
    return hit;
}

pack_sig_status pack_sig_verify_dir(const char *packdir,
                                    const char *keyring_dir,
                                    char *err, size_t errcap)
{
    char sigpath[PATH_CAP], manpath[PATH_CAP];
    struct stat st;
    unsigned char pk[32], sig[64];
    char *sigbuf = NULL, *manifest = NULL;
    char want_hex[65], got_hex[65];

    if (path_join(sigpath, sizeof sigpath, packdir, SIG_FILE_NAME,
                  err, errcap) != 0 ||
        path_join(manpath, sizeof manpath, packdir, MANIFEST_NAME,
                  err, errcap) != 0)
        return PACK_SIG_BAD;

    /* 1. Sidecar present? Absence is UNSIGNED (the prompt's case), not
     * evidence of tampering. */
    if (stat(sigpath, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            set_err(err, errcap, "pack is not signed (no manifest.sig)",
                    packdir);
            return PACK_SIG_UNSIGNED;
        }
        set_err(err, errcap, "cannot stat manifest.sig", sigpath);
        return PACK_SIG_BAD;
    }

    /* 2. Strict envelope parse. Anything off-shape is BAD: a mangled
     * sidecar is attack-shaped, never "missing". */
    sigbuf = read_bounded(sigpath, SMALLFILE_MAX, NULL, err, errcap);
    if (!sigbuf)
        return PACK_SIG_BAD;
    if (parse_sig_file(sigbuf, 0, pk, sig, err, errcap, sigpath) != 0) {
        free(sigbuf);
        return PACK_SIG_BAD;
    }
    free(sigbuf);

    /* 3. The manifest must carry exactly one integrity binding. A
     * signature over a manifest that binds no helper bytes proves
     * nothing about the helpers, so it refuses. */
    manifest = read_bounded(manpath, MANIFEST_MAX, NULL, err, errcap);
    if (!manifest)
        return PACK_SIG_BAD;
    {
        int f = find_integrity(manifest, want_hex, err, errcap);
        if (f <= 0) {
            if (f == 0)
                set_err(err, errcap,
                        "signed manifest has no integrity binding (refusing)",
                        manpath);
            free(manifest);
            return PACK_SIG_BAD;
        }
    }

    /* 4. Helper bytes must match the binding (catches tampered helpers
     * with an untouched manifest+sidecar). */
    if (pack_sig_integrity_compute(packdir, got_hex, err, errcap) != 0) {
        free(manifest);
        return PACK_SIG_BAD;
    }
    if (strcmp(want_hex, got_hex) != 0) {
        set_err(err, errcap,
                "helper contents do not match manifest integrity (tampered helpers?)",
                packdir);
        free(manifest);
        return PACK_SIG_BAD;
    }

    /* 5. The signing key must be in the keyring. Signed-by-unknown is
     * unverifiable, and unverifiable refuses (fail closed). */
    if (!keyring_has(keyring_dir ? keyring_dir : "", pk)) {
        set_err(err, errcap,
                "no trusted key for this signature (key not in keyring)",
                keyring_dir);
        free(manifest);
        return PACK_SIG_BAD;
    }

    /* 6. ed25519 over the exact manifest bytes. */
    if (invfs_ed25519_verify(sig, (const unsigned char *)manifest,
                             (unsigned long long)strlen(manifest),
                             pk) != 0) {
        set_err(err, errcap, "bad signature (manifest tampered or wrong key)",
                manpath);
        free(manifest);
        return PACK_SIG_BAD;
    }
    free(manifest);
    if (err && errcap)
        err[0] = 0;
    return PACK_SIG_OK;
}

int pack_sig_sign_dir(const char *packdir, const char *secfile,
                      char *err, size_t errcap)
{
    char manpath[PATH_CAP], sigpath[PATH_CAP];
    unsigned char sk[64], pk[32], sig[64];
    char *manifest = NULL, *rebuilt = NULL;
    size_t manlen = 0;
    char hex[65], pkhex[65], sighex[129];
    FILE *f;

    if (path_join(manpath, sizeof manpath, packdir, MANIFEST_NAME,
                  err, errcap) != 0 ||
        path_join(sigpath, sizeof sigpath, packdir, SIG_FILE_NAME,
                  err, errcap) != 0)
        return -1;
    if (load_key_file(secfile, "ed25519-sec", sk, 64, err, errcap) != 0)
        return -1;
    memcpy(pk, sk + 32, 32); /* TweetNaCl sk layout: seed || pub */

    /* 1. Bind the current helper bytes. */
    if (pack_sig_integrity_compute(packdir, hex, err, errcap) != 0)
        return -1;

    /* 2. Rewrite the manifest: byte-identical except the integrity line
     * is replaced (or appended). Lines above 1023 bytes refuse: the
     * line-oriented rewrite below could not preserve them. */
    manifest = read_bounded(manpath, MANIFEST_MAX, &manlen, err, errcap);
    if (!manifest)
        return -1;
    {
        /* worst case: old manifest + one new line. */
        rebuilt = (char *)malloc(manlen + 128);
        size_t off = 0;
        const char *p = manifest;
        if (!rebuilt) {
            set_err(err, errcap, "out of memory", manpath);
            free(manifest);
            return -1;
        }
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t ll = eol ? (size_t)(eol - p) : strlen(p);
            int is_integ = 0;
            if (ll > strlen(INTEGRITY_KEY) &&
                !memcmp(p, INTEGRITY_KEY, strlen(INTEGRITY_KEY))) {
                const char *v = p + strlen(INTEGRITY_KEY);
                while (v < p + ll && (*v == ' ' || *v == '\t'))
                    v++;
                if (v < p + ll && *v == '=')
                    is_integ = 1;
            }
            if (ll > 1023) {
                set_err(err, errcap,
                        "manifest has an overlong line; refusing to rewrite",
                        manpath);
                free(rebuilt);
                free(manifest);
                return -1;
            }
            if (!is_integ) {
                memcpy(rebuilt + off, p, ll);
                off += ll;
                rebuilt[off++] = '\n';
            }
            if (!eol)
                break;
            p = eol + 1;
        }
        off += (size_t)snprintf(rebuilt + off, 128, "%s = %s\n",
                                INTEGRITY_KEY, hex);
        f = fopen(manpath, "wb");
        if (!f) {
            set_err(err, errcap, "cannot rewrite manifest", manpath);
            free(rebuilt);
            free(manifest);
            return -1;
        }
        if (fwrite(rebuilt, 1, off, f) != off) {
            set_err(err, errcap, "short write on manifest", manpath);
            fclose(f);
            free(rebuilt);
            free(manifest);
            return -1;
        }
        fclose(f);
        free(rebuilt);
    }
    free(manifest);
    manifest = NULL;

    /* 3. Sign the exact bytes just written, sidecar beside the manifest. */
    manifest = read_bounded(manpath, MANIFEST_MAX, &manlen, err, errcap);
    if (!manifest)
        return -1;
    if (invfs_ed25519_sign(sig, (const unsigned char *)manifest,
                           (unsigned long long)manlen, sk) != 0) {
        set_err(err, errcap, "signing failed", manpath);
        free(manifest);
        return -1;
    }
    free(manifest);
    invfs_hex_encode(pk, 32, pkhex, sizeof pkhex);
    invfs_hex_encode(sig, 64, sighex, sizeof sighex);
    f = fopen(sigpath, "wb");
    if (!f) {
        set_err(err, errcap, "cannot write manifest.sig", sigpath);
        return -1;
    }
    fprintf(f, "sig-ed25519 %s %s\n", pkhex, sighex);
    if (fclose(f) != 0) {
        set_err(err, errcap, "short write on manifest.sig", sigpath);
        return -1;
    }
    if (err && errcap)
        err[0] = 0;
    return 0;
}

int pack_sig_keygen(const char *keyring_dir, const char *name,
                    char *err, size_t errcap)
{
    unsigned char pk[32], sk[64];
    char pkhex[65], skhex[129];
    char pubpath[PATH_CAP], secpath[PATH_CAP];
    size_t i, nl;
    FILE *f;

    if (!name || !(nl = strlen(name)) || nl > 64) {
        set_err(err, errcap, "bad key name (1..64 chars)", name);
        return -1;
    }
    for (i = 0; i < nl; i++) {
        char c = name[i];
        if (!(c == '.' || c == '_' || c == '-' ||
              (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z'))) {
            set_err(err, errcap,
                    "bad key name (letters, digits, . _ - only)", name);
            return -1;
        }
    }
    {
        char pub[PATH_CAP], sec[PATH_CAP];
        if (snprintf(pub, sizeof pub, "%s.pub", name) >= (int)sizeof pub ||
            snprintf(sec, sizeof sec, "%s.sec", name) >= (int)sizeof sec ||
            path_join(pubpath, sizeof pubpath, keyring_dir, pub,
                      err, errcap) != 0 ||
            path_join(secpath, sizeof secpath, keyring_dir, sec,
                      err, errcap) != 0)
            return -1;
    }
    {
        /* best-effort mkdir -p; failures surface at fopen below with
         * the real errno. */
        char tmp[PATH_CAP];
        snprintf(tmp, sizeof tmp, "%s", keyring_dir);
        {
            char *slash = tmp;
            /* create each prefix: $HOME, $HOME/.config, full path. */
            for (;;) {
                slash = strchr(slash + 1, '/');
                if (slash) {
                    char save = *slash;
                    *slash = 0;
                    mkdir(tmp, 0700);
                    *slash = save;
                } else {
                    mkdir(tmp, 0700);
                    break;
                }
            }
        }
    }
    if (invfs_ed25519_keypair(pk, sk) != 0) {
        set_err(err, errcap, "key generation failed (no OS randomness)",
                NULL);
        return -1;
    }
    invfs_hex_encode(pk, 32, pkhex, sizeof pkhex);
    invfs_hex_encode(sk, 64, skhex, sizeof skhex);
    f = fopen(secpath, "wb");
    if (!f) {
        set_err(err, errcap, "cannot write key file", secpath);
        goto fail;
    }
    /* 0600 before a single secret byte lands (umask is not the plan). */
    fchmod(fileno(f), 0600);
    fprintf(f, "ed25519-sec %s\n", skhex);
    if (fclose(f) != 0) {
        set_err(err, errcap, "short write on key file", secpath);
        goto fail;
    }
    f = fopen(pubpath, "wb");
    if (!f) {
        set_err(err, errcap, "cannot write key file", pubpath);
        unlink(secpath);
        goto fail;
    }
    fprintf(f, "ed25519-pub %s\n", pkhex);
    if (fclose(f) != 0) {
        set_err(err, errcap, "short write on key file", pubpath);
        unlink(secpath);
        unlink(pubpath);
        goto fail;
    }
    memset(sk, 0, sizeof sk);
    if (err && errcap)
        err[0] = 0;
    return 0;
fail:
    memset(sk, 0, sizeof sk);
    return -1;
}

int pack_sig_default_keyring(char *out, size_t outcap,
                             char *err, size_t errcap)
{
    const char *env = getenv("INVFS_KEYRING");
    const char *home;

    if (env && env[0]) {
        if (snprintf(out, outcap, "%s", env) >= (int)outcap) {
            set_err(err, errcap, "INVFS_KEYRING too long", env);
            return -1;
        }
        return 0;
    }
    home = getenv("HOME");
    if (!home || !home[0]) {
        set_err(err, errcap,
                "no keyring: INVFS_KEYRING unset and HOME unset", NULL);
        return -1;
    }
    if (snprintf(out, outcap, "%s/.config/invfs/keys",
                 home) >= (int)outcap) {
        set_err(err, errcap, "keyring path too long", home);
        return -1;
    }
    return 0;
}

int pack_sig_prompt_stdio(const char *packname, void *ctx)
{
    char line[128];

    (void)ctx;
    fprintf(stderr,
            "pack '%s' is not signed. Install unsigned pack? [y/N]: ",
            packname ? packname : "?");
    fflush(stderr);
    if (!fgets(line, sizeof line, stdin))
        return 0; /* closed stdin aborts (fail closed) */
    return line[0] == 'y' || line[0] == 'Y';
}

int pack_sig_gate_install(const char *packdir, const char *packname,
                          const char *keyring_dir,
                          int auto_yes, int dry_run, int skip_flag,
                          pack_sig_prompt_fn prompt, void *ctx,
                          char *err, size_t errcap)
{
    pack_sig_status st;
    char why[512];

    if (!prompt)
        prompt = pack_sig_prompt_stdio;
    st = pack_sig_verify_dir(packdir, keyring_dir, why, sizeof why);
    if (st == PACK_SIG_OK) {
        printf("pack '%s': signature OK\n", packname ? packname : "?");
        if (err && errcap)
            err[0] = 0;
        return 0;
    }
    if (st == PACK_SIG_BAD) {
        /* BAD is attack evidence: no knob proceeds. The -y/-n/skip
         * flags are about ABSENCE of a signature, never about a
         * signature that fails. */
        set_err(err, errcap, "refusing to install pack", why);
        return 1;
    }
    /* UNSIGNED from here on. */
    if (skip_flag) {
        fprintf(stderr, "pack '%s': installing UNSIGNED "
                "(--skip-signature-verification)\n",
                packname ? packname : "?");
        if (err && errcap)
            err[0] = 0;
        return 0;
    }
    if (auto_yes) {
        fprintf(stderr, "pack '%s': installing UNSIGNED pack (-y)\n",
                packname ? packname : "?");
        if (err && errcap)
            err[0] = 0;
        return 0;
    }
    if (dry_run) {
        /* -n never installs anything, so there is nothing to confirm:
         * report what a real run would do. */
        printf("would install UNSIGNED pack '%s' "
               "(no manifest.sig; -y or --skip-signature-verification "
               "to confirm)\n", packname ? packname : "?");
        if (err && errcap)
            err[0] = 0;
        return 0;
    }
    if (prompt(packname, ctx)) {
        if (err && errcap)
            err[0] = 0;
        return 0;
    }
    set_err(err, errcap, "aborted: pack is not signed",
            "use --skip-signature-verification to install unsigned");
    return 1;
}
