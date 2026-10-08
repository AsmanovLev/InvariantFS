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
#include "volume.h"   /* vol_open/vol_close for the volume-section legs */

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

/* ---------------- reserve/bootstrap: volume pack sections ----------------
 *
 * A volume carrying .invariantfs/codecpacks registers them into its own
 * section at open; two volumes may hold different packs under one algo
 * without colliding, and closing unloads only the closed volume's.
 * Built with real images (mkfs+import binaries, tz_reg_collapse_test
 * pattern): the scan path is the production vol_open tail.
 */
static int write_pack_tree(const char *root, const char *pname,
                           const char *algo, const char *extra)
{
    char d[512], p[576];
    int r;
    snprintf(d, sizeof d, "%s/.invariantfs/codecpacks/%s.codecpack", root,
             pname);
    snprintf(p, sizeof p, "%s/manifest", d);
    mkdir(root, 0755);
    {
        char a[512], b[512];
        snprintf(a, sizeof a, "%s/.invariantfs", root);
        snprintf(b, sizeof b, "%s/.invariantfs/codecpacks", root);
        mkdir(a, 0755);
        mkdir(b, 0755);
    }
    mkdir(d, 0755);
    {
        char man[768];
        snprintf(man, sizeof man,
                 "name = %s\nalgo = %s\ncaps = external\n%s"
                 "encode = bin/enc {in} {out}\n"
                 "decode = bin/dec {in} {out}\n",
                 pname, algo, extra ? extra : "");
        r = write_file(p, man, 0);
    }
    snprintf(p, sizeof p, "%s/bin", d);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/bin/enc", d);
    r |= write_file(p, "#!/bin/sh\ncp \"$1\" \"$2\"\n", 1);
    snprintf(p, sizeof p, "%s/bin/dec", d);
    r |= write_file(p, "#!/bin/sh\ncp \"$1\" \"$2\"\n", 1);
    return r;
}

static const invfs_codec *vol_algo(invfs_volume *v, uint32_t algo)
{
    return invfs_codec_by_algo_vol(v, algo);
}

static void test_volume_sections(const char *dir, const char *root)
{
    char imgA[384], imgB[384], srcA[384], srcB[384], cmd[1024];
    invfs_volume *vA = NULL, *vB = NULL;
    const invfs_codec *e;
    int err = 0;
    snprintf(imgA, sizeof imgA, "%s/invfs-volpack-a.img", dir);
    snprintf(imgB, sizeof imgB, "%s/invfs-volpack-b.img", dir);
    snprintf(srcA, sizeof srcA, "%s/vsrcA", dir);
    snprintf(srcB, sizeof srcB, "%s/vsrcB", dir);
    unlink(imgA);
    unlink(imgB);
    unsetenv("INVFS_CODECPACKS");
    invfs_codec_probe_reset();
    ok(write_pack_tree(srcA, "vpack", "44", NULL) == 0,
       "fixture: volume A pack tree");
    ok(write_pack_tree(srcA, "badpack", "45",
                       "os = definitely-not-an-os\n") == 0,
       "fixture: volume A foreign-os pack tree");
    ok(write_pack_tree(srcB, "vpack", "44", NULL) == 0,
       "fixture: volume B pack tree (same algo, other pack)");
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 >/dev/null 2>&1",
             root, imgA);
    ok(system(cmd) == 0, "fixture: mkfs A");
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 >/dev/null 2>&1",
             root, imgB);
    ok(system(cmd) == 0, "fixture: mkfs B");
    snprintf(cmd, sizeof cmd, "%s/bin/invf-import %s %s >/dev/null 2>&1",
             root, imgA, srcA);
    ok(system(cmd) == 0, "fixture: import A pack tree");
    snprintf(cmd, sizeof cmd, "%s/bin/invf-import %s %s >/dev/null 2>&1",
             root, imgB, srcB);
    ok(system(cmd) == 0, "fixture: import B pack tree");
    vA = vol_open(imgA, &err);
    ok(vA != NULL, "setup: open A");
    e = vol_algo(vA, 44);
    ok(e && strcmp(e->name, "vpack") == 0,
       "volume section: A's pack visible to A");
    ok(vol_algo(vA, 45) == NULL,
       "volume section: foreign-os pack refused at open");
    vB = vol_open(imgB, &err);
    ok(vB != NULL, "setup: open B");
    e = vol_algo(vB, 44);
    ok(e && strcmp(e->name, "vpack") == 0,
       "volume section: B's pack visible to B");
    e = vol_algo(vA, 44);
    ok(e && strcmp(e->name, "vpack") == 0,
       "volume section: A's view undisturbed by B (no collision)");
    vol_close(vA);
    vA = NULL;
    e = vol_algo(vB, 44);
    ok(e != NULL, "volume section: B survives A's close");
    vol_close(vB);
    vB = NULL;
    ok(invfs_codec_by_algo(44) == NULL,
       "volume section: close unloads (global clean)");
    invfs_codec_probe_reset();
    unlink(imgA);
    unlink(imgB);
}

/* ---------------- reserve 3/3: volume-pack exec materialization -----
 * A volume carrying a codecpack with runnable helpers must EXECUTE them:
 * probe answers from the staged host dir, pack_exec runs the staged
 * helper (its output marker proves the VOLUME's binary ran, not a host
 * tool -- algo 46 exists nowhere else), close unloads the section and
 * removes the staging, and noautopack declines before anything is staged.
 */
static int write_xpack_tree(const char *root)
{
    char d[512], p[576];
    int r;
    snprintf(d, sizeof d, "%s/.invariantfs/codecpacks/vxp.codecpack",
             root);
    snprintf(p, sizeof p, "%s/manifest", d);
    mkdir(root, 0755);
    {
        char a[512], b[512];
        snprintf(a, sizeof a, "%s/.invariantfs", root);
        snprintf(b, sizeof b, "%s/.invariantfs/codecpacks", root);
        mkdir(a, 0755);
        mkdir(b, 0755);
    }
    mkdir(d, 0755);
    r = write_file(p,
                   "name = vxp\nalgo = 46\ncaps = external\n"
                   "encode = bin/enc {in} {out}\n"
                   "decode = bin/dec {in} {out}\n", 0);
    snprintf(p, sizeof p, "%s/bin", d);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/bin/enc", d);
    /* the marker is the proof: only this volume's helper appends it */
    r |= write_file(p, "#!/bin/sh\ncat \"$1\" > \"$2\"\n"
                       "echo VOLPACK >> \"$2\"\n", 1);
    snprintf(p, sizeof p, "%s/bin/dec", d);
    r |= write_file(p, "#!/bin/sh\ncp \"$1\" \"$2\"\n", 1);
    return r;
}

static int file_has_suffix(const char *path, const char *suffix)
{
    FILE *f = fopen(path, "rb");
    long n;
    size_t sl = strlen(suffix);
    char *buf;
    int hit = 0;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0) {
        fclose(f);
        return 0;
    }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    buf[n] = 0;
    if ((size_t)n >= sl && strcmp(buf + n - sl, suffix) == 0) hit = 1;
    free(buf);
    return hit;
}

static void test_volume_exec(const char *dir, const char *root)
{
    char img[384], src[384], cmd[1024];
    char infile[384], outfile[384];
    invfs_volume *v = NULL;
    const invfs_codec *e;
    const char *hd;
    char staged[4096];
    struct stat st;
    int err = 0;
    snprintf(img, sizeof img, "%s/invfs-volpack-x.img", dir);
    snprintf(src, sizeof src, "%s/vsrcX", dir);
    snprintf(infile, sizeof infile, "%s/x-in.bin", dir);
    snprintf(outfile, sizeof outfile, "%s/x-out.bin", dir);
    unlink(img);
    unsetenv("INVFS_CODECPACKS");
    invfs_set_no_autopack(0);
    invfs_codec_probe_reset();
    ok(write_xpack_tree(src) == 0, "fixture: volume exec-pack tree");
    ok(write_file(infile, "exec-probe", 0) == 0,
       "fixture: exec input written");
    snprintf(cmd, sizeof cmd, "%s/bin/invf-mkfs %s 32 >/dev/null 2>&1",
             root, img);
    ok(system(cmd) == 0, "fixture: mkfs X");
    snprintf(cmd, sizeof cmd, "%s/bin/invf-import %s %s >/dev/null 2>&1",
             root, img, src);
    ok(system(cmd) == 0, "fixture: import X pack tree");
    v = vol_open(img, &err);
    ok(v != NULL, "setup: open X");
    e = vol_algo(v, 46);
    ok(e && strcmp(e->name, "vxp") == 0,
       "exec: volume pack visible to its volume");
    /* probe materializes: avail means the staged helper resolved */
    ok(e && e->probe && e->probe() == 1,
       "exec: volume pack probes available (staged)");
    hd = e ? invfs_codec_pack_host_dir(e) : NULL;
    ok(hd != NULL, "exec: host dir resolved for volume pack");
    if (hd) {
        snprintf(staged, sizeof staged, "%s", hd);
        ok(stat(staged, &st) == 0 && S_ISDIR(st.st_mode),
           "exec: staged dir exists on host");
    } else {
        staged[0] = 0;
    }
    /* the marker proves the VOLUME's helper ran, not a host tool */
    unlink(outfile);
    ok(e && invfs_codec_pack_exec(e, 1, infile, outfile) == 0,
       "exec: volume pack encode runs");
    ok(file_has_suffix(outfile, "exec-probeVOLPACK\n"),
       "exec: output carries the volume helper's marker");
    /* guard: decline before anything is staged */
    vol_close(v);
    v = NULL;
    invfs_set_no_autopack(1);
    invfs_codec_probe_reset();
    v = vol_open(img, &err);
    ok(v != NULL, "setup: reopen X under guard");
    e = vol_algo(v, 46);
    ok(e && strcmp(e->name, "vxp") == 0,
       "guard: registration unaffected (manifests still read)");
    ok(e && (!e->probe || e->probe() == 0),
       "guard: volume pack probes absent");
    ok(e && invfs_codec_pack_exec(e, 1, infile, outfile) != 0,
       "guard: volume pack exec declines");
    ok(e && invfs_codec_pack_host_dir(e) == NULL,
       "guard: nothing staged under noautopack");
    invfs_set_no_autopack(0);
    /* close unloads the section and removes the staging */
    vol_close(v);
    v = NULL;
    ok(invfs_codec_by_algo(46) == NULL,
       "exec: close unloads the volume section");
    ok(!staged[0] || stat(staged, &st) != 0,
       "exec: close removed the host staging");
    invfs_codec_probe_reset();
    unlink(img);
}

int main(int argc, char **argv)
{
    const char *root = getenv("PWD") ? getenv("PWD") : ".";
    const char *tdir = (argc > 1) ? argv[1] : "/tmp";
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
    test_volume_sections(tdir, root);
    test_volume_exec(tdir, root);
    printf("noautopack: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
