/*
 * pack.c — codec-pack registry manager for InvariantFS
 *
 *   invfs-pack list [family]
 *   invfs-pack info <pack>
 *   invfs-pack install <pack>
 *   invfs-pack remove <pack>
 *   invfs-pack verify [<pack>]
 *   invfs-pack alternatives <family>
 *   invfs-pack use <family> <pack>
 *   invfs-pack where
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include "blake3.h"

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
static int parse_manifest(const char *path, pack_info *pi)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
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
        }
    }
    fclose(f);
    return 0;
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


static void cmd_list(const char *filter_family)
{
    const char *roots[] = {
        env_codecpacks(),
        HOST_ROOT,
        LEGACY_ROOT,
        USR_ROOT,
        NULL
    };
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
            pack_info pi;
            memset(&pi, 0, sizeof pi);
            char mp[MAX_PATH + 64];
            if (pathf(mp, sizeof mp, "%s/manifest", full, NULL) != 0) continue;
            parse_manifest(mp, &pi);
            if (filter_family[0] && strcmp(pi.family, filter_family) != 0)
                continue;
            printf("%-20s %-8s %-6s %-12s %-10s %s\n",
                   pi.name[0] ? pi.name : de->d_name,
                   pi.algo,
                   pi.pack_version,
                   pi.family,
                   pi.priority[0] ? pi.priority : "-",
                   roots[r]);
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

static void cmd_install(const char *name)
{
    const char *roots[] = { env_codecpacks(), HOST_ROOT, LEGACY_ROOT, USR_ROOT, NULL };
    for (int r = 0; roots[r]; r++) {
        char src[MAX_PATH];
        snprintf(src, sizeof src, "%s/%s.codecpack", roots[r], name);
        struct stat st;
        if (stat(src, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (!is_codecpack(src)) continue;
        /* already installed? */
        char dst[MAX_PATH];
        snprintf(dst, sizeof dst, "%s/%s.codecpack", HOST_ROOT, name);
        if (stat(dst, &st) == 0) {
            fprintf(stderr, "pack '%s' already installed at %s\n", name, dst);
            return;
        }
        /* create host root if needed */
        mkdir("/.invariantfs", 0755);
        mkdir(HOST_ROOT, 0755);
        /* recursive copy */
        char cmd[MAX_PATH * 2 + 32];
        snprintf(cmd, sizeof cmd, "cp -a '%s' '%s'", src, dst);
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "failed to copy pack: %s -> %s\n", src, dst);
            return;
        }
        printf("installed '%s' -> %s\n", name, dst);
        return;
    }
    fprintf(stderr, "pack '%s' not found in any root\n", name);
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
    char cmd[MAX_PATH * 2 + 32];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dst);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "failed to remove %s\n", dst);
        return;
    }
    printf("removed '%s'\n", name);
}

static void verify_pack(const char *name, const char *root)
{
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s/%s.codecpack", root, name);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) return;

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
    if (!d) return;
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
}

static void cmd_verify(const char *name)
{
    const char *roots[] = { env_codecpacks(), HOST_ROOT, LEGACY_ROOT, USR_ROOT, NULL };
    if (name && name[0]) {
        for (int r = 0; roots[r]; r++)
            verify_pack(name, roots[r]);
        return;
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
            verify_pack(n, roots[r]);
        }
        closedir(d);
    }
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
        "usage: invfs-pack list [family]\n"
        "       invfs-pack info <pack>\n"
        "       invfs-pack install <pack>\n"
        "       invfs-pack remove <pack>\n"
        "       invfs-pack verify [<pack>]\n"
        "       invfs-pack alternatives <family>\n"
        "       invfs-pack use <family> <pack>\n"
        "       invfs-pack where\n"
    );
}

int main(int argc, char **argv)
{
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
        cmd_list(argc > 2 ? argv[2] : "");
    } else if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: invfs-pack info <pack>\n"); return 2; }
        cmd_info(argv[2]);
    } else if (strcmp(cmd, "install") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: invfs-pack install <pack>\n"); return 2; }
        cmd_install(argv[2]);
    } else if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: invfs-pack remove <pack>\n"); return 2; }
        cmd_remove(argv[2]);
    } else if (strcmp(cmd, "verify") == 0) {
        cmd_verify(argc > 2 ? argv[2] : NULL);
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
