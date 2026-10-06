/* v2_open_test.c — a format-v2 volume must be REFUSED, by name.
 *
 * Meta-v3 retired the v2 name index (WP-M21) and left the idx_* entry points
 * as no-ops. vol_open refused format v1 but let v2 through, and a v2 volume
 * that opens cannot name a single file: idx_get() returns NULL for every
 * lookup, idx_dir_live() returns 0, and idx_id_live() returning 0 makes
 * vol_retire_inode treat the last live record of an id as unshared. It also
 * accepts writes nothing can read back -- an error turned into an absence.
 *
 * The v2 branches are now deleted rather than left dormant, so the gate in
 * vol_open is load-bearing: without it a v2 volume opens into a build that
 * has no reader for it at all.
 *
 * The control cell matters as much as the assertion. "refused" is trivially
 * satisfiable by an vol_open that refuses EVERYTHING, so the first case opens
 * an untouched v3 volume and requires it to open. If the gate ever grew too
 * wide, that cell fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "volume_internal.h"
#include "blkio.h"

static int fails;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fails++;
}

/* Rewrite the superblock with VOLF_META cleared (and the CRC repaired), which
 * is exactly what a pre-v0.5 volume carries: VOLF_ASTV2 set, VOLF_META clear.
 * mkfs writes VOLF_ASTV2 unconditionally (src/cli/mkfs.c:385), so clearing
 * the v3 bit is the whole edit -- no other field distinguishes the two. */
static int demote_to_v2(const char *img)
{
    blkio io;
    invfs_superblock sb;
    int rc = -1;

    if (blkio_open(&io, img, 0) != 0)
        return -1;
    if (blkio_pread(&io, 0, &sb, sizeof sb) == 0) {
        if (!(sb.vol_flags & VOLF_ASTV2)) {
            fprintf(stderr, "  image lacks VOLF_ASTV2; cannot demote\n");
            goto out;
        }
        sb.vol_flags &= ~VOLF_META;
        /* vol_flags sits OUTSIDE the CRC32C span (0..0x7B), so the checksum
         * is unchanged -- recomputed anyway so the edit is correct whether
         * or not that ever changes. */
        sb.checksum = invfs_crc32c(&sb, offsetof(invfs_superblock, checksum));
        if (blkio_pwrite(&io, 0, &sb, sizeof sb) == 0)
            rc = 0;
    }
out:
    blkio_close(&io);
    return rc;
}

/* vol_open with stderr captured to <path>. Returns 1 if it returned a volume,
 * 0 if it refused. *err gets the vol_open error code. */
static int try_open(const char *img, const char *errpath, int *err)
{
    pid_t pid;
    int status = 0, opened = 0;

    fflush(NULL);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        int fd = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        invfs_volume *v;
        if (fd >= 0) {
            dup2(fd, STDERR_FILENO);
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        v = vol_open(img, err);
        opened = v != NULL;
        if (v)
            vol_close(v);
        _exit(opened ? 10 : 0);
    }
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        exit(2);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 10)
        opened = 1;
    else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("vol_open child died abnormally (status=%d)", status);
    return opened;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = '\0';
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    const char *dir = (argc > 1) ? argv[1] : "/tmp";
    /* The suite runs from the repo root; anchor_test.c uses the same "./bin"
     * convention for the binary it drives. */
    const char *root = ".";
    char img[512], errp[512], cmd[1200];
    int err = 0;
    char *msg;

    snprintf(img, sizeof img, "%s/v2_open_test.img", dir);
    snprintf(errp, sizeof errp, "%s/v2_open_test.err", dir);

    snprintf(cmd, sizeof cmd,
             "INVFS_CODECPACK_REGISTRY=none %s/bin/invf-mkfs %s 1 >/dev/null 2>&1",
             root, img);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot create volume with invf-mkfs (%s)\n", img);
        return 1;
    }

    /* --- cell 1: CONTROL. An untouched v3 volume opens. Without this the
     * whole test could go green on a vol_open that refuses everything. --- */
    if (!try_open(img, errp, &err))
        fail("CONTROL: a fresh v3 volume was refused (err=%d) -- the gate is "
             "too wide", err);
    else
        printf("ok  CONTROL  v3 volume opens\n");

    /* --- cell 2: the assertion. Demote to v2 and require a refusal. --- */
    if (demote_to_v2(img) != 0) {
        fail("could not demote %s to format v2", img);
        return fails ? 1 : 0;
    }
    if (try_open(img, errp, &err))
        fail("format v2 volume OPENED -- the retired v2 branches are gone; "
             "opening it yields a volume no name resolves on");
    else
        printf("ok  REFUSED  format v2 volume refused\n");

    /* --- cell 3: the message has to NAME the format. A refusal that says
     * "unsupported" tells an operator nothing about what they hold. --- */
    msg = slurp(errp);
    if (!msg || !*msg) {
        fail("vol_open refused silently; the operator is told nothing");
    } else if (!strstr(msg, "v2")) {
        fail("refusal does not name the format v2:\n%s", msg);
    } else {
        printf("ok  MESSAGE  refusal names the format v2\n");
        printf("---- vol_open said ----\n%s----------------------\n", msg);
    }
    free(msg);

    if (fails) {
        fprintf(stderr, "v2_open_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("v2_open_test: PASS\n");
    return 0;
}