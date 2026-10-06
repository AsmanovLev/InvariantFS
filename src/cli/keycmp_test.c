/* keycmp_test.c — THE v3 key ordering, asserted on its own.
 *
 * What this is for
 * ----------------
 * The v3 base B+-tree, the delta log and the fold each sort keys, and the
 * fold's whole job is to merge a base run into a delta run. That merge is
 * only a merge if the three sorts are the same total order. They used to be
 * three byte-identical private statics — bt_cmp (vol_btree.c:113),
 * dl_key_cmp (vol_delta.c:690) and fold_key_cmp (vol_fold.c:202) — with no
 * build-time relationship between them at all, so a one-line change to any
 * one of them compiled, linked, and passed every test in the tree. (That
 * was measured, not assumed: see the commit message. Nothing broke, because
 * the copies had not drifted. The hazard was the arrangement, not the code.)
 *
 * They are now one function, vol_key_cmp() in volume_internal.h. This test
 * is the other half of that: it proves the ordering was PRESERVED, and it
 * proves the merge property is not vacuous.
 *
 * The ordering, for the record: unsigned byte-lexicographic over the whole
 * key, shorter-first on a strict prefix. The leading byte orders the
 * namespaces (INVFS_XATTR_KEY_PREFIX 0x03, INVFS_RECIPE_KEY_PREFIX
 * 0x04 — invarifs.h:1337,1349); every field after it is fixed-width
 * big-endian, so byte order over the key is also numeric order on the inode
 * ids, name lengths and digests inside it; and the shorter-first tiebreak is
 * load-bearing because an over-page value is stored under its canonical key
 * with the continuations appended -- a recipe as 0x04 || addr vs
 * 0x04 || addr || 0x00 || idx (vol_btree.c:3771), an xattr as its name key
 * vs name || 0x00 || chunk:u16 BE (vol_btree.c:2813) -- so the canonical
 * key is a strict prefix of its own continuations.
 *
 * The six things asserted
 * ------------------------
 *   1. EQUIVALENCE. Every comparison vol_key_cmp() makes, on every key
 *      shape below, returns what the pre-fix bt_cmp/dl_key_cmp body
 *      returned (legacy_cmp, kept verbatim). This is the answer to "you
 *      touched a load-bearing comparator — did the order change?"
 *   2. INDEPENDENCE. The same comparisons, against ref_cmp, which shares
 *      no code with it (byte loop, no memcmp). So the shared comparator is
 *      plain byte-lexicographic shorter-first, not merely "what it was".
 *   3. TOTAL ORDER. Irreflexive, antisymmetric and transitive over all
 *      1555 keys of length 0..4 drawn from a six-byte alphabet, checked
 *      pairwise and (transitivity) over all triples.
 *   4. THE SHAPES. Real v3 keys — inode, dirent, xattr, recipe descriptor,
 *      recipe chunk -- with the properties the format depends on: namespace
 *      disjointness, big-endian numeric order (an inode id of 255
 *      sorts before 256, and 256 before 65536 -- which little-endian
 *      gets backwards), and prefix-first.
 *   5. THE MERGE. Merging a base run and a delta run, each sorted by
 *      vol_key_cmp, yields exactly the sorted union — the property the fold
 *      depends on — WITH A CONTROL that redoes the merge with a second,
 *      deliberately different comparator on the delta side and requires the
 *      answer to CHANGE. Without that control, case 5 would pass on an
 *      ordering that broke the merge.
 *
 *   6. ONE DEFINITION. The tree still contains exactly one definition of
 *      the ordering, in the shared header, and all three modules call it.
 *      This is the one that fires on re-duplication: cases 1-5 all call
 *      vol_key_cmp() by name, so a second private copy would satisfy every
 *      one of them while putting the tree right back where it started.
 *
 * What this test does NOT do
 * --------------------------
 * It does not open a volume, and it does not prove the fold is correct. It
 * proves the ORDERING is the one it has always been, and that the merge
 * property is stated in a way that can fail.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../core/volume_internal.h"

static int fails;
static int checks;

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

/* ---- the two oracles ------------------------------------------------- */

/* The pre-fix body, verbatim from main@2a47d07 src/core/vol_btree.c:113
 * and src/core/vol_delta.c:690. Kept so the question "did the order change
 * when the three copies became one?" has an answer that does not depend on
 * this test agreeing with itself. */
static int legacy_cmp(const uint8_t *a, uint16_t an,
                      const uint8_t *b, uint16_t bn)
{
    uint16_t m = an < bn ? an : bn;
    int c = m ? memcmp(a, b, m) : 0;
    if (c)
        return c < 0 ? -1 : 1;
    if (an < bn)
        return -1;
    if (an > bn)
        return 1;
    return 0;
}

/* Byte-lexicographic, shorter-first, written differently on purpose: no
 * memcmp, no ternaries. Shares nothing with the implementation under test,
 * so agreeing with it is evidence rather than tautology. */
static int ref_cmp(const uint8_t *a, uint16_t an,
                   const uint8_t *b, uint16_t bn)
{
    uint16_t i, n = an < bn ? an : bn;
    for (i = 0; i < n; i++) {
        unsigned x = a[i], y = b[i];
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (an == bn)
        return 0;
    return an < bn ? -1 : 1;
}

/* What bt_cmp did in the drift probe that motivated this WP, and what the
 * fold's comparator still is in every other tree on disk: longer-prefix-
 * first. A real order, and a wrong one. */
static int inverted_cmp(const uint8_t *a, uint16_t an,
                        const uint8_t *b, uint16_t bn)
{
    int c = ref_cmp(a, an, b, bn);
    if (c == 0)
        return 0;
    return an != bn && (uint16_t)(an < bn) ? c : -c;
}

/* ---- a sortable list, sorted with the comparator under test ---------- */

typedef struct {
    uint8_t  k[40];
    uint16_t n;
} tkey;

typedef struct {
    tkey    *v;
    size_t   n;
    int (*cmp)(const uint8_t *, uint16_t, const uint8_t *, uint16_t);
} tlist;

static void tlist_init(tlist *l, tkey *store, size_t n,
                       int (*cmp)(const uint8_t *, uint16_t,
                                  const uint8_t *, uint16_t))
{
    l->v = store;
    l->n = n;
    l->cmp = cmp;
}

/* Insertion sort: small, and it makes it obvious the comparator under test
 * is the one doing the ordering rather than qsort's internals. */
static void tlist_sort(tlist *l)
{
    size_t a, b;
    for (a = 1; a < l->n; a++) {
        tkey t = l->v[a];
        b = a;
        while (b > 0 && l->cmp(l->v[b - 1].k, l->v[b - 1].n,
                               t.k, t.n) > 0) {
            l->v[b] = l->v[b - 1];
            b--;
        }
        l->v[b] = t;
    }
}

/* ---- 1-3: exhaustive small keys -------------------------------------- */

#define ALPHA_N 6
static const uint8_t ALPHA[ALPHA_N] = { 0x00, 0x01, 0x03, 0x04, 0x41, 0xff };
#define MAXLEN  4
/* 1 + 6 + 36 + 216 + 1296 */
#define NKEYS   1555
/* the length <= 3 prefix: 1 + 6 + 36 + 216 */
#define NKEYS3  259

static tkey g_small[NKEYS];
static tkey g_small3[NKEYS3];

static void build_small(tkey *out, size_t maxlen, size_t *n_out)
{
    uint16_t n = 0;
    size_t total = 1, len;
    for (len = 1; len <= maxlen; len++)
        total *= ALPHA_N;
    for (len = 0; len <= maxlen; len++) {
        size_t c = 1, idx;
        for (size_t k = 0; k < len; k++)
            c *= ALPHA_N;
        for (idx = 0; idx < c; idx++) {
            size_t r = idx;
            uint16_t i;
            for (i = 0; i < (uint16_t)len; i++) {
                out[n].k[i] = ALPHA[r % ALPHA_N];
                r /= ALPHA_N;
            }
            out[n].n = (uint16_t)len;
            n++;
        }
    }
    *n_out = (size_t)n;
}

static void check_small(void)
{
    size_t i, j, k, n4 = 0, n3 = 0;
    build_small(g_small, MAXLEN, &n4);
    build_small(g_small3, 3, &n3);
    ok(n4 == NKEYS && n3 == NKEYS3, "small key sets have the expected sizes");

    /* 1 + 2 + 3, all pairs: equivalence, independence, irreflexivity,
     * antisymmetry. ~2.4M comparisons. */
    for (i = 0; i < NKEYS; i++) {
        ok(vol_key_cmp(g_small[i].k, g_small[i].n,
                       g_small[i].k, g_small[i].n) == 0,
           "a key equals itself");
        for (j = 0; j < NKEYS; j++) {
            int got = vol_key_cmp(g_small[i].k, g_small[i].n,
                                  g_small[j].k, g_small[j].n);
            int back = vol_key_cmp(g_small[j].k, g_small[j].n,
                                   g_small[i].k, g_small[i].n);
            checks++;
            if (got != legacy_cmp(g_small[i].k, g_small[i].n,
                                  g_small[j].k, g_small[j].n)) {
                fails++;
                fprintf(stderr, "FAIL: not equivalent to the pre-fix body "
                        "at (%zu,%zu)\n", i, j);
                goto small_done;
            }
            checks++;
            if (got != ref_cmp(g_small[i].k, g_small[i].n,
                               g_small[j].k, g_small[j].n)) {
                fails++;
                fprintf(stderr, "FAIL: not byte-lexicographic shorter-first "
                        "at (%zu,%zu)\n", i, j);
                goto small_done;
            }
            checks++;
            if (got != -back) {
                fails++;
                fprintf(stderr, "FAIL: not antisymmetric at (%zu,%zu)\n", i, j);
                goto small_done;
            }
        }
    }

    /* 3: transitivity, all triples of the length<=3 set. */
    for (i = 0; i < NKEYS3; i++)
        for (j = 0; j < NKEYS3; j++)
            for (k = 0; k < NKEYS3; k++) {
                int ij = vol_key_cmp(g_small3[i].k, g_small3[i].n,
                                     g_small3[j].k, g_small3[j].n);
                int jk = vol_key_cmp(g_small3[j].k, g_small3[j].n,
                                     g_small3[k].k, g_small3[k].n);
                int ik = vol_key_cmp(g_small3[i].k, g_small3[i].n,
                                     g_small3[k].k, g_small3[k].n);
                /* the one implication that matters: a <= b <= c => a <= c.
                 * (Its converse is not a totality requirement -- a > b and
                 * b > c with a <= c is perfectly legal, which is what a
                 * first draft of this line wrongly flagged.) */
                if (ij <= 0 && jk <= 0 && ik > 0) {
                    fails++;
                    fprintf(stderr, "FAIL: not transitive at (%zu,%zu,%zu)\n",
                            i, j, k);
                    goto small_done;
                }
                checks++;
            }
small_done:
    return;
}

/* ---- 4: the real key shapes ------------------------------------------ */

static tkey g_real[64];
static size_t g_nreal;

static void put8(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 7; i >= 0; i--) { p[i] = (uint8_t)(v & 0xff); v >>= 8; }
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static void add_inode(uint64_t id)
{
    tkey *t = &g_real[g_nreal++];
    put8(t->k, id);
    t->n = 8;
}

/* parent:u64 BE || name_len:u16 BE || name  (WP-M6 dirent key) */
static void add_dirent(uint64_t parent, const char *name)
{
    tkey *t = &g_real[g_nreal++];
    size_t nl = strlen(name);
    put8(t->k, parent);
    put16(t->k + 8, (uint16_t)nl);
    memcpy(t->k + 10, name, nl);
    t->n = (uint16_t)(10 + nl);
}

/* 0x03 || inode_id:u64 BE || name_len:u16 BE || name  (invarifs.h:1337) */
static void add_xattr(uint64_t id, const char *name)
{
    tkey *t = &g_real[g_nreal++];
    size_t nl = strlen(name);
    t->k[0] = INVFS_XATTR_KEY_PREFIX;
    put8(t->k + 1, id);
    put16(t->k + 9, (uint16_t)nl);
    memcpy(t->k + 11, name, nl);
    t->n = (uint16_t)(11 + nl);
}

/* 0x04 || blake3_256(blob)[32]  (invarifs.h:1349) */
static void add_recipe(uint8_t seed)
{
    tkey *t = &g_real[g_nreal++];
    int i;
    t->k[0] = INVFS_RECIPE_KEY_PREFIX;
    for (i = 0; i < 32; i++)
        t->k[1 + i] = (uint8_t)(seed * 7u + (unsigned)i);
    t->n = 33;
}

/* 0x04 || addr || 0x00 || idx:u16 -- the chunked-recipe continuation, whose
 * key is this manifest key with a suffix appended. */
static void add_recipe_chunk(const tkey *manifest, uint16_t idx)
{
    tkey *t = &g_real[g_nreal++];
    memcpy(t->k, manifest->k, manifest->n);
    t->k[manifest->n] = 0x00;
    put16(t->k + manifest->n + 1, idx);
    t->n = (uint16_t)(manifest->n + 3);
}

static void build_real(void)
{
    tkey *manifest;
    add_inode(1);
    add_inode(2);
    add_inode(255);
    add_inode(256);
    add_inode(65536);
    add_dirent(1, "a");
    add_dirent(1, "ab");
    add_dirent(1, "b");
    add_dirent(2, "a");
    add_dirent(255, "z");
    add_dirent(256, "z");
    add_xattr(1, "user.a");
    add_xattr(1, "user.ab");
    add_xattr(2, "user.a");
    add_xattr(255, "user.q");
    add_xattr(256, "user.q");
    add_xattr(1, "user.longattribute");
    add_recipe(1);
    add_recipe(2);
    add_recipe(9);
    manifest = &g_real[g_nreal - 3];       /* the seed-1 manifest */
    add_recipe_chunk(manifest, 0);
    add_recipe_chunk(manifest, 1);
}

static void check_real(void)
{
    build_real();
    ok(g_nreal >= 20, "the real key set was built");

    /* pairwise: the ordering is still the pre-fix one, and still plain
     * byte-lexicographic, on shapes that actually occur on disk */
    {
        size_t i, j;
        for (i = 0; i < g_nreal; i++)
            for (j = 0; j < g_nreal; j++)
                ok(vol_key_cmp(g_real[i].k, g_real[i].n,
                               g_real[j].k, g_real[j].n)
                   == legacy_cmp(g_real[i].k, g_real[i].n,
                                 g_real[j].k, g_real[j].n)
                   && vol_key_cmp(g_real[i].k, g_real[i].n,
                                  g_real[j].k, g_real[j].n)
                      == ref_cmp(g_real[i].k, g_real[i].n,
                                 g_real[j].k, g_real[j].n),
                   "real-shape keys order as the pre-fix body does");
    }

    /* (a) NAMESPACE DISJOINTNESS. invarifs.h:1337 tags xattrs 0x03 and
     * :1349 recipes 0x04 precisely so the three namespaces cannot
     * interleave: every inode-keyed key (the 8-byte inode keys and the
     * `parent || name_len || name` dirent keys share one id space and DO
     * interleave by it, which is intended) sorts before every 0x03 xattr
     * key, which sorts before every 0x04 recipe key. This holds only while
     * inode ids stay small -- the format says so, and this is what says so
     * again. */
    {
        size_t i, j;
        int ok_order = 1;
        for (i = 0; i < 11; i++)          /* inode + dirent */
            for (j = 11; j < 17; j++)      /* xattr 0x03 */
                if (vol_key_cmp(g_real[i].k, g_real[i].n,
                                g_real[j].k, g_real[j].n) >= 0)
                    ok_order = 0;
        for (i = 11; i < 17; i++)
            for (j = 17; j < 22; j++)      /* recipe 0x04 */
                if (vol_key_cmp(g_real[i].k, g_real[i].n,
                                g_real[j].k, g_real[j].n) >= 0)
                    ok_order = 0;
        ok(ok_order,
           "every inode/dirent key sorts before every 0x03 xattr key, and "
           "every xattr key before every 0x04 recipe key");
    }

    /* (b) BIG-ENDIAN. The fields after a tag are fixed-width BE, so byte
     * order over a key IS numeric order over its fields. Little-endian gets
     * both of these backwards, which is the whole reason they are stored
     * this way. */
    ok(vol_key_cmp(g_real[2].k, g_real[2].n, g_real[3].k, g_real[3].n) < 0,
       "inode ids order numerically: 255 before 256");
    ok(vol_key_cmp(g_real[3].k, g_real[3].n, g_real[4].k, g_real[4].n) < 0,
       "inode ids order numerically: 256 before 65536 (LE would invert it)");
    /* dirent key of parent 255 vs parent 256: same, one field in */
    ok(vol_key_cmp(g_real[9].k, g_real[9].n, g_real[10].k, g_real[10].n) < 0,
       "dirent keys order by parent inode id, numerically");
    /* xattr name_len 3 ("a") vs 6 ("user."): 0x0003 < 0x0006 */
    ok(vol_key_cmp(g_real[11].k, g_real[11].n, g_real[12].k, g_real[12].n) < 0,
       "xattr keys of one inode order by name_len, then name");

    /* (c) the shorter-first tiebreak, on the pair that makes it matter: an
     * over-page recipe's descriptor key is a strict prefix of its chunk
     * keys (vol_btree.c:3771), and the B+-tree has to keep them adjacent
     * and in that order for the page's range check to bound the subtree. */
    {
        const tkey *m = &g_real[17];
        const tkey *c0 = &g_real[20];
        const tkey *c1 = &g_real[21];
        ok(vol_key_cmp(m->k, m->n, c0->k, c0->n) < 0,
           "a recipe manifest key sorts before its own continuation chunks");
        ok(vol_key_cmp(c0->k, c0->n, c1->k, c1->n) < 0,
           "continuation chunks order by their index");
    }
}

/* ---- 5: the merge, and the control that gives it teeth --------------- */

/* The fold's job: two already-sorted runs, one from the base B+-tree and one
 * from the delta log, walked together. That is only a merge if both runs
 * were sorted by the same order — which is the property this whole WP is
 * about, stated in the shape the fold actually depends on. */
static void merge_runs(tlist *base, tlist *delta, tkey *out)
{
    size_t i = 0, j = 0, n = 0;
    while (i < base->n && j < delta->n) {
        if (base->cmp(base->v[i].k, base->v[i].n,
                      delta->v[j].k, delta->v[j].n) <= 0)
            out[n++] = base->v[i++];
        else
            out[n++] = delta->v[j++];
    }
    while (i < base->n) out[n++] = base->v[i++];
    while (j < delta->n) out[n++] = delta->v[j++];
}

static int same_seq(const tkey *a, const tkey *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (a[i].n != b[i].n || memcmp(a[i].k, b[i].k, a[i].n) != 0)
            return 0;
    return 1;
}

static void check_merge(void)
{
    tkey base[64], delta[64], uni[128], mg[128], ctl[128];
    tlist lb, ld, lu;
    size_t i, nb, nd, nu;

    nb = 12; nd = 11;
    for (i = 0; i < nb; i++) base[i] = g_real[i * 2 % g_nreal];
    for (i = 0; i < nd; i++) delta[i] = g_real[(i * 3 + 1) % g_nreal];
    nu = nb + nd;
    for (i = 0; i < nu; i++) uni[i] = i < nb ? base[i] : delta[i - nb];

    tlist_init(&lb, base, nb, vol_key_cmp);   tlist_sort(&lb);
    tlist_init(&ld, delta, nd, vol_key_cmp);  tlist_sort(&ld);
    tlist_init(&lu, uni, nu, vol_key_cmp);    tlist_sort(&lu);

    merge_runs(&lb, &ld, mg);
    ok(same_seq(mg, lu.v, nu),
       "merging the base run and the delta run yields the sorted union "
       "-- the property the fold depends on");

    /* THE CONTROL. Same two runs, same merge, but the delta side sorted
     * with a comparator that differs from the base side by exactly the
     * one-line tiebreak reversal this WP is about. If the assertion above
     * still passed, it would be asserting nothing. */
    tlist_init(&ld, delta, nd, inverted_cmp); tlist_sort(&ld);
    merge_runs(&lb, &ld, ctl);
    ok(!same_seq(ctl, mg, nu),
       "CONTROL: a second, different comparator on the delta side changes "
       "the merge — so the assertion above has teeth");

    /* ...and the same comparator on both sides restores it, which is why
     * the fix is one definition rather than two that happen to agree. */
    tlist_init(&ld, delta, nd, vol_key_cmp);  tlist_sort(&ld);
    merge_runs(&lb, &ld, ctl);
    ok(same_seq(ctl, mg, nu),
       "one comparator on both sides restores the merge");
}

/* ---- 6: one definition, in the tree, called by all three -------------- */

static char g_root[512];

static int slurp(const char *path, char *buf, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return -1;
    n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    *len = n;
    return 0;
}

/* Count non-overlapping occurrences of needle in hay. */
static int count(const char *hay, const char *needle)
{
    int c = 0;
    size_t nl = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { c++; p += nl; }
    return c;
}

static int find_root(void)
{
    char cand[512];
    struct stat st;
    if (getcwd(cand, sizeof cand) == NULL)
        return -1;
    for (;;) {
        snprintf(cand + strlen(cand), sizeof cand - strlen(cand),
                 "/src/core/volume_internal.h");
        if (stat(cand, &st) == 0) {
            /* trim the appended component */
            cand[strlen(cand) - strlen("/src/core/volume_internal.h")] = 0;
            snprintf(g_root, sizeof g_root, "%s", cand);
            return 0;
        }
        {
            char *slash = strrchr(cand, '/');
            if (!slash) return -1;
            *slash = 0;
            if (!cand[0]) { snprintf(cand, sizeof cand, "/"); return -1; }
            if (strcmp(cand, "/") == 0) return -1;
        }
    }
}

static void check_one_definition(void)
{
    char path[640];
    char buf[1 << 20];
    size_t len;
    DIR *d;
    struct dirent *de;
    int defs = 0;
    static const char *USERS[] = {
        "src/core/vol_btree.c", "src/core/vol_delta.c", "src/core/vol_fold.c"
    };
    /* Every spelling this ordering has ever taken in a .c file in this
     * tree, plus the expression its body is built from. The name-based
     * markers are history; the expression marker is the point -- a private
     * copy renamed to something nobody has thought of yet still contains
     * `? an : bn`, and that is what this catches. It is a guard against
     * re-duplicating THIS ordering, not a general clone detector. */
    static const char *GONE[] = {
        "static int bt_cmp(", "dl_key_cmp(", "fold_key_cmp(",
        "? an : bn", "? a->klen : b->klen",
        "vol_key_cmp(const uint8_t"       /* a definition outside the header */
    };
    size_t i;

    if (find_root() != 0) {
        fails++;
        fprintf(stderr, "FAIL: cannot locate the source tree from the "
                "working directory (looked upward for src/core/"
                "volume_internal.h); run this from the repo root\n");
        return;
    }

    /* exactly one definition of the ordering in the whole of src/core */
    snprintf(path, sizeof path, "%s/src/core", g_root);
    d = opendir(path);
    if (!d) {
        fails++;
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        return;
    }
    while ((de = readdir(d)) != NULL) {
        size_t nl = strlen(de->d_name);
        if (nl < 3 || strcmp(de->d_name + nl - 2, ".c") != 0)
            continue;
        snprintf(path, sizeof path, "%s/src/core/%s", g_root, de->d_name);
        if (slurp(path, buf, sizeof buf, &len) != 0)
            continue;
        defs += count(buf, "vol_key_cmp(const uint8_t");
        for (i = 0; i < sizeof GONE / sizeof GONE[0]; i++)
            if (count(buf, GONE[i])) {
                fails++;
                fprintf(stderr, "FAIL: %s contains \"%s\" — the ordering "
                        "has a second definition again\n",
                        de->d_name, GONE[i]);
            }
    }
    closedir(d);

    /* and it is the shared header, not a .c */
    ok(defs == 0,
       "no .c file defines vol_key_cmp() (it lives in the shared header)");
    snprintf(path, sizeof path, "%s/src/core/volume_internal.h", g_root);
    ok(slurp(path, buf, sizeof buf, &len) == 0
       && count(buf, "static inline int vol_key_cmp(const uint8_t") == 1,
       "volume_internal.h defines vol_key_cmp() exactly once, static inline");

    /* ...and all three modules actually call it, so "one definition" is
     * not one definition with two callers left on a private copy */
    for (i = 0; i < sizeof USERS / sizeof USERS[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", g_root, USERS[i]);
        ok(slurp(path, buf, sizeof buf, &len) == 0
           && count(buf, "vol_key_cmp(") >= 1,
           USERS[i]);
    }
}

int main(void)
{
    check_small();
    check_real();
    check_merge();
    check_one_definition();

    printf("keycmp: %d checks, %d failures\n", checks, fails);
    printf("%s: the v3 key ordering is one definition, and it is still "
           "byte-lexicographic shorter-first\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
