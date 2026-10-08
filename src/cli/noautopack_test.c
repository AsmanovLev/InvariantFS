/*
 * noautopack_test.c -- P0-1 guard (owner policy, not a bug).
 *
 * With noautopack armed (setter or INVFS_NO_AUTOPACK), no pack-supplied
 * binary is ever executed: the manifest probe reports absent, and the
 * codec exec / container cmd trampolines decline. Hostile fixtures touch
 * a sentinel file whenever they run; every guarded leg asserts the
 * sentinel stayed absent AND the call declined, and every guarded leg has
 * a guard-off control proving the fixture WOULD have executed -- so no
 * leg can pass vacuously.
 *
 * Wired into `make test` shard-1 next to codec_test (same registry).
 * INVFS_CODECPACKS_SYS=0 pins the registry shape: host-installed packs
 * must not change what the fixtures see.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codec.h"
#include "invarifs.h"

static int checks, failures;

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

static int write_file(const char *path, const char *data, int exec)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    if (fwrite(data, 1, strlen(data), f) != strlen(data)) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    if (exec && chmod(path, 0755) != 0)
        return -1;
    return 0;
}

static int sentinel_hit(const char *sent)
{
    struct stat st;
    return stat(sent, &st) == 0;
}

static const invfs_codec *find_algo(uint32_t algo)
{
    size_t n = 0, i;
    const invfs_codec *all = invfs_codec_all(&n);
    for (i = 0; i < n; i++)
        if (all[i].algo == algo)
            return &all[i];
    return NULL;
}

int main(void)
{
    char dir[256], packs[320], empty[320], bin[320];
    char cpack[384], kpack[384], path[448];
    char sent_probe[384], sent_exec[384], sent_cmd[384];
    char infile[384], outfile[384];
    const invfs_codec *pmp, *e;
    int r;

    snprintf(dir, sizeof dir, "/tmp/invfs_noautopack_%d", (int)getpid());
    snprintf(packs, sizeof packs, "%s/packs", dir);
    snprintf(empty, sizeof empty, "%s/empty", dir);
    snprintf(bin, sizeof bin, "%s/bin", dir);
    snprintf(cpack, sizeof cpack, "%s/packs/evil.codecpack", dir);
    snprintf(kpack, sizeof kpack, "%s/packs/evilc.codecpack", dir);
    snprintf(sent_probe, sizeof sent_probe, "%s/sent.probe", dir);
    snprintf(sent_exec, sizeof sent_exec, "%s/sent.exec", dir);
    snprintf(sent_cmd, sizeof sent_cmd, "%s/sent.cmd", dir);
    snprintf(infile, sizeof infile, "%s/in.bin", dir);
    snprintf(outfile, sizeof outfile, "%s/out.bin", dir);
    mkdir(dir, 0755);
    mkdir(packs, 0755);
    mkdir(empty, 0755);
    mkdir(bin, 0755);
    mkdir(cpack, 0755);
    mkdir(kpack, 0755);

    setenv("INVFS_CODECPACKS_SYS", "0", 1);

    /* Hostile self-describing probe tool: answers the manifest query AND
     * leaves a fingerprint on EVERY invocation. */
    snprintf(path, sizeof path, "%s/packMP3", bin);
    r = write_file(path,
                   "#!/bin/sh\n"
                   "echo got >> \"$INVFS_NOAUTOPACK_SENT\"\n"
                   "if [ \"$1\" = \"--invfs-manifest\" ]; then echo name=pmp; exit 0; fi\n"
                   "exit 1\n", 1);
    ok(r == 0, "fixture: hostile probe tool written");
    setenv("INVFS_NOAUTOPACK_SENT", sent_probe, 1);
    {
        char p[512];
        snprintf(p, sizeof p, "%s:/usr/bin:/bin", bin);
        setenv("PATH", p, 1);
    }
    /* No pack dir claims algo 11: the probe must reach the PATH tool. */
    setenv("INVFS_CODECPACKS", empty, 1);
    invfs_codec_probe_reset();
    pmp = invfs_codec_by_algo(INVFS_ALGO_PMP);

    /* CONTROL (guard off): the tool runs and the probe admits it. */
    unlink(sent_probe);
    invfs_set_no_autopack(0);
    unsetenv("INVFS_NO_AUTOPACK");
    ok(pmp->probe() == 1, "control: hostile tool executes, probe admits");
    ok(sentinel_hit(sent_probe), "control: sentinel proves execution happened");

    /* Guard via setter: no exec, probe reports absent. */
    unlink(sent_probe);
    invfs_set_no_autopack(1);
    invfs_codec_probe_reset();
    ok(pmp->probe() == 0, "setter guard: probe reports absent");
    ok(!sentinel_hit(sent_probe), "setter guard: hostile tool never ran");
    invfs_set_no_autopack(0);

    /* Guard via env: same, without touching the setter. */
    unlink(sent_probe);
    setenv("INVFS_NO_AUTOPACK", "1", 1);
    invfs_codec_probe_reset();
    ok(pmp->probe() == 0, "env guard: probe reports absent");
    ok(!sentinel_hit(sent_probe), "env guard: hostile tool never ran");
    unsetenv("INVFS_NO_AUTOPACK");

    /* Hostile codec pack (algo 40, free): encode fingerprints + copies. */
    snprintf(path, sizeof path, "%s/bin", cpack);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/manifest", cpack);
    r = write_file(path,
                   "name = evil\n"
                   "algo = 40\n"
                   "caps = external\n"
                   "encode = bin/enc {in} {out}\n"
                   "decode = bin/dec {in} {out}\n", 0);
    char enc[512], dec[512], enm[512];
    snprintf(enc, sizeof enc,
             "#!/bin/sh\necho got >> %s\ncp \"$1\" \"$2\"\n",
             sent_exec);
    snprintf(dec, sizeof dec,
             "#!/bin/sh\necho got >> %s\ncp \"$1\" \"$2\"\n",
             sent_exec);
    snprintf(enm, sizeof enm,
             "#!/bin/sh\necho got >> %s\necho members > \"$2\"\n",
             sent_cmd);
    snprintf(path, sizeof path, "%s/bin/enc", cpack);
    r |= write_file(path, enc, 1);
    snprintf(path, sizeof path, "%s/bin/dec", cpack);
    r |= write_file(path, dec, 1);
    ok(r == 0, "fixture: hostile codec pack written");
    setenv("INVFS_NOAUTOPACK_SENT", sent_exec, 1);
    setenv("INVFS_CODECPACKS", packs, 1);
    invfs_codec_probe_reset();
    e = find_algo(40);
    ok(e != NULL, "fixture: hostile codec pack registered");
    r = write_file(infile, "payload", 0);
    ok(r == 0, "fixture: pack io input written");

    /* CONTROL: pack exec runs and copies. */
    unlink(sent_exec);
    unlink(outfile);
    invfs_set_no_autopack(0);
    ok(e && invfs_codec_pack_exec(e, 1, infile, outfile) == 0,
       "control: hostile pack exec runs");
    ok(sentinel_hit(sent_exec), "control: sentinel proves pack exec happened");

    /* Guard: exec declines before forking. */
    unlink(sent_exec);
    unlink(outfile);
    invfs_set_no_autopack(1);
    ok(e && invfs_codec_pack_exec(e, 1, infile, outfile) != 0,
       "setter guard: hostile pack exec declines");
    ok(!sentinel_hit(sent_exec), "setter guard: hostile pack never ran");
    invfs_set_no_autopack(0);

    /* Hostile container pack (algo 41): enumerate fingerprints. */
    snprintf(path, sizeof path, "%s/bin", kpack);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/manifest", kpack);
    r = write_file(path,
                   "name = evilc\n"
                   "algo = 41\n"
                   "type = container\n"
                   "enumerate = bin/enum {in} {out}\n"
                   "extract = bin/extr {in} {out}\n"
                   "strip = bin/strip {in} {out}\n"
                   "rebuild = bin/reb {in} {out}\n", 0);
    snprintf(path, sizeof path, "%s/bin/enum", kpack);
    r |= write_file(path, enm, 1);
    snprintf(path, sizeof path, "%s/bin/extr", kpack);
    r |= write_file(path, "#!/bin/sh\nexit 0\n", 1);
    snprintf(path, sizeof path, "%s/bin/strip", kpack);
    r |= write_file(path, "#!/bin/sh\nexit 0\n", 1);
    snprintf(path, sizeof path, "%s/bin/reb", kpack);
    r |= write_file(path, "#!/bin/sh\nexit 0\n", 1);
    ok(r == 0, "fixture: hostile container pack written");
    setenv("INVFS_NOAUTOPACK_SENT", sent_cmd, 1);
    invfs_codec_probe_reset();
    e = find_algo(41);
    ok(e != NULL, "fixture: hostile container pack registered");

    /* CONTROL: container cmd runs. */
    unlink(sent_cmd);
    unlink(outfile);
    invfs_set_no_autopack(0);
    ok(e && invfs_codec_pack_cmd(e, INVFS_PACK_CMD_ENUMERATE, infile,
                                 NULL, NULL, NULL, outfile) == 0,
       "control: hostile container cmd runs");
    ok(sentinel_hit(sent_cmd), "control: sentinel proves container cmd happened");

    /* Guard: container cmd declines before forking. */
    unlink(sent_cmd);
    unlink(outfile);
    invfs_set_no_autopack(1);
    ok(e && invfs_codec_pack_cmd(e, INVFS_PACK_CMD_ENUMERATE, infile,
                                 NULL, NULL, NULL, outfile) != 0,
       "setter guard: hostile container cmd declines");
    ok(!sentinel_hit(sent_cmd), "setter guard: hostile container cmd never ran");
    invfs_set_no_autopack(0);

    invfs_codec_probe_reset();
    printf("noautopack: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
