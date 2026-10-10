/*
 * invf-sweep.c — offline sweep driver for Linux
 *
 *   invf-sweep <image> [--dry-run] [--log <file>]
 *                      [--seal [5|10|20|25]|--unseal]
 *                      [--heal [group ...]]
 *                      [--redundant-blocks <f>]
 *                      [--redundant-paranoic <f>[:rs-vm|rs-cauchy]]
 *                      [--free-redundant]
 *                      [--redundant-bench]
 *
 * Walks live records (same CRC-validated scan as invf-ls), feeds every
 * regular file with segments to vol_sweep_one() — the unified per-inode
 * dispatch (containers/transcodes/text-batching/generic ZSTD-19). Then the
 * per-segment dedupe pass (vol_sweep_dedupe, WP12(h)) merges identical
 * stored segments. Text candidates defer into the volume's accumulator and
 * are sealed into shared PPMd batches by vol_tz_flush() at the end of the
 * run, after the dead-batch GC (vol_tz_gc). The author's sweep.c CLI is
 * Windows-only.
 *
 * WP201 (native v3 seal, par2-inspired, no compat): --seal [pct] seals
 * AFTER the sweep is fully flushed (full recompute, footer committed last,
 * verify-after-write before the run may claim success; see vol_seal).
 * pct picks the fixed (k,m) menu — 5->(20,1), 10->(9,1, default),
 * 20->(8,2), 25->(6,2) — over 64 KiB symbols; --unseal retires the seal
 * files, without sweeping. With --dry-run, --seal prints the seal plan
 * (groups, parity bytes, overhead) and changes nothing.
 *
 * WP402 (explicit heal only, never automatic): --heal [group ...]
 * rebuilds drifted seal groups from parity -- reconstruct, verify the
 * reconstruction against the manifest hashes, write back, re-verify.
 * No group args heals every group detection names (the same recompute
 * the scrub uses; printed as `seal-heal-needed <group> <reason>`);
 * explicit group ids heal exactly those. No sweep walk runs with --heal;
 * with --dry-run it prints the plan and changes nothing.
 *
 * The --redundant-* flags are the v2 spelling of the same knob and are
 * mapped onto the fixed menu: --redundant-blocks <f> snaps 1/f to the
 * nearest menu k (20/9/8/6); --redundant-paranoic <f>[:algo] snaps f*100
 * to the nearest menu pct (5/10/20/25) and honours the :rs-vm|:rs-cauchy
 * suffix for the group code (default rs-vm); --free-redundant removes the
 * seal (same as --unseal). A bare run (no redundancy flags) on a sealed
 * volume auto-reseals after the sweep. --redundant-bench prints rs-vm vs
 * rs-cauchy MB/s and exits (it benchmarks the shared rs.c math).
 *
 * Every non-dry run captures an SPT0 SAVEPOINT in "prepare", before the
 * walk, and the window it opens is what invf-rollback undoes the sweep from.
 * There is no retention registry and no second checkpoint kind: the savepoint
 * pins the blocks the captured generation's recipes named, and the NEXT
 * capture discharges it (AGENTS.md 2.5). Checkpointing is declined (the sweep
 * runs without one) on read-only/recovering volumes and with
 * INVFS_CHECKPOINT=0.
 *
 * WP22e: --fast narrows the per-file decision to "generic or nothing"
 * (RAW files take the per-segment profile recompress; classification,
 * container decomposition, codec transcodes, batching, dedupe and the
 * promotion pass never run; the walk, the checkpoint and the reports are
 * the usual ones).
 *
 * WP22e: the run ends with online inode-area compaction when the dead
 * share of the area (superseded versions + tombstones) exceeds ~30% of the
 * used bytes ("inode area compacted: X -> Y bytes"). Never while a
 * savepoint is live (rollback truncates to its recorded positions)
 * or on a read-only volume; INVFS_NO_COMPACT=1 disables the automatic
 * pass. --compact forces the pass alone (no walk, no checkpoint).
 *
 * WP23: --extract-packs <dir> is the sweepboot helper mode (see
 * tools/sweepboot-init.sh): with NO FUSE MOUNT and no sweep, the volume
 * is opened through the engine alone and the codecpack directory stored
 * ON the volume ("/.invfs/codecpacks" when present, else
 * "/usr/lib/invfs/codecpacks") is materialized into <dir> (a tmpfs
 * scratch in the initramfs), which is then printed on stdout as the
 * mode's single payload line. The pack files are plain files -- on a
 * swept volume they are PPMd/ZSTD batch members, decoded in-process by
 * the ordinary read path -- so a maintenance boot can run the sweep with
 * INVFS_CODECPACKS=<dir> and the volume is SELF-HOSTING: it carries the
 * very tools its own sweep needs. Read-only volumes open fine (the mode
 * writes nothing to the volume; read heat accrues exactly like any
 * mount's reads).
 */
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "invarifs.h"
#include "volume.h"
#include "volume_internal.h"   /* WP146: vol_name_is_container_sibling */
#include "vol_spt0.h"
#include "vol_walk.h"   /* WP135: a walk's status is not optional */
#include "vol_reclaim.h"
#include "codec.h"
#include "rs.h"
#include "vol_seal.h"   /* WP201: fixed seal menu + dry-run plan */

typedef struct sw_bucket { struct sw_bucket *next; int slot; } sw_bucket;

/* WP14b: container-part deferrals ("name!partN") are aggregated per
 * container and printed as one summary line at the end of the walk --
 * a Silesia mozilla/samba/xml run would otherwise log 1573 near-identical
 * per-part lines. */
typedef struct {
    char prefix[256];   /* container name including the '!' */
    int  n_text;        /* parts deferred to PPMd batches */
    int  n_bin;         /* parts deferred to ZSTD batches */
} part_agg;

static part_agg *g_parts;
static size_t   g_parts_n, g_parts_cap;

/* WP201: --dry-run seal plan accumulator (read-only namespace walk). */
typedef struct {
    uint64_t n;       /* coverable entries (REG + LNK content carriers) */
    uint64_t bytes;   /* REG bytes (LNK targets are small; plan rounds up) */
} seal_plan_ctx;

static int seal_plan_cb(void *ctx_, const char *path, uint64_t ino,
                        uint32_t type, uint64_t size, int64_t mtime)
{
    seal_plan_ctx *c = ctx_;
    (void)ino; (void)mtime;
    if (!path || !path[0] || (unsigned char)path[0] == 0x01)
        return 0;
    if (type != INVFS_ITYP_REG && type != INVFS_ITYP_LNK)
        return 0;
    c->n++;
    if (type == INVFS_ITYP_REG)
        c->bytes += size;
    else
        c->bytes += 4096;   /* symlink target: bounded over-estimate */
    return 0;
}

static void sw_progress_suspend(void);   /* static UI helper, defined below */
/* invfs_sweep_ui_active is global (src/core/volume.h). */

/* Map a seal request onto the fixed menu. do_bench!=0 allows the rs_bench
 * fallback for a suffix-less --redundant-paranoic (live path only; the
 * dry-run plan takes VM and says so). 0 ok, -1 bench failure (the caller
 * owns the volume close + exit). */
static int seal_request_menu(invfs_volume *vol, int seal, int seal_pct,
                             double rb_f, double rp_f, int rp_algo,
                             int do_bench, unsigned *mk, unsigned *mm,
                             int *algo)
{
    unsigned k = 9, m = 1;
    int a = RS_ALGO_VM;
    if (seal) {
        seal_menu(seal_pct, &k, &m);
    } else if (rb_f >= 0) {
        /* 1/f snaps to the nearest menu k (20/9/8/6). */
        long lk = (long)(1.0 / rb_f + 0.5);
        unsigned cands[] = { 20, 9, 8, 6 };
        unsigned best = 9, i;
        long bd = labs(lk - 9);
        for (i = 0; i < 4; i++) {
            long d = labs(lk - (long)cands[i]);
            if (d < bd) { bd = d; best = cands[i]; }
        }
        seal_menu(best == 20 ? 5 : best == 9 ? 10 : best == 8 ? 20 : 25,
                  &k, &m);
        if (!invfs_sweep_ui_active())
            fprintf(stderr, "redundant-blocks: %g snaps to the fixed "
                    "menu (k=%u,m=%u)\n", rb_f, k, m);
    } else if (rp_f >= 0) {
        /* f*100 snaps to the nearest menu pct (5/10/20/25). */
        double p = rp_f * 100.0;
        int pcts[] = { 5, 10, 20, 25 };
        int best = 10, i;
        double bd = p >= 10 ? p - 10 : 10 - p;
        for (i = 0; i < 4; i++) {
            double d = p >= pcts[i] ? p - pcts[i] : pcts[i] - p;
            if (d < bd) { bd = d; best = pcts[i]; }
        }
        seal_menu(best, &k, &m);
        a = rp_algo;
        if (!a) {
            /* no explicit suffix: keep the live algo; on the first
             * paranoic configure the bench picks the winner */
            uint32_t ok1, om;
            int oa;
            vol_redun_state(vol, &ok1, &oa, &om);
            if (oa) {
                a = oa;
            } else if (!do_bench) {
                a = RS_ALGO_VM;   /* dry-run: no bench, say the default */
            } else {
                double vm, ca;
                if (rs_bench(32, 4, INVFS_BLOCK_SIZE, 512, &vm, &ca) != 0) {
                    sw_progress_suspend();
                    fprintf(stderr, "redundant-paranoic: internal "
                                    "bench failed\n");
                    return -1;
                }
                a = vm >= ca ? RS_ALGO_VM : RS_ALGO_CAUCHY;
                if (!invfs_sweep_ui_active())
                    fprintf(stderr, "redundant-paranoic: bench picked %s "
                            "(rs-vm %.1f vs rs-cauchy %.1f MB/s)\n",
                            rs_algo_name(a), vm, ca);
            }
        }
        if (!invfs_sweep_ui_active())
            fprintf(stderr, "redundant-paranoic: %g snaps to the fixed "
                    "menu (k=%u,m=%u,%s)\n", rp_f, k, m,
                    rs_algo_name(a));
    }
    if (mk) *mk = k;
    if (mm) *mm = m;
    if (algo) *algo = a;
    return 0;
}

typedef struct {
    unsigned current;
    unsigned total;
    const char *name;
    uint64_t start_ms;
    uint64_t last_ms;
    uint64_t done;
    uint64_t goal;
    double fraction;
    uint64_t interval_ms;
    int active;
} sweep_ui_state;

static sweep_ui_state g_ui;
static int g_progress_tty;
static int g_progress_line_active;
static int g_color_mode = -1;

/* ---- live decomposition tree (TTY only) ---------------------------------
 *
 * The progress line says "3/7 transform 50%" and nothing about WHAT it is
 * chewing on. But every name the per-file progress callback sees already IS
 * the decomposition chain: a nested container is addressed by its ancestry,
 * "img.qcow2!mbr0001-diskimg!mbr0001-p0001". So the tree needs no new core
 * plumbing -- split the name on '!' and you have the path.
 *
 * Drawn only when the UI owns a TTY. When the output is redirected to a log
 * g_progress_tty is 0, the panel never renders and the log stays exactly as
 * before, so scripted runs and benchmarks are unaffected. */
#define TREE_MAX_DEPTH 10
/* a tree path is "!".join of up to TREE_MAX_DEPTH 288-byte names */
#define TREE_PATH_MAX (TREE_MAX_DEPTH * 288 + 8)

static char            g_tree[TREE_MAX_DEPTH][288];
static size_t          g_tree_depth;
static uint64_t        g_tree_done, g_tree_total;
static uint64_t        g_tree_leaves;
static size_t          g_tree_rows;    /* rows the last paint occupied */
static invfs_volume   *g_tree_vol;
/* WP147: sw_tree_set consults the dashboard's opt-in flag, and the flag is
 * declared with the dashboard block below. A tentative definition here (C11
 * 6.9.2p2: a file-scope declaration with no initializer and no storage-class
 * specifier is a tentative definition, and two of them in one TU are the same
 * object) -- not a second variable that could ever drift from the real one. */
static int             g_dash_on;

/* ---- split a name into the chain THIS VOLUME can prove ------------------
 *
 * WP147. This used to be `while (*p) { cut at the next '!' }` -- a question
 * about a BYTE. A user file `notes!final.txt` was therefore drawn as container
 * `notes` with member `final.txt`, and the panel under it said `depth 2` for a
 * file with no container anywhere on the volume: the same false statement
 * WP146 removed from the report lines, drawn as a tree instead.
 *
 * The core owns the real question (vol_name_is_container_sibling,
 * src/core/volume_internal.h:1045) -- a minted suffix shape AND a live
 * container inode -- and this tool CAN ask it: `v` is the volume the collect
 * stage walked (vol_walk_strict, :1637) and `name` is a path off that walk.
 *
 * WHY A CHAIN SPLITTER AND NOT ONE PREDICATE CALL. The tree is a TREE, so the
 * decision is per LEVEL, not per file: a real member must still hang under its
 * container. So the predicate is asked on the path-so-far at every '!', and the
 * first level it does not vouch for ends the chain -- the remainder is carried
 * UNSPLIT as one row, under its own name, with its own size. A fix that made
 * every '!' name flat would break the nesting a real decomposition needs; this
 * one stops claiming a nesting nobody can prove, and nothing else.
 *
 * Two consequences worth stating:
 *
 *   - `box.tar!part0`: at the first '!' the path-so-far is the whole name, the
 *     predicate says yes (`part0` is minted, `box.tar` is live), so the chain
 *     nests exactly as before.
 *   - `nest.splt!mbr0000-chunk0!mbrmap` (a real nested decomposition --
 *     tools/test-containerpack.sh:399 lists that name on a volume): level 0
 *     passes, level 1 cannot, because the core predicate deliberately answers
 *     "not a sibling" for a name carrying two '!' (src/core/vol_records.c:296:
 *     `a!b!c` splits as `a` + `b!c`, and `b!c` is not a minted shape). So the
 *     tail is shown whole rather than broken into a nesting nothing here can
 *     vouch for. That is the same answer the sweep LANES give that name, not a
 *     new one -- the tree and the lanes cannot disagree about a chain.
 *
 * Returns the number of segments written; seg[0..n-1] joined by '!' is the
 * original name again. */
#define TREE_SEG_MAX 288
static size_t sw_chain_split(invfs_volume *v, const char *name,
                             char seg[][TREE_SEG_MAX], size_t max)
{
    const char *start = name;
    size_t n = 0;

    if (!name || !name[0] || !seg || !max) return 0;
    /* n + 1 < max: the unsplit remainder always needs a row of its own. */
    while (n + 1 < max) {
        const char *bang = strchr(start, '!');
        const char *segend;
        char path[TREE_PATH_MAX];
        size_t plen, slen;

        if (!bang) break;
        /* The predicate judges THIS LEVEL, not its container: the path-so-far
         * runs from the start of the name to the end of the segment `bang`
         * closes -- `box.tar!part3`, NOT `box.tar`. Asking about the prefix
         * would ask whether the CONTAINER is a sibling of something, which it
         * is not (a container is a prefix, never a member), and every real
         * decomposition would flatten to one row. */
        segend = strchr(bang + 1, '!');
        if (!segend) segend = name + strlen(name);
        plen = (size_t)(segend - name);
        if (plen >= sizeof path) break;         /* unsplittable: carry the rest */
        memcpy(path, name, plen);
        path[plen] = '\0';
        /* Not ours -> everything from here on is ONE name, the user's. */
        if (!vol_name_is_container_sibling(v, path)) break;
        slen = (size_t)(bang - start);
        if (slen >= TREE_SEG_MAX) slen = TREE_SEG_MAX - 1;
        memcpy(seg[n], start, slen);
        seg[n][slen] = '\0';
        n++;
        start = bang + 1;
    }
    {
        size_t rlen = strlen(start);
        int truncated = rlen >= TREE_SEG_MAX;
        if (truncated) rlen = TREE_SEG_MAX - 1;
        memcpy(seg[n], start, rlen);
        seg[n][rlen] = '\0';
        if (truncated) snprintf(seg[n], TREE_SEG_MAX, "...");
        n++;
    }
    return n;
}

/* remember the chain we were handed; a shorter name is a sibling, so the
 * path is simply rebuilt from scratch every time */
static void sw_tree_set(invfs_volume *v, const char *name,
                        uint64_t done, uint64_t total)
{
    g_tree_vol = v;
    g_tree_done = done;
    g_tree_total = total;
    if (!name || !name[0]) { g_tree_depth = 0; return; }
    /* Nothing consumes the chain unless the panel owns the terminal or
     * --dash is on, and the predicate costs a name lookup on the rare name
     * that has both a '!' and a minted suffix. A redirected run -- the shape
     * every benchmark and every CI log has -- must not start paying for a
     * picture nobody sees, and before WP147 this was pure string work. */
    if (!g_progress_tty && !g_dash_on) { g_tree_depth = 0; return; }
    g_tree_depth = sw_chain_split(v, name, g_tree, TREE_MAX_DEPTH);
}

/* ---- live dashboard (opt-in: invf-sweep --dash <file.html>) -------------
 *
 * A browser view of what the sweep is doing RIGHT NOW. The design choice that
 * makes it scale is "focus on the active object": the volume will hold
 * millions of leaves, but only one decomposition chain is live at a time, so
 * the dashboard renders that chain plus a short ring of recently finished
 * containers -- never the whole population.
 *
 * The page is a single self-contained HTML file that the sweep rewrites about
 * once a second. A browser pointed at it re-reads it on the meta-refresh, so
 * there is no server, no port and no CORS: nothing leaves the machine and the
 * volume lock is never shared (the sweep owns the volume; the browser only
 * ever sees this file).
 */
#define DASH_RECENT 12

/* defined further down, next to the rest of the UI */
static uint64_t sw_now_ms(void);
static void sw_tree_size(const char *name, char *out, size_t cap);

static char     g_dash_path[512];
static const char *g_volpath;
static int      g_dash_on;
static uint64_t g_dash_last_ms;
static int      g_dash_leaves;

static struct {
    char     name[256];
    uint64_t size;
    int      members;
    int      depth;
} g_recent[DASH_RECENT];
static int g_recent_n;

/* Record a container that just finished decomposing, with the members it
 * produced. Sizes come from the name index via vol_list_dir, so this costs
 * one directory listing and no per-node stat. */
static void sw_dash_note_container(invfs_volume *v, const char *name)
{
    invfs_dirent *ents;
    char pfx[300];
    int n, i, members = 0, slot;
    uint64_t total = 0;
    size_t pl;

    if (!g_dash_on || !v || !name) return;
    pl = (size_t)snprintf(pfx, sizeof pfx, "%s!mbr", name);
    if (pl >= sizeof pfx) return;
    ents = (invfs_dirent *)malloc(sizeof *ents * 4096);
    if (!ents) return;
    n = vol_list_dir(v, "/", ents, 4096);
    for (i = 0; i < n; i++) {
        if (strncmp(ents[i].name, pfx, pl)) continue;
        members++;
        total += ents[i].size;
    }
    free(ents);
    if (!members) return;

    slot = g_recent_n < DASH_RECENT ? g_recent_n++ : DASH_RECENT - 1;
    if (g_recent_n == DASH_RECENT)
        memmove(&g_recent[0], &g_recent[1], sizeof g_recent[0] * (DASH_RECENT - 1));
    snprintf(g_recent[slot].name, sizeof g_recent[slot].name, "%s", name);
    g_recent[slot].size = total;
    g_recent[slot].members = members;
    {
        /* WP147: this used to be "how many '!' does the name contain?", which
         * is not the question. Depth is how many ENCLOSING containers this
         * volume can vouch for, and the answer is the chain sw_tree_set draws
         * -- the same function, so the dashboard's indentation and the TTY
         * panel cannot drift apart the way two string tests did (WP136).
         *
         * The reachable case that mattered: a name a USER typed, holding a '!'
         * (`photos.img!backup`), beside a real `photos.img!backup!mbr0001-x`
         * left by a lane that decomposed it. Counting '!' said depth 1 and the
         * dashboard indented the file as a member of a container `photos.img`
         * that has never existed. sw_chain_split says one segment, depth 0:
         * the name the user typed, at the level the user put it. A genuine
         * member still nests -- `box.tar!part0` is two segments, depth 1 --
         * because that is a chain this tree minted. */
        char chain[TREE_MAX_DEPTH][TREE_SEG_MAX];
        size_t n = sw_chain_split(v, name, chain, TREE_MAX_DEPTH);
        g_recent[slot].depth = n ? (int)(n - 1) : 0;
    }
}

/* ---- HTML emission ---------------------------------------------------- */

static void dash_json_escape(FILE *f, const char *s)
{
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc((int)c, f); }
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc((int)c, f);
    }
}

static void sw_dash_write(invfs_volume *v, const char *volpath)
{
    FILE *f;
    uint64_t now = sw_now_ms();
    int i;

    if (!g_dash_on) return;
    if (g_dash_last_ms && now - g_dash_last_ms < 1000) return;
    g_dash_last_ms = now;
    f = fopen(g_dash_path, "w");
    if (!f) return;

    fprintf(f, "<!doctype html><meta charset=utf-8>");
    fprintf(f, "<meta http-equiv=refresh content=1>");
    fprintf(f, "<title>InvariantFS sweep</title><style>");
    fprintf(f,
      ":root{--bg:#0d1117;--fg:#c9d1d9;--dim:#6e7681;--ac:#58a6ff;"
      "--ok:#3fb950;--warn:#d29922;--raw:#8b949e;--txt:#79c0ff;--bin:#d2a8ff}");
    fprintf(f,
      "body{background:var(--bg);color:var(--fg);font:13px/1.55 ui-monospace,"
      "SFMono-Regular,Menlo,monospace;margin:0;padding:18px 22px}");
    fprintf(f,
      "h1{font-size:15px;font-weight:600;margin:0 0 2px;letter-spacing:.4px}");
    fprintf(f,
      ".sub{color:var(--dim);font-size:11.5px;margin-bottom:16px}");
    fprintf(f,
      ".bar{height:5px;background:#21262d;border-radius:3px;overflow:hidden;"
      "margin:5px 0 16px}");
    fprintf(f, "%s",
      ".bar>i{display:block;height:100%;background:var(--ac);"
      "transition:width .4s}");
    fprintf(f,
      ".act{border-left:2px solid var(--ac);padding:7px 12px;margin:0 0 16px;"
      "background:#161b22;border-radius:0 5px 5px 0}");
    fprintf(f,
      ".n{color:var(--ac)}.d{color:var(--dim)}.s{color:var(--ok)}");
    fprintf(f,
      ".row{display:flex;justify-content:space-between;gap:14px}");
    fprintf(f,
      ".lbl{color:var(--dim);font-size:11px;text-transform:uppercase;"
      "letter-spacing:.9px;margin:14px 0 5px}");
    fprintf(f,
      ".e{display:flex;align-items:center;gap:9px;padding:1.5px 0}");
    fprintf(f,
      ".nm{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}");
    fprintf(f,
      ".g{display:inline-block;width:7px;height:7px;border-radius:2px;"
      "flex:0 0 auto}");
    fprintf(f,
      "hr{border:0;border-top:1px solid #21262d;margin:16px 0}");
    fprintf(f,"</style><body>");

    fprintf(f, "<h1>InvariantFS &mdash; live sweep</h1><div class=sub>");
    dash_json_escape(f, volpath ? volpath : "");
    fprintf(f, "</div>");

    /* stage bar */
    {
        double pct = g_ui.goal ? (double)g_ui.done / (double)g_ui.goal : 0.0;
        if (pct > 1.0) pct = 1.0;
        fprintf(f,
            "<div class=row><span><b>[%u/%u]</b> %s</span>"
            "<span class=d>%llu/%llu &middot; %llus</span></div>",
            g_ui.current, g_ui.total, g_ui.name ? g_ui.name : "",
            (unsigned long long)g_ui.done, (unsigned long long)g_ui.goal,
            (unsigned long long)((now - g_ui.start_ms) / 1000));
        fprintf(f, "<div class=bar><i style=width:%.1f%%></i></div>",
                pct * 100.0);
    }

    /* the active chain: the whole point -- what is being decomposed, and how
     * deep the nesting currently goes */
    if (g_tree_depth) {
        char path[TREE_MAX_DEPTH][TREE_PATH_MAX];
        size_t k;
        fprintf(f, "<div class=act>");
        for (k = 0; k < g_tree_depth; k++) {
            char sz[24] = "";
            if (k == 0) snprintf(path[k], sizeof path[k], "%s", g_tree[k]);
            else snprintf(path[k], sizeof path[k], "%s!%s", path[k - 1], g_tree[k]);
            sw_tree_size(path[k], sz, sizeof sz);
            fprintf(f,
                "<div class=row><span class=nm style=\"padding-left:%zupx\">"
                "%s%s</span><span class=d>%s</span></div>",
                k * 14, k ? k + 1 == g_tree_depth ? "`&nbsp; " : "|&nbsp; " : "",
                g_tree[k], sz);
        }
        fprintf(f,
            "<div class=sub style=margin:6px 0 0\">depth <b>%zu</b>"
            " &middot; segments %llu/%llu &middot; leaves %d</div>",
            g_tree_depth, (unsigned long long)g_tree_done,
            (unsigned long long)g_tree_total, g_dash_leaves);
        fprintf(f, "</div>");
    }

    /* recently finished containers */
    if (g_recent_n) {
        fprintf(f, "<div class=lbl>decomposed &mdash; deepest first</div>");
        for (i = g_recent_n - 1; i >= 0; i--) {
            double mib = (double)g_recent[i].size / 1048576.0;
            fprintf(f,
                "<div class=e><span class=g style=background:var(--txt)>"
                "</span><span class=nm style=\"padding-left:%dpx\">%s"
                "</span><span class=d>%d members &middot; %.1f MiB</span></div>",
                g_recent[i].depth * 10, g_recent[i].name,
                g_recent[i].members, mib);
        }
    }
    fprintf(f, "<hr><div class=sub>auto-refresh 1s &middot; "
               "focus: active object &middot; no data leaves this machine</div>");
    fclose(f);
}

static void sw_tree_size(const char *name, char *out, size_t cap)
{
    uint64_t sz = 0;
    out[0] = '\0';
    if (!g_tree_vol) return;
    if (vol_stat_full(g_tree_vol, name, NULL, &sz, NULL) != 0 || !sz) return;
    if (sz >= 1073741824ull)
        snprintf(out, cap, "%.2fG", (double)sz / 1073741824.0);
    else if (sz >= 1048576ull)
        snprintf(out, cap, "%.1fM", (double)sz / 1048576.0);
    else if (sz >= 1024ull)
        snprintf(out, cap, "%.1fK", (double)sz / 1024.0);
    else
        snprintf(out, cap, "%lluB", (unsigned long long)sz);
}

/* Erase the stage line AND the block under it, leaving the cursor on the
 * stage line's row. The cursor sits one row below the block after a paint. */
static void sw_tree_erase(void)
{
    int i;
    if (!g_progress_tty) { g_tree_rows = 0; return; }
    for (i = 0; i <= (int)g_tree_rows + 1; i++) {
        fputs("\r\033[2K", stderr);
        if (i <= (int)g_tree_rows) fputs("\033[1A", stderr);
    }
    g_tree_rows = 0;
}

/* Paint the block under the already-printed stage line. */
static void sw_tree_render(void)
{
    char path[TREE_MAX_DEPTH][TREE_PATH_MAX];
    size_t i;

    if (!g_progress_tty || !g_tree_depth) return;
    for (i = 0; i < g_tree_depth; i++) {
        if (i == 0)
            snprintf(path[i], sizeof path[i], "%s", g_tree[i]);
        else
            snprintf(path[i], sizeof path[i], "%s!%s", path[i - 1], g_tree[i]);
    }
    for (i = 0; i < g_tree_depth; i++) {
        char sz[24] = "";
        sw_tree_size(path[i], sz, sizeof sz);
        fprintf(stderr, "  %s%-46s %8s%s\n",
                i ? "  " : "", path[i], sz, "");
    }
    fprintf(stderr, "  %-*s %8s   depth %zu, segments %llu/%llu, leaves %llu\n",
            46, "", "", g_tree_depth,
            (unsigned long long)g_tree_done,
            (unsigned long long)g_tree_total,
            (unsigned long long)g_tree_leaves);
    g_tree_rows = g_tree_depth + 1;
}

static int sw_color_enabled(void)
{
    if (g_color_mode >= 0) return g_color_mode;
    return g_progress_tty && !getenv("NO_COLOR");
}

static uint64_t sw_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sw_duration(uint64_t ms, char *buf, size_t cap)
{
    unsigned long long sec = (unsigned long long)(ms / 1000u);
    if (sec < 60)
        /* under a minute the remainder is MILLISECONDS -- printing it with an
         * "s" suffix read as "elapsed 941s" and made fast stages look slow */
        snprintf(buf, cap, "%llums", (unsigned long long)(ms % 1000u));
    else if (sec < 3600)
        snprintf(buf, cap, "%llum%02llus", sec / 60u, sec % 60u);
    else
        snprintf(buf, cap, "%lluh%02llum", sec / 3600u,
                 (sec % 3600u) / 60u);
}

static void sw_progress_interval(void)
{
    const char *s;
    char *end = NULL;
    unsigned long n;

    if (g_ui.interval_ms) return;
    g_ui.interval_ms = 250u;
#ifndef _WIN32
    if (!g_progress_tty) g_ui.interval_ms = 5000u;
#endif
    s = getenv("INVFS_SWEEP_PROGRESS_MS");
    if (!s || !*s) return;
    n = strtoul(s, &end, 10);
    if (end != s && *end == '\0' && n >= 100 && n <= 60000)
        g_ui.interval_ms = n;
}

static void sw_stage_total(unsigned total)
{
    memset(&g_ui, 0, sizeof g_ui);
    g_ui.total = total;
    sw_progress_interval();
}

static void sw_stage_line(uint64_t now, const char *detail, int force,
                          int final)
{
    uint64_t elapsed, eta_ms = 0;
    char elapsed_s[32], eta_s[32], count_s[64], line[768];
    const char *eta = "--";
    double pct_value = 100.0;
    char pct_buf[24];
    const char *pct = "     -";
    const char *c_reset = "", *c_bracket = "", *c_name = "";
    const char *c_pct = "", *c_eta = "", *c_elapsed = "", *c_dim = "";
    int color, n;

    if (!g_ui.active) return;
    elapsed = now >= g_ui.start_ms ? now - g_ui.start_ms : 0;
    if (!force && now < g_ui.last_ms + g_ui.interval_ms) return;
    g_ui.last_ms = now;
    sw_duration(elapsed, elapsed_s, sizeof elapsed_s);
    if (g_ui.goal) {
        if (g_ui.done > g_ui.goal) g_ui.done = g_ui.goal;
        if (g_ui.fraction > g_ui.goal) g_ui.fraction = (double)g_ui.goal;
        pct_value = g_ui.fraction * 100.0 / (double)g_ui.goal;
        snprintf(pct_buf, sizeof pct_buf, "%5.1f%%", pct_value);
        pct = pct_buf;
        if (g_ui.fraction > 0.0 && g_ui.fraction < g_ui.goal) {
            eta_ms = (uint64_t)((double)elapsed *
                                ((double)(g_ui.goal - g_ui.fraction) /
                                 g_ui.fraction));
            sw_duration(eta_ms, eta_s, sizeof eta_s);
            eta = eta_s;
        } else if (g_ui.fraction >= g_ui.goal) {
            eta = "0s";
        }
    }
    if (g_ui.goal)
        snprintf(count_s, sizeof count_s, "%llu/%llu",
                 (unsigned long long)g_ui.done,
                 (unsigned long long)g_ui.goal);
    else
        snprintf(count_s, sizeof count_s, "%llu",
                 (unsigned long long)g_ui.done);
    color = sw_color_enabled();
    if (color) {
        c_reset = "\033[0m";
        c_bracket = "\033[36m";
        c_name = "\033[1;36m";
        c_pct = pct_value >= 75.0 ? "\033[32m" :
                pct_value >= 25.0 ? "\033[36m" : "\033[33m";
        c_eta = "\033[35m";
        c_elapsed = "\033[1;37m";
        c_dim = "\033[2m";
    }
    n = snprintf(line, sizeof line, "%s[%u/%u]%s %s%-10s%s %s%s%s  %s%s%s  ETA %s%s%s  %selapsed %s%s",
                 c_bracket, g_ui.current, g_ui.total, c_reset,
                 c_name, g_ui.name, c_reset,
                 c_pct, pct, c_reset, c_dim, count_s, c_reset,
                 c_eta, eta, c_reset,
                 c_elapsed, elapsed_s, c_reset);
    if (n < 0 || (size_t)n >= sizeof line) return;
    (void)final;
    if (detail && *detail) {
        size_t used = (size_t)n;
        n = snprintf(line + used, sizeof line - used, "  %s%s%s",
                     c_dim, detail, c_reset);
        if (n < 0 || (size_t)n >= sizeof line - used) return;
    }
#ifndef _WIN32
    if (g_dash_on) sw_dash_write(g_tree_vol, g_volpath);
    if (g_progress_tty) {
        sw_tree_erase();
        if (g_progress_line_active) fputs("\r\033[2K", stderr);
        fputs(line, stderr);
        sw_tree_render();
        g_progress_line_active = 1;
        return;
    }
#endif
    fputs(line, stderr);
    fputc('\n', stderr);
}

static void sw_stage_begin(unsigned current, const char *name,
                           uint64_t goal, const char *detail)
{
    uint64_t now = sw_now_ms();
    CD_SET(name);   /* CORRUPT_DEBUG: attribute allocs/frees to this stage */
    g_ui.current = current;
    g_ui.name = name;
    g_ui.start_ms = now;
    g_ui.last_ms = now;
    g_ui.done = 0;
    g_ui.goal = goal;
    g_ui.active = 1;
    sw_stage_line(now, detail, 1, 0);
}

static void sw_stage_update(uint64_t done, uint64_t goal, const char *detail)
{
    g_ui.done = done;
    g_ui.fraction = (double)done;
    if (goal) g_ui.goal = goal;
    sw_stage_line(sw_now_ms(), detail, 0, 0);
}

static void sw_stage_fraction(double fraction, uint64_t done, uint64_t goal,
                               const char *detail)
{
    g_ui.fraction = fraction;
    g_ui.done = done;
    if (goal) g_ui.goal = goal;
    sw_stage_line(sw_now_ms(), detail, 0, 0);
}

static void sw_progress_suspend(void)
{
#ifndef _WIN32
    if (g_progress_tty && g_progress_line_active) {
        sw_tree_erase();
        if (g_progress_line_active) fputs("\r\033[2K", stderr);
        fputc('\n', stderr);
        g_progress_line_active = 0;
    }
#endif
}

static void sw_progress_finish(void)
{
#ifndef _WIN32
    if (g_progress_tty && g_progress_line_active) {
        fputc('\n', stderr);
        g_progress_line_active = 0;
    }
#endif
}

static void sw_stage_end(const char *detail)
{
    uint64_t now = sw_now_ms();
    if (g_ui.goal && g_ui.done < g_ui.goal) g_ui.done = g_ui.goal;
    g_ui.fraction = (double)g_ui.goal;
    sw_stage_line(now, detail, 1, 1);
    g_ui.active = 0;
}

#ifndef _WIN32
static FILE *g_log_file;
static int g_log_out_pipe[2] = { -1, -1 };
static int g_log_err_pipe[2] = { -1, -1 };
static int g_log_saved_stdout = -1;
static int g_log_saved_stderr = -1;
static pthread_t g_log_thread;
static int g_log_thread_started;
static volatile int g_log_stopping;

static int sw_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void *sw_log_pump(void *user)
{
    struct pollfd pfd[2];
    (void)user;
    pfd[0].fd = g_log_out_pipe[0];
    pfd[1].fd = g_log_err_pipe[0];
    pfd[0].events = pfd[1].events = POLLIN;
    for (;;) {
        uint8_t buf[8192];
        int i, ready;
        pfd[0].revents = pfd[1].revents = 0;
        ready = poll(pfd, 2, 100);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (i = 0; i < 2; i++) {
            if (pfd[i].fd >= 0 && ready > 0 &&
                (pfd[i].revents & (POLLIN|POLLHUP))) {
                ssize_t n = read(pfd[i].fd, buf, sizeof buf);
                if (n > 0) {
                    int console = i ? g_log_saved_stderr : g_log_saved_stdout;
                    (void)sw_write_all(console, buf, (size_t)n);
                    (void)fwrite(buf, 1, (size_t)n, g_log_file);
                    fflush(g_log_file);
                } else if (n == 0) {
                    pfd[i].fd = -1;
                }
            }
        }
        if (pfd[0].fd < 0 && pfd[1].fd < 0) break;
        if (ready == 0 && g_log_stopping) break;
    }
    return NULL;
}

static void sw_log_cleanup(void)
{
    if (g_log_saved_stdout >= 0)
        (void)dup2(g_log_saved_stdout, STDOUT_FILENO);
    if (g_log_saved_stderr >= 0)
        (void)dup2(g_log_saved_stderr, STDERR_FILENO);
    if (g_log_out_pipe[0] >= 0) close(g_log_out_pipe[0]);
    if (g_log_out_pipe[1] >= 0) close(g_log_out_pipe[1]);
    if (g_log_err_pipe[0] >= 0) close(g_log_err_pipe[0]);
    if (g_log_err_pipe[1] >= 0) close(g_log_err_pipe[1]);
    if (g_log_saved_stdout >= 0) close(g_log_saved_stdout);
    if (g_log_saved_stderr >= 0) close(g_log_saved_stderr);
    g_log_out_pipe[0] = g_log_out_pipe[1] = -1;
    g_log_err_pipe[0] = g_log_err_pipe[1] = -1;
    g_log_saved_stdout = g_log_saved_stderr = -1;
    if (g_log_file) fclose(g_log_file);
    g_log_file = NULL;
}

static void sw_log_stop(void)
{
    if (!g_log_thread_started) {
        sw_log_cleanup();
        return;
    }
    fflush(stdout);
    fflush(stderr);
    if (g_log_saved_stdout >= 0)
        (void)dup2(g_log_saved_stdout, STDOUT_FILENO);
    if (g_log_saved_stderr >= 0)
        (void)dup2(g_log_saved_stderr, STDERR_FILENO);
    g_log_stopping = 1;
    (void)pthread_join(g_log_thread, NULL);
    g_log_thread_started = 0;
    sw_log_cleanup();
}

static int sw_log_start(const char *path, const char *image)
{
    time_t now = time(NULL);
    struct tm tm;
    char stamp[64];

    if (!path || !*path) return 0;
    g_log_file = fopen(path, "a");
    if (!g_log_file) {
        fprintf(stderr, "cannot open sweep log %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S%z", &tm);
    fprintf(g_log_file, "==== invf-sweep %s pid=%ld image=%s ====\n",
            stamp, (long)getpid(), image ? image : "-");
    fflush(g_log_file);
    if (pipe(g_log_out_pipe) != 0 || pipe(g_log_err_pipe) != 0) {
        fprintf(stderr, "cannot create sweep log pipes: %s\n", strerror(errno));
        sw_log_cleanup();
        return -1;
    }
    g_log_saved_stdout = dup(STDOUT_FILENO);
    g_log_saved_stderr = dup(STDERR_FILENO);
    if (g_log_saved_stdout < 0 || g_log_saved_stderr < 0 ||
        dup2(g_log_out_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(g_log_err_pipe[1], STDERR_FILENO) < 0) {
        fprintf(stderr, "cannot redirect sweep output: %s\n", strerror(errno));
        g_log_stopping = 0;
        sw_log_cleanup();
        return -1;
    }
    close(g_log_out_pipe[1]);
    close(g_log_err_pipe[1]);
    g_log_out_pipe[1] = g_log_err_pipe[1] = -1;
    if (pthread_create(&g_log_thread, NULL, sw_log_pump, NULL) != 0) {
        fprintf(stderr, "cannot start sweep log pump: %s\n", strerror(errno));
        g_log_stopping = 0;
        sw_log_cleanup();
        return -1;
    }
    g_log_thread_started = 1;
    (void)atexit(sw_log_stop);
    return 0;
}
#endif

static void sw_file_progress(void *user, const char *name,
                             uint64_t done, uint64_t total)
{
    char detail[256];
    const char *base = name ? strrchr(name, '/') : NULL;
    uint64_t completed = g_ui.done;
    double fraction = (double)completed;

    sw_tree_set((invfs_volume *)user, name, done, total);
    if (total) fraction += (double)done / (double)total;
    if (base) base++;
    else base = name ? name : "";
    snprintf(detail, sizeof detail, "segments %llu/%llu  %s",
             (unsigned long long)done, (unsigned long long)total, base);
    sw_stage_fraction(fraction, completed, g_ui.goal, detail);
}

static void sw_dedupe_progress(void *user, const invfs_dedupe_progress *p)
{
    char detail[192];
    const char *phase;

    (void)user;
    if (!p || !p->phase) return;
    phase = p->phase;
    if (strcmp(phase, "hash") == 0 ||
        strcmp(phase, "hash_done") == 0) {
        snprintf(detail, sizeof detail, "hashing %llu segments",
                 (unsigned long long)p->done);
        sw_stage_update(p->done, p->total, detail);
        if (!invfs_sweep_ui_active() && strcmp(phase, "hash_done") == 0)
            sw_progress_suspend();
    } else if (strcmp(phase, "sort") == 0) {
        snprintf(detail, sizeof detail, "hashed=%llu sorted",
                 (unsigned long long)p->done);
        sw_stage_update(p->done, p->total, detail);
    } else if (strcmp(phase, "scan") == 0) {
        snprintf(detail, sizeof detail, "candidates=%llu",
                 (unsigned long long)p->candidates);
        sw_stage_update(p->done, p->total, detail);
    } else if (strcmp(phase, "merge") == 0) {
        double mib = (double)p->freed * (double)INVFS_BLOCK_SIZE /
                     (1024.0 * 1024.0);
        snprintf(detail, sizeof detail,
                 "merged=%llu cross=%llu intra=%llu freed=%.1f MiB",
                 (unsigned long long)p->merged,
                 (unsigned long long)p->cross_merged,
                 (unsigned long long)p->intra_merged,
                 mib);
        sw_stage_update(p->done, p->total, detail);
        if (!invfs_sweep_ui_active() && p->total && p->done == p->total)
            sw_progress_suspend();
    }
}

static void part_agg_add(const char *name, int binary)
{
    const char *bang = strchr(name, '!');
    size_t plen = bang ? (size_t)(bang - name) + 1 : 0;
    size_t i;

    if (!plen || plen >= 256) return;
    for (i = 0; i < g_parts_n; i++)
        if (strncmp(g_parts[i].prefix, name, plen) == 0 &&
            g_parts[i].prefix[plen] == 0)
            break;
    if (i == g_parts_n) {
        if (g_parts_n == g_parts_cap) {
            size_t nc = g_parts_cap ? g_parts_cap * 2 : 16;
            part_agg *na = realloc(g_parts, nc * sizeof *na);
            if (!na) return;
            g_parts = na;
            g_parts_cap = nc;
        }
        memset(&g_parts[i], 0, sizeof g_parts[i]);
        memcpy(g_parts[i].prefix, name, plen);
        g_parts_n++;
    }
    if (binary) g_parts[i].n_bin++;
    else        g_parts[i].n_text++;
}

static void part_agg_print(void)
{
    size_t i;
    if (invfs_sweep_ui_active()) {
        free(g_parts);
        g_parts = NULL;
        g_parts_n = g_parts_cap = 0;
        return;
    }
    for (i = 0; i < g_parts_n; i++) {
        if (g_parts[i].n_bin)
            printf("  %s*: %d parts -> ZSTD batch\n", g_parts[i].prefix,
                   g_parts[i].n_bin);
        if (g_parts[i].n_text)
            printf("  %s*: %d parts -> PPMd batch\n", g_parts[i].prefix,
                   g_parts[i].n_text);
    }
    free(g_parts);
    g_parts = NULL;
    g_parts_n = g_parts_cap = 0;
}

static uint64_t sw_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* ---- WP23: --extract-packs (sweepboot self-hosting) -------------------
 * The sweep's codecpacks are expected to live ON the rootfs volume (that
 * is what "self-hosting" means), but the maintenance boot needs them
 * BEFORE the volume is mounted -- and a FUSE mount is exactly what the
 * sweep must be exclusive against. So the packs are read out through the
 * engine alone: plain files, decoded in-process by the ordinary read
 * path (a swept volume keeps them as PPMd/ZSTD batch members; nothing
 * here depends on their stored shape).
 */

/* mkdir -p for the extraction target; 0 = exists/created */
static int xp_mkdirs(const char *path)
{
    char tmp[1024];
    size_t n = strlen(path), i;
    if (n == 0 || n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (tmp[i] != '/' && tmp[i] != '\0') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    return 0;
}

/* one volume file -> <dstdir>/<rel>; parents created; mode = the
 * recorded meta when present, 0755 otherwise (pack helpers under bin/
 * are exec'd by name, so an executable default is the safe one) */
static int xp_file(invfs_volume *v, const char *vname, const char *rel,
                   const char *dstdir)
{
    char dst[1024];
    uint8_t *buf = NULL;
    size_t len = 0;
    uint64_t id;
    FILE *f;
    invfs_meta_pub m;
    long mode = 0755;
    int rc = -1;

    if (snprintf(dst, sizeof dst, "%s/%s", dstdir, rel) >= (int)sizeof dst)
        return -1;
    id = vol_find(v, vname);
    if (!id) return -1;
    if (vol_read_file(v, id, &buf, &len) != 0) return -1;
    {
        char *sl = strrchr(dst, '/');
        if (sl) {
            *sl = '\0';
            if (xp_mkdirs(dst) != 0) { free(buf); return -1; }
            *sl = '/';
        }
    }
    f = fopen(dst, "wb");
    if (!f) { free(buf); return -1; }
    if (len && fwrite(buf, 1, len, f) != len) { fclose(f); free(buf); return -1; }
    if (fclose(f) != 0) { free(buf); return -1; }
    if (vol_get_meta(v, id, &m) == 0 && (m.mode & 0777))
        mode = m.mode & 0777;
    if (chmod(dst, (mode_t)mode) != 0) { free(buf); return -1; }
    free(buf);
    rc = 0;
    return rc;
}

static int xp_walk(invfs_volume *v, const char *vdir, const char *rel,
                   const char *dstdir, unsigned depth,
                   unsigned long *files_out)
{
    invfs_dirent *ents = NULL;
    int cap = 256, n, i, rc = 0;

    for (;;) {   /* grow the listing window until the dir fits */
        invfs_dirent *ne = realloc(ents, (size_t)cap * sizeof *ents);
        if (!ne) { free(ents); return -1; }
        ents = ne;
        n = vol_list_dir(v, vdir, ents, cap);
        if (n < 0) { free(ents); return -1; }
        if (n < cap) break;
        cap *= 2;
    }
    for (i = 0; i < n && rc == 0; i++) {
        char vchild[512], rchild[512];
        if (snprintf(vchild, sizeof vchild, "%s/%s", vdir, ents[i].name) >=
                (int)sizeof vchild ||
            snprintf(rchild, sizeof rchild, "%s%s%s", rel, rel[0] ? "/" : "",
                     ents[i].name) >= (int)sizeof rchild) {
            rc = -1; break;
        }
        if (ents[i].is_dir) {
            if (depth < 16)
                rc = xp_walk(v, vchild, rchild, dstdir, depth + 1, files_out);
            else
                rc = -1;
        } else {
            if (xp_file(v, vchild, rchild, dstdir) != 0) {
                fprintf(stderr, "extract-packs: cannot materialize %s\n",
                        vchild);
                rc = -1;
            } else {
                (*files_out)++;
            }
        }
    }
    free(ents);
    return rc;
}

/* The mode body: open volume already held. Returns 0 (dir printed on
 * stdout even when the volume carries no packs -- an empty dir is the
 * honest "nothing to self-host with") or 1. */
static int extract_packs(invfs_volume *vol, const char *dir)
{
    static const char *const roots[] = {
        ".invfs/codecpacks",          /* /.invfs/codecpacks */
        "usr/lib/invfs/codecpacks",   /* the system location */
    };
    const char *root = NULL;
    unsigned long files = 0;
    size_t i;

    for (i = 0; i < sizeof roots / sizeof roots[0]; i++)
        if (vol_is_dir(vol, roots[i])) { root = roots[i]; break; }
    if (xp_mkdirs(dir) != 0) {
        fprintf(stderr, "extract-packs: cannot create %s\n", dir);
        return 1;
    }
    if (root && xp_walk(vol, root, "", dir, 0, &files) != 0) {
        fprintf(stderr, "extract-packs: walk of /%s failed\n", root);
        return 1;
    }
    if (root)
        fprintf(stderr, "extract-packs: /%s -> %s (%lu files)\n",
                root, dir, files);
    else
        fprintf(stderr, "extract-packs: volume carries no codecpack dir\n");
    printf("%s\n", dir);   /* the payload line sweepboot-init.sh reads */
    return 0;
}

static int sw_find(sw_bucket **tab, size_t mask, char (*names)[256],
                   const char *name)
{
    const sw_bucket *b;
    if (!tab) return -1;
    for (b = tab[sw_hash(name) & mask]; b; b = b->next)
        if (strcmp(names[b->slot], name) == 0) return b->slot;
    return -1;
}

static void sw_insert(sw_bucket ***tabp, size_t *maskp, size_t *countp,
                      char (*names)[256], int slot)
{
    sw_bucket *b;
    size_t h;
    if (!*tabp) {
        *tabp = (sw_bucket **)calloc(1024, sizeof **tabp);
        *maskp = 1023;
    } else if (*countp > *maskp) {
        size_t ncap = (*maskp + 1) * 2, i;
        sw_bucket **nt = (sw_bucket **)calloc(ncap, sizeof *nt);
        if (nt) {
            for (i = 0; i <= *maskp; i++) {
                sw_bucket *e = (*tabp)[i];
                while (e) {
                    sw_bucket *nx = e->next;
                    size_t nb = sw_hash(names[e->slot]) & (ncap - 1);
                    e->next = nt[nb]; nt[nb] = e;
                    e = nx;
                }
            }
            free(*tabp);
            *tabp = nt;
            *maskp = ncap - 1;
        }
    }
    b = (sw_bucket *)malloc(sizeof *b);
    if (!b) return;
    b->slot = slot;
    h = sw_hash(names[slot]) & *maskp;
    b->next = (*tabp)[h];
    (*tabp)[h] = b;
    (*countp)++;
}


/* WP-M21b: v3 live-set collector, fed by vol_walk in a single O(n)
 * hierarchical pass. A v3 volume has no record stream to walk, so the
 * WP42 walker above finds nothing and the sweep silently no-ops; this
 * feeds the same arrays instead. Sizes come directly from the inode row;
 * record positions stay 0 (the fold replaces linear compaction). */
typedef struct {
    invfs_volume *vol;
    char (**names)[256];
    uint64_t **inodes, **sizes, **poss;
    sw_bucket ***tab;
    size_t *tmask, *tcount;
    int *count, *cap;
    int oom;
} collect_ctx;

static int sweep_walk_cb(void *ctx_, const char *path, uint64_t inode_id,
                            uint32_t type, uint64_t size, int64_t mtime)
{
    collect_ctx *c = (collect_ctx *)ctx_;
    char (*names)[256] = *c->names;
    uint64_t *inodes = *c->inodes;
    uint64_t *sizes = *c->sizes;
    uint64_t *poss = *c->poss;
    int i;
    (void)mtime;

    if (type == INVFS_ITYP_DIR)
        return 0;                       /* directories don't have file content */
    if (!path || !path[0] || (unsigned char)path[0] == 0x01)
        return 0;
    if (strlen(path) > 255)
        return 0;                       /* collector slots hold 256 B */

    i = sw_find(*c->tab, *c->tmask, names, path);
    if (i >= 0) {
        inodes[i] = inode_id; sizes[i] = size; poss[i] = 0;
        return 0;
    }
    if (*c->count == *c->cap) {
        int ncap = *c->cap ? *c->cap * 2 : 512;
        char (*nn)[256] =
            (char (*)[256])realloc(names, (size_t)ncap * 256);
        uint64_t *ni =
            (uint64_t *)realloc(inodes, (size_t)ncap * sizeof *ni);
        uint64_t *ns =
            (uint64_t *)realloc(sizes, (size_t)ncap * sizeof *ns);
        uint64_t *np =
            (uint64_t *)realloc(poss, (size_t)ncap * sizeof *np);
        if (nn) names = nn;
        if (ni) inodes = ni;
        if (ns) sizes = ns;
        if (np) poss = np;
        *c->names = names; *c->inodes = inodes;
        *c->sizes = sizes; *c->poss = poss;
        *c->cap = ncap;
        if (!nn || !ni || !ns || !np) { c->oom = 1; return 1; }
    }
    strncpy(names[*c->count], path, 256);
    names[*c->count][255] = 0;
    inodes[*c->count] = inode_id;
    sizes[*c->count] = size;
    poss[*c->count] = 0;
    sw_insert(c->tab, c->tmask, c->tcount, names, *c->count);
    (*c->count)++;
    return 0;
}

/* WP64: graceful Ctrl+C — finish the current file, then exit cleanly. */
static volatile sig_atomic_t g_stop = 0;

#ifndef _WIN32
static void on_sigint(int sig)
{
    if (g_stop) {          /* second Ctrl+C: restore default, die now */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_stop = 1;
    fprintf(stderr, "\n^C  stopping after the current file"
                    " (Ctrl+C again aborts now, losing it)\n");
}
#endif

int main(int argc, char **argv)
{
    invfs_volume *vol;
    int err, dry = 0, seal = 0, unseal = 0, bench = 0, realize = 0;
    int seal_pct = 10;               /* WP201: --seal [5|10|20|25] */
    int heal = 0;                    /* WP402: --heal [group ...] */
    uint32_t *heal_gids = NULL;
    size_t heal_n = 0, heal_cap = 0;
    int no_realize = 0, stopped = 0;
    int fast = 0;
    const char *extract_dir = NULL;    /* WP23 --extract-packs mode */
    const char *log_path = NULL;
    const char *color_arg = NULL;
    double rb_f = -1.0, rp_f = -1.0;   /* <0: flag absent */
    int rp_algo = 0;                   /* explicit :rs-vm/:rs-cauchy suffix */
    int auto_reseal = 0;
    int count = 0, cap = 0, kept = 0, swept = 0, skipped = 0, failed = 0;
    /* A vol_flush that failed at the durability point. LATCHED, not a
     * counter: a run whose data was rewritten but whose flush failed has
     * left the volume in a state the caller must be told about, so a later
     * flush that happens to succeed must NOT clear the fact that one did
     * not. The sweep still runs to the end -- the operator wants the seal
     * and the reclaim -- but it does not get to call that a success. */
    int flush_failed = 0;
    unsigned ui_heat = 0, ui_tier = 0, ui_dedupe = 0;
    unsigned ui_batches = 0, ui_finalize = 0, ui_seal = 0;
    invfs_dedupe_stats ui_dedupe_stats;
    char (*names)[256] = NULL;
    uint64_t *inodes = NULL;
    uint64_t *sizes = NULL;
    uint64_t *poss = NULL;   /* each name's current record position
                                (v2 position-kill matching, WP22c) */
    const char *img;
    sw_bucket **tab = NULL;
    size_t tmask = 0, tcount = 0;
    int i;

    for (int j = 1; j < argc; j++) {
        if (strcmp(argv[j], "-h") == 0 || strcmp(argv[j], "--help") == 0) {
            fprintf(stderr,
                "usage: %s <image> [--dry-run] [--fast] [--compact]\n"
                "           [--seal [5|10|20|25]|--unseal]\n"
                "           [--heal [group ...]]\n"
                "           [--redundant-blocks <f>]\n"
                "           [--redundant-paranoic <f>[:rs-vm|rs-cauchy]]\n"
                "           [--free-redundant] [--redundant-bench]\n"
                "           [--realize]  (accept the last sweep: free its\n"
                "                         the savepoint that opened this window)\n"
                "           [--no-realize] (keep the previous checkpoint live;\n"
                "                         blocks stay held until the next sweep)\n"
                "           [--log <file>] (append combined stdout/stderr)\n"
                "           [--dash <file.html>] (write a live browser view of\n"
                "                         the active decomposition to <file.html>;\n"
                "                         the sweep rewrites it ~1x/sec, open it\n"
                "                         in a browser -- no server, no network)\n"
                "           [--color auto|always|never] (default: auto; NO_COLOR honored)\n"
                "  --fast      cheap pass: RAW files take the generic\n"
                "              per-segment recompress only (no classification,\n"
                "              transcodes, decomposition, batching or dedupe)\n"
                "  --compact   run only the inode-area compaction pass\n"
                "           [--extract-packs <dir>]  (WP23 sweepboot: copy the\n"
                "                         volume's codepack dir to <dir>,\n"
                "                         engine-side, no sweep, no FUSE)\n",
                argv[0]);
            return 2;
        }
        if (strcmp(argv[j], "-v") == 0 || strcmp(argv[j], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
    }

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <image> [--dry-run] [--fast] [--compact]\n"
                "           [--seal [5|10|20|25]|--unseal]\n"
                "           [--heal [group ...]]\n"
                "           [--redundant-blocks <f>]\n"
                "           [--redundant-paranoic <f>[:rs-vm|rs-cauchy]]\n"
                "           [--free-redundant] [--redundant-bench]\n"
                "           [--realize]  (accept the last sweep: free its\n"
                "                         the savepoint that opened this window)\n"
                "           [--no-realize] (keep the previous checkpoint live;\n"
                "                         blocks stay held until the next sweep)\n"
                "           [--log <file>] (append combined stdout/stderr)\n"
                "           [--color auto|always|never] (default: auto; NO_COLOR honored)\n"
                "  --fast      cheap pass: RAW files take the generic\n"
                "              per-segment recompress only (no classification,\n"
                "              transcodes, decomposition, batching or dedupe)\n"
                "  --compact   run only the inode-area compaction pass\n"
                "           [--extract-packs <dir>]  (WP23 sweepboot: copy the\n"
                "                         volume's codepack dir to <dir>,\n"
                "                         engine-side, no sweep, no FUSE)\n",
                argv[0]);
        return 2;
    }
    img = argv[1];
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--dash") == 0 && i + 1 < argc) {
            snprintf(g_dash_path, sizeof g_dash_path, "%s", argv[++i]);
            g_dash_on = 1;
        } else if (strcmp(a, "--dry-run") == 0) {
            dry = 1;
        } else if (strcmp(a, "--realize") == 0) {
            realize = 1;
        } else if (strcmp(a, "--no-realize") == 0) {
            no_realize = 1;
        } else if (strcmp(a, "--fast") == 0) {
            fast = 1;
        } else if (strcmp(a, "--compact") == 0) {
            /* retired in WP-M21; accepted silently as a no-op */
        } else if (strcmp(a, "--log") == 0 && i + 1 < argc) {
            log_path = argv[++i];
        } else if (strcmp(a, "--color") == 0 && i + 1 < argc) {
            color_arg = argv[++i];
        } else if (strcmp(a, "--extract-packs") == 0 && i + 1 < argc) {
            extract_dir = argv[++i];
        } else if (strcmp(a, "--seal") == 0) {
            /* WP201: optional percent (default 10); a following bare
             * 5|10|20|25 is consumed, anything else stays an operand. */
            seal = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' &&
                argv[i + 1][0] <= '9') {
                /* A leading-digit operand is a percent (or an error).
                 * Non-numeric operands (image paths never start with a
                 * digit... unless they do — then spell --seal last) are
                 * rejected here rather than silently sealed over. */
                char *endp = NULL;
                long p = strtol(argv[i + 1], &endp, 10);
                if (!endp || *endp != '\0' ||
                    (p != 5 && p != 10 && p != 20 && p != 25)) {
                    fprintf(stderr, "--seal: want 5|10|20|25, got '%s'\n",
                            argv[i + 1]);
                    return 2;
                }
                seal_pct = (int)p;
                i++;
            }
        } else if (strcmp(a, "--unseal") == 0 ||
                   strcmp(a, "--free-redundant") == 0) {
            unseal = 1;
        } else if (strcmp(a, "--heal") == 0) {
            /* WP402: explicit group ids are the following all-digit
             * operands (none = heal every group detection names).
             * Non-numeric operands are NOT consumed here: an unknown
             * flag after --heal must still be rejected below, never
             * silently healed over. */
            heal = 1;
            while (i + 1 < argc) {
                const char *q = argv[i + 1];
                size_t qi = 0;
                unsigned long g;
                char *endp = NULL;
                if (!*q) break;
                while (q[qi] && q[qi] >= '0' && q[qi] <= '9') qi++;
                if (q[qi] != '\0') break;
                g = strtoul(argv[i + 1], &endp, 10);
                if (endp == argv[i + 1] || *endp != '\0' ||
                    g > 16777216ul) {
                    fprintf(stderr, "--heal: bad group id '%s'\n",
                            argv[i + 1]);
                    free(heal_gids);
                    return 2;
                }
                if (heal_n == heal_cap) {
                    size_t nc = heal_cap ? heal_cap * 2 : 8;
                    uint32_t *ng = realloc(heal_gids,
                                           nc * sizeof *ng);
                    if (!ng) {
                        fprintf(stderr, "--heal: out of memory\n");
                        free(heal_gids);
                        return 1;
                    }
                    heal_gids = ng;
                    heal_cap = nc;
                }
                heal_gids[heal_n++] = (uint32_t)g;
                i++;
            }
        } else if (strcmp(a, "--redundant-bench") == 0) {
            bench = 1;
        } else if (strcmp(a, "--redundant-blocks") == 0 && i + 1 < argc) {
            char *endp = NULL;
            rb_f = strtod(argv[++i], &endp);
            if (endp == argv[i] || *endp != '\0' || !(rb_f > 0.0)) {
                fprintf(stderr, "--redundant-blocks: bad fraction '%s'\n",
                        argv[i]);
                return 2;
            }
        } else if (strcmp(a, "--redundant-paranoic") == 0 && i + 1 < argc) {
            char *endp = NULL;
            const char *colon;
            rp_f = strtod(argv[++i], &endp);
            if (endp == argv[i] || !(rp_f > 0.0 && rp_f < 1.0) ||
                (*endp != '\0' && *endp != ':')) {
                fprintf(stderr, "--redundant-paranoic: bad fraction '%s'\n",
                        argv[i]);
                return 2;
            }
            colon = strchr(argv[i], ':');
            if (colon) {
                if (strcmp(colon + 1, "rs-vm") == 0)
                    rp_algo = RS_ALGO_VM;
                else if (strcmp(colon + 1, "rs-cauchy") == 0)
                    rp_algo = RS_ALGO_CAUCHY;
                else {
                    fprintf(stderr, "--redundant-paranoic: unknown algo "
                                    "'%s'\n", colon + 1);
                    return 2;
                }
            }
        } else {
            fprintf(stderr, "unknown flag '%s'\n", a);
            return 2;
        }
    }
    /* WP201: --dry-run with a seal request is the seal plan (read-only
     * estimate, no mutation); dry with unseal/bench/realize still
     * conflicts. WP402: --heal runs no sweep walk at all, so it stands
     * alone apart from --dry-run (plan mode), --log and --color. */
    if (heal &&
        (seal || unseal || bench || rb_f >= 0 || rp_f >= 0 || realize ||
         no_realize || fast || extract_dir)) {
        fprintf(stderr, "--heal is a standalone mode (only --dry-run may "
                "join it)\n");
        free(heal_gids);
        return 2;
    }
    if (unseal + bench > 0 &&
        (seal || rb_f >= 0 || rp_f >= 0 || realize || extract_dir)) {
        fprintf(stderr, "conflicting flags\n");
        return 2;
    }
    if (dry && (realize || extract_dir)) {
        fprintf(stderr, "conflicting flags\n");
        return 2;
    }
    if (extract_dir &&
        (dry || unseal || bench || seal || rb_f >= 0 || rp_f >= 0 || realize)) {
        fprintf(stderr, "--extract-packs is a standalone mode\n");
        return 2;
    }
    if (seal && (rb_f >= 0 || rp_f >= 0)) {
        fprintf(stderr, "--seal conflicts with --redundant-*\n");
        return 2;
    }
    /* WP-M21: --compact retired; the fold (vol_fold_request) is the
     * only reclaim path. WP116: it does NOT run here. The only callers of
     * vol_fold_request / vol_reclaim_schedule are in vol_sweep.c's
     * vol_sweep_pending(), which this offline tool never reaches -- the
     * FUSE daemon path, and the only one that drains. An earlier version
     * of this comment claimed the fold "always runs as part of a normal
     * sweep"; that was false for the offline path and hid a v3 COW base
     * page leak (see docs/adr/ for the root-stack design). */

    if (!log_path) log_path = getenv("INVFS_SWEEP_LOG");
#ifndef _WIN32
    g_progress_tty = isatty(STDERR_FILENO) && !(log_path && *log_path);
#else
    g_progress_tty = 0;
#endif
    invfs_sweep_ui_set(g_progress_tty);
    if (g_progress_tty) atexit(sw_progress_finish);
    if (!color_arg) color_arg = getenv("INVFS_SWEEP_COLOR");
    if (color_arg && *color_arg) {
        if (strcmp(color_arg, "auto") == 0) g_color_mode = -1;
        else if (strcmp(color_arg, "always") == 0) g_color_mode = 1;
        else if (strcmp(color_arg, "never") == 0) g_color_mode = 0;
        else {
            fprintf(stderr, "--color: expected auto, always, or never\n");
            return 2;
        }
    }
#ifndef _WIN32
    if (sw_log_start(log_path, img) != 0) return 1;
#else
    if (log_path && *log_path) {
        fprintf(stderr, "--log is not supported on this platform\n");
        return 1;
    }
#endif

    /* per-file lines go to stdout, the summary to stderr: unbuffered, or a
     * redirected log tears a line at every 4 KB flush boundary */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifndef _WIN32
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
#endif

    /* WP20b --redundant-bench: synthetic head-to-head, no volume needed
     * (k=32, m=4, 64 MiB of data in RAM) */
    if (bench) {
        double vm, ca;
        if (rs_bench(32, 4, INVFS_BLOCK_SIZE, 512, &vm, &ca) != 0) {
            fprintf(stderr, "--redundant-bench: benchmark failed\n");
            return 1;
        }
        printf("[bench] rs-vm: %.1f MB/s, rs-cauchy: %.1f MB/s "
               "(k=32, m=4, 64 MiB data); winner: %s\n",
               vm, ca, vm >= ca ? "rs-vm" : "rs-cauchy");
        return 0;
    }

    /* WP16b: the codec profile rides the environment (INVFS_PROFILE).
     * Capture the setting BEFORE vol_open publishes the default into the
     * env: a default run must produce byte-identical LOGS too. */
    {
        int prof_from_env = getenv("INVFS_PROFILE") != NULL;
        g_volpath = img;
        vol = vol_open(img, &err);
        if (!vol) {
            fprintf(stderr, "cannot open volume %s (err %d)\n", img, err);
            return 1;
        }
        if (prof_from_env) {
            int ga = invfs_profile_generic_algo((int)vol_get_profile(vol));
            if (!invfs_sweep_ui_active()) {
                if (ga == INVFS_ALGO_ZSTD)
                    fprintf(stderr, "profile: %s (generic zstd level %d)\n",
                            invfs_profile_name((int)vol_get_profile(vol)),
                            invfs_profile_zstd_level((int)vol_get_profile(vol)));
                else
                    fprintf(stderr, "profile: %s (generic %s)\n",
                            invfs_profile_name((int)vol_get_profile(vol)),
                            ga == INVFS_ALGO_LZ4 ? "lz4" : "verbatim");
            }
        }
    }
    /* WP10 memory policy: same size grammar as INVFS_ARC_BYTES in volume.c;
     * unset keeps the volume default. */
    {
        const char *dl = getenv("INVFS_DEC_MEM_LIMIT");
        if (dl) {
            char *endp = NULL;
            unsigned long long want = strtoull(dl, &endp, 10);
            unsigned long long mult = 1;
            int ok = (endp != dl);
            if (ok) {
                while (*endp == ' ' || *endp == '\t') endp++;
                switch (*endp) {
                    case 'k': case 'K': mult = 1024ull; endp++; break;
                    case 'm': case 'M': mult = 1024ull * 1024; endp++; break;
                    case 'g': case 'G': mult = 1024ull * 1024 * 1024; endp++; break;
                    default: break;
                }
                if (*endp == 'b' || *endp == 'B') endp++;
                while (*endp == ' ' || *endp == '\t') endp++;
                if (*endp != '\0') ok = 0;
                if (want > (unsigned long long)SIZE_MAX / mult) ok = 0;
            }
            if (ok)
                vol_set_dec_mem_limit(vol, (uint64_t)(want * mult));
            else
                fprintf(stderr, "[sweep] INVFS_DEC_MEM_LIMIT=\"%s\" is not a "
                                "size; ignored\n", dl);
        }
    }
    (void)vol_sb(vol);

    /* WP-M21: the inline inode-area compaction + the CMP0 recovery preflight
     * both retired; --compact is now a recognised-but-removed flag (we
     * accept "invf-sweep --compact foo.img" as a synonym for a normal
     * sweep of foo.img). WP116 corrected the rationale: the fold is NOT
     * unconditional inside this run -- nothing here calls
     * vol_fold_request; only the FUSE drain (vol_sweep_pending) does. */

    /* WP23 --extract-packs: a standalone, read-only, engine-side mode for
     * the sweepboot maintenance boot (tools/sweepboot-init.sh). No sweep,
     * no checkpoint, no seal -- copy the on-volume codecpack dir out and
     * leave. */
    if (extract_dir) {
        int xrc = extract_packs(vol, extract_dir);
        vol_close(vol);
        return xrc;
    }

    {
        unsigned ui_total = 3;
        int live_seal = 0;

        if (!dry) {
            uint32_t k1c, m2c;
            int l2c;
            if (!unseal && vol_redun_state(vol, &k1c, &l2c, &m2c))
                live_seal = 1;
        }
        if (!dry && !fast) {
            ui_heat = 4;
            ui_tier = vol_ndev(vol) == 2 ? 5 : 0;
            ui_dedupe = ui_tier ? 6 : 5;
            ui_batches = ui_dedupe + 1;
            ui_finalize = ui_batches + 1;
            ui_seal = (seal || rb_f >= 0 || rp_f >= 0 || live_seal) ?
                      ui_finalize + 1 : 0;
            ui_total = ui_seal ? ui_seal : ui_finalize;
        } else if (!dry) {
            ui_finalize = 4;
            if (seal || rb_f >= 0 || rp_f >= 0 || live_seal) {
                ui_seal = 5;
                ui_total = ui_seal;
            } else {
                ui_total = ui_finalize;
            }
        }
        sw_stage_total(ui_total);
        sw_stage_begin(1, "prepare", 0, "checkpoint + policy");
    }

    /* WP201: the requested seal configuration, mapped onto the fixed menu
     * (in-memory only — vol_redun_config never touches the volume, so the
     * dry-run plan below may share the mapping). --seal [pct] is the
     * native spelling; the v2 --redundant-* spellings snap onto the same
     * menu. */
    if (!dry && (seal || rb_f >= 0 || rp_f >= 0)) {
        unsigned mk = 9, mm = 1;
        int algo = RS_ALGO_VM;
        if (seal_request_menu(vol, seal, seal_pct, rb_f, rp_f, rp_algo, 1,
                              &mk, &mm, &algo) != 0) {
            vol_close(vol);
            return 1;
        }
        vol_redun_config(vol, mk, algo, mm);
    }
    /* auto-reseal: no redundancy flags but a live descriptor -> continue
     * the persisted configuration after the sweep. Never with --heal:
     * the heal verdict must be read against the seal it repaired. */
    if (!seal && !unseal && !heal && !dry && rb_f < 0 && rp_f < 0) {
        uint32_t k1c, m2c;
        int l2c;
        if (vol_redun_state(vol, &k1c, &l2c, &m2c)) {
            auto_reseal = 1;
            if (!invfs_sweep_ui_active())
                fprintf(stderr, "redundancy: live RDP0 descriptor (k1=%u, "
                        "l2=%s m2=%u) -- auto-reseal after sweep\n",
                        (unsigned)k1c, rs_algo_name(l2c), (unsigned)m2c);
        }
    }

    /* WP20 --unseal: free all parity blocks and remove the owners; no sweep
     * walk runs (there is nothing to recompress, only seal state to drop). */
    if (unseal) {
        invfs_seal_report rep;
        if (vol_seal(vol, 1, &rep) != 0) {
            fprintf(stderr, "unseal failed\n");
            vol_close(vol);
            return 1;
        }
        printf("[unseal] %llu parity blocks freed, seal removed\n",
               (unsigned long long)rep.freed);
        /* Same durability point as the sweep below: the unseal just gave
         * every parity block back, and until that is flushed the allocator
         * will hand those blocks out again. */
        if (vol_flush(vol) != 0) {
            fprintf(stderr,
                    "FATAL: the flush after --unseal failed -- the parity "
                    "blocks just freed are NOT durable.\n");
            vol_close(vol);
            return 1;
        }
        vol_close(vol);
        return 0;
    }

    /* WP402 --heal: explicit drift repair, no sweep walk runs. Detection
     * (no group args) or the listed groups are reconstructed from
     * parity, verified against the manifest hashes, written back, and
     * re-verified; the footer is never rewritten. Under --dry-run the
     * engine detects and prints the plan, changing nothing. */
    if (heal) {
        invfs_seal_heal_report hrep;
        int hrc;
        memset(&hrep, 0, sizeof hrep);
        hrc = vol_seal_heal(vol, heal_n ? heal_gids : NULL, heal_n, dry,
                            &hrep);
        free(heal_gids);
        heal_gids = NULL;
        if (!dry && (hrep.groups_healed || hrep.groups_parity)) {
            /* Durability point for the bytes just committed: same rule
             * as the seal stage -- an unflushed heal is not a heal. */
            if (vol_flush(vol) != 0) {
                fprintf(stderr,
                        "FATAL: the flush after --heal failed -- the "
                        "reconstructed bytes just written are NOT "
                        "durable.\n");
                vol_close(vol);
                return 1;
            }
        }
        if (!dry && hrc == 0)
            printf("[heal] %llu healed, %llu parity-rewritten, %llu "
                   "already clean, %llu failed (%llu bytes rewritten)\n",
                   (unsigned long long)hrep.groups_healed,
                   (unsigned long long)hrep.groups_parity,
                   (unsigned long long)hrep.groups_clean,
                   (unsigned long long)hrep.groups_failed,
                   (unsigned long long)hrep.bytes_rewritten);
        vol_close(vol);
        return hrc == 0 ? 0 : 1;
    }

    /* Resolve the previous sweep's rollback window. --realize is the
     * standalone point of no return (drop the previous save point), then a
     * normal sweep proceeds. A dry run touches nothing. A failed capture
     * never stops the sweep -- the run just goes unsavepointed.
     *
     * F9: NOTHING destructive happens here. An earlier shape captured (and
     * reclaimed!) in prepare and refused in collect, leaving a refused
     * sweep's frees behind (leg-5 dead-end: live roots freed by a drop-mode
     * prepare, collect refusing too late). Collect is read-only, so the
     * K=1 drop and the fresh capture move to after the live set validates
     * (below, before transform): a refused sweep is then bit-identical to
     * one never run. The --realize drop moves with it for the same reason:
     * an explicit point of no return is still no return when it happens
     * after a read-only validation. */
    if (!dry && no_realize) {
        /* --no-realize keeps the previous save point live; every other run
         * arms a fresh one after the walk, so the live window is always
         * the LAST sweep. K=1. */
        if (!invfs_sweep_ui_active())
            fprintf(stderr, "save point: kept previous (--no-realize)\n");
    }
    sw_stage_end(dry ? "dry-run policy ready" : "policy ready");

    sw_stage_begin(2, "collect", 0, "walking live inodes");


    /* Collect the live set from the namespace (base tree + delta overlay).
     * vol_open refuses any volume without VOLF_META, so there is no record
     * stream to walk and no second collector. */
    {
        vol_walk_t w;
        int wrc;
        collect_ctx vc;
        memset(&vc, 0, sizeof vc);
        vc.vol = vol;
        vc.names = &names; vc.inodes = &inodes;
        vc.sizes = &sizes; vc.poss = &poss;
        vc.tab = &tab; vc.tmask = &tmask; vc.tcount = &tcount;
        vc.count = &count; vc.cap = &cap;
        vol_walk_init(&w, vol, "invf-sweep collect");
        /* WP135: the STRICT walk -- the collect's output is the input to
         * every mutating stage below it. */
        wrc = vol_walk_strict(vol, sweep_walk_cb, &vc);
        /* `vc.count` is the int the collect callback increments, by POINTER:
         * *(vc.count) is what the walk delivered. Casting the pointer itself
         * is how the first draft of this line printed a 47-bit address as an
         * entry count. */
        vol_walk_result(&w, wrc, (size_t)*vc.count, (size_t)*vc.count);
        if (vol_walk_commit(&w) != 0) {
            sw_progress_suspend();
            /* TWO CASES, and they are not the same volume state. A walk that
             * stopped having already delivered entries produced a PARTIAL
             * live set; a walk that stopped before delivering anything
             * produced NO live set at all, and the stages below would then
             * run over an empty list and report a clean sweep of a volume
             * they never read. The second is strictly worse -- it is not a
             * subset, it is nothing -- and an operator reading "0 files" in
             * the summary deserves to know the volume was never walked
             * rather than believed to be empty. */
            fprintf(stderr,
                    "invf-sweep: the v3 namespace walk did not complete. %s\n"
                    "Refusing to %s: a partial live set is not a smaller "
                    "sweep, it is the wrong one. Run invf-fsck on the image "
                    "first.\n",
                    *vc.count
                        ? "It stopped partway through, after delivering some "
                          "of the live set."
                        : "It stopped before delivering ANY entry, so this "
                          "volume yielded no live set at all -- an empty "
                          "sweep here means the volume was never read, not "
                          "that it holds nothing worth sweeping.",
                    dry ? "plan" : "sweep");
            return 1;
        }
        if (vc.oom) {
            sw_progress_suspend();
            fprintf(stderr, "out of memory\n");
            return 1;
        }
    }

    /* The walk above collects the newest record per name, but the live
     * answer is the name index's consistent cut (a torn newest version is
     * hidden and the name resolves to an older id, or is absent). Sweep
     * exactly the live ids -- sweeping a hidden entry would fail its reads
     * and could resurrect dead ids' blocks. */
    /* WP22d: the walk above collects the newest record per name, but the
     * live answer is the name index's consistent cut (a torn newest
     * version is hidden and the name resolves to an older id, or is
     * absent). Sweep exactly the live ids -- sweeping a hidden record
     * would fail its reads and could resurrect dead ids' blocks. */
    {
        int j;
        char detail[128];
        kept = 0;
        for (j = 0; j < count; j++) {
            uint64_t live;
            if (inodes[j] == 0 || sizes[j] == 0) continue;
            live = vol_find(vol, names[j]);
            if (live == 0) { inodes[j] = 0; continue; }
            if (live != inodes[j]) {
                /* the name now resolves to a DIFFERENT record than the one
                 * the walk saw: a container commit (or a decomposition
                 * migration re-deriving one) replaced it mid-walk, so the
                 * cached size belongs to the retired record. Refresh it or
                 * the sweep compares a fresh file against a stale length. */
                uint64_t nsz = 0;
                if (vol_stat_full(vol, names[j], NULL, &nsz, NULL) == 0 &&
                    nsz > 0)
                    sizes[j] = nsz;
            }
            inodes[j] = live;   /* may be the fallback version's id */
            kept++;
        }
        if (!invfs_sweep_ui_active())
            fprintf(stderr, "live entries: %d (of %d walked)\n", kept, count);
        snprintf(detail, sizeof detail, "%d live / %d walked", kept, count);
        sw_stage_end(detail);
    }

    /* F9: arm the rollback window HERE, after the live set validated and
     * before the first mutating stage. Collect above is read-only, so a
     * sweep refused there leaves the volume bit-identical (the prepare
     * used to capture and reclaim first, and a refused sweep kept its
     * frees). K=1: the previous window is dropped only once the new one
     * is captured and proven -- never the reverse. A dry run and
     * --no-realize still touch nothing. */
    if (!dry && !no_realize) {
        if (realize) {
            int drc = spt0_drop(vol);
            if (drc < 0) {
                sw_progress_suspend();
                fprintf(stderr, "save point: realizing the previous "
                                "run failed\n");
                vol_close(vol);
                return 1;
            }
            if (!invfs_sweep_ui_active())
                fprintf(stderr, drc > 0
                        ? "save point: previous run realized\n"
                        : "save point: nothing to realize\n");
        } else if (spt0_info(vol, NULL)) {
            /* bare sweep: replace the previous window (K=1) */
            (void)spt0_drop(vol);
        }
        if (spt0_capture(vol) == 0) {
            invfs_spt0 sp;
            if (spt0_info(vol, &sp) && !invfs_sweep_ui_active())
                fprintf(stderr, "save point captured "
                                "(base_root=%llu delta_end=%llu)\n",
                        (unsigned long long)sp.base_root,
                        (unsigned long long)sp.delta_end);
            /* F7: a capture that returned 0 may still not exist on
             * disk (acknowledged-but-discarded writes), and spt0_info
             * above only reports memory. Without a PROVABLE window a
             * torn publish has no way back, so a failed verification
             * abandons the pass -- fail closed. spt0_drop unwinds the
             * in-memory window and its pin; the volume is unchanged. */
            if (spt0_verify_live(vol) != 0) {
                sw_progress_suspend();
                fprintf(stderr, "save point: capture did not land "
                                "(re-read mismatch); REFUSING the pass "
                                "(the volume is unchanged)\n");
                (void)spt0_drop(vol);
                vol_close(vol);
                return 1;
            }
        } else {
            sw_progress_suspend();
            fprintf(stderr, "save point: capture failed; sweeping "
                            "without one\n");
        }
        if (!invfs_sweep_ui_active())
            fprintf(stderr, "savepoint ready\n");
    }

    /* WP19: the once-per-RUN heat decay (rheat >>= 1, wheat -= 1), after the
     * savepoint arms and before transform, so the lane dispatch below sees
     * post-decay values. (It used to run before the walk; F9 moved the
     * capture after collect, and the decay stays with the mutating
     * stages -- a refused sweep must leave the volume untouched.) */
    if (!dry)
        vol_heat_sweep_begin(vol);

    /* sweep candidates: regular files with actual payload */
    sw_stage_begin(3, dry ? "plan" : "transform", (uint64_t)count,
                   dry ? "would sweep" : "files");
    for (int i = 0; i < count; i++) {
#ifndef _WIN32
        if (g_stop) { stopped = 1; break; }
        /* WP21 test hook (tools/test-rollback.sh): die mid-walk, after N
         * candidates, with the checkpoint armed and retention half-filled
         * -- the crash-mid-sweep rollback leg. (Keyed on the walk index:
         * deferred batching candidates move neither swept nor skipped.) */
        {
            const char *ab = getenv("INVFS_SWEEP_ABORT_AFTER");
            if (ab && !dry && i + 1 == atoi(ab) && atoi(ab) > 0)
                kill(getpid(), SIGKILL);
        }
#endif
        if (inodes[i] == 0 || sizes[i] == 0) {
            skipped++;
            sw_stage_update((uint64_t)i + 1u, (uint64_t)count, "skipped");
            continue;
        }
        if (dry) {
            char detail[320];
            if (!invfs_sweep_ui_active()) {
                sw_progress_suspend();
                printf("would sweep %s (%llu bytes)\n",
                       names[i], (unsigned long long)sizes[i]);
            }
            snprintf(detail, sizeof detail, "%s", names[i]);
            sw_stage_update((uint64_t)i + 1u, (uint64_t)count, detail);
            continue;
        }
        if (!invfs_sweep_ui_active()) {
            sw_progress_suspend();
            fprintf(stderr, "  sweeping %s (%llu bytes)...\n",
                    names[i], (unsigned long long)sizes[i]);
        }
        {
            /* vol_sweep_one: 0 = nothing to do, >0 = transcoded/swept,
             * 7 = JPEG->JXL, 9 = text deferred into the batch accumulator,
             * 10 = binary deferred into the WP14a binary accumulator (both
             * sealed by vol_tz_flush below), 11 = exe-as-container carve
             * (WP14b M2), >=100 = codecpack transcode (100+algo, WP13),
             * <0 = hard error */
            int rc;
            if (fast) {
                sw_stage_update((uint64_t)i, (uint64_t)count, "transforming");
                /* WP22e --fast: the decision narrows to "generic or
                 * nothing" (vol_sweep_file_generic: 0 = swept to Shadow,
                 * 1 = nothing to do, <0 = hard error). No per-file line:
                 * the generic floor prints none in the full pass either. */
                rc = vol_sweep_file_generic(vol, inodes[i]);
                if (rc == 0) swept++;
                else if (rc > 0) skipped++;
                else failed++;
                goto progress;
            }
            sw_stage_update((uint64_t)i, (uint64_t)count, "transforming");
            /* show what we are about to chew on, before any per-segment
             * callback arrives: a container decomposition reports none */
            sw_tree_set(vol, names[i], 0, 0);
            sw_stage_fraction((double)i, (uint64_t)i, (uint64_t)count,
                              names[i]);
            rc = vol_sweep_one_ex(vol, inodes[i], names[i],
                                  sw_file_progress, vol);
            if (rc < 0 && getenv("INVFS_DEBUG_PACKS"))
                /* a 50k-file sweep says nothing about WHICH file failed */
                fprintf(stderr, "sweep: %s: FAILED (rc=%d)\n",
                        names[i], rc);
            /* WP146 -- "has a '!'" is not the question; "is this one of OURS?"
             * is. This used to be `strchr(names[i], '!')`, which asks a
             * question about BYTES IN A NAME, and it made the sweep say
             * something false: part_agg_add sums under the prefix up to the
             * first '!', so N independent user files named doc!000.txt ..
             * doc!299.txt are reported to the operator as
             *
             *     doc!*: 300 parts -> PPMd batch
             *
             * which is one container's summary line. There is no container.
             * The lanes were RIGHT -- WP136 made them run on these files and
             * batch them, which is correct -- and the REPORT was the thing
             * still answering the pre-WP135 question, so the fix made the
             * output worse rather than better: before, the file was skipped
             * and the sweep said nothing about it; now it is transformed
             * correctly and MIS-ATTRIBUTED. No error anywhere: this branch
             * prints a success line and the run exits 0.
             *
             * THE TOOL CAN REACH THE VOLUME. `vol` is right here, and
             * names[i] is a name read off this volume by the collect stage
             * (:1734), not scraped out of a log -- so this is not a case
             * where a weaker string predicate is forced on us. The core
             * already owns the exact question (vol_name_is_container_sibling,
             * src/core/volume_internal.h:1045) and answers it with the
             * volume: minted suffix shape AND a live container inode. Ask it
             * the same question rather than adding a fourth string test.
             *
             * NOTE WHAT THIS DOES NOT REVIVE. Both rc==9 and rc==10 are
             * emitted from inside `if (!vol_name_is_container_sibling(v,
             * name))` (src/core/vol_sweep.c:695 :704 and :819 :823), so on
             * this tree a lane-minted sibling can never reach this branch and
             * the WP14b aggregation is currently DORMANT for real members --
             * measured: a genuine tar decomposition reports its container and
             * each !partN by name and produces no `box.tar!*: N parts` line
             * at all. The call stays because that is a property of the lanes,
             * not of the report, and a lane that returns 9/10 for a member
             * again should aggregate rather than print one line per member
             * (WP14b: a Silesia run logged 1573 near-identical lines). What
             * is gone is the population that was never legitimate. */
            if (rc == 9) {
                if (vol_name_is_container_sibling(vol, names[i]))
                    part_agg_add(names[i], 0);
                else if (!invfs_sweep_ui_active())
                    printf("  %s: text -> PPMd batch\n", names[i]);
            }
            else if (rc == 10) {
                if (vol_name_is_container_sibling(vol, names[i]))
                    part_agg_add(names[i], 1);
                else if (!invfs_sweep_ui_active())
                    printf("  %s: binary -> ZSTD batch\n", names[i]);
            }
            else if (rc == 11) {
                swept++;
                if (!invfs_sweep_ui_active())
                    printf("  %s: exe media -> JXL (%u parts)\n", names[i],
                           vol_exer_last_parts(vol));
            }
            else if (rc == 7) {
                swept++;
                if (!invfs_sweep_ui_active())
                    printf("  %s: JPEG -> JXL (lossless)\n", names[i]);
            }
            else if (rc >= 100) {
                const invfs_codec *pc = invfs_codec_by_algo((uint32_t)(rc - 100));
                swept++;
                sw_dash_note_container(vol, names[i]);
                if (!invfs_sweep_ui_active())
                    printf("  %s: %s (codecpack)\n", names[i],
                           pc ? pc->name : "unknown-pack");
            }
            else if (rc > 0) swept++;
            else if (rc == 0) {
                /* rc == 0 is "no lane claimed this file". It used to be
                 * counted into `skipped` with no line at all, so a pack that
                 * declined -- size guard, memory policy, ENOSPC deferral --
                 * was indistinguishable from a file with nothing to do, and
                 * the summary read "skipped=1" with nothing to explain it.
                 * A policy that is supposed to leave a stamp (DEFER_ENOSPC)
                 * was likewise indistinguishable from one that silently did
                 * nothing. Name the file. */
                skipped++;
                if (!invfs_sweep_ui_active())
                    printf("  %s: no lane claimed it (declined or nothing to do)\n",
                           names[i]);
            }
            else failed++;
        }
progress:
        {
            char detail[160];
            snprintf(detail, sizeof detail, "swept=%d skipped=%d failed=%d",
                     swept, skipped, failed);
            sw_stage_update((uint64_t)i + 1u, (uint64_t)count, detail);
        }
        if (!invfs_sweep_ui_active() && i > 0 && (i + 1) % 5000 == 0)
            fprintf(stderr, "  ..%d files processed (swept=%d)\n",
                    i + 1, swept);
    }

    {
        char detail[160];
        snprintf(detail, sizeof detail,
                 "swept=%d skipped=%d failed=%d", swept, skipped, failed);
        sw_stage_end(detail);
    }

    /* WP14b: print the aggregated container-part deferral lines collected
     * during the walk (one line per container instead of one per part) */
    part_agg_print();

    /* WP19: extract read-hot PPMd batch members to standalone per-segment
     * ZSTD (class GENERIC) -- between the walk and the dedupe pass, so the
     * promoted segments can merge and the GC below reclaims any batch the
     * promotions killed. The pass prints its own counts.
     * WP22e: --fast skips this (a transcode), along with dedupe/batching. */
    if (!dry && !fast) {
        int hrc;
        sw_stage_begin(ui_heat, "heat", 0, "promoting hot batch members");
        hrc = vol_heat_promote(vol);
        if (hrc < 0) {
            sw_progress_suspend();
            fprintf(stderr, "heat: promotion pass failed (sweep results "
                            "are intact)\n");
        }
        sw_stage_end(hrc < 0 ? "failed; sweep data intact" : "promotion complete");
    }

    /* WP25 rule 9: two-device tier migration -- canonical stays on dev1;
     * read-hot canonical segments get a dev0 acceleration copy, arena
     * pressure (<20% free) evicts the coldest copies. Runs after the
     * decay + promotion so post-decay rheat governs (the WP19 hysteresis
     * applies). No-op on a single-device volume. */
    if (!dry && !fast && vol_ndev(vol) == 2) {
        int trc;
        sw_stage_begin(ui_tier, "tier", 0, "balancing hot/cold devices");
        trc = vol_tier_migrate(vol);
        if (trc < 0) {
            sw_progress_suspend();
            fprintf(stderr, "tier: migration pass failed (sweep results "
                            "are intact)\n");
        }
        sw_stage_end(trc < 0 ? "failed; sweep data intact" : "migration complete");
    }

    /* WP12(h): per-segment dedupe between the walk and the text-batch GC
     * (order: walk -> dedupe -> GC -> flush). The walk's transcodes are
     * what create the duplicates worth finding -- identical content lands
     * in Shadow as identical segments -- and dedupe runs before the GC so
     * it never sees a zone==TEXT entry (WP10 §11). The pass prints its
     * own merged/freed counts. */
    if (!dry && !fast) {
        int drc;
        char detail[224];
        sw_stage_begin(ui_dedupe, "dedupe", 0, "hashing live segments");
        memset(&ui_dedupe_stats, 0, sizeof ui_dedupe_stats);
        drc = vol_sweep_dedupe_ex(vol, &ui_dedupe_stats,
                                  sw_dedupe_progress, NULL);
        if (drc < 0) {
            sw_progress_suspend();
            fprintf(stderr, "dedupe: pass failed (sweep results are intact)\n");
        }
        snprintf(detail, sizeof detail,
                 "cross=%llu intra=%llu merged=%llu freed=%.1f MiB",
                 (unsigned long long)ui_dedupe_stats.cross_merged,
                 (unsigned long long)ui_dedupe_stats.intra_merged,
                 (unsigned long long)ui_dedupe_stats.segments_merged,
                 (double)ui_dedupe_stats.blocks_freed *
                 (double)INVFS_BLOCK_SIZE / (1024.0 * 1024.0));
        sw_stage_end(drc < 0 ? "failed; sweep data intact" : detail);
        /* The stage line only reaches a TTY. Without this, a batch/CI sweep
         * ran the dedupe pass SILENTLY -- no merged count, no freed bytes --
         * which is exactly the line an operator (and test-dedupe) greps for.
         * Print it whenever the UI is off, in the same key=value shape. */
        if (!invfs_sweep_ui_active())
            fprintf(stderr, "dedupe: merged %llu segments, freed %llu blocks "
                    "(%.1f MiB; cross %llu, intra %llu)\n",
                    (unsigned long long)ui_dedupe_stats.segments_merged,
                    (unsigned long long)ui_dedupe_stats.blocks_freed,
                    (double)ui_dedupe_stats.blocks_freed *
                    (double)INVFS_BLOCK_SIZE / (1024.0 * 1024.0),
                    (unsigned long long)ui_dedupe_stats.cross_merged,
                    (unsigned long long)ui_dedupe_stats.intra_merged);
    }

    /* WP10 §7 + WP14a: reclaim owner batches no live member references,
     * then seal the accumulated text AND binary candidates into shared
     * batches (one vol_tz_flush drains both accumulators). The deferred
     * counts come from the accumulators themselves: parts deferred at
     * container-explode time (WP14b) never produced a walk line.
     * --fast deferred nothing, so the GC/flush are skipped with it.
     * WP78: v3 volumes now defer too (TEXT/BATCHED_BIN batches published as
     * v3 recipe deltas), and vol_tz_gc/vol_tz_flush dispatch to the v3
     * registry path, so the pass runs on both formats. */
    if (!dry && !fast) {
        int gcrc;
        size_t tzp, bzp;
        char detail[128];
        int tzrc;
        sw_stage_begin(ui_batches, "batches", 0, "GC + shared batch flush");
        gcrc = vol_tz_gc(vol);
        tzp = vol_acc_pending(vol, 0);
        bzp = vol_acc_pending(vol, 1);
        if (gcrc > 0) {
            if (!invfs_sweep_ui_active())
                printf("text gc: %u dead batches reclaimed\n", (unsigned)gcrc);
        } else if (gcrc < 0) {
            sw_progress_suspend();
            fprintf(stderr, "text gc failed (rc=%d)\n", gcrc);
        }
        tzrc = vol_tz_flush(vol);
        if (tzrc == 0) {
            if (!invfs_sweep_ui_active()) {
                if (tzp)
                    printf("text batches flushed (%zu deferred)\n", tzp);
                if (bzp)
                    printf("binary batches flushed (%zu deferred)\n", bzp);
            }
        }
        else if (tzrc < 0) {
            sw_progress_suspend();
            fprintf(stderr, "batch flush failed (rc=%d)\n", tzrc);
            failed++;
        }
        snprintf(detail, sizeof detail, "gc=%d text=%zu binary=%zu",
                 gcrc, tzp, bzp);
        sw_stage_end(tzrc < 0 ? "flush failed" : detail);
    }

    if (!dry) {
        sw_stage_begin(ui_finalize, "finalize", 0, "checkpoint + volume flush");
    }
    if (!invfs_sweep_ui_active()) {
        fprintf(stderr, "sweep done: swept=%d skipped=%d failed=%d%s\n",
                swept, skipped, failed,
                stopped ? " (stopped by Ctrl+C)" : "");
        /* WP42: the CLI-style summary line the big-volume e2e parses
         * (`sweep: N swept`); mirrors src/cli/sweep.c's report. */
        fprintf(stderr, "sweep: %d swept\n", swept);
    }


    if (!dry) {
        int frc = vol_flush(vol);
        if (frc != 0) {
            sw_progress_suspend();
            flush_failed = 1;
            /* This is THE durability point: everything above it rewrote
             * the volume's data. A flush that fails here means that rewrite
             * is not on stable storage, so the run has produced partial
             * success -- the volume now holds new segments and a new
             * recipe that the bitmap and the superblock do not yet vouch
             * for. Say so plainly, and fail: the caller asked whether the
             * data is safe, and it is not. The volume is not rolled back
             * and the failure is not retried into a quiet success -- the
             * operator gets this line, the nonzero exit, and a volume left
             * dirty for the next mount to replay. */
            fprintf(stderr,
                    "FATAL: the final volume flush failed -- this sweep "
                    "rewrote data that is NOT durable.\n"
                    "       The volume is left dirty (see any latch line "
                    "above); run invf-fsck before trusting it.\n"
                    "       This run did NOT complete.\n");
        }
        sw_stage_end(frc == 0 ? "volume durable" : "flush failed");
    }

    /* WP201: the seal stage. A requested seal runs AFTER the sweep is
     * fully flushed (parity covers the post-sweep state); a live seal
     * with no flags auto-reseals (full recompute). Under --dry-run the
     * stage is a read-only plan: what WOULD be sealed, in groups, bytes
     * and overhead — the volume is not touched. */
    if (seal || rb_f >= 0 || rp_f >= 0 || auto_reseal) {
        if (dry) {
            /* Read-only plan: the REQUESTED menu over walked REG sizes.
             * Nothing here configures or writes (no vol_redun_config). */
            unsigned pk = 9, pm = 1;
            int pa = RS_ALGO_VM;
            vol_walk_t pw;
            seal_plan_ctx pc;
            uint64_t pg = 0, ppb = 0;
            memset(&pc, 0, sizeof pc);
            seal_request_menu(vol, seal, seal_pct, rb_f, rp_f, rp_algo, 0,
                              &pk, &pm, &pa);
            vol_walk_init(&pw, vol, "seal plan walk");
            vol_walk_result(&pw, vol_walk_strict(vol, seal_plan_cb, &pc),
                            pc.n, pc.n);
            if (vol_walk_commit(&pw) != 0) {
                sw_progress_suspend();
                fprintf(stderr, "seal plan: namespace walk stopped; "
                        "no plan available\n");
            } else {
                double ov = pc.bytes ? 100.0 * (double)(pm * SEAL_SYM_BYTES) /
                    ((double)pk * SEAL_SYM_BYTES) : 0.0;
                seal_plan(pk, pm, pc.bytes, &pg, &ppb);
                printf("[seal] plan: %llu files, %llu data bytes -> "
                       "%llu groups (k=%u,m=%u,%s, 64 KiB symbols), "
                       "%llu parity bytes (~%.1f%%); no changes (dry-run)\n",
                       (unsigned long long)pc.n,
                       (unsigned long long)pc.bytes,
                       (unsigned long long)pg, pk, pm, rs_algo_name(pa),
                       (unsigned long long)ppb, ov);
            }
        } else {
        sw_stage_begin(ui_seal, "seal", 0, "updating parity stripes");
        invfs_seal_report rep;
        uint32_t k1c, m2c;
        int l2c;
        vol_redun_state(vol, &k1c, &l2c, &m2c);
        if (vol_seal(vol, 0, &rep) != 0) {
            sw_progress_suspend();
            fprintf(stderr, "seal failed\n");
            sw_stage_end("failed");
            vol_close(vol);
            return 1;
        }
        /* WP201: v1 report line. l2c carries the GROUP code here (there is
         * no second layer; l2_* report fields stay 0 and no [seal2] line
         * is printed). Every seal is a full recompute, so updated ==
         * groups and unchanged == 0 by construction, not by accident. */
        if (!invfs_sweep_ui_active()) {
            printf("[seal] %llu groups, %llu parity blocks, overhead "
                   "%.2f%% of covered data; %llu groups (re)written "
                   "(full recompute, k=%u, m=%u, %s)",
                   (unsigned long long)rep.stripes,
                   (unsigned long long)rep.parity_blocks, rep.overhead_pct,
                   (unsigned long long)rep.updated,
                   (unsigned)k1c, (unsigned)m2c, rs_algo_name(l2c));
            if (rep.added || rep.freed)
                printf(" (%llu added, %llu stale freed)",
                       (unsigned long long)rep.added,
                       (unsigned long long)rep.freed);
            if (rep.unprotected)
                printf(", %llu unprotected (ENOSPC)",
                       (unsigned long long)rep.unprotected);
            printf("\n");
        }
        {
            int frc = vol_flush(vol);
            char detail[160];
            if (frc != 0) {
                sw_progress_suspend();
                /* Same durability point, second call: the seal's parity
                 * stripes were just written. Latched for the same reason
                 * -- if the main flush above already failed, this one
                 * succeeding does not unmake that. */
                flush_failed = 1;
                fprintf(stderr,
                        "FATAL: the flush after the seal failed -- the "
                        "parity stripes just written are NOT durable.\n");
            }
            snprintf(detail, sizeof detail,
                     "groups=%llu updated=%llu parity=%llu",
                     (unsigned long long)rep.stripes,
                     (unsigned long long)rep.updated,
                     (unsigned long long)rep.parity_blocks);
            sw_stage_end(frc == 0 ? detail : "flush failed after seal");
        }
        } /* end WP201 dry-plan else (live seal) */
    }

    /* WP121: the offline sweep holds the volume exclusively, so this is the
     * quiescent point the orphan collector's caller contract asks for. The
     * FUSE drain reaches the same collector through fold_reclaim_hook; the
     * offline path had no reclaim call at all, which is why a swept volume
     * never converged. Gated on INVFS_RECLAIM_ORPHANS=1 (default off) and
     * a no-op otherwise, so nothing here changes on an unset environment.
     *
     * WP126: this is the FULL drain, not the fold path's single bounded
     * pass. That split is the whole design: the fold path has to be cheap
     * enough to run on every fold of a live root, and a sweep has no such
     * constraint, so the sweep keeps paying for a complete collection and
     * the compression result is unchanged. The drain loops until a whole
     * pass frees nothing, so it terminates; each individual pass is still
     * budgeted, so a sweep cannot be surprised by one enormous read. */
    {
        uint64_t orphans = 0;
        if (vol_reclaim_orphans_full(vol, &orphans) < 0) {
            sw_progress_suspend();
            fprintf(stderr, "warning: v3 orphan reclaim failed\n");
        } else if (orphans && !invfs_sweep_ui_active()) {
            printf("[reclaim] %llu orphaned v3 base page(s) collected\n",
                   (unsigned long long)orphans);
        }
    }

    sw_progress_finish();
    vol_close(vol);
#ifndef _WIN32
    sw_log_stop();
#endif
    /* The exit contract, in one place. `failed` counts files that did not
     * sweep;
     * `flush_failed` latches a failed durability point. Any of the three
     * means "do not report this volume as swept and durable". */
    return (failed || flush_failed) ? 1 : 0;
}
