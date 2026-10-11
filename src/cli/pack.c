/*
 * pack.c — codec-pack registry manager for InvariantFS
 *
 *   invfs-pack list [--volume IMG] [--installed|-i] [family]
 *   invfs-pack info <pack>
 *   invfs-pack install [-y|-n] [--skip-signature-verification] [--volume IMG] <pack|./path>
 *   invfs-pack remove <pack>
 *   invfs-pack verify [<pack>]
 *   invfs-pack keygen [--keyring DIR] [name]
 *   invfs-pack sign --key SECFILE <pack|./path>
 *   invfs-pack alternatives <family>
 *   invfs-pack use <family> <pack>
 *   invfs-pack where
 *
 * Pack signatures v1 (WP203): `sign` binds the manifest (+ its integrity
 * line over every helper file) with ed25519 into manifest.sig; `install`
 * verifies and REFUSES a bad signature always, prompting on a missing
 * one. See src/cli/pack_sig.h for the trust model and file formats.
 *
 * Registry fetch (WP608): `install <name>[@<version>]` (no slash) falls
 * back to <repo>/codecpacks/index.json when the name is not installed
 * locally; `--repo file://...|https://...` overrides the built-in
 * default. Fetched files are sha256-verified against the pack listing
 * and pass the same signature gate; no bypass. `--release` switches the
 * same resolve (explicit @version or latest from index.json, no GitHub
<<<<<<< HEAD
 * API) to the registry's GitHub release tarballs
 * (<name>-<version>-<platform>.tar.gz, see tools/package-release.sh in
 * the registry repo): the tarball is unpacked into the tool scratch and
 * the extracted files are verified against the raw-tree sha256 listing
 * plus the same WP203 gate — a bad release refuses exactly like a bad
 * raw fetch.
=======
 * API) to the registry's GitHub release .ivpack bundles
 * (<name>-<version>.ivpack under tag v1.1.0, see
 * registry/docs/IVPACK-RELEASES.md): the bundle is unzipped (fork+execvp
 * unzip, entry names validated before extract) into the tool scratch and
 * the extracted files are verified against the raw-tree sha256 listing
 * plus the same WP203 gate — a bad bundle refuses exactly like a bad
 * raw fetch. A pack with no .ivpack asset falls back to the round-2
 * release tarball (<name>-<version>-<platform>.tar.gz) with one loud
 * line; nothing else falls back. v1.1.0 ships no manifest.sig sidecars,
 * so .ivpack installs take the gate's missing-signature path (prompt;
 * -y proceeds, -n reports).
>>>>>>> wp/608-pack-registry-fetch
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include "blake3.h"
#include "pack_sig.h"
#include "tool_scratch.h"
#include "volume.h"   /* vol_open for --volume (links CORE_O) */

#define HOST_ROOT "/.invariantfs/codecpacks"
#define LEGACY_ROOT "/.invfs/codecpacks"
#define USR_ROOT "/usr/lib/invfs/codecpacks"
#define PACK_CONF "/.invariantfs/codecpacks/packs.conf"

#define MAX_FIELDS 64
#define MAX_LINE 1024
#define MAX_PATH 1024
#define MAX_PACKS 256

/* WP608: the registry is the only network source invfs-pack speaks to.
 * A bare `install <name>` (no slash) that is not installed locally
 * resolves through <repo>/codecpacks/index.json, downloads the pack's
 * files into the tool scratch, sha256-verifies every file against the
 * pack's sha256 listing, and hands the staged tree to the EXISTING local
 * install path below — including the WP203 signature gate. Fetched packs
 * get no bypass: a bad signature refuses, a missing one prompts.
 * file:// repos work offline (plain file copies, no curl); https://
 * repos download through curl (execvp, never installed).
 *
 * Release mode (round 2, --release) keeps this resolve and only changes
<<<<<<< HEAD
 * the download: the tarball comes from the registry's GitHub release
 * assets (DEFAULT_RELEASE_BASE/releases/download/<tag>/, or
 * <repo>/releases/download/<tag>/ under an explicit --repo), is
 * unpacked with tar into the same scratch, and is verified against the
 * raw-tree sha256 listing plus the same gate. */
#define DEFAULT_REPO \
    "https://raw.githubusercontent.com/AsmanovLev/InvariantFS-registry/main"
/* Release-asset base: build.yml uploads the dist tarballs as release assets
 * on `v*` tags, so a published tarball lives at
 * <base>/releases/download/<tag>/<name>-<version>-<platform>.tar.gz. */
#define DEFAULT_RELEASE_BASE \
    "https://github.com/AsmanovLev/InvariantFS-registry"
=======
 * the download: round 3 prefers the .ivpack bundle
 * (<name>-<version>.ivpack, default tag v1.1.0, --release-tag overrides)
 * and falls back to the round-2 tarball (default tag v<version>) with
 * one loud line only when the pack has no .ivpack asset. Both come
 * from the registry's GitHub release assets
 * (DEFAULT_RELEASE_BASE/releases/download/<tag>/, or
 * <repo>/releases/download/<tag>/ under an explicit --repo), are
 * unpacked (unzip/tar) into the same scratch, and are verified against
 * the raw-tree sha256 listing plus the same gate. */
#define DEFAULT_REPO \
    "https://raw.githubusercontent.com/AsmanovLev/InvariantFS-registry/main"
/* Release-asset base: the registry publishes .ivpack bundles (v1.1.0+)
 * and the older dist tarballs as release assets on version tags, so a
 * published asset lives at
 * <base>/releases/download/<tag>/<name>-<version>.ivpack (or
 * <name>-<version>-<platform>.tar.gz for the tarball fallback). */
#define DEFAULT_RELEASE_BASE \
    "https://github.com/AsmanovLev/InvariantFS-registry"
/* Default tag carrying the .ivpack assets (registry/docs/IVPACK-RELEASES.md:
 * one tag carries all packs; v1.1.0 is the .ivpack deliverable). */
#define DEFAULT_IVPACK_TAG "v1.1.0"
>>>>>>> wp/608-pack-registry-fetch
/* Cap on files named by one sha256 listing (release extras check). */
#define MAX_REL_FILES 1024

/* What the fetch resolves: pinned version and/or release-asset mode. */
typedef struct {
    const char *want_ver;   /* NULL/"" = latest from index.json */
<<<<<<< HEAD
    int release;            /* 1 = fetch the release tarball */
    const char *rel_tag;    /* NULL/"" = default v<version> */
=======
    int release;            /* 1 = fetch the release asset (.ivpack first,
                             * tarball fallback when the pack has none) */
    const char *rel_tag;    /* NULL/"" = v1.1.0 for .ivpack, v<version>
                             * for the tarball fallback */
>>>>>>> wp/608-pack-registry-fetch
    const char *platform;   /* NULL/"" = default <uname>-linux-musl */
} fetch_opts;
#define MAX_REG_PACKS 512

static int rmtree(const char *path); /* defined with the volume helpers */
static int is_codecpack(const char *dir); /* manifest present in dir */
<<<<<<< HEAD
=======
static int read_source_marker(const char *path, char *out,
                              size_t cap); /* defined with install below */
>>>>>>> wp/608-pack-registry-fetch

typedef struct {
    char key[128];
    char val[512];
} field;

typedef struct {
    char name[128];
    char path[MAX_PATH + 64];
    char algo[16];
    char pack_version[16];
    char codec_id[16];
    char version[16];
    char min_read[16];
    char family[64];
    char category[32];
    char provides[128];
    char replaces[128];
    char conflicts[128];
    char priority[16];
    char caps[128];
    char type[32];
    char requires[256];
    char integrity[128];
    int  field_count;
    field fields[MAX_FIELDS];
} pack_info;

/* resolve the env var, splitting on ':' */
static const char *env_codecpacks(void)
{
    return getenv("INVFS_CODECPACKS");
}

/* compute blake3 of a file; hex digest in hex_out (>=65 bytes) */

/* strip trailing \n and \r */
static void strip_nl(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == '\n' || s[l-1] == '\r')) s[--l] = 0;
}

/* parse a manifest file; fills pack_info fields */
static int parse_manifest_stream(FILE *f, pack_info *pi)
{
    char line[MAX_LINE];
    pi->field_count = 0;
    while (fgets(line, sizeof line, f)) {
        strip_nl(line);
        if (line[0] == '#' || line[0] == 0) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        char *ke = k + strlen(k) - 1;
        while (ke > k && (*ke == ' ' || *ke == '\t')) *ke-- = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (pi->field_count < MAX_FIELDS) {
            /* strncpy(127) does not terminate unless the source is short;
             * the value IS a C string later, so copy and cap by hand */
            size_t kl = strlen(k);
            if (kl > 127) kl = 127;
            memcpy(pi->fields[pi->field_count].key, k, kl);
            pi->fields[pi->field_count].key[kl] = 0;
            {
                size_t vl = strlen(v);
                if (vl > 511) vl = 511;
                memcpy(pi->fields[pi->field_count].val, v, vl);
                pi->fields[pi->field_count].val[vl] = 0;
            }
            pi->field_count++;
        }
        /* map well-known keys */
        if (strcmp(k, "name") == 0) {
            strncpy(pi->name, v, 127);
        } else if (strcmp(k, "algo") == 0) {
            strncpy(pi->algo, v, 15);
        } else if (strcmp(k, "pack_version") == 0) {
            strncpy(pi->pack_version, v, 15);
        } else if (strcmp(k, "codec_id") == 0) {
            strncpy(pi->codec_id, v, 15);
        } else if (strcmp(k, "version") == 0) {
            strncpy(pi->version, v, 15);
        } else if (strcmp(k, "min_read") == 0) {
            strncpy(pi->min_read, v, 15);
        } else if (strcmp(k, "family") == 0) {
            strncpy(pi->family, v, 63);
        } else if (strcmp(k, "category") == 0) {
            strncpy(pi->category, v, 31);
        } else if (strcmp(k, "provides") == 0) {
            strncpy(pi->provides, v, 127);
        } else if (strcmp(k, "replaces") == 0) {
            strncpy(pi->replaces, v, 127);
        } else if (strcmp(k, "conflicts") == 0) {
            strncpy(pi->conflicts, v, 127);
        } else if (strcmp(k, "priority") == 0) {
            strncpy(pi->priority, v, 15);
        } else if (strcmp(k, "caps") == 0) {
            strncpy(pi->caps, v, 127);
        } else if (strcmp(k, "type") == 0) {
            strncpy(pi->type, v, 31);
        } else if (strcmp(k, "requires") == 0) {
            strncpy(pi->requires, v, 255);
        } else if (strcmp(k, "integrity") == 0) {
            /* WP203: helper-content binding, signed via manifest.sig.
             * Parsed like any other field; enforced by pack_sig, not
             * by this parser (which stays total: unknown keys land in
             * fields[] and never fail the parse). */
            strncpy(pi->integrity, v, 127);
        }
    }
    return 0;
}

static int parse_manifest(const char *path, pack_info *pi)
{
    FILE *f = fopen(path, "r");
    int rc;
    if (!f) return -1;
    rc = parse_manifest_stream(f, pi);
    fclose(f);
    return rc;
}

/* snprintf a path, refusing on truncation. Every one of these strings is
 * fed to stat()/fopen(): a silently shortened path would open a different
 * file (or none) and report a wrong verdict. -1 = would not fit. */
static int pathf(char *dst, size_t cap, const char *fmt, const char *a,
                 const char *b)
{
    int n = b ? snprintf(dst, cap, fmt, a, b) : snprintf(dst, cap, fmt, a);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* ---- WP608 registry fetch (ported from the WP60 seed design) ---- */

typedef struct { int http; char base[MAX_PATH * 2]; } pack_repo;

static const char *repo_default(void)
{
    const char *e = getenv("INVFS_PACK_REPO");
    return (e && *e) ? e : DEFAULT_REPO;
}

static void repo_resolve(pack_repo *r, const char *spec)
{
    const char *base;
    if (!spec || !*spec)
        spec = repo_default();
    if (strncmp(spec, "file://", 7) == 0) {
        r->http = 0;
        base = spec + 7;
    } else if (strncmp(spec, "http://", 7) == 0 ||
               strncmp(spec, "https://", 8) == 0) {
        r->http = 1;
        base = spec;
    } else {
        /* bare path: a local directory, same as file:// without the scheme */
        r->http = 0;
        base = spec;
    }
    snprintf(r->base, sizeof r->base, "%s", base);
    {
        size_t l = strlen(r->base);
        while (l > 1 && r->base[l - 1] == '/')
            r->base[--l] = 0;
    }
}

/* join a repo base with a relative path; -1 = would not fit */
static int repof(char *dst, size_t cap, const pack_repo *r, const char *rel)
{
    int n = snprintf(dst, cap, "%s/%s", r->base, rel);
    return (n < 0 || (size_t)n >= (int)cap) ? -1 : 0;
}

static char *pack_read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    buf[n] = 0;
    if (len_out)
        *len_out = (size_t)n;
    return buf;
}

static int pack_copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out;
    char buf[65536];
    size_t r;
    if (!in)
        return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((r = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, r, out) != r) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    if (ferror(in)) {
        fclose(in);
        fclose(out);
        return -1;
    }
    fclose(in);
    return fclose(out);
}

/* mkdir -p (parents only matter for staged multi-file packs) */
static int pack_mkdir_p(const char *path)
{
    char tmp[MAX_PATH];
    size_t len, i;
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp)
        return -1;
    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = 0;
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Run curl (fork+execvp, no shell: the URL derives from --repo, which is
 * external input). Returns 0 on success, -1 on failure; *missing is set
 * when curl itself is absent (exit 127), so the caller can print the
 * distro hint instead of a bare failure. */
static int curl_run(const char *url, const char *outpath, char **mem,
                    size_t *memlen, int *missing)
{
    int pipefd[2] = { -1, -1 };
    pid_t pid;
    int status = 0;
    if (missing)
        *missing = 0;
    if (mem && pipe(pipefd) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        if (mem) {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        return -1;
    }
    if (pid == 0) {
        char *argv[8];
        int ac = 0;
        if (mem) {
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
        }
        argv[ac++] = (char *)"curl";
        argv[ac++] = (char *)"-fsSL";
        argv[ac++] = (char *)"--retry";
        argv[ac++] = (char *)"2";
        if (!mem) {
            argv[ac++] = (char *)"-o";
            argv[ac++] = (char *)outpath;
        }
        argv[ac++] = (char *)url;
        argv[ac] = NULL;
        execvp("curl", argv);
        _exit(127);
    }
    if (mem) {
        size_t cap = 65536, n = 0;
        char *buf = (char *)malloc(cap);
        ssize_t r;
        if (!buf) {
            close(pipefd[1]);
            close(pipefd[0]);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
            return -1;
        }
        close(pipefd[1]);
        while ((r = read(pipefd[0], buf + n, cap - n)) > 0) {
            n += (size_t)r;
            if (n == cap) {
                char *nb;
                cap *= 2;
                nb = (char *)realloc(buf, cap);
                if (!nb) {
                    free(buf);
                    close(pipefd[0]);
                    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                        ;
                    return -1;
                }
                buf = nb;
            }
        }
        close(pipefd[0]);
        buf[n] = 0;
        *mem = buf;
        if (memlen)
            *memlen = n;
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status))
        return -1;
    if (WEXITSTATUS(status) == 127) {
        if (missing)
            *missing = 1;
        return -1;
    }
    return WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* Distro hint when a helper is missing. Same table shape as the WP60 seed:
 * no external tool is ever installed; the operator gets the command. */
static void pack_dep_hint(const char *tool)
{
    static const struct {
        const char *tool, *deb, *arch, *gentoo, *qv, *fedora;
    } deptab[] = {
        { "python3", "apt install python3", "pacman -S python",
          "emerge dev-lang/python", "xbps-install python3",
          "dnf install python3" },
        { "cjxl", "apt install libjxl-tools", "pacman -S libjxl",
          "emerge media-libs/libjxl", "xbps-install libjxl",
          "dnf install libjxl-tools" },
        { "djxl", "apt install libjxl-tools", "pacman -S libjxl",
          "emerge media-libs/libjxl", "xbps-install libjxl",
          "dnf install libjxl-tools" },
        { "mac", "apt install monkeys-audio", "pacman -S mac",
          "emerge media-sound/mac", "xbps-install mac",
          "dnf install mac" },
        { "wavpack", "apt install wavpack", "pacman -S wavpack",
          "emerge media-sound/wavpack", "xbps-install wavpack",
          "dnf install wavpack" },
        { "wvunpack", "apt install wavpack", "pacman -S wavpack",
          "emerge media-sound/wavpack", "xbps-install wavpack",
          "dnf install wavpack" },
        { "flac", "apt install flac", "pacman -S flac",
          "emerge media-sound/flac", "xbps-install flac",
          "dnf install flac" },
        { "7zz", "apt install 7zip", "pacman -S 7zip",
          "emerge app-arch/7zip", "xbps-install 7zip",
          "dnf install 7zip" },
        { "curl", "apt install curl", "pacman -S curl",
          "emerge net-misc/curl", "xbps-install curl",
          "dnf install curl" },
        { "tar", "apt install tar", "pacman -S tar",
          "emerge app-arch/tar", "xbps-install tar",
          "dnf install tar" },
<<<<<<< HEAD
=======
        { "unzip", "apt install unzip", "pacman -S unzip",
          "emerge app-arch/unzip", "xbps-install unzip",
          "dnf install unzip" },
>>>>>>> wp/608-pack-registry-fetch
    };
    char id[64] = "";
    const char *distro = "unknown";
    size_t i;
    char *text = pack_read_file("/etc/os-release", NULL);
    if (text) {
        char *p = strstr(text, "ID=");
        if (p) {
            char *v = strchr(p, '=') + 1, *e;
            if (*v == '"')
                v++;
            e = v;
            while (*e && *e != '\n' && *e != '"')
                e++;
            *e = 0;
            snprintf(id, sizeof id, "%s", v);
        }
        free(text);
    }
    if (strstr(id, "debian") || strstr(id, "ubuntu"))
        distro = "debian";
    else if (strstr(id, "arch"))
        distro = "arch";
    else if (strstr(id, "gentoo"))
        distro = "gentoo";
    else if (strstr(id, "void"))
        distro = "void";
    else if (strstr(id, "fedora") || strstr(id, "rhel") ||
             strstr(id, "centos"))
        distro = "fedora";
    for (i = 0; i < sizeof deptab / sizeof deptab[0]; i++) {
        if (strcmp(deptab[i].tool, tool) != 0)
            continue;
        if (strcmp(distro, "debian") == 0)
            fprintf(stderr, "    Debian/Ubuntu: sudo %s\n", deptab[i].deb);
        else if (strcmp(distro, "arch") == 0)
            fprintf(stderr, "    Arch:          sudo %s\n", deptab[i].arch);
        else if (strcmp(distro, "gentoo") == 0)
            fprintf(stderr, "    Gentoo:        sudo %s\n", deptab[i].gentoo);
        else if (strcmp(distro, "void") == 0)
            fprintf(stderr, "    Void:          sudo %s\n", deptab[i].qv);
        else if (strcmp(distro, "fedora") == 0)
            fprintf(stderr, "    Fedora/RHEL:   sudo %s\n", deptab[i].fedora);
        else
            fprintf(stderr, "    install package providing: %s\n", tool);
        return;
    }
    fprintf(stderr, "    install package providing: %s\n", tool);
}

static void repo_curl_missing(void)
{
    fprintf(stderr, "invfs-pack: curl is required to fetch from an "
            "https:// registry but was not found on PATH. "
            "invfs-pack never installs tools itself; install curl:\n");
    pack_dep_hint("curl");
}

static void repo_tar_missing(void)
{
    fprintf(stderr, "invfs-pack: tar is required to unpack a --release "
            "tarball but was not found on PATH. "
            "invfs-pack never installs tools itself; install tar:\n");
    pack_dep_hint("tar");
}

<<<<<<< HEAD
=======
static void repo_unzip_missing(void)
{
    fprintf(stderr, "invfs-pack: unzip is required to unpack a --release "
            ".ivpack bundle but was not found on PATH. "
            "invfs-pack never installs tools itself; install unzip:\n");
    pack_dep_hint("unzip");
}

>>>>>>> wp/608-pack-registry-fetch
/* Fetch a repo-relative path into memory (caller frees) or into a file.
 * file:// repos are plain copies (offline-capable); https:// repos go
 * through curl. 0 on success, -1 on failure (*missing set when curl is
 * absent). */
static int repo_fetch_mem(pack_repo *r, const char *rel, char **buf,
                          size_t *len, int *missing)
{
    if (missing)
        *missing = 0;
    if (buf)
        *buf = NULL;
    if (len)
        *len = 0;
    if (!r->http) {
        char path[MAX_PATH * 2 + 64];
        if (repof(path, sizeof path, r, rel) != 0)
            return -1;
        if (buf)
            *buf = pack_read_file(path, len);
        return (buf && !*buf) ? -1 : 0;
    }
    {
        char url[MAX_PATH * 2 + 256];
        int n = snprintf(url, sizeof url, "%s/%s", r->base, rel);
        if (n < 0 || (size_t)n >= sizeof url)
            return -1;
        if (buf)
            return curl_run(url, NULL, buf, len, missing);
        return -1;
    }
}

static int repo_fetch_file(pack_repo *r, const char *rel, const char *dest,
                           int *missing)
{
    if (missing)
        *missing = 0;
    if (!r->http) {
        char path[MAX_PATH * 2 + 64];
        if (repof(path, sizeof path, r, rel) != 0)
            return -1;
        return pack_copy_file(path, dest);
    }
    {
        char url[MAX_PATH * 2 + 256];
        int n = snprintf(url, sizeof url, "%s/%s", r->base, rel);
        if (n < 0 || (size_t)n >= sizeof url)
            return -1;
        return curl_run(url, dest, NULL, NULL, missing);
    }
}

/* ---- minimal index.json reader ----
 *
 * Only the fields the manager needs are extracted from each entry of
 * the top-level "packs" array: name, version, pack_version, algo,
 * family, priority, and the manifest/sha256 relative paths (which
 * default to codecpacks/<name>/<version>/{manifest,sha256} when the
 * index does not carry them). A full JSON DOM would be overkill for
 * two commands; the scanner below is brace-balanced and string-aware,
 * so a "}" inside a description cannot end an entry early. */
typedef struct {
    char name[128];
    char version[64];
    char pack_version[16];
    char algo[16];
    char family[64];
    char priority[16];
    char manifest_rel[256];
    char sha256_rel[256];
} reg_pack;

/* copy the string value of "key" inside one {...} object slice */
static void json_field(const char *obj, size_t len, const char *key,
                       char *out, size_t cap)
{
    char pat[72];
    const char *end = obj + len;
    const char *p;
    out[0] = 0;
    if (snprintf(pat, sizeof pat, "\"%s\"", key) >= (int)sizeof pat)
        return;
    p = obj;
    while (p && p < end && (p = strstr(p, pat)) != NULL && p < end) {
        const char *v = p + strlen(pat);
        const char *s;
        size_t n = 0;
        while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' ||
                           *v == '\r'))
            v++;
        if (v >= end || *v != ':') {
            p++;
            continue;
        }
        v++;
        while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' ||
                           *v == '\r'))
            v++;
        if (v >= end || *v != '"') {
            p++;
            continue;
        }
        v++;
        s = v;
        while (v < end && *v != '"') {
            if (*v == '\\' && v + 1 < end)
                v += 2;
            else
                v++;
        }
        /* unescape into out (only \" and \\ matter for our fields) */
        while (s < v && n + 1 < cap) {
            if (*s == '\\' && s + 1 < v && (s[1] == '"' || s[1] == '\\'))
                s++;
            out[n++] = *s++;
        }
        out[n] = 0;
        return;
    }
}

/* copy a numeric field (algo, priority, pack_version) as text */
static void json_num(const char *obj, size_t len, const char *key,
                     char *out, size_t cap)
{
    char pat[72];
    const char *end = obj + len;
    const char *p = obj;
    out[0] = 0;
    if (snprintf(pat, sizeof pat, "\"%s\"", key) >= (int)sizeof pat)
        return;
    while (p && p < end && (p = strstr(p, pat)) != NULL && p < end) {
        const char *v = p + strlen(pat);
        const char *s;
        size_t n = 0;
        while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' ||
                           *v == '\r'))
            v++;
        if (v >= end || *v != ':') {
            p++;
            continue;
        }
        v++;
        while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' ||
                           *v == '\r'))
            v++;
        s = v;
        if (s < end && *s == '"') {
            /* quoted number: same shape as a string field */
            json_field(obj, len, key, out, cap);
            return;
        }
        while (v < end && ((*v >= '0' && *v <= '9') || *v == '-' ||
                           *v == '+' || *v == '.'))
            v++;
        while (s < v && n + 1 < cap)
            out[n++] = *s++;
        out[n] = 0;
        return;
    }
}

static int parse_index(const char *text, reg_pack *out, int cap)
{
    const char *arr;
    const char *p;
    int n = 0;
    arr = strstr(text, "\"packs\"");
    if (!arr)
        return -1;
    arr = strchr(arr, '[');
    if (!arr)
        return -1;
    p = arr + 1;
    while (*p && n < cap) {
        const char *start;
        int depth;
        int instr = 0;
        size_t len;
        reg_pack *e;
        while (*p && *p != '{') {
            if (*p == ']')
                return n;
            p++;
        }
        if (*p != '{')
            break;
        start = p;
        depth = 0;
        instr = 0;
        while (*p) {
            if (instr) {
                if (*p == '\\' && p[1])
                    p++;
                else if (*p == '"')
                    instr = 0;
            } else if (*p == '"') {
                instr = 1;
            } else if (*p == '{') {
                depth++;
            } else if (*p == '}') {
                depth--;
                if (depth == 0) {
                    p++;
                    break;
                }
            }
            p++;
        }
        if (depth != 0)
            return -1;
        len = (size_t)(p - start);
        e = &out[n];
        memset(e, 0, sizeof *e);
        json_field(start, len, "name", e->name, sizeof e->name);
        json_field(start, len, "version", e->version, sizeof e->version);
        json_num(start, len, "pack_version", e->pack_version,
                 sizeof e->pack_version);
        json_num(start, len, "algo", e->algo, sizeof e->algo);
        json_field(start, len, "family", e->family, sizeof e->family);
        json_num(start, len, "priority", e->priority, sizeof e->priority);
        json_field(start, len, "manifest", e->manifest_rel,
                   sizeof e->manifest_rel);
        json_field(start, len, "sha256", e->sha256_rel,
                   sizeof e->sha256_rel);
        if (!e->name[0])
            continue;
        if (!e->manifest_rel[0] && e->version[0])
            snprintf(e->manifest_rel, sizeof e->manifest_rel,
                     "codecpacks/%s/%s/manifest", e->name, e->version);
        if (!e->sha256_rel[0] && e->version[0])
            snprintf(e->sha256_rel, sizeof e->sha256_rel,
                     "codecpacks/%s/%s/sha256", e->name, e->version);
        n++;
    }
    return n;
}

/* numeric dotted-triple compare (1.0.0 < 1.2.0); falls back to strcmp */
static int vercmp3(const char *a, const char *b)
{
    long va[3] = { 0, 0, 0 }, vb[3] = { 0, 0, 0 };
    int na = sscanf(a, "%ld.%ld.%ld", &va[0], &va[1], &va[2]);
    int nb = sscanf(b, "%ld.%ld.%ld", &vb[0], &vb[1], &vb[2]);
    int i;
    if (na < 3 || nb < 3)
        return strcmp(a, b);
    for (i = 0; i < 3; i++) {
        if (va[i] < vb[i])
            return -1;
        if (va[i] > vb[i])
            return 1;
    }
    return 0;
}

/* ---- sha256 (for verifying fetched files against the pack listing) ---- */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} r256_ctx;

static uint32_t r256_ror(uint32_t x, int k)
{
    return (x >> k) | (x << (32 - k));
}

static void r256_block(r256_ctx *c, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
    uint32_t w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) |
               ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = r256_ror(w[i - 15], 7) ^ r256_ror(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = r256_ror(w[i - 2], 17) ^ r256_ror(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t S1 = r256_ror(e, 6) ^ r256_ror(e, 11) ^ r256_ror(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t S0 = r256_ror(a, 2) ^ r256_ror(a, 13) ^ r256_ror(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        t1 = h + S1 + ch + k[i] + w[i];
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void r256_init(r256_ctx *c)
{
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
    c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
    c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->len = 0;
    c->n = 0;
}

static void r256_update(r256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->len += len;
    while (len) {
        size_t take = 64 - c->n;
        if (take > len)
            take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        len -= take;
        if (c->n == 64) {
            r256_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void r256_final(r256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;
    uint8_t b;
    int i, j;
    b = 0x80;
    r256_update(c, &b, 1);
    b = 0x00;
    while (c->n != 56)
        r256_update(c, &b, 1);
    for (i = 7; i >= 0; i--) {
        b = (uint8_t)(bits >> (i * 8));
        r256_update(c, &b, 1);
    }
    for (j = 0; j < 8; j++) {
        out[j * 4] = (uint8_t)(c->h[j] >> 24);
        out[j * 4 + 1] = (uint8_t)(c->h[j] >> 16);
        out[j * 4 + 2] = (uint8_t)(c->h[j] >> 8);
        out[j * 4 + 3] = (uint8_t)(c->h[j]);
    }
}

static int r256_file(const char *path, char hex[65])
{
    FILE *f = fopen(path, "rb");
    r256_ctx c;
    uint8_t digest[32];
    uint8_t buf[65536];
    size_t r;
    int i;
    static const char hx[] = "0123456789abcdef";
    if (!f)
        return -1;
    r256_init(&c);
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        r256_update(&c, buf, r);
    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    r256_final(&c, digest);
    for (i = 0; i < 32; i++) {
        hex[i * 2] = hx[digest[i] >> 4];
        hex[i * 2 + 1] = hx[digest[i] & 15];
    }
    hex[64] = 0;
    return 0;
}

static int hex64_ok(const char *s)
{
    int i;
    if (strlen(s) != 64)
        return 0;
    for (i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

/* Fetch the registry index. Returns the entry count (>= 0) with *out
 * malloc'd (caller frees), -1 when the repo is unreachable or the index
 * does not parse. *missing is set when an https fetch failed only
 * because curl is absent. */
static int repo_load_index(pack_repo *r, reg_pack **out, int *missing)
{
    char *text = NULL;
    size_t len = 0;
    reg_pack *packs;
    int n;
    if (repo_fetch_mem(r, "codecpacks/index.json", &text, &len,
                       missing) != 0 || !text)
        return -1;
    packs = (reg_pack *)malloc(sizeof *packs * MAX_REG_PACKS);
    if (!packs) {
        free(text);
        return -1;
    }
    n = parse_index(text, packs, MAX_REG_PACKS);
    free(text);
    if (n < 0) {
        free(packs);
        return -1;
    }
    *out = packs;
    return n;
}

static const reg_pack *repo_best(const reg_pack *packs, int n,
                                 const char *name)
{
    const reg_pack *best = NULL;
    int i;
    for (i = 0; i < n; i++) {
        if (strcmp(packs[i].name, name) != 0)
            continue;
        if (!best || vercmp3(packs[i].version, best->version) > 0)
            best = &packs[i];
    }
    return best;
}

/* Pick by explicit version, or the latest when want_ver is NULL/"".
 * Returns NULL when no entry matches (caller names the registry). */
static const reg_pack *repo_pick(const reg_pack *packs, int n,
                                 const char *name, const char *want_ver)
{
    int i;
    if (!want_ver || !*want_ver)
        return repo_best(packs, n, name);
    for (i = 0; i < n; i++) {
        if (strcmp(packs[i].name, name) != 0)
            continue;
        if (strcmp(packs[i].version, want_ver) == 0)
            return &packs[i];
    }
    return NULL;
}

/* Split <name>[@<version>] (bare names only: path targets with a slash
 * never reach here). 0 with base[]/want[] filled (want[] empty when no
 * @), -1 when the @ form is malformed. */
static int split_namever(const char *arg, char *base, size_t bcap,
                         char *want, size_t wcap)
{
    const char *at = strchr(arg, '@');
    size_t bl;
    base[0] = 0;
    want[0] = 0;
    if (!at) {
        if (snprintf(base, bcap, "%s", arg) >= (int)bcap)
            return -1;
        return 0;
    }
    if (at == arg || !at[1] || strchr(at + 1, '@') != NULL ||
        strchr(at + 1, '/') != NULL)
        return -1;
    bl = (size_t)(at - arg);
    if (bl == 0 || bl >= bcap || strlen(at + 1) >= wcap)
        return -1;
    memcpy(base, arg, bl);
    base[bl] = 0;
    snprintf(want, wcap, "%s", at + 1);
    return 0;
}

/* package-release.sh's default platform string: <uname>-linux-musl. */
static void default_platform(char *out, size_t cap)
{
    struct utsname u;
    if (uname(&u) == 0 && u.machine[0])
        snprintf(out, cap, "%.50s-linux-musl", u.machine);
    else
        snprintf(out, cap, "%s", "x86_64-linux-musl");
}

/* Run `tar -tzf <tb>`, collecting entry names (NUL-terminated, malloc'd,
 * caller frees). fork+execvp, no shell. *missing set when tar itself is
 * absent (exit 127). */
static int tar_list(const char *tb, char **out, size_t *outlen,
                    int *missing)
{
    int pipefd[2] = { -1, -1 };
    pid_t pid;
    int status = 0;
    size_t cap = 4096, n = 0;
    char *buf;
    ssize_t r;
    if (missing)
        *missing = 0;
    if (out)
        *out = NULL;
    if (outlen)
        *outlen = 0;
    if (pipe(pipefd) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        char *argv[4];
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        argv[0] = (char *)"tar";
        argv[1] = (char *)"-tzf";
        argv[2] = (char *)tb;
        argv[3] = NULL;
        execvp("tar", argv);
        _exit(127);
    }
    buf = (char *)malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        close(pipefd[1]);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }
    close(pipefd[1]);
    while ((r = read(pipefd[0], buf + n, cap - n)) > 0) {
        n += (size_t)r;
        if (n == cap) {
            char *nb;
            cap *= 2;
            nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                close(pipefd[0]);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                    ;
                return -1;
            }
            buf = nb;
        }
    }
    close(pipefd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status)) {
        free(buf);
        return -1;
    }
    if (WEXITSTATUS(status) == 127) {
        if (missing)
            *missing = 1;
        free(buf);
        return -1;
    }
    if (WEXITSTATUS(status) != 0) {
        free(buf);
        return -1;
    }
    if (n >= cap) {
        char *nb = (char *)realloc(buf, n + 1);
        if (!nb) {
            free(buf);
            return -1;
        }
        buf = nb;
    }
    buf[n] = 0;
    *out = buf;
    if (outlen)
        *outlen = n;
    return 0;
}

/* Run `tar -xzf <tb> -C <dir>`. fork+execvp, no shell. */
static int tar_extract(const char *tb, const char *dir, int *missing)
{
    pid_t pid;
    int status = 0;
    char *argv[6];
    if (missing)
        *missing = 0;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        argv[0] = (char *)"tar";
        argv[1] = (char *)"-xzf";
        argv[2] = (char *)tb;
        argv[3] = (char *)"-C";
        argv[4] = (char *)dir;
        argv[5] = NULL;
        execvp("tar", argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status))
        return -1;
    if (WEXITSTATUS(status) == 127) {
        if (missing)
            *missing = 1;
        return -1;
    }
    return WEXITSTATUS(status) == 0 ? 0 : -1;
}

<<<<<<< HEAD
=======
/* Run `unzip -Z1 <zb>`, collecting entry names (one per line, malloc'd,
 * caller frees). fork+execvp, no shell. *missing set when unzip itself
 * is absent (exit 127). */
static int zip_list(const char *zb, char **out, size_t *outlen,
                    int *missing)
{
    int pipefd[2] = { -1, -1 };
    pid_t pid;
    int status = 0;
    size_t cap = 4096, n = 0;
    char *buf;
    ssize_t r;
    if (missing)
        *missing = 0;
    if (out)
        *out = NULL;
    if (outlen)
        *outlen = 0;
    if (pipe(pipefd) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        char *argv[4];
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        argv[0] = (char *)"unzip";
        argv[1] = (char *)"-Z1";
        argv[2] = (char *)zb;
        argv[3] = NULL;
        execvp("unzip", argv);
        _exit(127);
    }
    buf = (char *)malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        close(pipefd[1]);
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }
    close(pipefd[1]);
    while ((r = read(pipefd[0], buf + n, cap - n)) > 0) {
        n += (size_t)r;
        if (n == cap) {
            char *nb;
            cap *= 2;
            nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                close(pipefd[0]);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                    ;
                return -1;
            }
            buf = nb;
        }
    }
    close(pipefd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status)) {
        free(buf);
        return -1;
    }
    if (WEXITSTATUS(status) == 127) {
        if (missing)
            *missing = 1;
        free(buf);
        return -1;
    }
    if (WEXITSTATUS(status) != 0) {
        free(buf);
        return -1;
    }
    if (n >= cap) {
        char *nb = (char *)realloc(buf, n + 1);
        if (!nb) {
            free(buf);
            return -1;
        }
        buf = nb;
    }
    buf[n] = 0;
    *out = buf;
    if (outlen)
        *outlen = n;
    return 0;
}

/* Run `unzip -q -o <zb> -d <dir>`. fork+execvp, no shell. */
static int zip_extract(const char *zb, const char *dir, int *missing)
{
    pid_t pid;
    int status = 0;
    char *argv[7];
    if (missing)
        *missing = 0;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        argv[0] = (char *)"unzip";
        argv[1] = (char *)"-q";
        argv[2] = (char *)"-o";
        argv[3] = (char *)zb;
        argv[4] = (char *)"-d";
        argv[5] = (char *)dir;
        argv[6] = NULL;
        execvp("unzip", argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status))
        return -1;
    if (WEXITSTATUS(status) == 127) {
        if (missing)
            *missing = 1;
        return -1;
    }
    return WEXITSTATUS(status) == 0 ? 0 : -1;
}

/* An .ivpack entry is safe only when it stays under the archive root
 * (which becomes the staged pack dir): no absolute path, no ".."
 * component, no backslash (never a separator in a well-formed zip, so
 * one is either a crafted entry or a non-portable name — refuse both).
 * Directory entries (trailing slash) pass; symlink/special bits cannot
 * be seen in names, so they are refused after extract by the lstat walk
 * (extras_walk), same split as the tarball path. Leading "./" is
 * tolerated (harmless archiver style). */
static int zip_entry_ok(const char *e)
{
    const char *p;
    size_t len;
    while (e[0] == '.' && e[1] == '/')
        e += 2;
    if (e[0] == '/' || e[0] == 0)
        return -1;
    if (strchr(e, '\\') != NULL)
        return -1;
    len = strlen(e);
    if (len > 0 && e[len - 1] == '/')
        len--; /* directory entry: check the path leading to it */
    if (len == 0)
        return -1;
    p = e;
    for (;;) {
        const char *s = strchr(p, '/');
        size_t cl = s ? (size_t)(s - p) : len - (size_t)(p - e);
        if (cl == 0 || (cl == 1 && p[0] == '.') ||
            (cl == 2 && p[0] == '.' && p[1] == '.'))
            return -1;
        if (!s)
            break;
        p = s + 1;
        if ((size_t)(p - e) >= len)
            break;
    }
    return 0;
}

>>>>>>> wp/608-pack-registry-fetch
/* A tar entry is safe only when it stays under top (<name>/<version>):
 * no absolute path, no ".." component, and the top prefix itself
 * (so a crafted tarball cannot write outside the extraction dir or
 * smuggle files that the extras check below would then have to catch
 * by name). Leading "./" is tolerated (harmless tar style). */
static int tar_entry_ok(const char *e, const char *top)
{
    size_t tl = strlen(top);
    const char *p;
    while (e[0] == '.' && e[1] == '/')
        e += 2;
    if (e[0] == '/' || e[0] == 0)
        return -1;
    if (strncmp(e, top, tl) != 0)
        return -1;
    if (e[tl] != 0 && e[tl] != '/')
        return -1;
    p = e;
    for (;;) {
        const char *s = strchr(p, '/');
        size_t cl = s ? (size_t)(s - p) : strlen(p);
        if (cl == 2 && p[0] == '.' && p[1] == '.')
            return -1;
        if (!s)
            break;
        p = s + 1;
        if (!*p)
            break;
    }
    return 0;
}

typedef struct {
    const char (*rels)[256];
    int nrels;
    size_t rootlen; /* strlen(topdir)+1: full = topdir + "/" + rel */
    char badrel[256];
    int nonregular; /* set when a symlink/fifo/device is found */
} extras_ctx;

/* Recursive walk over the extracted tree: every regular file must be
 * named by the sha256 listing (except the listing itself, which is
 * transport), and nothing may be a symlink or special file (a symlink
 * would make the hash check read through to outside the tarball).
 * Returns 0, -1 on a walk error, -2 on an unlisted/non-regular file. */
static int extras_walk(const char *dir, extras_ctx *c)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        char full[MAX_PATH * 2 + 64];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (snprintf(full, sizeof full, "%s/%s", dir,
                     de->d_name) >= (int)sizeof full) {
            closedir(d);
            return -1;
        }
        if (lstat(full, &st) != 0) {
            closedir(d);
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (extras_walk(full, c) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            const char *rel = full + c->rootlen;
            int i, known = 0;
            if (strcmp(rel, "sha256") == 0)
                continue;
            for (i = 0; i < c->nrels; i++) {
                if (strcmp(rel, c->rels[i]) == 0) {
                    known = 1;
                    break;
                }
            }
            if (!known) {
                snprintf(c->badrel, sizeof c->badrel, "%s", rel);
                closedir(d);
                return -2;
            }
        } else {
            c->nonregular = 1;
            snprintf(c->badrel, sizeof c->badrel, "%s",
                     full + c->rootlen);
            closedir(d);
            return -2;
        }
    }
    closedir(d);
    return 0;
}

<<<<<<< HEAD
/* Fetch one pack's RELEASE tarball into the tool scratch, unpack it,
 * and sha256-verify every extracted file against the raw-tree sha256
=======
/* Fetch one pack's RELEASE tarball (round-2 fallback) into the tool scratch,
 * unpack it, and sha256-verify every extracted file against the raw-tree sha256
>>>>>>> wp/608-pack-registry-fetch
 * listing (fetched through `raw`, exactly like the raw path) — so a
 * tarball whose content was tampered with refuses with the same
 * diagnostic as a tampered raw fetch. The WP203 signature gate then runs
 * unchanged over the staged tree (see cmd_install): no bypass.
 *
 * Layout contract (from the registry's tools/package-release.sh +
 * .github/workflows/build.yml; the registry has cut zero tags as of
 * 2026-10-12, so this is defined from the publisher, not observed):
 * dist/<name>-<version>-<platform>.tar.gz containing
 * <name>/<version>/{manifest,sha256,sources,bin helpers}, uploaded as a
 * release asset on a `v*` tag. Default tag is v<version>
 * (--release-tag overrides); default platform is <uname>-linux-musl,
 * package-release.sh's own default (--platform overrides). */
<<<<<<< HEAD
static int repo_fetch_release(const reg_pack *best, pack_repo *raw,
                              const char *repo_spec, const fetch_opts *o,
=======
static int repo_fetch_tarball(const reg_pack *best, pack_repo *raw,
                                const char *repo_spec, const fetch_opts *o,
>>>>>>> wp/608-pack-registry-fetch
                              char *src_out, size_t src_cap,
                              char *dstname_out, size_t dst_cap,
                              char *stage_top_out, size_t stage_cap,
                              int *missing)
{
    char plat[64], platdef[64], tag[128];
    char tarball[320], top[256], asset_rel[640];
    pack_repo ar;
    const char *custom;
    char scratch[MAX_PATH], staged[MAX_PATH], dl[MAX_PATH];
    char tbpath[MAX_PATH], topdir[MAX_PATH], shpath[MAX_PATH];
    char *shtext = NULL;
    size_t shlen = 0;
    char *save = NULL, *line;
    char (*rels)[256] = NULL;
    int nrels = 0, files = 0, miss = 0;
    src_out[0] = 0;
    stage_top_out[0] = 0;
    if (missing)
        *missing = 0;
    if (o->platform && o->platform[0])
        snprintf(plat, sizeof plat, "%s", o->platform);
    else {
        default_platform(platdef, sizeof platdef);
        snprintf(plat, sizeof plat, "%s", platdef);
    }
    if (strstr(plat, "/") != NULL || strstr(plat, "..") != NULL) {
        fprintf(stderr, "invfs-pack: bad --platform: %s\n", plat);
        return -1;
    }
    if (o->rel_tag && o->rel_tag[0])
        snprintf(tag, sizeof tag, "%s", o->rel_tag);
    else
        snprintf(tag, sizeof tag, "v%s", best->version);
    if (strstr(tag, "/") != NULL || strstr(tag, "..") != NULL) {
        fprintf(stderr, "invfs-pack: bad --release-tag: %s\n", tag);
        return -1;
    }
    if (snprintf(tarball, sizeof tarball, "%s-%s-%s.tar.gz", best->name,
                 best->version, plat) >= (int)sizeof tarball ||
        snprintf(top, sizeof top, "%s/%s", best->name,
                 best->version) >= (int)sizeof top) {
        fprintf(stderr, "invfs-pack: release name too long: %s %s\n",
                best->name, best->version);
        return -1;
    }
    /* Asset base: an explicit --repo (or $INVFS_PACK_REPO) keeps its
     * releases/ subtree (this is where hermetic file:// fixtures live);
     * otherwise the real GitHub release downloads. The version resolve
     * above still came from the raw-tree index.json — no GitHub API. */
    custom = (repo_spec && *repo_spec) ? repo_spec : getenv("INVFS_PACK_REPO");
    if (custom && *custom) {
        repo_resolve(&ar, custom);
        if (snprintf(asset_rel, sizeof asset_rel,
                     "releases/download/%s/%s", tag,
                     tarball) >= (int)sizeof asset_rel) {
            fprintf(stderr, "invfs-pack: release path too long\n");
            return -1;
        }
    } else {
        char base[512];
        if (snprintf(base, sizeof base, "%s/releases/download/%s",
                     DEFAULT_RELEASE_BASE, tag) >= (int)sizeof base) {
            fprintf(stderr, "invfs-pack: release path too long\n");
            return -1;
        }
        repo_resolve(&ar, base);
        if (snprintf(asset_rel, sizeof asset_rel, "%s",
                     tarball) >= (int)sizeof asset_rel) {
            fprintf(stderr, "invfs-pack: release path too long\n");
            return -1;
        }
    }
    printf("fetching %s %s release %s/%s from %s\n", best->name,
           best->version, tag, tarball, ar.base);
    /* the tarball plus its unpacked tree: size like any tool job */
    if (tool_tmpdir(scratch, sizeof scratch, 1 << 20) != 0) {
        fprintf(stderr, "invfs-pack: no scratch available for the fetch\n");
        return -1;
    }
    if (snprintf(stage_top_out, stage_cap, "%s", scratch) >=
        (int)stage_cap) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    {
        size_t nl = strlen(best->name);
        if (nl > 10 && strcmp(best->name + nl - 10, ".codecpack") == 0) {
            if (snprintf(dstname_out, dst_cap, "%s", best->name) >=
                (int)dst_cap) {
                fprintf(stderr, "invfs-pack: pack name too long: %s\n",
                        best->name);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
        } else {
            if (snprintf(dstname_out, dst_cap, "%s.codecpack",
                         best->name) >= (int)dst_cap) {
                fprintf(stderr, "invfs-pack: pack name too long: %s\n",
                        best->name);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
        }
    }
    if (snprintf(staged, sizeof staged, "%s/%s", scratch,
                 dstname_out) >= (int)sizeof staged ||
        snprintf(dl, sizeof dl, "%s/dl", scratch) >= (int)sizeof dl ||
        snprintf(tbpath, sizeof tbpath, "%s/pack.tar.gz",
                 scratch) >= (int)sizeof tbpath ||
        pack_mkdir_p(dl) != 0) {
        fprintf(stderr, "invfs-pack: cannot stage under %s\n", scratch);
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    if (repo_fetch_file(&ar, asset_rel, tbpath, &miss) != 0) {
        if (miss) {
            if (missing)
                *missing = 1;
            repo_curl_missing();
        } else {
            fprintf(stderr, "invfs-pack: cannot fetch %s/%s\n"
                    "  (release asset: check --release-tag/--platform, "
                    "or no release was ever published for this version)\n",
                    ar.base, asset_rel);
        }
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    /* list-then-extract: every entry must stay under <name>/<version> */
    {
        char *entries = NULL;
        size_t elen = 0;
        int tmiss = 0;
        char *esave = NULL, *e;
        int any = 0;
        if (tar_list(tbpath, &entries, &elen, &tmiss) != 0) {
            if (tmiss) {
                if (missing)
                    *missing = 1;
                repo_tar_missing();
            } else {
                fprintf(stderr, "invfs-pack: REFUSED: cannot list "
                        "release tarball for %s %s (corrupt "
                        "download?)\n", best->name, best->version);
            }
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        for (e = strtok_r(entries, "\n", &esave); e;
             e = strtok_r(NULL, "\n", &esave)) {
            while (*e == ' ' || *e == '\t')
                e++;
            strip_nl(e);
            if (!*e)
                continue;
            if (tar_entry_ok(e, top) != 0) {
                fprintf(stderr, "invfs-pack: REFUSED: release tarball "
                        "for %s %s escapes %s: %s\n", best->name,
                        best->version, top, e);
                free(entries);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
            any++;
        }
        free(entries);
        if (!any) {
            fprintf(stderr, "invfs-pack: REFUSED: empty release tarball "
                    "for %s %s\n", best->name, best->version);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (tar_extract(tbpath, dl, &tmiss) != 0) {
            if (tmiss) {
                if (missing)
                    *missing = 1;
                repo_tar_missing();
            } else {
                fprintf(stderr, "invfs-pack: REFUSED: cannot unpack "
                        "release tarball for %s %s (corrupt "
                        "download?)\n", best->name, best->version);
            }
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
    }
    if (snprintf(topdir, sizeof topdir, "%s/%s", dl, top) >=
        (int)sizeof topdir) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    {
        struct stat st;
        if (stat(topdir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "invfs-pack: REFUSED: release tarball for "
                    "%s %s has no %s/ tree\n", best->name,
                    best->version, top);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
    }
    /* the expected hashes live in the raw tree (same listing the raw
     * path reads): a tarball re-hashed by its tamperer still fails. */
    if (repo_fetch_mem(raw, best->sha256_rel, &shtext, &shlen,
                       &miss) != 0 || !shtext) {
        if (miss) {
            if (missing)
                *missing = 1;
            repo_curl_missing();
        } else {
            fprintf(stderr, "invfs-pack: cannot fetch %s/%s to verify "
                    "the release tarball\n", raw->base,
                    best->sha256_rel);
        }
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    rels = (char (*)[256])malloc(sizeof *rels * MAX_REL_FILES);
    if (!rels) {
        fprintf(stderr, "invfs-pack: out of memory\n");
        free(shtext);
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    for (line = strtok_r(shtext, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char expect[128], rel[256], dest[MAX_PATH];
        char got[65];
        char *sp;
        struct stat st;
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line || *line == '#')
            continue;
        if (sscanf(line, "%127s", expect) != 1)
            continue;
        sp = strstr(line, "  ");
        if (!sp)
            sp = strchr(line, ' ');
        if (!sp)
            continue;
        sp++;
        while (*sp == ' ')
            sp++;
        snprintf(rel, sizeof rel, "%s", sp);
        {
            char *e2 = rel + strlen(rel);
            while (e2 > rel && (e2[-1] == '\r' || e2[-1] == ' '))
                *--e2 = 0;
        }
        if (strcmp(rel, "sha256") == 0)
            continue; /* the listing itself: transport, not content */
        if (!rel[0] || strstr(rel, "..") != NULL) {
            fprintf(stderr, "invfs-pack: bad path in sha256 listing: %s\n",
                    rel);
            free(shtext);
            free(rels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (!hex64_ok(expect)) {
            fprintf(stderr, "invfs-pack: bad sha256 line for %s\n", rel);
            free(shtext);
            free(rels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (snprintf(dest, sizeof dest, "%s/%s", topdir,
                     rel) >= (int)sizeof dest) {
            fprintf(stderr, "invfs-pack: path too long: %s\n", rel);
            free(shtext);
            free(rels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (lstat(dest, &st) != 0 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "invfs-pack: REFUSED: %s %s/%s is missing "
                    "from the release tarball (tampered pack?)\n",
                    best->name, best->version, rel);
            free(shtext);
            free(rels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (r256_file(dest, got) != 0 ||
            strcasecmp(expect, got) != 0) {
            fprintf(stderr, "invfs-pack: REFUSED: sha256 mismatch for "
                    "%s %s/%s (tampered pack?)\n", best->name,
                    best->version, rel);
            free(shtext);
            free(rels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (nrels < MAX_REL_FILES)
            snprintf(rels[nrels++], sizeof rels[0], "%s", rel);
        files++;
    }
    free(shtext);
    if (files == 0) {
        fprintf(stderr, "invfs-pack: REFUSED: empty sha256 listing for "
                "'%s' (nothing verified)\n", best->name);
        free(rels);
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    /* the tarball must carry exactly the listed set: anything extra
     * (or any symlink/special file) refuses, same class as a mismatch. */
    {
        extras_ctx cx;
        int rc;
        cx.rels = (const char (*)[256])rels;
        cx.nrels = nrels;
        cx.rootlen = strlen(topdir) + 1;
        cx.badrel[0] = 0;
        cx.nonregular = 0;
        rc = extras_walk(topdir, &cx);
        if (rc == -2) {
            if (cx.nonregular)
                fprintf(stderr, "invfs-pack: REFUSED: release tarball "
                        "for %s %s carries a non-regular file %s "
                        "(tampered pack?)\n", best->name, best->version,
                        cx.badrel);
            else
                fprintf(stderr, "invfs-pack: REFUSED: release tarball "
                        "for %s %s carries unlisted file %s (tampered "
                        "pack?)\n", best->name, best->version,
                        cx.badrel);
            free(rels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (rc != 0) {
            fprintf(stderr, "invfs-pack: cannot scan unpacked release "
                    "for %s %s\n", best->name, best->version);
            free(rels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
    }
    free(rels);
    /* The listing is transport, not pack content: drop it from the staged
     * tree before the install handoff, exactly like the raw path. */
    if (snprintf(shpath, sizeof shpath, "%s/sha256",
                 topdir) >= (int)sizeof shpath) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    unlink(shpath);
    /* the tarball is no longer needed; drop it before the handoff so a
     * later rmtree only ever sees the verified tree */
    unlink(tbpath);
    if (rename(topdir, staged) != 0) {
        fprintf(stderr, "invfs-pack: cannot stage release pack\n");
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    if (!is_codecpack(staged)) {
        fprintf(stderr, "invfs-pack: fetched '%s' has no manifest (not a "
                ".codecpack)\n", best->name);
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    if (snprintf(src_out, src_cap, "%s", staged) >= (int)src_cap) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    printf("fetched %s %s (%d file(s) verified)\n", best->name,
           best->version, files);
    return 0;
}

<<<<<<< HEAD
=======
/* Fetch one pack's .ivpack bundle (round 3, preferred under --release)
 * into the tool scratch, unzip it, and verify it two ways: first the
 * bundle's own `sha256` file over every other entry (a tampered bundle
 * refuses here, exactly like a tampered tarball), then every raw-tree
 * sha256-listed file present in the bundle with an equal hash (the
 * listing is fetched through `raw`, exactly like the raw path, so a
 * bundle re-hashed by its tamperer still fails). `manifest.sig` is the
 * one listed file allowed to be absent: v1.1.0 ships no sig sidecars,
 * so .ivpack installs take the WP203 missing-signature path (prompt;
 * -y proceeds, -n reports) — no bypass, no fail-closed invention.
 *
 * Layout contract (registry/docs/IVPACK-RELEASES.md):
 * <name>-<version>.ivpack (zip -0 -X) containing manifest, sha256,
 * lib/<name>.so (iff the pack has one) and bin/* at the archive root.
 * The archive root becomes the staged pack dir: manifest + bin/ live
 * (the engine uses them today), lib/ is laid out beside them, verified
 * but dormant. The bundle's sha256 file is transport, dropped before
 * the install handoff like the raw listing. The WP203 gate then runs
 * unchanged over the staged tree (see cmd_install).
 *
 * Returns 0 on success, 1 when the pack simply has no .ivpack asset
 * (the caller falls back to the tarball with one loud line), -1 on a
 * refusal (corrupt/tampered bundle, unzip absent (*missing set)). A
 * bundle that downloaded whole but fails verification NEVER falls back:
 * that is tampering, not absence. */
static int repo_fetch_ivpack(const reg_pack *best, pack_repo *raw,
                             const char *repo_spec, const fetch_opts *o,
                             char *src_out, size_t src_cap,
                             char *dstname_out, size_t dst_cap,
                             char *stage_top_out, size_t stage_cap,
                             int *missing)
{
    char tag[128];
    char asset[320];
    char asset_rel[640];
    pack_repo ar;
    const char *custom;
    char scratch[MAX_PATH], staged[MAX_PATH], dl[MAX_PATH];
    char zbpath[MAX_PATH], shpath[MAX_PATH];
    char *shtext = NULL;
    size_t shlen = 0;
    char *save = NULL, *line;
    char (*brels)[256] = NULL;
    int nbrels = 0, bfiles = 0;
    int miss = 0;
    src_out[0] = 0;
    stage_top_out[0] = 0;
    if (missing)
        *missing = 0;
    if (o->rel_tag && o->rel_tag[0])
        snprintf(tag, sizeof tag, "%s", o->rel_tag);
    else
        snprintf(tag, sizeof tag, "%s", DEFAULT_IVPACK_TAG);
    if (strstr(tag, "/") != NULL || strstr(tag, "..") != NULL) {
        fprintf(stderr, "invfs-pack: bad --release-tag: %s\n", tag);
        return -1;
    }
    if (snprintf(asset, sizeof asset, "%s-%s.ivpack", best->name,
                 best->version) >= (int)sizeof asset) {
        fprintf(stderr, "invfs-pack: release name too long: %s %s\n",
                best->name, best->version);
        return -1;
    }
    /* Same asset base as the tarball path: an explicit --repo (or
     * $INVFS_PACK_REPO) keeps its releases/ subtree (hermetic file://
     * fixtures); otherwise the real GitHub release downloads. */
    custom = (repo_spec && *repo_spec) ? repo_spec : getenv("INVFS_PACK_REPO");
    if (custom && *custom) {
        repo_resolve(&ar, custom);
        if (snprintf(asset_rel, sizeof asset_rel,
                     "releases/download/%s/%s", tag,
                     asset) >= (int)sizeof asset_rel) {
            fprintf(stderr, "invfs-pack: release path too long\n");
            return -1;
        }
    } else {
        char base[512];
        if (snprintf(base, sizeof base, "%s/releases/download/%s",
                     DEFAULT_RELEASE_BASE, tag) >= (int)sizeof base) {
            fprintf(stderr, "invfs-pack: release path too long\n");
            return -1;
        }
        repo_resolve(&ar, base);
        if (snprintf(asset_rel, sizeof asset_rel, "%s",
                     asset) >= (int)sizeof asset_rel) {
            fprintf(stderr, "invfs-pack: release path too long\n");
            return -1;
        }
    }
    printf("fetching %s %s release %s/%s from %s\n", best->name,
           best->version, tag, asset, ar.base);
    if (tool_tmpdir(scratch, sizeof scratch, 1 << 20) != 0) {
        fprintf(stderr, "invfs-pack: no scratch available for the fetch\n");
        return -1;
    }
    if (snprintf(stage_top_out, stage_cap, "%s", scratch) >=
        (int)stage_cap) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    {
        size_t nl = strlen(best->name);
        if (nl > 10 && strcmp(best->name + nl - 10, ".codecpack") == 0) {
            if (snprintf(dstname_out, dst_cap, "%s", best->name) >=
                (int)dst_cap) {
                fprintf(stderr, "invfs-pack: pack name too long: %s\n",
                        best->name);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
        } else {
            if (snprintf(dstname_out, dst_cap, "%s.codecpack",
                         best->name) >= (int)dst_cap) {
                fprintf(stderr, "invfs-pack: pack name too long: %s\n",
                        best->name);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
        }
    }
    if (snprintf(staged, sizeof staged, "%s/%s", scratch,
                 dstname_out) >= (int)sizeof staged ||
        snprintf(dl, sizeof dl, "%s/dl", scratch) >= (int)sizeof dl ||
        snprintf(zbpath, sizeof zbpath, "%s/pack.ivpack",
                 scratch) >= (int)sizeof zbpath ||
        pack_mkdir_p(dl) != 0) {
        fprintf(stderr, "invfs-pack: cannot stage under %s\n", scratch);
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    if (repo_fetch_file(&ar, asset_rel, zbpath, &miss) != 0) {
        if (miss) {
            if (missing)
                *missing = 1;
            repo_curl_missing();
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        /* No bundle for this pack under this tag: absence, not
         * tampering. The caller falls back (loud) to the tarball. */
        rmtree(scratch);
        stage_top_out[0] = 0;
        return 1;
    }
    /* list-then-extract: every entry must stay under the archive root */
    {
        char *entries = NULL;
        size_t elen = 0;
        int zmiss = 0;
        char *esave = NULL, *e;
        int any = 0;
        if (zip_list(zbpath, &entries, &elen, &zmiss) != 0) {
            if (zmiss) {
                if (missing)
                    *missing = 1;
                repo_unzip_missing();
            } else {
                fprintf(stderr, "invfs-pack: REFUSED: cannot list "
                        ".ivpack bundle for %s %s (corrupt "
                        "download?)\n", best->name, best->version);
            }
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        for (e = strtok_r(entries, "\n", &esave); e;
             e = strtok_r(NULL, "\n", &esave)) {
            while (*e == ' ' || *e == '\t')
                e++;
            strip_nl(e);
            if (!*e)
                continue;
            if (zip_entry_ok(e) != 0) {
                fprintf(stderr, "invfs-pack: REFUSED: .ivpack bundle "
                        "for %s %s escapes the pack dir: %s\n", best->name,
                        best->version, e);
                free(entries);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
            any++;
        }
        free(entries);
        if (!any) {
            fprintf(stderr, "invfs-pack: REFUSED: empty .ivpack bundle "
                    "for %s %s\n", best->name, best->version);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (zip_extract(zbpath, dl, &zmiss) != 0) {
            if (zmiss) {
                if (missing)
                    *missing = 1;
                repo_unzip_missing();
            } else {
                fprintf(stderr, "invfs-pack: REFUSED: cannot unpack "
                        ".ivpack bundle for %s %s (corrupt "
                        "download?)\n", best->name, best->version);
            }
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
    }
    /* the bundle's own sha256 covers every other entry: verify first */
    if (snprintf(shpath, sizeof shpath, "%s/sha256",
                 dl) >= (int)sizeof shpath) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    shtext = pack_read_file(shpath, &shlen);
    if (!shtext) {
        fprintf(stderr, "invfs-pack: REFUSED: .ivpack bundle for %s %s "
                "has no sha256 listing (tampered pack?)\n", best->name,
                best->version);
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    brels = (char (*)[256])malloc(sizeof *brels * MAX_REL_FILES);
    if (!brels) {
        fprintf(stderr, "invfs-pack: out of memory\n");
        free(shtext);
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    for (line = strtok_r(shtext, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char expect[128], rel[256], dest[MAX_PATH];
        char got[65];
        char *sp;
        struct stat st;
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line || *line == '#')
            continue;
        if (sscanf(line, "%127s", expect) != 1)
            continue;
        sp = strstr(line, "  ");
        if (!sp)
            sp = strchr(line, ' ');
        if (!sp)
            continue;
        sp++;
        while (*sp == ' ')
            sp++;
        snprintf(rel, sizeof rel, "%s", sp);
        {
            char *e2 = rel + strlen(rel);
            while (e2 > rel && (e2[-1] == '\r' || e2[-1] == ' '))
                *--e2 = 0;
        }
        if (strcmp(rel, "sha256") == 0)
            continue; /* the listing itself: transport, not content */
        if (!rel[0] || zip_entry_ok(rel) != 0) {
            fprintf(stderr, "invfs-pack: bad path in .ivpack sha256 "
                    "listing: %s\n", rel);
            free(shtext);
            free(brels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (!hex64_ok(expect)) {
            fprintf(stderr, "invfs-pack: bad sha256 line for %s\n", rel);
            free(shtext);
            free(brels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (snprintf(dest, sizeof dest, "%s/%s", dl,
                     rel) >= (int)sizeof dest) {
            fprintf(stderr, "invfs-pack: path too long: %s\n", rel);
            free(shtext);
            free(brels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (lstat(dest, &st) != 0 || !S_ISREG(st.st_mode)) {
            fprintf(stderr, "invfs-pack: REFUSED: %s %s/%s is missing "
                    "from the .ivpack bundle (tampered pack?)\n",
                    best->name, best->version, rel);
            free(shtext);
            free(brels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (r256_file(dest, got) != 0 ||
            strcasecmp(expect, got) != 0) {
            fprintf(stderr, "invfs-pack: REFUSED: sha256 mismatch for "
                    "%s %s/%s (tampered pack?)\n", best->name,
                    best->version, rel);
            free(shtext);
            free(brels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (nbrels < MAX_REL_FILES)
            snprintf(brels[nbrels++], sizeof brels[0], "%s", rel);
        bfiles++;
    }
    free(shtext);
    if (bfiles == 0) {
        fprintf(stderr, "invfs-pack: REFUSED: empty sha256 listing in "
                ".ivpack bundle for '%s' (nothing verified)\n", best->name);
        free(brels);
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    /* the bundle must carry exactly the listed set: anything extra
     * (or any symlink/special file) refuses, same class as a mismatch. */
    {
        extras_ctx cx;
        int rc;
        cx.rels = (const char (*)[256])brels;
        cx.nrels = nbrels;
        cx.rootlen = strlen(dl) + 1;
        cx.badrel[0] = 0;
        cx.nonregular = 0;
        rc = extras_walk(dl, &cx);
        if (rc == -2) {
            if (cx.nonregular)
                fprintf(stderr, "invfs-pack: REFUSED: .ivpack bundle "
                        "for %s %s carries a non-regular file %s "
                        "(tampered pack?)\n", best->name, best->version,
                        cx.badrel);
            else
                fprintf(stderr, "invfs-pack: REFUSED: .ivpack bundle "
                        "for %s %s carries unlisted file %s (tampered "
                        "pack?)\n", best->name, best->version,
                        cx.badrel);
            free(brels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        if (rc != 0) {
            fprintf(stderr, "invfs-pack: cannot scan unpacked .ivpack "
                    "for %s %s\n", best->name, best->version);
            free(brels);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
    }
    free(brels);
    /* cross-check against the raw-tree listing: every listed file except
     * the listing itself and manifest.sig (v1.1.0 ships none) must be
     * present with an equal hash. A bundle re-hashed by its tamperer
     * still fails here, because the expected hashes come from the raw
     * tree, not the bundle. */
    {
        char *rshtext = NULL;
        size_t rshlen = 0;
        char *rsave = NULL, *rline;
        int rfiles = 0, rmiss = 0;
        if (repo_fetch_mem(raw, best->sha256_rel, &rshtext, &rshlen,
                           &rmiss) != 0 || !rshtext) {
            if (rmiss) {
                if (missing)
                    *missing = 1;
                repo_curl_missing();
            } else {
                fprintf(stderr, "invfs-pack: cannot fetch %s/%s to verify "
                        "the .ivpack bundle\n", raw->base,
                        best->sha256_rel);
            }
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
        for (rline = strtok_r(rshtext, "\n", &rsave); rline;
             rline = strtok_r(NULL, "\n", &rsave)) {
            char expect[128], rel[256], dest[MAX_PATH];
            char got[65];
            char *sp;
            struct stat st;
            while (*rline == ' ' || *rline == '\t')
                rline++;
            if (!*rline || *rline == '#')
                continue;
            if (sscanf(rline, "%127s", expect) != 1)
                continue;
            sp = strstr(rline, "  ");
            if (!sp)
                sp = strchr(rline, ' ');
            if (!sp)
                continue;
            sp++;
            while (*sp == ' ')
                sp++;
            snprintf(rel, sizeof rel, "%s", sp);
            {
                char *e2 = rel + strlen(rel);
                while (e2 > rel && (e2[-1] == '\r' || e2[-1] == ' '))
                    *--e2 = 0;
            }
            if (strcmp(rel, "sha256") == 0)
                continue;
            if (strcmp(rel, "manifest.sig") == 0)
                continue; /* v1.1.0 ships no sig sidecar: the WP203
                           * gate below takes the missing-signature path */
            if (!rel[0] || zip_entry_ok(rel) != 0) {
                fprintf(stderr, "invfs-pack: bad path in sha256 "
                        "listing: %s\n", rel);
                free(rshtext);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
            if (!hex64_ok(expect)) {
                fprintf(stderr, "invfs-pack: bad sha256 line for %s\n",
                        rel);
                free(rshtext);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
            if (snprintf(dest, sizeof dest, "%s/%s", dl,
                         rel) >= (int)sizeof dest) {
                fprintf(stderr, "invfs-pack: path too long: %s\n", rel);
                free(rshtext);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
            if (lstat(dest, &st) != 0 || !S_ISREG(st.st_mode)) {
                fprintf(stderr, "invfs-pack: REFUSED: %s %s/%s is missing "
                        "from the .ivpack bundle (tampered pack?)\n",
                        best->name, best->version, rel);
                free(rshtext);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
            if (r256_file(dest, got) != 0 ||
                strcasecmp(expect, got) != 0) {
                fprintf(stderr, "invfs-pack: REFUSED: sha256 mismatch for "
                        "%s %s/%s (tampered pack?)\n", best->name,
                        best->version, rel);
                free(rshtext);
                rmtree(scratch);
                stage_top_out[0] = 0;
                return -1;
            }
            rfiles++;
        }
        free(rshtext);
        if (rfiles == 0) {
            fprintf(stderr, "invfs-pack: REFUSED: empty sha256 listing for "
                    "'%s' (nothing verified)\n", best->name);
            rmtree(scratch);
            stage_top_out[0] = 0;
            return -1;
        }
    }
    /* The bundle listing is transport, not pack content: drop it from
     * the staged tree before the install handoff, exactly like the raw
     * and tarball paths. lib/ stays: verified above, dormant by design. */
    unlink(shpath);
    /* the bundle is no longer needed; drop it before the handoff so a
     * later rmtree only ever sees the verified tree */
    unlink(zbpath);
    if (rename(dl, staged) != 0) {
        fprintf(stderr, "invfs-pack: cannot stage .ivpack pack\n");
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    if (!is_codecpack(staged)) {
        fprintf(stderr, "invfs-pack: fetched '%s' has no manifest (not a "
                ".codecpack)\n", best->name);
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    if (snprintf(src_out, src_cap, "%s", staged) >= (int)src_cap) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        rmtree(scratch);
        stage_top_out[0] = 0;
        return -1;
    }
    printf("fetched %s %s (.ivpack, %d file(s) verified)\n", best->name,
           best->version, bfiles);
    return 0;
}

/* --release dispatch (round 3): prefer the .ivpack bundle; fall back to
 * the round-2 tarball with exactly one loud line, and only when the
 * pack has no .ivpack asset (repo_fetch_ivpack returns 1). A bundle
 * that exists but fails verification refuses — never falls back.
 * *ivpack_out reports which asset won (1 = .ivpack, 0 = tarball). */
static int repo_fetch_release(const reg_pack *best, pack_repo *raw,
                              const char *repo_spec, const fetch_opts *o,
                              char *src_out, size_t src_cap,
                              char *dstname_out, size_t dst_cap,
                              char *stage_top_out, size_t stage_cap,
                              int *ivpack_out, int *missing)
{
    int rc = repo_fetch_ivpack(best, raw, repo_spec, o, src_out, src_cap,
                               dstname_out, dst_cap, stage_top_out,
                               stage_cap, missing);
    if (rc == 0) {
        if (ivpack_out)
            *ivpack_out = 1;
        return 0;
    }
    if (rc == 1) {
        char tag[128];
        if (o->rel_tag && o->rel_tag[0])
            snprintf(tag, sizeof tag, "%s", o->rel_tag);
        else
            snprintf(tag, sizeof tag, "%s", DEFAULT_IVPACK_TAG);
        fprintf(stderr, "invfs-pack: no .ivpack asset for %s %s under "
                "tag %s; falling back to the release tarball\n",
                best->name, best->version, tag);
        rc = repo_fetch_tarball(best, raw, repo_spec, o, src_out, src_cap,
                                dstname_out, dst_cap, stage_top_out,
                                stage_cap, missing);
        if (rc == 0 && ivpack_out)
            *ivpack_out = 0;
        return rc;
    }
    return -1;
}

>>>>>>> wp/608-pack-registry-fetch
/* Fetch one pack by registry name into the tool scratch and sha256-verify
 * every file against the pack's sha256 listing. On success src_out holds
 * the staged <name>.codecpack dir, dstname_out its canonical install
 * name, and stage_top_out the scratch top (the caller rmtree()s it after
 * the existing install path finishes, pass or fail). `o` pins the
 * version (NULL/"" = latest) and selects the release tarball over the
 * raw tree. Returns 0, or -1 with a diagnostic printed. */
static int repo_fetch_pack(const char *name, const char *repo_spec,
                           const fetch_opts *o, char *src_out, size_t src_cap, char *dstname_out,
                           size_t dst_cap, char *stage_top_out,
<<<<<<< HEAD
                           size_t stage_cap, int *missing)
=======
                           size_t stage_cap, int *ivpack_out, int *missing)
>>>>>>> wp/608-pack-registry-fetch
{
    pack_repo r;
    reg_pack *packs = NULL;
    int npacks, miss = 0;
    const reg_pack *best;
    char scratch[MAX_PATH];
    char staged[MAX_PATH];
    char packdir_rel[256];
    char shpath[MAX_PATH];
    char *shtext = NULL;
    size_t shlen = 0;
    char *save = NULL, *line;
    int files = 0;
    src_out[0] = 0;
    stage_top_out[0] = 0;
<<<<<<< HEAD
=======
    if (ivpack_out)
        *ivpack_out = 0;
>>>>>>> wp/608-pack-registry-fetch
    if (missing)
        *missing = 0;
    repo_resolve(&r, repo_spec);
    npacks = repo_load_index(&r, &packs, &miss);
    if (missing)
        *missing = miss;
    if (npacks < 0) {
        if (miss)
            repo_curl_missing();
        else
            fprintf(stderr, "invfs-pack: registry unreachable: no "
                    "codecpacks/index.json under %s%s\n", r.http ? "" : "file://", r.base);
        return -1;
    }
    best = repo_pick(packs, npacks, name, o ? o->want_ver : NULL);
    if (o && o->want_ver && *o->want_ver && !best) {
        fprintf(stderr, "invfs-pack: pack '%s' version '%s' not found "
                "in registry %s\n", name, o->want_ver, r.base);
        free(packs);
        return -1;
    }
    if (!best) {
        fprintf(stderr, "invfs-pack: pack '%s' not found in registry %s\n",
                name, r.base);
        free(packs);
        return -1;
    }
    if (o && o->release) {
        int rc = repo_fetch_release(best, &r, repo_spec, o, src_out,
                                    src_cap, dstname_out, dst_cap,
<<<<<<< HEAD
                                    stage_top_out, stage_cap, missing);
=======
                                    stage_top_out, stage_cap, ivpack_out,
                                    missing);
>>>>>>> wp/608-pack-registry-fetch
        free(packs);
        return rc;
    }
    printf("fetching %s %s from %s\n", best->name,
           best->version[0] ? best->version : "?", r.base);
    /* stage in the tool scratch (sized like any other tool job) */
    if (tool_tmpdir(scratch, sizeof scratch, 65536) != 0) {
        fprintf(stderr, "invfs-pack: no scratch available for the fetch\n");
        free(packs);
        return -1;
    }
    if (snprintf(stage_top_out, stage_cap, "%s", scratch) >=
        (int)stage_cap) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        free(packs);
        return -1;
    }
    {
        size_t nl = strlen(name);
        if (nl > 10 && strcmp(name + nl - 10, ".codecpack") == 0) {
            if (snprintf(dstname_out, dst_cap, "%s", name) >=
                (int)dst_cap) {
                fprintf(stderr, "invfs-pack: pack name too long: %s\n",
                        name);
                free(packs);
                return -1;
            }
        } else {
            if (snprintf(dstname_out, dst_cap, "%s.codecpack", name) >=
                (int)dst_cap) {
                fprintf(stderr, "invfs-pack: pack name too long: %s\n",
                        name);
                free(packs);
                return -1;
            }
        }
    }
    if (snprintf(staged, sizeof staged, "%s/%s", scratch,
                 dstname_out) >= (int)sizeof staged ||
        pack_mkdir_p(staged) != 0) {
        fprintf(stderr, "invfs-pack: cannot stage under %s\n", scratch);
        free(packs);
        return -1;
    }
    /* the sha256 listing lives next to the pack files; its own directory
     * is the download root for every listed relative path */
    {
        const char *slash = strrchr(best->sha256_rel, '/');
        size_t dl = slash ? (size_t)(slash - best->sha256_rel) : 0;
        if (dl == 0 || dl >= sizeof packdir_rel) {
            fprintf(stderr, "invfs-pack: bad sha256 path in index for "
                    "'%s'\n", name);
            free(packs);
            return -1;
        }
        memcpy(packdir_rel, best->sha256_rel, dl);
        packdir_rel[dl] = 0;
    }
    if (snprintf(shpath, sizeof shpath, "%s/sha256", staged) >=
        (int)sizeof shpath) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        free(packs);
        return -1;
    }
    if (repo_fetch_file(&r, best->sha256_rel, shpath, &miss) != 0) {
        if (miss) {
            if (missing)
                *missing = 1;
            repo_curl_missing();
        } else {
            fprintf(stderr, "invfs-pack: cannot fetch %s/%s\n",
                    r.base, best->sha256_rel);
        }
        free(packs);
        return -1;
    }
    shtext = pack_read_file(shpath, &shlen);
    if (!shtext) {
        fprintf(stderr, "invfs-pack: cannot read fetched sha256\n");
        free(packs);
        return -1;
    }
    for (line = strtok_r(shtext, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char expect[128], rel[256], full[MAX_PATH * 2 + 64];
        char dest[MAX_PATH];
        char got[65];
        char *sp;
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line || *line == '#')
            continue;
        if (sscanf(line, "%127s", expect) != 1)
            continue;
        sp = strstr(line, "  ");
        if (!sp)
            sp = strchr(line, ' ');
        if (!sp)
            continue;
        sp++;
        while (*sp == ' ')
            sp++;
        snprintf(rel, sizeof rel, "%s", sp);
        {
            char *e = rel + strlen(rel);
            while (e > rel && (e[-1] == '\r' || e[-1] == ' '))
                *--e = 0;
        }
        if (strcmp(rel, "sha256") == 0)
            continue; /* the listing itself: already fetched, not content */
        if (!rel[0] || strstr(rel, "..") != NULL) {
            fprintf(stderr, "invfs-pack: bad path in sha256 listing: %s\n",
                    rel);
            free(shtext);
            free(packs);
            return -1;
        }
        if (!hex64_ok(expect)) {
            fprintf(stderr, "invfs-pack: bad sha256 line for %s\n", rel);
            free(shtext);
            free(packs);
            return -1;
        }
        if (snprintf(full, sizeof full, "%s/%s", packdir_rel, rel) >=
            (int)sizeof full ||
            snprintf(dest, sizeof dest, "%s/%s", staged, rel) >=
            (int)sizeof dest) {
            fprintf(stderr, "invfs-pack: path too long: %s\n", rel);
            free(shtext);
            free(packs);
            return -1;
        }
        {
            char *slash = strrchr(dest, '/');
            if (slash && slash != dest) {
                *slash = 0;
                if (pack_mkdir_p(dest) != 0) {
                    fprintf(stderr, "invfs-pack: cannot stage %s\n", rel);
                    free(shtext);
                    free(packs);
                    return -1;
                }
                *slash = '/';
            }
        }
        if (repo_fetch_file(&r, full, dest, &miss) != 0) {
            if (miss) {
                if (missing)
                    *missing = 1;
                repo_curl_missing();
            } else {
                fprintf(stderr, "invfs-pack: cannot fetch %s/%s\n",
                        r.base, full);
            }
            free(shtext);
            free(packs);
            return -1;
        }
        if (r256_file(dest, got) != 0 ||
            strcasecmp(expect, got) != 0) {
            fprintf(stderr, "invfs-pack: REFUSED: sha256 mismatch for "
                    "%s %s/%s (tampered pack?)\n", best->name,
                    best->version, rel);
            free(shtext);
            free(packs);
            return -1;
        }
        files++;
    }
    free(shtext);
    if (files == 0) {
        fprintf(stderr, "invfs-pack: REFUSED: empty sha256 listing for "
                "'%s' (nothing verified)\n", name);
        free(packs);
        return -1;
    }
    /* The listing is transport, not pack content: drop it from the staged
     * tree before the install handoff, or it would land in the installed
     * pack (where the local `verify` would misread it as a stored hash).
     * Sign the fixture/authored pack without the listing inside, and the
     * staged set is exactly the signed set. */
    unlink(shpath);
    if (!is_codecpack(staged)) {
        fprintf(stderr, "invfs-pack: fetched '%s' has no manifest (not a "
                ".codecpack)\n", name);
        free(packs);
        return -1;
    }
    if (snprintf(src_out, src_cap, "%s", staged) >= (int)src_cap) {
        fprintf(stderr, "invfs-pack: scratch path too long\n");
        free(packs);
        return -1;
    }
    printf("fetched %s %s (%d file(s) verified)\n", best->name,
           best->version, files);
    free(packs);
    return 0;
}

/* check if a directory exists and looks like a .codecpack */
static int is_codecpack(const char *dir)
{
    char mp[MAX_PATH + 64];
    struct stat st;
    if (pathf(mp, sizeof mp, "%s/manifest", dir, NULL) != 0) return 0;
    return stat(mp, &st) == 0 && S_ISREG(st.st_mode);
}

/* read a single value from packs.conf: "family = pack_name" */
static int read_conf_value(const char *family, char *out, size_t outlen)
{
    FILE *f = fopen(PACK_CONF, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    while (fgets(line, sizeof line, f)) {
        strip_nl(line);
        if (line[0] == '#' || line[0] == 0) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (*k == ' ' || *k == '\t') k++;
        char *ke = k + strlen(k) - 1;
        while (ke > k && (*ke == ' ' || *ke == '\t')) *ke-- = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (strcmp(k, family) == 0) {
            strncpy(out, v, outlen - 1);
            out[outlen - 1] = 0;
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

/* write/update packs.conf */
static int write_conf_value(const char *family, const char *pack)
{
    char lines[256][MAX_LINE];
    int n = 0, found = 0;
    FILE *f = fopen(PACK_CONF, "r");
    if (f) {
        while (n < 256 && fgets(lines[n], MAX_LINE, f)) {
            strip_nl(lines[n]);
            char *eq = strchr(lines[n], '=');
            if (eq) {
                char *k = lines[n];
                while (*k == ' ' || *k == '\t') k++;
                char tmp = *eq; *eq = 0;
                int match = strcmp(k, family) == 0;
                *eq = tmp;
                if (match) {
                    found = 1;
                    snprintf(lines[n], MAX_LINE, "%s = %s", family, pack);
                }
            }
            n++;
        }
        fclose(f);
    }
    if (!found) {
        snprintf(lines[n], MAX_LINE, "%s = %s", family, pack);
        n++;
    }
    /* ensure directory exists */
    mkdir("/.invariantfs", 0755);
    mkdir("/.invariantfs/codecpacks", 0755);
    f = fopen(PACK_CONF, "w");
    if (!f) { perror(PACK_CONF); return -1; }
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\n", lines[i]);
    fclose(f);
    return 0;
}

/* ---- commands ---- */

static void cmd_where(const char *repo_spec)
{
    pack_repo r;
    printf("Pack roots (search order):\n");
    const char *env = env_codecpacks();
    if (env) printf("  $INVFS_CODECPACKS = %s\n", env);
    printf("  %s  (host, trusted)\n", HOST_ROOT);
    printf("  %s  (host, legacy)\n", LEGACY_ROOT);
    printf("  %s  (system)\n", USR_ROOT);
    repo_resolve(&r, repo_spec);
    printf("Registry (bare `install <name>`):\n");
    printf("  %s%s\n", r.http ? "" : "file://", r.base);
    printf("  (--repo URL or $INVFS_PACK_REPO overrides the default)\n");
}


/* list the packs carried by an (offline) volume image */
static void cmd_list_volume(const char *img, const char *filter_family)
{
    int err = 0, cap = 64, n = 0, i, found = 0;
    invfs_volume *v;
    invfs_dirent *ents = NULL;
    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
        return;
    }
    for (;;) {
        free(ents);
        ents = (invfs_dirent *)malloc((size_t)cap * sizeof *ents);
        if (!ents) { vol_close(v); return; }
        n = vol_list_dir(v, ".invariantfs/codecpacks", ents, cap);
        if (n < 0) break;
        if (n < cap) break;
        cap *= 2;
    }
    printf("%-20s %-8s %-6s %-12s %-10s %s\n",
           "NAME", "ALGO", "VER", "FAMILY", "PRIORITY", "SOURCE");
    if (n > 0) {
        for (i = 0; i < n; i++) {
            char nm[256], mpath[384];
            size_t nl;
            uint64_t ino;
            uint8_t *buf = NULL;
            size_t blen = 0;
            pack_info pi;
            snprintf(nm, sizeof nm, "%s", ents[i].name);
            nl = strlen(nm);
            while (nl && nm[nl - 1] == '/') nm[--nl] = 0;
            if (nl < 11 || strcmp(nm + nl - 10, ".codecpack") != 0)
                continue;
            if (snprintf(mpath, sizeof mpath,
                         ".invariantfs/codecpacks/%s/manifest",
                         nm) >= (int)sizeof mpath)
                continue;
            ino = vol_find(v, mpath);
            if (!ino) continue;
            if (vol_read_file(v, ino, &buf, &blen) != 0 || !buf) {
                free(buf);
                continue;
            }
            /* manifests are small text: NUL-terminate + stream-parse */
            memset(&pi, 0, sizeof pi);
            {
                char *text = (char *)malloc(blen + 1);
                FILE *f;
                if (!text) { free(buf); continue; }
                memcpy(text, buf, blen);
                text[blen] = 0;
                free(buf);
                f = fmemopen(text, blen, "r");
                if (!f) { free(text); continue; }
                parse_manifest_stream(f, &pi);
                fclose(f);
                free(text);
            }
            if (filter_family[0] && strcmp(pi.family, filter_family) != 0)
                continue;
            /* install-source sibling recorded at install time */
            {
                char spath[448], text[16] = "";
                uint8_t *sbuf = NULL;
                size_t slen = 0;
                uint64_t sino;
                char srctag[16] = "";
                if (snprintf(spath, sizeof spath,
                             ".invariantfs/codecpacks/%s.source",
                             nm) < (int)sizeof spath &&
                    (sino = vol_find(v, spath)) != 0 &&
                    vol_read_file(v, sino, &sbuf, &slen) == 0 && sbuf) {
                    size_t cp = slen < sizeof text - 1 ? slen : sizeof text - 1;
                    memcpy(text, sbuf, cp);
                    text[cp] = 0;
                    free(sbuf);
                    strip_nl(text);
                    if (strcmp(text, "ivpack") == 0 ||
                        strcmp(text, "raw") == 0 ||
                        strcmp(text, "local") == 0)
                        snprintf(srctag, sizeof srctag, " [%s]", text);
                } else {
                    free(sbuf);
                }
                printf("%-20s %-8s %-6s %-12s %-10s volume:%s%s\n",
                       pi.name[0] ? pi.name : nm,
                       pi.algo, pi.pack_version, pi.family,
                       pi.priority[0] ? pi.priority : "-", img, srctag);
            }
            found++;
        }
    }
    free(ents);
    vol_close(v);
    if (!found) printf("  (none)\n");
    else printf("%d pack(s)\n", found);
}

static void cmd_list(const char *filter_family, int installed_only,
                   const char *repo_spec)
{
    const char *roots_all[] = {
        env_codecpacks(),
        HOST_ROOT,
        LEGACY_ROOT,
        USR_ROOT,
        NULL
    };
    const char *roots_inst[] = { HOST_ROOT, NULL };
    const char **roots = installed_only ? roots_inst : roots_all;
    int found = 0;
    printf("%-20s %-8s %-6s %-12s %-10s %s\n",
           "NAME", "ALGO", "VER", "FAMILY", "PRIORITY", "SOURCE");
    for (int r = 0; roots[r]; r++) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char full[MAX_PATH + 64];
            if (pathf(full, sizeof full, "%s/%s", roots[r], de->d_name) != 0)
                continue;
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (!is_codecpack(full)) continue;
            /* installed = same dirname present under the host root */
            char probe[MAX_PATH + 64];
            struct stat pst;
            int is_host = (strcmp(roots[r], HOST_ROOT) == 0);
            int is_inst = is_host; /* --installed scans the host root itself */
            if (!is_inst &&
                pathf(probe, sizeof probe, "%s/%s", HOST_ROOT, de->d_name) == 0 &&
                stat(probe, &pst) == 0 && S_ISDIR(pst.st_mode))
                is_inst = 1;
            pack_info pi;
            char srctag[16] = "";
            char markpath[MAX_PATH + 80];
            memset(&pi, 0, sizeof pi);
            if (pathf(markpath, sizeof markpath, "%s.source", full,
                       NULL) == 0) {
                char kind[16];
                if (read_source_marker(markpath, kind,
                                       sizeof kind) == 0)
                    snprintf(srctag, sizeof srctag, " [%s]", kind);
            }
            char mp[MAX_PATH + 64];
            if (pathf(mp, sizeof mp, "%s/manifest", full, NULL) != 0) continue;
            parse_manifest(mp, &pi);
            if (filter_family[0] && strcmp(pi.family, filter_family) != 0)
                continue;
            printf("%-20s %-8s %-6s %-12s %-10s %s%s%s\n",
                   pi.name[0] ? pi.name : de->d_name,
                   pi.algo,
                   pi.pack_version,
                   pi.family,
                   pi.priority[0] ? pi.priority : "-",
                   roots[r],
                   is_inst ? " [installed]" : "",
                   srctag);
            found++;
        }
        closedir(d);
    }
    if (!found) printf("  (none)\n");
    else printf("%d pack(s)\n", found);
    if (installed_only)
        return;
    /* WP608: registry-available packs alongside the installed ones.
     * An unreachable repo degrades to installed-only with one loud
     * line — never a silent empty addition. */
    {
        pack_repo r;
        reg_pack *packs = NULL;
        int npacks, miss = 0, i, shown = 0;
        repo_resolve(&r, repo_spec);
        npacks = repo_load_index(&r, &packs, &miss);
        if (npacks < 0) {
            if (miss)
                repo_curl_missing();
            fprintf(stderr, "warning: registry %s%s unreachable "
                    "(no codecpacks/index.json): showing installed "
                    "packs only\n", r.http ? "" : "file://", r.base);
            return;
        }
        for (i = 0; i < npacks; i++) {
            char probe[MAX_PATH + 64];
            struct stat pst;
            int is_inst;
            if (filter_family[0] && strcmp(packs[i].family, filter_family) != 0)
                continue;
            if (pathf(probe, sizeof probe, "%s/%s.codecpack", HOST_ROOT,
                      packs[i].name) != 0)
                continue;
            is_inst = (stat(probe, &pst) == 0 && S_ISDIR(pst.st_mode));
            printf("%-20s %-8s %-6s %-12s %-10s %s%s\n",
                   packs[i].name,
                   packs[i].algo[0] ? packs[i].algo : "-",
                   packs[i].pack_version[0] ? packs[i].pack_version : "-",
                   packs[i].family[0] ? packs[i].family : "-",
                   packs[i].priority[0] ? packs[i].priority : "-",
                   r.base,
                   is_inst ? " [installed]" : " [available]");
            shown++;
        }
        if (shown)
            printf("%d pack(s) available in registry\n", shown);
        free(packs);
    }
}

static void cmd_info(const char *name)
{
    const char *roots[] = { env_codecpacks(), HOST_ROOT, LEGACY_ROOT, USR_ROOT, NULL };
    for (int r = 0; roots[r]; r++) {
        char full[MAX_PATH + 64];
        if (pathf(full, sizeof full, "%s/%s.codecpack", roots[r], name) != 0)
            continue;
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        pack_info pi;
        memset(&pi, 0, sizeof pi);
        char mp[MAX_PATH + 64];
        if (pathf(mp, sizeof mp, "%s/manifest", full, NULL) != 0) continue;
        if (parse_manifest(mp, &pi) != 0) continue;
        printf("Pack: %s\n", pi.name);
        printf("  source:       %s\n", roots[r]);
        printf("  algo:         %s\n", pi.algo);
        printf("  pack_version: %s\n", pi.pack_version);
        if (pi.codec_id[0])   printf("  codec_id:     %s\n", pi.codec_id);
        if (pi.version[0])    printf("  version:      %s\n", pi.version);
        if (pi.min_read[0])   printf("  min_read:     %s\n", pi.min_read);
        if (pi.family[0])     printf("  family:       %s\n", pi.family);
        if (pi.category[0])   printf("  category:     %s\n", pi.category);
        if (pi.provides[0])   printf("  provides:     %s\n", pi.provides);
        if (pi.replaces[0])   printf("  replaces:     %s\n", pi.replaces);
        if (pi.conflicts[0])  printf("  conflicts:    %s\n", pi.conflicts);
        if (pi.priority[0])   printf("  priority:     %s\n", pi.priority);
        if (pi.type[0])       printf("  type:         %s\n", pi.type);
        if (pi.caps[0])       printf("  caps:         %s\n", pi.caps);
        if (pi.requires[0])   printf("  requires:     %s\n", pi.requires);
        /* show all raw fields */
        printf("  --- all fields ---\n");
        for (int i = 0; i < pi.field_count; i++)
            printf("  %s = %s\n", pi.fields[i].key, pi.fields[i].val);
        return;
    }
    fprintf(stderr, "pack '%s' not found\n", name);
}

/* copy a pack dir to the host root (fork+exec cp -a, no shell). */
static void copy_pack_dir(const char *src, const char *dst)
{
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "failed to fork for copy: %s -> %s\n", src, dst);
        return;
    }
    if (pid == 0) {
        execlp("cp", "cp", "-a", src, dst, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "failed to copy pack: %s -> %s\n", src, dst);
        return;
    }
    printf("installed '%s' -> %s\n", src, dst);
}

/* Quiet local lookup for a bare name (both spellings). 0 with
 * src[]/dstname[] filled, -1 when no installed root carries it.
 * Unlike resolve_pack_source this prints nothing: cmd_install falls
 * through to the registry fetch on -1. */
static int find_local_pack(const char *name, char *src, size_t src_cap,
                           char *dstname, size_t dst_cap)
{
    const char *roots[] = { env_codecpacks(), HOST_ROOT, LEGACY_ROOT,
                            USR_ROOT, NULL };
    const char *want = name;
    char wantbuf[128 + 16];
    int r;
    if (strlen(want) <= 10 ||
        strcmp(want + strlen(want) - 10, ".codecpack") != 0) {
        if (snprintf(wantbuf, sizeof wantbuf, "%s.codecpack", name) >=
            (int)sizeof wantbuf)
            return -1;
        want = wantbuf;
    }
    if (snprintf(dstname, dst_cap, "%s", want) >= (int)dst_cap)
        return -1;
    for (r = 0; roots[r]; r++) {
        char cand[MAX_PATH];
        struct stat st;
        if (!roots[r])
            continue;
        if (snprintf(cand, sizeof cand, "%s/%s", roots[r], want) >=
            (int)sizeof cand)
            continue;
        if (stat(cand, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;
        if (!is_codecpack(cand))
            continue;
        if (snprintf(src, src_cap, "%s", cand) >= (int)src_cap)
            continue;
        return 0;
    }
    return -1;
}

/* Resolve <name|./path> to a source pack dir + canonical dst dirname.
 * Returns 0 with src[]/dstname[] filled, -1 with a diagnostic printed. */
static int resolve_pack_source(const char *name, char *src, size_t src_cap,
                               char *dstname, size_t dst_cap)
{
    src[0] = 0;

    if (strchr(name, '/')) {
        /* explicit path to a .codecpack dir (./path, ../, absolute) */
        struct stat st;
        size_t l;
        const char *base;
        if (stat(name, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "pack path '%s' is not a directory\n", name);
            return -1;
        }
        if (snprintf(src, src_cap, "%s", name) >= (int)src_cap) {
            fprintf(stderr, "pack path too long: %s\n", name);
            return -1;
        }
        /* strip trailing slashes for the manifest check + basename */
        l = strlen(src);
        while (l > 1 && src[l - 1] == '/') src[--l] = 0;
        if (!is_codecpack(src)) {
            fprintf(stderr, "pack path '%s' has no manifest (not a .codecpack)\n", name);
            return -1;
        }
        base = strrchr(src, '/');
        base = base ? base + 1 : src;
        if (snprintf(dstname, dst_cap, "%s", base) >= (int)dst_cap) {
            fprintf(stderr, "pack name too long: %s\n", base);
            return -1;
        }
        if (strlen(dstname) <= 10 || strcmp(dstname + strlen(dstname) - 10, ".codecpack") != 0) {
            if (snprintf(dstname, dst_cap, "%s.codecpack", base) >= (int)dst_cap) {
                fprintf(stderr, "pack name too long: %s\n", base);
                return -1;
            }
        }
    } else {
        const char *roots[] = { env_codecpacks(), HOST_ROOT, LEGACY_ROOT, USR_ROOT, NULL };
        const char *want = name;
        char wantbuf[128 + 16];
        /* allow both "jxl" and "jxl.codecpack" spellings */
        if (strlen(want) <= 10 || strcmp(want + strlen(want) - 10, ".codecpack") != 0) {
            if (snprintf(wantbuf, sizeof wantbuf, "%s.codecpack", name) >= (int)sizeof wantbuf) {
                fprintf(stderr, "pack name too long: %s\n", name);
                return -1;
            }
            want = wantbuf;
        }
        if (snprintf(dstname, dst_cap, "%s", want) >= (int)dst_cap) {
            fprintf(stderr, "pack name too long: %s\n", name);
            return -1;
        }
        for (int r = 0; roots[r]; r++) {
            char cand[MAX_PATH];
            struct stat st;
            if (snprintf(cand, sizeof cand, "%s/%s", roots[r], want) >= (int)sizeof cand)
                continue;
            if (stat(cand, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (!is_codecpack(cand)) continue;
            if (snprintf(src, src_cap, "%s", cand) >= (int)src_cap) continue;
            break;
        }
        if (!src[0]) {
            fprintf(stderr, "pack '%s' not found in any root\n", name);
            return -1;
        }
    }
    return 0;
}

/* ---- install to a volume image (reserve follow-up, owner request) ----
 * Reuses the tested import path: stage <pack> under
 * <tmp>/.invariantfs/codecpacks/ and invf-import the staging tree.
 * The volume must be OFFLINE (unmounted), like invf-sweep offline. */

static int rmtree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    struct stat st;
    char full[MAX_PATH + 64];
    if (!d) {
        if (unlink(path) == 0 || errno == ENOENT) return 0;
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (snprintf(full, sizeof full, "%s/%s", path,
                     de->d_name) >= (int)sizeof full) {
            closedir(d);
            return -1;
        }
        if (lstat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (rmtree(full) != 0) { closedir(d); return -1; }
        } else if (unlink(full) != 0 && errno != ENOENT) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    return rmdir(path);
}

/* resolve a sibling tool (invf-import): $INVFS_TOOLS, argv[0]'s dir,
 * PATH, then the system tool dir. Returns 0 with dst filled. */
static int find_tool(const char *prog, const char *argv0, char *dst,
                     size_t cap)
{
    const char *e = getenv("INVFS_TOOLS");
    static const char *sysdirs[] = { "/usr/lib/invfs/tools",
                                     "/usr/local/bin", "/usr/bin", NULL };
    int i;
    if (e && *e) {
        if (snprintf(dst, cap, "%s/%s", e, prog) < (int)cap &&
            access(dst, X_OK) == 0)
            return 0;
    }
    if (argv0) {
        const char *s = strrchr(argv0, '/');
        if (s && (size_t)(s - argv0) < MAX_PATH - 1) {
            char dir[MAX_PATH];
            memcpy(dir, argv0, (size_t)(s - argv0));
            dir[s - argv0] = 0;
            if (snprintf(dst, cap, "%s/%s", dir, prog) < (int)cap &&
                access(dst, X_OK) == 0)
                return 0;
        }
    }
    for (i = 0; sysdirs[i]; i++) {
        if (snprintf(dst, cap, "%s/%s", sysdirs[i],
                     prog) < (int)cap &&
            access(dst, X_OK) == 0)
            return 0;
    }
    /* last resort: execvp's PATH search in the child */
    if (snprintf(dst, cap, "%s", prog) >= (int)cap) return -1;
    return 0;
}

static int run_argv(char *const argv[])
{
    pid_t pid = fork();
    int status = 0;
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

static const char *g_argv0;

/* ---- install-source markers ([ivpack] vs [raw] vs [local]) ----
 * `list` reports where an installed pack came from. The answer is
 * recorded at install time in a SIBLING file, never inside the pack
 * dir: the WP203 integrity binding covers every regular file except
 * manifest and manifest.sig, so anything written into the dir would
 * turn the next `verify` BAD. Siblings are ignored by every pack scan
 * (all require S_ISDIR and the .codecpack suffix), by import, and by
 * fsck. A pack installed before markers existed simply shows no tag. */
static const char *pack_source_kind(int staged, int ivpack)
{
    if (!staged)
        return "local";
    return ivpack ? "ivpack" : "raw";
}

static void write_source_marker_file(const char *path, const char *kind)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "invfs-pack: warning: cannot record install "
                "source in %s: %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "%s\n", kind);
    fclose(f);
}

static int read_source_marker(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(out, (int)cap, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    strip_nl(out);
    if (strcmp(out, "ivpack") != 0 && strcmp(out, "raw") != 0 &&
        strcmp(out, "local") != 0)
        return -1;
    return 0;
}

static int cmd_install_volume(const char *src, const char *dstname,
                               const char *volume, int overwrite,
                               int dry_run, const char *srckind)
{
    char stage[] = "/tmp/invfs-pack-XXXXXX";
    char sub[MAX_PATH], dest[MAX_PATH], import[MAX_PATH];
    char mpath[MAX_PATH + 64];
    int err = 0;
    invfs_volume *v;
    uint64_t ino;
    char *av[4];
    int rc;
    /* already aboard? (import merges; without -y refuse the duplicate) */
    v = vol_open(volume, &err);
    if (!v) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", volume, err);
        return 1;
    }
    if (snprintf(mpath, sizeof mpath, ".invariantfs/codecpacks/%s/manifest",
                 dstname) >= (int)sizeof mpath) {
        fprintf(stderr, "pack name too long: %s\n", dstname);
        vol_close(v);
        return 1;
    }
    ino = vol_find(v, mpath);
    if (dry_run) {
        printf("would import '%s' -> %s:.invariantfs/codecpacks/%s%s\n",
               src, volume, dstname,
               ino ? " (already aboard, -y to re-import)" : "");
        vol_close(v);
        return 0;
    }
    vol_close(v);
    if (ino && !overwrite) {
        fprintf(stderr, "pack '%s' already on volume %s (use -y to re-import)\n",
                dstname, volume);
        return 1;
    }
    if (!mkdtemp(stage)) {
        fprintf(stderr, "cannot stage: %s\n", strerror(errno));
        return 1;
    }
    if (snprintf(sub, sizeof sub, "%s/.invariantfs/codecpacks",
                 stage) >= (int)sizeof sub ||
        snprintf(dest, sizeof dest, "%s/%s", sub,
                 dstname) >= (int)sizeof dest) {
        fprintf(stderr, "pack name too long: %s\n", dstname);
        rmtree(stage);
        return 1;
    }
    {
        char inv[512];
        if (snprintf(inv, sizeof inv, "%s/.invariantfs",
                     stage) >= (int)sizeof inv ||
            mkdir(inv, 0700) != 0 ||
            mkdir(sub, 0700) != 0) {
            fprintf(stderr, "cannot stage: %s\n", strerror(errno));
            rmtree(stage);
            return 1;
        }
    }
    /* cp -a, no shell (pack paths are external input) */
    {
        pid_t pid = fork();
        int status = 0;
        if (pid < 0) {
            fprintf(stderr, "cannot stage: fork failed\n");
            rmtree(stage);
            return 1;
        }
        if (pid == 0) {
            execlp("cp", "cp", "-a", src, dest, (char *)NULL);
            _exit(127);
        }
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "cannot stage pack\n");
            rmtree(stage);
            return 1;
        }
    }
    if (find_tool("invf-import", g_argv0, import, sizeof import) != 0) {
        fprintf(stderr, "invf-import not found\n");
        rmtree(stage);
        return 1;
    }
    /* install-source sibling: imported alongside the pack dir, read by
     * `list --volume` as [ivpack]/[raw]/[local]. Outside the pack dir
     * so the WP203 integrity binding never sees it. */
    {
        char mark[MAX_PATH];
        FILE *mf;
        if (snprintf(mark, sizeof mark, "%s/%s.source", sub,
                     dstname) >= (int)sizeof mark) {
            fprintf(stderr, "pack name too long: %s\n", dstname);
            rmtree(stage);
            return 1;
        }
        mf = fopen(mark, "w");
        if (!mf) {
            fprintf(stderr, "cannot stage source marker: %s\n",
                    strerror(errno));
            rmtree(stage);
            return 1;
        }
        fprintf(mf, "%s\n", srckind ? srckind : "local");
        fclose(mf);
    }
    av[0] = import; av[1] = (char *)volume; av[2] = stage; av[3] = NULL;
    rc = run_argv(av);
    rmtree(stage);
    if (rc != 0) {
        fprintf(stderr, "import into %s failed (rc=%d)\n", volume, rc);
        return 1;
    }
    printf("installed '%s' -> %s:.invariantfs/codecpacks/%s\n", src,
           volume, dstname);
    return 0;
}
/* Resolve the signature keyring once per install/verify run. On failure
 * (INVFS_KEYRING unset and HOME unset) the ring is empty: signed packs
 * then refuse as unverifiable, unsigned packs are unaffected. The
 * warning names the cause so the refusal below reads complete. */
static void resolve_keyring(char *out, size_t cap)
{
    char err[256];
    if (pack_sig_default_keyring(out, cap, err, sizeof err) != 0) {
        fprintf(stderr, "pack signatures: %s\n", err);
        out[0] = 0;
    }
}

static int cmd_install(const char *name, int overwrite, int dry_run,
                       const char *volume, int skip_sig,
                       const char *repo_spec, int release,
                       const char *rel_tag, const char *platform)
{
    char src[MAX_PATH];
    char dstname[128 + 16];
    char stage_top[MAX_PATH];
<<<<<<< HEAD
    int staged = 0;
=======
    const char *srckind = "local";
    int staged = 0;
    int was_ivpack = 0;
>>>>>>> wp/608-pack-registry-fetch
    int rc;
    stage_top[0] = 0;
    if (!release && ((rel_tag && *rel_tag) || (platform && *platform)))
        fprintf(stderr, "invfs-pack: note: --release-tag/--platform "
                "need --release; ignoring them\n");
    if (strchr(name, '/')) {
        if (release) {
            fprintf(stderr, "invfs-pack: --release takes a registry pack "
                    "name, not a path: %s\n", name);
            return 1;
        }
        if (resolve_pack_source(name, src, sizeof src, dstname,
                                sizeof dstname) != 0)
            return 1;
    } else {
        char base[128], want[64], fetch_name[128];
        size_t bl;
        fetch_opts o;
        int missing = 0;
        if (split_namever(name, base, sizeof base, want,
                          sizeof want) != 0) {
            fprintf(stderr, "invfs-pack: bad pack name '%s' "
                    "(want <name>[@<version>])\n", name);
            return 1;
        }
        /* the fetch matches registry names, which never carry the
         * installed `.codecpack` suffix spelling */
        snprintf(fetch_name, sizeof fetch_name, "%s", base);
        bl = strlen(fetch_name);
        if (bl > 10 &&
            strcmp(fetch_name + bl - 10, ".codecpack") == 0)
            fetch_name[bl - 10] = 0;
        o.want_ver = want[0] ? want : NULL;
        o.release = release;
        o.rel_tag = rel_tag;
        o.platform = platform;
        if (want[0]) {
            /* Pinned version: always a registry fetch (local roots
             * carry no reliable version), raw tree or --release. */
            if (repo_fetch_pack(fetch_name, repo_spec, &o, src,
                                sizeof src, dstname, sizeof dstname,
<<<<<<< HEAD
                                stage_top, sizeof stage_top,
                                &missing) != 0)
                return 1;
            staged = 1;
=======
                                stage_top, sizeof stage_top, &was_ivpack,
                                &missing) != 0)
                return 1;
            staged = 1;
            srckind = pack_source_kind(1, was_ivpack);
>>>>>>> wp/608-pack-registry-fetch
        } else if (find_local_pack(name, src, sizeof src, dstname,
                                   sizeof dstname) != 0) {
            /* Not installed locally: resolve through the registry,
             * stage into the tool scratch, and install the staged
             * tree through the same gate below. */
            if (repo_fetch_pack(fetch_name, repo_spec, &o, src,
                                sizeof src, dstname, sizeof dstname,
<<<<<<< HEAD
                                stage_top, sizeof stage_top,
                                &missing) != 0)
                return 1;
            staged = 1;
=======
                                stage_top, sizeof stage_top, &was_ivpack,
                                &missing) != 0)
                return 1;
            staged = 1;
            srckind = pack_source_kind(1, was_ivpack);
>>>>>>> wp/608-pack-registry-fetch
        }
    }
    /* WP203 install gate: bad signature refuses (always, no knob);
     * missing signature prompts (-y answers yes, -n reports and
     * installs nothing, --skip proceeds). Runs before EITHER target
     * (host root or --volume import): the trust boundary is the
     * install, not the destination. Fetched packs pass this same gate. */
    {
        char keyring[MAX_PATH];
        char why[512];
        resolve_keyring(keyring, sizeof keyring);
        if (pack_sig_gate_install(src, dstname, keyring, overwrite,
                                  dry_run, skip_sig, NULL, NULL,
                                  why, sizeof why) != 0) {
            fprintf(stderr, "%s\n", why);
            if (staged)
                rmtree(stage_top);
            return 1;
        }
    }
    if (volume && volume[0]) {
<<<<<<< HEAD
        rc = cmd_install_volume(src, dstname, volume, overwrite, dry_run);
=======
        rc = cmd_install_volume(src, dstname, volume, overwrite, dry_run,
                                srckind);
>>>>>>> wp/608-pack-registry-fetch
        if (staged)
            rmtree(stage_top);
        return rc;
    }

    {
        char dst[MAX_PATH];
        struct stat st;
        int exists;
        if (snprintf(dst, sizeof dst, "%s/%s", HOST_ROOT, dstname) >= (int)sizeof dst) {
            fprintf(stderr, "pack name too long: %s\n", dstname);
            rc = 1;
            goto done;
        }
        exists = (stat(dst, &st) == 0 && S_ISDIR(st.st_mode));
        if (dry_run) {
            if (exists)
                printf("would reinstall '%s' -> %s (%s)\n", src, dst,
                       overwrite ? "overwrite" : "exists, needs -y");
            else
                printf("would install '%s' -> %s\n", src, dst);
            rc = 0;
            goto done;
        }
        if (exists && !overwrite) {
            fprintf(stderr, "pack '%s' already installed at %s (use -y to overwrite)\n",
                    dstname, dst);
            rc = 1;
            goto done;
        }
        /* create host root if needed */
        mkdir("/.invariantfs", 0755);
        mkdir(HOST_ROOT, 0755);
        if (exists) {
            /* -y overwrite: drop the old tree first (no shell; see below) */
            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "failed to fork for remove %s\n", dst);
                rc = 1;
                goto done;
            }
            if (pid == 0) {
                execlp("rm", "rm", "-rf", dst, (char *)NULL);
                _exit(127);
            }
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                fprintf(stderr, "failed to remove %s\n", dst);
                rc = 1;
                goto done;
            }
        }
        /* recursive copy. No shell: the pack name comes from outside
         * (registry listing, CLI arg), and single-quote wrapping breaks
         * out on a \' in the path (CodeQL command-line-injection).
         * fork+execlp passes src/dst as argv, never parsed. */
        copy_pack_dir(src, dst);
<<<<<<< HEAD
=======
        /* install-source sibling for `list` ([ivpack]/[raw]/[local]).
         * Outside the pack dir so the WP203 binding never sees it;
         * warn-only: the install itself already succeeded. */
        {
            char mark[MAX_PATH + 16];
            if (snprintf(mark, sizeof mark, "%s.source",
                         dst) < (int)sizeof mark)
                write_source_marker_file(mark, srckind);
        }
>>>>>>> wp/608-pack-registry-fetch
        rc = 0;
    }
done:
    if (staged)
        rmtree(stage_top);
    return rc;
}

/* ---- WP203: keygen + sign ---- */

static int cmd_keygen(const char *keyring_opt, const char *name)
{
    char keyring[MAX_PATH];
    char err[512];
    if (!name || !name[0])
        name = "default";
    if (keyring_opt && keyring_opt[0]) {
        if (snprintf(keyring, sizeof keyring, "%s",
                     keyring_opt) >= (int)sizeof keyring) {
            fprintf(stderr, "keyring path too long: %s\n", keyring_opt);
            return 1;
        }
    } else {
        resolve_keyring(keyring, sizeof keyring);
        if (!keyring[0])
            return 1;
    }
    if (pack_sig_keygen(keyring, name, err, sizeof err) != 0) {
        fprintf(stderr, "keygen: %s\n", err);
        return 1;
    }
    printf("keygen: wrote %s/%s.sec (0600, keep secret) + %s/%s.pub\n",
           keyring, name, keyring, name);
    printf("keygen: install %s/%s.pub into the keyring of every host that "
           "should trust this signer\n", keyring, name);
    return 0;
}

static int cmd_sign(const char *keyfile, const char *target)
{
    char src[MAX_PATH];
    char dstname[128 + 16];
    char err[512];
    if (!keyfile || !target) {
        fprintf(stderr,
                "usage: invfs-pack sign --key SECFILE <pack|./path>\n");
        return 2;
    }
    if (resolve_pack_source(target, src, sizeof src, dstname,
                            sizeof dstname) != 0)
        return 1;
    if (pack_sig_sign_dir(src, keyfile, err, sizeof err) != 0) {
        fprintf(stderr, "sign: %s\n", err);
        return 1;
    }
    printf("signed '%s' (+ integrity binding, manifest.sig)\n", src);
    return 0;
}

static void cmd_remove(const char *name)
{
    char dst[MAX_PATH];
    struct stat st;
    snprintf(dst, sizeof dst, "%s/%s.codecpack", HOST_ROOT, name);
    if (stat(dst, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "pack '%s' not installed at %s\n", name, HOST_ROOT);
        return;
    }    /* No shell (see cmd_install): dst derives from the pack name. */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "failed to fork for remove %s\n", dst);
        return;
    }
    if (pid == 0) {
        execlp("rm", "rm", "-rf", dst, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "failed to remove %s\n", dst);
        return;
    }
    /* drop the install-source sibling too (best effort) */
    {
        char mark[MAX_PATH + 16];
        if (snprintf(mark, sizeof mark, "%s.source",
                     dst) < (int)sizeof mark)
            unlink(mark);
    }
    printf("removed '%s'\n", name);
}

/* Signature status of one installed pack dir. Printed by verify next
 * to the content hash: SIG-OK (signed, key trusted), UNSIGNED (no
 * sidecar — absence, not evidence), BAD (refuse-class: tampered or
 * untrusted). Returns 0/1/2 in pack_sig_status terms for the exit code
 * (BAD is the only one that fails the run: unsigned is the prompt's
 * case at install time, not a verify failure). */
static int sig_status_of(const char *dir, const char *keyring)
{
    char why[512];
    pack_sig_status st =
        pack_sig_verify_dir(dir, keyring, why, sizeof why);
    if (st == PACK_SIG_OK)
        printf("  sig: OK (signed, key trusted)\n");
    else if (st == PACK_SIG_UNSIGNED)
        printf("  sig: UNSIGNED (no manifest.sig)\n");
    else
        printf("  sig: BAD (%s)\n", why);
    return (int)st;
}

static int verify_pack(const char *name, const char *root,
                       const char *keyring)
{
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s/%s.codecpack", root, name);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return PACK_SIG_UNSIGNED;

    /* read existing sha256 file if present */
    char sha_path[MAX_PATH + 16];
    snprintf(sha_path, sizeof sha_path, "%s/sha256", dir);
    FILE *sf = fopen(sha_path, "r");
    char stored_hash[128] = {0};
    if (sf) {
        if (fscanf(sf, "%127s", stored_hash) != 1) stored_hash[0] = 0;
        fclose(sf);
    }

    /* compute blake3 of all non-directory files in the pack */
    blake3_hasher bh;
    blake3_hasher_init(&bh);

    DIR *d = opendir(dir);
    if (!d) return PACK_SIG_UNSIGNED;
    struct dirent *de;
    int count = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strcmp(de->d_name, "sha256") == 0) continue;
        char fp[MAX_PATH];
        if (pathf(fp, sizeof fp, "%s/%s", dir, de->d_name) != 0) continue;
        struct stat fs;
        if (stat(fp, &fs) != 0 || !S_ISREG(fs.st_mode)) continue;
        FILE *f = fopen(fp, "rb");
        if (!f) continue;
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            blake3_hasher_update(&bh, buf, n);
        fclose(f);
        count++;
    }
    closedir(d);

    uint8_t out[BLAKE3_OUT_LEN];
    blake3_hasher_finalize(&bh, out, BLAKE3_OUT_LEN);
    char hex[BLAKE3_OUT_LEN * 2 + 1];
    for (int i = 0; i < BLAKE3_OUT_LEN; i++)
        sprintf(hex + i * 2, "%02x", out[i]);
    hex[BLAKE3_OUT_LEN * 2] = 0;

    if (stored_hash[0]) {
        if (strcmp(hex, stored_hash) == 0)
            printf("%-20s OK  (blake3: %s)\n", name, hex);
        else {
            printf("%-20s MISMATCH  (expected %s, got %s)\n",
                   name, stored_hash, hex);
        }
    } else {
        printf("%-20s (no sha256 file) blake3: %s\n", name, hex);
    }
    (void)count;
    return sig_status_of(dir, keyring);
}

static int cmd_verify(const char *name)
{
    const char *roots[] = { env_codecpacks(), HOST_ROOT, LEGACY_ROOT, USR_ROOT, NULL };
    char keyring[MAX_PATH];
    int bad = 0;
    resolve_keyring(keyring, sizeof keyring);
    if (name && name[0]) {
        for (int r = 0; roots[r]; r++)
            if (verify_pack(name, roots[r], keyring) == PACK_SIG_BAD)
                bad = 1;
        return bad;
    }
    /* verify all packs */
    for (int r = 0; roots[r]; r++) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char full[MAX_PATH + 64];
            if (pathf(full, sizeof full, "%s/%s", roots[r], de->d_name) != 0)
                continue;
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (!is_codecpack(full)) continue;
            /* extract name from dir */
            char n[128];
            const char *p = strrchr(de->d_name, '/');
            if (!p) p = de->d_name; else p++;
            strncpy(n, p, 127); n[127] = 0;
            char *dot = strstr(n, ".codecpack");
            if (dot) *dot = 0;
            if (verify_pack(n, roots[r], keyring) == PACK_SIG_BAD)
                bad = 1;
        }
        closedir(d);
    }
    return bad;
}

static void cmd_alternatives(const char *family)
{
    const char *roots[] = { env_codecpacks(), HOST_ROOT, LEGACY_ROOT, USR_ROOT, NULL };
    int found = 0;

    /* read current choice */
    char current[128] = {0};
    read_conf_value(family, current, sizeof current);

    printf("Candidates for family '%s':\n", family);
    for (int r = 0; roots[r]; r++) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char full[MAX_PATH + 64];
            if (pathf(full, sizeof full, "%s/%s", roots[r], de->d_name) != 0)
                continue;
            struct stat st;
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            if (!is_codecpack(full)) continue;
            pack_info pi;
            memset(&pi, 0, sizeof pi);
            char mp[MAX_PATH + 64];
            if (pathf(mp, sizeof mp, "%s/manifest", full, NULL) != 0) continue;
            parse_manifest(mp, &pi);
            /* match by provides OR family */
            int match = 0;
            if (pi.provides[0] && strcmp(pi.provides, family) == 0) match = 1;
            if (pi.family[0] && strcmp(pi.family, family) == 0) match = 1;
            if (!match) continue;
            const char *n = pi.name[0] ? pi.name : de->d_name;
            int is_primary = (current[0] && strcmp(current, n) == 0);
            printf("  %s%-20s priority=%s%s\n",
                   is_primary ? "* " : "  ",
                   n,
                   pi.priority[0] ? pi.priority : "-",
                   is_primary ? "  (primary)" : "");
            found++;
        }
        closedir(d);
    }
    if (!found) printf("  (none found)\n");
}

static void cmd_use(const char *family, const char *pack)
{
    /* verify pack exists and provides this family */
    const char *roots[] = { env_codecpacks(), HOST_ROOT, LEGACY_ROOT, USR_ROOT, NULL };
    int ok = 0;
    for (int r = 0; roots[r]; r++) {
        char full[MAX_PATH + 64];
        snprintf(full, sizeof full, "%s/%s.codecpack", roots[r], pack);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (!is_codecpack(full)) continue;
        pack_info pi;
        memset(&pi, 0, sizeof pi);
        char mp[MAX_PATH + 64];
        if (pathf(mp, sizeof mp, "%s/manifest", full, NULL) != 0) continue;
        parse_manifest(mp, &pi);
        if ((pi.provides[0] && strcmp(pi.provides, family) == 0) ||
            (pi.family[0] && strcmp(pi.family, family) == 0)) {
            ok = 1;
            break;
        }
    }
    if (!ok) {
        fprintf(stderr, "pack '%s' does not provide family '%s'\n", pack, family);
        return;
    }
    if (write_conf_value(family, pack) == 0)
        printf("set primary encoder for '%s' -> %s\n", family, pack);
}

/* ---- main ---- */

static void usage(void)
{
    fprintf(stderr,
        "usage: invfs-pack list [--volume IMG] [--installed|-i] [--repo URL] [family]\n"
        "       invfs-pack info <pack>\n"
        "       invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] [--repo URL] [--release [--release-tag TAG] [--platform PLAT]] <pack[@version]|./path>\n"
        "       invfs-pack remove <pack>\n"
        "       invfs-pack verify [<pack>]\n"
        "       invfs-pack keygen [--keyring DIR] [name]\n"
        "       invfs-pack sign --key SECFILE <pack|./path>\n"
        "       invfs-pack alternatives <family>\n"
        "       invfs-pack use <family> <pack>\n"
        "       invfs-pack where [--repo URL]\n"
        "\n"
        "registry (bare names): `install <name>` (no slash) that is not\n"
        "installed locally resolves through <repo>/codecpacks/index.json,\n"
        "downloads into the tool scratch, sha256-verifies every file, and\n"
        "installs through the same signature gate below. `name@version`\n"
        "pins the version and always resolves through the registry (never\n"
        "the local roots); a version that is not in the index fails loud.\n"
<<<<<<< HEAD
        "--release fetches the pack's release tarball\n"
        "<name>-<version>-<platform>.tar.gz from the registry's GitHub\n"
        "release assets instead of the raw tree (default tag v<version>,\n"
        "default platform <uname>-linux-musl, as published by the\n"
        "registry's tools/package-release.sh; --release-tag/--platform\n"
        "override). The tarball is unpacked into the tool scratch and the\n"
        "extracted files are verified against the raw-tree sha256 listing\n"
        "plus the same gate -- a bad release refuses exactly like a bad\n"
        "raw fetch. With --repo the tarball is read under\n"
=======
        "--release fetches the pack's .ivpack bundle\n"
        "<name>-<version>.ivpack from the registry's GitHub\n"
        "release assets instead of the raw tree (default tag v1.1.0;\n"
        "--release-tag overrides). The bundle is unzipped into the tool\n"
        "scratch (needs unzip on PATH; entry names are validated before\n"
        "extract, symlinks/special files refused) and the extracted files\n"
        "are verified against the raw-tree sha256 listing plus the same\n"
        "gate -- a bad bundle refuses exactly like a bad raw fetch. A pack\n"
        "with no .ivpack asset falls back to the release tarball\n"
        "<name>-<version>-<platform>.tar.gz (default tag v<version>,\n"
        "default platform <uname>-linux-musl; --release-tag/--platform\n"
        "override) with one loud line; nothing else falls back (needs tar).\n"
        "v1.1.0 ships no signatures, so .ivpack installs take the\n"
        "missing-signature path below (prompt; -y proceeds, -n reports).\n"
        "With --repo assets are read under\n"
>>>>>>> wp/608-pack-registry-fetch
        "<repo>/releases/download/<tag>/ (hermetic fixtures). --repo takes\n"
        "a file:// dir (offline) or https:// base; $INVFS_PACK_REPO also\n"
        "works. Default: https://raw.githubusercontent.com/\n"
        "AsmanovLev/InvariantFS-registry/main. `list` (without\n"
        "--installed) shows registry packs as [available]/[installed]; an\n"
        "unreachable registry warns once and lists installed only.\n"
<<<<<<< HEAD
=======
        "Installed packs carry their install source: `list` shows [ivpack],\n"
        "[raw], or [local] next to the source (packs installed before this\n"
        "was recorded show no tag).\n"
>>>>>>> wp/608-pack-registry-fetch
        "\n"
        "signatures (v1, ed25519 over the manifest): a pack with a BAD\n"
        "signature is REFUSED, always, no knob. A pack with NO signature\n"
        "prompts `Install unsigned pack? [y/N]` unless -y (yes),\n"
        "--skip-signature-verification (proceed), or -n (dry-run: report\n"
        "only, install nothing). Keys live in $INVFS_KEYRING, else\n"
        "~/.config/invfs/keys.\n"
    );
}

int main(int argc, char **argv)
{
    /* Never die on SIGPIPE (exit 141) when a downstream filter closes
     * the pipe early. The suites pipe `list --volume 2>&1 | grep -q`,
     * and grep -q quits at the FIRST match — which is the unbuffered
     * stderr progress line, always ahead of the buffered stdout rows.
     * What used to be a usually-won race (exit before grep closes)
     * becomes a usually-lost one as soon as list does any per-row work
     * (the install-source read), so a green suite turns into a lottery.
     * Ignore the signal process-wide instead: writes then fail with
     * EPIPE and the tool still exits on its own terms. Fork+exec
     * children inherit the disposition, but none of them writes to a
     * pipe its reader abandons (list/extract pipes are always drained
     * by the parent; cp/rm/import write files, not stdout). */
    signal(SIGPIPE, SIG_IGN);
    g_argv0 = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 2;
        }
    }

    if (argc < 2) { usage(); return 2; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "where") == 0) {
        const char *repo = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--repo") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack where [--repo URL]\n");
                    return 2;
                }
                repo = argv[i];
            } else {
                fprintf(stderr, "usage: invfs-pack where [--repo URL]\n");
                return 2;
            }
        }
        cmd_where(repo);
    } else if (strcmp(cmd, "list") == 0) {
        int installed_only = 0;
        const char *family = "";
        const char *volume = NULL;
        const char *repo = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--installed") == 0 || strcmp(argv[i], "-i") == 0)
                installed_only = 1;
            else if (strcmp(argv[i], "--volume") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack list [--volume IMG] [--installed|-i] [--repo URL] [family]\n");
                    return 2;
                }
                volume = argv[i];
            }
            else if (strcmp(argv[i], "--repo") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack list [--volume IMG] [--installed|-i] [--repo URL] [family]\n");
                    return 2;
                }
                repo = argv[i];
            }
            else if (!family[0])
                family = argv[i];
            else {
                fprintf(stderr, "usage: invfs-pack list [--volume IMG] [--installed|-i] [--repo URL] [family]\n");
                return 2;
            }
        }
        if (volume)
            cmd_list_volume(volume, family);
        else
            cmd_list(family, installed_only, repo);
    } else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: invfs-pack info <pack>\n"); return 2; }
        cmd_info(argv[2]);
    } else if (strcmp(cmd, "install") == 0) {
        int overwrite = 0, dry_run = 0, skip_sig = 0, release = 0;
        const char *target = NULL;
        const char *volume = NULL;
        const char *repo = NULL;
        const char *rel_tag = NULL;
        const char *platform = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0)
                overwrite = 1;
            else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0)
                dry_run = 1;
            else if (strcmp(argv[i], "--skip-signature-verification") == 0)
                skip_sig = 1;
            else if (strcmp(argv[i], "--volume") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] [--repo URL] [--release [--release-tag TAG] [--platform PLAT]] <pack[@version]|./path>\n");
                    return 2;
                }
                volume = argv[i];
            }
            else if (strcmp(argv[i], "--repo") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] [--repo URL] [--release [--release-tag TAG] [--platform PLAT]] <pack[@version]|./path>\n");
                    return 2;
                }
                repo = argv[i];
            }
            else if (strcmp(argv[i], "--release") == 0)
                release = 1;
            else if (strcmp(argv[i], "--release-tag") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] [--repo URL] [--release [--release-tag TAG] [--platform PLAT]] <pack[@version]|./path>\n");
                    return 2;
                }
                rel_tag = argv[i];
            }
            else if (strcmp(argv[i], "--platform") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] [--repo URL] [--release [--release-tag TAG] [--platform PLAT]] <pack[@version]|./path>\n");
                    return 2;
                }
                platform = argv[i];
            }
            else if (!target)
                target = argv[i];
            else {
                fprintf(stderr, "usage: invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] [--repo URL] [--release [--release-tag TAG] [--platform PLAT]] <pack[@version]|./path>\n");
                return 2;
            }
        }
        if (!target) { fprintf(stderr, "usage: invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] [--repo URL] [--release [--release-tag TAG] [--platform PLAT]] <pack[@version]|./path>\n"); return 2; }
        return cmd_install(target, overwrite, dry_run, volume, skip_sig, repo, release, rel_tag, platform);
    } else if (strcmp(cmd, "keygen") == 0) {
        const char *keyring = NULL;
        const char *name = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--keyring") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack keygen [--keyring DIR] [name]\n");
                    return 2;
                }
                keyring = argv[i];
            }
            else if (!name)
                name = argv[i];
            else {
                fprintf(stderr, "usage: invfs-pack keygen [--keyring DIR] [name]\n");
                return 2;
            }
        }
        return cmd_keygen(keyring, name);
    } else if (strcmp(cmd, "sign") == 0) {
        const char *keyfile = NULL;
        const char *target = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--key") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack sign --key SECFILE <pack|./path>\n");
                    return 2;
                }
                keyfile = argv[i];
            }
            else if (!target)
                target = argv[i];
            else {
                fprintf(stderr, "usage: invfs-pack sign --key SECFILE <pack|./path>\n");
                return 2;
            }
        }
        return cmd_sign(keyfile, target);
    } else if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: invfs-pack remove <pack>\n"); return 2; }
        cmd_remove(argv[2]);
    } else if (strcmp(cmd, "verify") == 0) {
        return cmd_verify(argc > 2 ? argv[2] : NULL);
    } else if (strcmp(cmd, "alternatives") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: invfs-pack alternatives <family>\n"); return 2; }
        cmd_alternatives(argv[2]);
    } else if (strcmp(cmd, "use") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: invfs-pack use <family> <pack>\n"); return 2; }
        cmd_use(argv[2], argv[3]);
    } else {
        fprintf(stderr, "unknown command: %s\n", cmd);
        usage();
        return 1;
    }
    return 0;
}
