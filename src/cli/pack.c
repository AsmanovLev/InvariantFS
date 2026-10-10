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
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

#include "blake3.h"
#include "pack_sig.h"
#include "volume.h"   /* vol_open for --volume (links CORE_O) */

#define HOST_ROOT "/.invariantfs/codecpacks"
#define LEGACY_ROOT "/.invfs/codecpacks"
#define USR_ROOT "/usr/lib/invfs/codecpacks"
#define PACK_CONF "/.invariantfs/codecpacks/packs.conf"

#define MAX_FIELDS 64
#define MAX_LINE 1024
#define MAX_PATH 1024
#define MAX_PACKS 256

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

static void cmd_where(void)
{
    printf("Pack roots (search order):\n");
    const char *env = env_codecpacks();
    if (env) printf("  $INVFS_CODECPACKS = %s\n", env);
    printf("  %s  (host, trusted)\n", HOST_ROOT);
    printf("  %s  (host, legacy)\n", LEGACY_ROOT);
    printf("  %s  (system)\n", USR_ROOT);
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
            printf("%-20s %-8s %-6s %-12s %-10s volume:%s\n",
                   pi.name[0] ? pi.name : nm,
                   pi.algo, pi.pack_version, pi.family,
                   pi.priority[0] ? pi.priority : "-", img);
            found++;
        }
    }
    free(ents);
    vol_close(v);
    if (!found) printf("  (none)\n");
    else printf("%d pack(s)\n", found);
}

static void cmd_list(const char *filter_family, int installed_only)
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
            memset(&pi, 0, sizeof pi);
            char mp[MAX_PATH + 64];
            if (pathf(mp, sizeof mp, "%s/manifest", full, NULL) != 0) continue;
            parse_manifest(mp, &pi);
            if (filter_family[0] && strcmp(pi.family, filter_family) != 0)
                continue;
            printf("%-20s %-8s %-6s %-12s %-10s %s%s\n",
                   pi.name[0] ? pi.name : de->d_name,
                   pi.algo,
                   pi.pack_version,
                   pi.family,
                   pi.priority[0] ? pi.priority : "-",
                   roots[r],
                   is_inst ? " [installed]" : "");
            found++;
        }
        closedir(d);
    }
    if (!found) printf("  (none)\n");
    else printf("%d pack(s)\n", found);
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

static int cmd_install_volume(const char *src, const char *dstname,
                               const char *volume, int overwrite,
                               int dry_run)
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
                       const char *volume, int skip_sig)
{
    char src[MAX_PATH];
    char dstname[128 + 16];
    if (resolve_pack_source(name, src, sizeof src, dstname,
                            sizeof dstname) != 0)
        return 1;
    /* WP203 install gate: bad signature refuses (always, no knob);
     * missing signature prompts (-y answers yes, -n reports and
     * installs nothing, --skip proceeds). Runs before EITHER target
     * (host root or --volume import): the trust boundary is the
     * install, not the destination. */
    {
        char keyring[MAX_PATH];
        char why[512];
        resolve_keyring(keyring, sizeof keyring);
        if (pack_sig_gate_install(src, dstname, keyring, overwrite,
                                  dry_run, skip_sig, NULL, NULL,
                                  why, sizeof why) != 0) {
            fprintf(stderr, "%s\n", why);
            return 1;
        }
    }
    if (volume && volume[0])
        return cmd_install_volume(src, dstname, volume, overwrite, dry_run);

    {
        char dst[MAX_PATH];
        struct stat st;
        int exists;
        if (snprintf(dst, sizeof dst, "%s/%s", HOST_ROOT, dstname) >= (int)sizeof dst) {
            fprintf(stderr, "pack name too long: %s\n", dstname);
            return 1;
        }
        exists = (stat(dst, &st) == 0 && S_ISDIR(st.st_mode));
        if (dry_run) {
            if (exists)
                printf("would reinstall '%s' -> %s (%s)\n", src, dst,
                       overwrite ? "overwrite" : "exists, needs -y");
            else
                printf("would install '%s' -> %s\n", src, dst);
            return 0;
        }
        if (exists && !overwrite) {
            fprintf(stderr, "pack '%s' already installed at %s (use -y to overwrite)\n",
                    dstname, dst);
            return 1;
        }
        /* create host root if needed */
        mkdir("/.invariantfs", 0755);
        mkdir(HOST_ROOT, 0755);
        if (exists) {
            /* -y overwrite: drop the old tree first (no shell; see below) */
            pid_t pid = fork();
            if (pid < 0) {
                fprintf(stderr, "failed to fork for remove %s\n", dst);
                return 1;
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
                return 1;
            }
        }
        /* recursive copy. No shell: the pack name comes from outside
         * (registry listing, CLI arg), and single-quote wrapping breaks
         * out on a \' in the path (CodeQL command-line-injection).
         * fork+execlp passes src/dst as argv, never parsed. */
        copy_pack_dir(src, dst);
    }
    return 0;
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
    }
    /* No shell (see cmd_install): dst derives from the pack name. */
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
        "usage: invfs-pack list [--volume IMG] [--installed|-i] [family]\n"
        "       invfs-pack info <pack>\n"
        "       invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] <pack|./path>\n"
        "       invfs-pack remove <pack>\n"
        "       invfs-pack verify [<pack>]\n"
        "       invfs-pack keygen [--keyring DIR] [name]\n"
        "       invfs-pack sign --key SECFILE <pack|./path>\n"
        "       invfs-pack alternatives <family>\n"
        "       invfs-pack use <family> <pack>\n"
        "       invfs-pack where\n"
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
        cmd_where();
    } else if (strcmp(cmd, "list") == 0) {
        int installed_only = 0;
        const char *family = "";
        const char *volume = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--installed") == 0 || strcmp(argv[i], "-i") == 0)
                installed_only = 1;
            else if (strcmp(argv[i], "--volume") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack list [--volume IMG] [--installed|-i] [family]\n");
                    return 2;
                }
                volume = argv[i];
            }
            else if (!family[0])
                family = argv[i];
            else {
                fprintf(stderr, "usage: invfs-pack list [--volume IMG] [--installed|-i] [family]\n");
                return 2;
            }
        }
        if (volume)
            cmd_list_volume(volume, family);
        else
            cmd_list(family, installed_only);
    } else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: invfs-pack info <pack>\n"); return 2; }
        cmd_info(argv[2]);
    } else if (strcmp(cmd, "install") == 0) {
        int overwrite = 0, dry_run = 0, skip_sig = 0;
        const char *target = NULL;
        const char *volume = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-y") == 0 || strcmp(argv[i], "--yes") == 0)
                overwrite = 1;
            else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0)
                dry_run = 1;
            else if (strcmp(argv[i], "--skip-signature-verification") == 0)
                skip_sig = 1;
            else if (strcmp(argv[i], "--volume") == 0) {
                if (++i >= argc) {
                    fprintf(stderr, "usage: invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] <pack|./path>\n");
                    return 2;
                }
                volume = argv[i];
            }
            else if (!target)
                target = argv[i];
            else {
                fprintf(stderr, "usage: invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] <pack|./path>\n");
                return 2;
            }
        }
        if (!target) { fprintf(stderr, "usage: invfs-pack install [-y|--yes] [-n|--dry-run] [--skip-signature-verification] [--volume IMG] <pack|./path>\n"); return 2; }
        return cmd_install(target, overwrite, dry_run, volume, skip_sig);
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
