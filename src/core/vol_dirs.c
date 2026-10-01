/* vol_dirs.c — virtual directories (prefix-based), rename/move,
 * hard links, replace/unlink. Split from volume.c. */

#include "volume_internal.h"
#include "vol_fault.h"

/* Test-only door (src/core/vol_fault.h): the arming state of THIS
 * translation unit, which a test in another one cannot reach by hand --
 * unsetenv+setenv does not re-arm, because the freed string's address is
 * usually handed straight back and the pointer compare sees no change.
 * Same role as invfs_vol_btree_fault_reload(), in the file that owns the
 * walk fault sites below. */
void invfs_vol_dirs_fault_reload(void)
{
    invfs_vol_fault_reload();
}


/* ---- virtual directories (prefix-based; mkdir creates an empty anchor
   file named "dir/"; nested files "dir/x" make dir visible) ---- */

/* does a directory exist? anchor "name/" or any file under "name/" */
int vol_is_dir(invfs_volume *v, const char *name)
{
    return vol_v3_path_is_dir(v, name);
}


/* create empty directory anchor "name/" */
uint64_t vol_mkdir(invfs_volume *v, const char *name)
{
    return (v->sb.vol_flags & VOLF_READONLY) ? 0 : vol_v3_mkdir(v, name);
}


/* remove directory: must be empty (only the anchor, no files inside) */
int vol_rmdir(invfs_volume *v, const char *name)
{
    return vol_v3_rmdir(v, name);
}


/* auto-create parent anchors for a path: "a/b/c.txt" -> "a/", "a/b/" */
int vol_ensure_path(invfs_volume *v, const char *name)
{
    return vol_v3_ensure_path(v, name);
}


static int dirent_cmp(const void *a, const void *b)
{
    return strcmp(((const invfs_dirent *)a)->name,
                  ((const invfs_dirent *)b)->name);
}


/* ================================================================== */
/* WP-M6: metadata-v3 namespace (dirent-tree path layer)              */
/*                                                                    */
/* A v3 volume has no v2 name index and no anchor records: the name   */
/* lives in the base B+-tree's dirent keys (see vol_btree.c). These   */
/* helpers resolve mount-relative paths componentwise and back the    */
/* public vol_* namespace API so FUSE keeps calling by name.          */
/*                                                                    */
/* The root directory is INVFS_V3_ROOT_INO. A path component is       */
/* looked up as (parent inode, name) -> child inode; a directory's    */
/* own anchor (name_len 0) marks it as a directory. Mutations COW the */
/* base tree and publish the root (WP-M2 double slot); there is no    */
/* delta yet (WP-M7).                                                 */
/* ================================================================== */

static const char *v3_skip_slash(const char *p)
{
    if (!p)
        return "";
    while (*p == '/')
        p++;
    return p;
}

/* Split "a/b/c" into parent "a/b" and leaf "c" (leading/trailing slashes
 * ignored). parent may be empty (the root). 0 = ok, -1 = malformed. */
static int v3_split_path(const char *name, char *parent, size_t pcap,
                         char *leaf, size_t lcap)
{
    const char *p = v3_skip_slash(name);
    size_t n = strlen(p), k, pl, ll;

    while (n > 0 && p[n - 1] == '/')
        n--;
    if (n == 0)
        return -1;                        /* no leaf */
    k = n;
    while (k > 0 && p[k - 1] != '/')
        k--;
    ll = n - k;
    if (ll == 0 || ll >= lcap)
        return -1;
    memcpy(leaf, p + k, ll);
    leaf[ll] = 0;
    pl = k;
    while (pl > 0 && p[pl - 1] == '/')
        pl--;
    if (pl >= pcap)
        return -1;
    memcpy(parent, p, pl);
    parent[pl] = 0;
    return 0;
}

int vol_v3_path_lookup(invfs_volume *v, const char *name, uint64_t *ino_out)
{
    const char *p = v3_skip_slash(name);
    uint64_t cur = INVFS_V3_ROOT_INO;
    char comp[INVFS_MAX_NAME + 1];

    if (!v || !ino_out)
        return -1;
    while (*p) {
        const char *s = strchr(p, '/');
        size_t n = s ? (size_t)(s - p) : strlen(p);
        uint64_t child = 0;
        int rc;

        if (n == 0) { p = s ? s + 1 : p + n; continue; }
        if (n > INVFS_MAX_NAME)
            return -1;
        memcpy(comp, p, n);
        comp[n] = 0;
        rc = vol_v3_dirent_get(v, cur, comp, &child);
        if (rc != 1)
            return rc;                    /* 0 absent, -1 error */
        if (!child)
            return 0;                     /* dangling dirent */
        cur = child;
        if (!s)
            break;
        p = s + 1;
    }
    *ino_out = cur;
    return 1;
}

int vol_v3_path_is_dir(invfs_volume *v, const char *name)
{
    uint64_t ino;
    invfs_v3_inode in;

    if (!v)
        return 0;
    if (v3_skip_slash(name)[0] == 0)
        return 1;                         /* root always exists */
    if (vol_v3_path_lookup(v, name, &ino) != 1)
        return 0;
    if (vol_v3_inode_get(v, ino, &in) != 1)
        return 0;
    return in.type == INVFS_ITYP_DIR;
}

int vol_v3_path_stat(invfs_volume *v, const char *name, uint64_t *id_out,
                     uint64_t *size_out, uint64_t *ctime_out)
{
    uint64_t ino;
    invfs_v3_inode in;

    if (!v)
        return -1;
    if (vol_v3_path_lookup(v, name, &ino) != 1)
        return -1;
    if (vol_v3_inode_get(v, ino, &in) != 1)
        return -1;
    if (id_out)
        *id_out = ino;
    if (size_out)
        *size_out = in.size;
    if (ctime_out)
        *ctime_out = (uint64_t)in.mtime;  /* row has no ctime (WP-M5) */
    return 0;
}

typedef struct {
    invfs_volume *v;
    invfs_dirent *ents;
    int max, n;
} v3_list_ctx;

static int v3_list_cb(void *ctx_, const char *nm, size_t nlen, uint64_t child)
{
    v3_list_ctx *c = (v3_list_ctx *)ctx_;
    invfs_v3_inode in;
    int irc;

    if (c->n >= c->max)
        return 1;                         /* full: stop the scan */
    if (nlen && (unsigned char)nm[0] == 0x01)
        return 0;                         /* internal owner/registry */
    if (nlen >= sizeof c->ents[0].name)
        return 0;
    irc = vol_v3_inode_get(c->v, child, &in);
    /* WP86: -1 is EIO (a quarantined or unreadable inode row), not a dangling
     * dirent. Skipping it silently would hand readdir a shorter listing that
     * looks complete -- the file would simply be gone. Abort the scan so the
     * caller reports the error; a name is never invented and never hidden. */
    if (irc < 0)
        return -1;
    if (irc == 0)
        return 0;                         /* dangling dirent: skip */
    memcpy(c->ents[c->n].name, nm, nlen);
    c->ents[c->n].name[nlen] = 0;
    c->ents[c->n].is_dir = (in.type == INVFS_ITYP_DIR);
    c->ents[c->n].size = c->ents[c->n].is_dir ? 0 : in.size;
    c->ents[c->n].ctime = (uint64_t)in.mtime;
    c->n++;
    return 0;
}

int vol_v3_path_list_dir(invfs_volume *v, const char *dir,
                         invfs_dirent *ents, int max)
{
    uint64_t pino;
    v3_list_ctx c;
    int rc;

    if (!v || !ents || max <= 0)
        return 0;
    if (v3_skip_slash(dir)[0] == 0) {
        pino = INVFS_V3_ROOT_INO;
    } else {
        /* WP135: test-only seam for the case the `else` below used to turn
         * into an empty directory -- the directory's OWN dirent sits in an
         * unreadable range. Unset in production; one getenv and one pointer
         * compare. Returns the same errno the real lookup failure would. */
        if (invfs_vol_fault("path_list_dir_lookup"))
            return -EIO;
        /* WP135: `!= 1` collapsed two different volume states into one
         * answer. vol_v3_path_lookup returns 1 found, 0 absent and -1
         * ERROR, and 0 was returned for both of the others -- so a
         * directory whose OWN dirent sits in a quarantined range (the
         * name lookup EIOs) was reported as an EMPTY DIRECTORY, with rc 0,
         * while damage one level down -- in the children range -- has
         * always produced -EIO at :224 below. The same volume therefore
         * answered `ls` two different ways depending on which range was
         * unreadable, and the first way is indistinguishable from a
         * directory that genuinely has no entries.
         *
         * 0 stays 0: a name that resolves to nothing is not this function's
         * error to invent (a dirent can be unlinked between the lookup and
         * the listing, and the caller's ENOENT is the honest answer there).
         * Only a lookup that FAILED becomes -EIO. */
        int lrc = vol_v3_path_lookup(v, dir, &pino);
        if (lrc < 0)
            return -EIO;
        if (lrc == 0)
            return 0;
    }
    c.v = v;
    c.ents = ents;
    c.max = max;
    c.n = 0;
    rc = vol_v3_dirent_scan(v, pino, v3_list_cb, &c);
    if (rc != 0 && rc != 1)
        return -EIO;
    /* the tree orders by (name_len, name); v2 listings are name-sorted and
     * the FUSE readdir caller expects that, so sort here. */
    if (c.n > 1)
        qsort(ents, (size_t)c.n, sizeof *ents, dirent_cmp);
    return c.n;
}

/* Create (or replace as an empty node) `name`, putting the inode row before
 * the dirent (a dirent must never point at a missing inode). `meta` may be
 * NULL (REG defaults). Returns the inode id, 0 on failure. Data payloads are
 * WP-M8 (recipes), so a v3 node created here is always empty.
 *
 * WP135: NO '!' CHECK HERE, DELIBERATELY. This is not the user-name boundary:
 * the containerpack lane creates its member inodes through vol_create_file
 * -> here, under names it mints itself ("x.zip!mbr0000-a.txt",
 * vol_cpack.c:3622 :3741 :3843 :3956). A check at this level refuses the
 * lane's own children and the container never decomposes -- measured by
 * tools/test-p7z-batch.sh, which went red with "0 member siblings" the first
 * time it was tried here. The reservation belongs at the places a USER name
 * enters: vol_replace_file, vol_replace_file_with_meta,
 * vol_create_file_with_meta, vol_create_symlink, vol_create_special,
 * vol_v3_mkdir, vol_v3_rename, vol_v3_hardlink, vol_write_begin, plus
 * invf-cp's vol_create_file call and the six FUSE name-introducing ops
 * (fuse_fs.c, fuse_reserved_name). */
uint64_t vol_v3_create_node(invfs_volume *v, const char *name,
                            const invfs_meta_pub *meta)
{
    char parent[600], leaf[INVFS_MAX_NAME + 1];
    uint8_t old_addr[INVFS_V3_RECIPE_ADDR_LEN];
    uint64_t pino, id, existing = 0;
    invfs_v3_inode in;
    int rc, had_content = 0;

    if (!v)
        return 0;
    if (v->sb.vol_flags & VOLF_READONLY)
        return 0;
    if (v3_split_path(name, parent, sizeof parent, leaf, sizeof leaf) != 0)
        return 0;
    if (vol_v3_path_lookup(v, parent, &pino) != 1)
        return 0;
    if (!vol_v3_path_is_dir(v, parent))
        return 0;
    memset(old_addr, 0, sizeof old_addr);
    rc = vol_v3_dirent_get(v, pino, leaf, &existing);
    if (rc < 0)
        return 0;
    if (rc == 1) {
        id = existing;
        if (vol_v3_inode_get(v, id, &in) != 1)
            memset(&in, 0, sizeof in);
        else if (in.type == INVFS_ITYP_REG && in.size > 0) {
            /* CAPTURE the superseded recipe; the free waits for the row that
             * supersedes it to be durable. `in.type` is read here, BEFORE
             * `meta` can overwrite it below -- a REG replaced by a symlink
             * still has data blocks to release. */
            memcpy(old_addr, in.recipe_addr, sizeof old_addr);
            had_content = 1;
        }
    } else {
        id = vol_v3_inode_alloc(v);
        if (!id)
            return 0;
        memset(&in, 0, sizeof in);
    }
    if (meta) {
        in.type = meta->type;
        in.mode = meta->mode;
        in.uid = meta->uid;
        in.gid = meta->gid;
        in.mtime = meta->mtime;
        in.atime = meta->atime;
        in.nlink = meta->nlink ? meta->nlink : 1;
        in.rdev = meta->rdev;
    } else if (in.type == 0) {            /* 0 == INVFS_ITYP_REG */
        in.type = INVFS_ITYP_REG;
        in.mode = 0644;
        in.nlink = 1;
        in.mtime = in.atime = (int64_t)time(NULL);
    }
    if (in.nlink == 0)
        in.nlink = 1;
    if (in.type == INVFS_ITYP_LNK && meta && meta->target[0]) {
        size_t tl = strlen(meta->target);
        if (tl >= INVFS_META_TARGET_MAX)
            return 0;
        if (vol_v3_recipe_store(v, (const uint8_t *)meta->target, tl, in.recipe_addr) != 0)
            return 0;
        in.size = tl;
        memset(&in.recipe, 0, sizeof in.recipe);
    } else {
        in.size = 0;                          /* WP-M8: content cleared */
        memset(&in.recipe, 0, sizeof in.recipe);
        memset(in.recipe_addr, 0, sizeof in.recipe_addr);
    }
    if (vol_v3_inode_delta_put(v, id, &in) != 0)
        return 0;
    /* The supersede is now durable in the delta log and the live row names
     * the NEW recipe, so the old blocks are unreachable by anything that can
     * still be read -- free them now, and only now. Mirrors vol_v3_unlink:
     * the row goes first, the blocks second.
     *
     * Deferring costs nothing here. The row this publishes names NO recipe
     * for a REG/DIR replacement (recipe_addr is memset to zero, and a zero
     * address names no blocks -- vol_v3_free_recipe_blocks, vol_ast.c:151),
     * so nothing live points into the old recipe when the free runs; the
     * recipe BLOB is a base-tree value under 0x04 || BLAKE3(blob), reclaimed
     * by the fold, not here. Where a recipe IS published (a symlink target)
     * the address is BLAKE3 of the blob, so identical content recurs at the
     * SAME address -- which is exactly why the free is guarded on the row
     * having MOVED. That is the guard vol_v3_release_superseded_blob already
     * carries, and it is load-bearing twice over: vol_v3_inode_delta_put only
     * invalidates the pba map when recipe_addr changes, so freeing an
     * address the row still names would decrement counts the map attributes
     * to nothing. */
    if (had_content &&
        memcmp(in.recipe_addr, old_addr, INVFS_V3_RECIPE_ADDR_LEN) != 0)
        vol_v3_free_recipe_blocks(v, old_addr, 0);
    if (vol_v3_dirent_delta_put(v, pino, leaf, id) != 0)
        return 0;
    if (in.type == INVFS_ITYP_DIR)
        vol_v3_dirent_delta_put(v, id, "", id);  /* directory anchor */
    return id;
}

/* WP-M9: create (or replace) `name` with a row that already carries its
 * content address and logical size, then insert the dirent. The write
 * commit publishes the inode row BEFORE the dirent (design §4 durability
 * ordering), so a crash between the two leaves an orphaned row, never a
 * dirent that names an empty or partial inode. Returns the inode id, or 0
 * with the volume untouched. */
uint64_t vol_v3_create_content_node(invfs_volume *v, const char *name,
                                    uint64_t size,
                                    const uint8_t recipe_addr[INVFS_V3_RECIPE_ADDR_LEN])
{
    char parent[600], leaf[INVFS_MAX_NAME + 1];
    uint64_t pino, id, existing = 0;
    invfs_v3_inode in;
    int rc;

    if (!v || !recipe_addr)
        return 0;
    if (v->sb.vol_flags & VOLF_READONLY)
        return 0;
    if (v3_split_path(name, parent, sizeof parent, leaf, sizeof leaf) != 0)
        return 0;
    if (vol_v3_path_lookup(v, parent, &pino) != 1)
        return 0;
    if (!vol_v3_path_is_dir(v, parent))
        return 0;
    rc = vol_v3_dirent_get(v, pino, leaf, &existing);
    if (rc < 0)
        return 0;
    if (rc == 1) {
        id = existing;
        if (vol_v3_inode_get(v, id, &in) != 1)
            memset(&in, 0, sizeof in);
    } else {
        id = vol_v3_inode_alloc(v);
        if (!id)
            return 0;
        memset(&in, 0, sizeof in);
    }
    if (in.type == 0)
        in.type = INVFS_ITYP_REG;
    if (in.mode == 0)
        in.mode = 0644;
    if (in.nlink == 0)
        in.nlink = 1;
    in.mtime = in.atime = (int64_t)time(NULL);
    in.size = size;
    memset(&in.recipe, 0, sizeof in.recipe);
    memcpy(in.recipe_addr, recipe_addr, INVFS_V3_RECIPE_ADDR_LEN);
    /* row first, dirent second: a torn create is an orphan, not a dangling
     * name. An already-present dirent is left in place (replace-in-place). */
    if (vol_v3_inode_delta_put(v, id, &in) != 0)
        return 0;
    if (rc != 1 && vol_v3_dirent_delta_put(v, pino, leaf, id) != 0)
        return 0;
    return id;
}

uint64_t vol_v3_set_meta(invfs_volume *v, const char *name,
                         const invfs_meta_pub *meta)
{
    uint64_t id;
    invfs_v3_inode in;

    if (!v || !meta)
        return 0;
    if (v->sb.vol_flags & VOLF_READONLY)
        return 0;
    if (vol_v3_path_lookup(v, name, &id) != 1)
        return 0;
    if (vol_v3_inode_get(v, id, &in) != 1)
        return 0;
    if (meta->type)
        in.type = meta->type;
    in.mode = meta->mode;
    in.uid = meta->uid;
    in.gid = meta->gid;
    if (meta->mtime)
        in.mtime = meta->mtime;
    if (meta->atime)
        in.atime = meta->atime;
    if (meta->nlink)
        in.nlink = meta->nlink;
    in.rdev = meta->rdev;
    if (in.type == INVFS_ITYP_LNK && meta->target[0]) {
        size_t tl = strlen(meta->target);
        if (tl >= INVFS_META_TARGET_MAX)
            return 0;
        uint8_t addr[INVFS_V3_RECIPE_ADDR_LEN];
        if (vol_v3_recipe_store(v, (const uint8_t *)meta->target, tl, addr) != 0)
            return 0;
        memcpy(in.recipe_addr, addr, INVFS_V3_RECIPE_ADDR_LEN);
        in.size = tl;
    }
    if (vol_v3_inode_delta_put(v, id, &in) != 0)
        return 0;
    if (in.type == INVFS_ITYP_DIR)
        vol_v3_dirent_delta_put(v, id, "", id);  /* ensure the anchor */
    return id;
}

uint64_t vol_v3_mkdir(invfs_volume *v, const char *name)
{
    char parent[600], leaf[INVFS_MAX_NAME + 1];
    uint64_t pino, id, existing = 0;
    invfs_v3_inode in;
    int rc;

    if (!v)
        return 0;
    if (v->sb.vol_flags & VOLF_READONLY)
        return 0;
    /* WP135: a directory name is a user name like any other, and '!' is
     * reserved for container siblings. See vol_v3_create_node above. */
    if (name_refused_internal_ns(name))
        return 0;
    if (v3_split_path(name, parent, sizeof parent, leaf, sizeof leaf) != 0)
        return 0;
    if (vol_v3_path_lookup(v, parent, &pino) != 1)
        return 0;
    if (!vol_v3_path_is_dir(v, parent))
        return 0;
    rc = vol_v3_dirent_get(v, pino, leaf, &existing);
    if (rc == 1)
        return 0;                         /* EEXIST */
    if (rc < 0)
        return 0;
    id = vol_v3_inode_alloc(v);
    if (!id)
        return 0;
    memset(&in, 0, sizeof in);
    in.type = INVFS_ITYP_DIR;
    in.mode = 0755;
    in.nlink = 2;
    in.mtime = in.atime = (int64_t)time(NULL);
    if (vol_v3_inode_delta_put(v, id, &in) != 0)
        return 0;
    if (vol_v3_dirent_delta_put(v, id, "", id) != 0)     /* anchor */
        return 0;
    if (vol_v3_dirent_delta_put(v, pino, leaf, id) != 0)
        return 0;
    return id;
}

static int v3_count_cb(void *ctx_, const char *nm, size_t nlen, uint64_t child)
{
    int *n = (int *)ctx_;
    (void)nm; (void)nlen; (void)child;
    (*n)++;
    return 0;
}

int vol_v3_rmdir(invfs_volume *v, const char *name)
{
    char parent[600], leaf[INVFS_MAX_NAME + 1];
    uint64_t id, pino;
    int n = 0, rc;

    if (!v)
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;
    if (v3_split_path(name, parent, sizeof parent, leaf, sizeof leaf) != 0)
        return -1;
    if (vol_v3_path_lookup(v, name, &id) != 1)
        return -1;
    if (!vol_v3_path_is_dir(v, name))
        return -1;
    rc = vol_v3_dirent_scan(v, id, v3_count_cb, &n);
    if (rc != 0 && rc != 1)
        return -1;
    if (n > 0)
        return -2;                        /* ENOTEMPTY */
    if (vol_v3_path_lookup(v, parent, &pino) != 1)
        return -1;
    if (vol_v3_dirent_delta_del(v, pino, leaf) != 0)
        return -1;
    if (vol_v3_dirent_delta_del(v, id, "") != 0)
        return -1;
    if (vol_v3_inode_delta_delete(v, id) != 0)
        return -1;
    return 0;
}

int vol_v3_unlink(invfs_volume *v, const char *name)
{
    char parent[600], leaf[INVFS_MAX_NAME + 1];
    uint64_t id, pino;
    invfs_v3_inode in;

    if (!v)
        return -1;
    if (v3_split_path(name, parent, sizeof parent, leaf, sizeof leaf) != 0)
        return -1;
    if (vol_v3_path_lookup(v, name, &id) != 1)
        return -1;
    if (vol_v3_inode_get(v, id, &in) != 1)
        return -1;
    if (in.type == INVFS_ITYP_DIR)
        return -1;                        /* EISDIR: use rmdir */
    if (vol_v3_path_lookup(v, parent, &pino) != 1)
        return -1;
    if (in.nlink <= 1) {
        int rc = 0;
        /* WP-M17 frozen transition (design §4): drop the dirent, then the
         * count; only nlink == 0 frees the row (and, via inode_delete, the
         * xattr keys). Name-first means a crash between the two leaves nlink
         * >= the live name count -- an inode may leak, but a live dirent can
         * never point at a freed row. That ordering is unchanged; what
         * changed is that the name now goes INSIDE the map's hold, and the
         * map is taken BEFORE it rather than after.
         *
         * WP unlink-takes-map-after-dirent-drop. This used to take the map
         * at the old :552, AFTER the dirent delete at the old :540, and the
         * comment there said the ensure had to happen "while this row still
         * names its recipe". That is the wrong invariant, and c47cf65 is
         * what says so: pba_ref_ensure's build reaches an inode THROUGH ITS
         * DIRENT (v3_walk_dir, below -- vol_v3_path_list_dir, then
         * vol_v3_path_lookup at :761-765), so once the dirent is gone the
         * walk cannot reach the row AT ALL, whatever the row still says. A
         * rebuild after the delete is short by exactly this row's
         * contribution:
         *
         *   two files share pba P     a correct map says 2
         *   the dirent is dropped     the walk can no longer see B
         *   pba_ref_ensure rebuilds   count(P) = 1
         *   pba_ref_modify(P, -1)     count(P) = 0
         *   vol_free_blocks(P)        P is freed while A still names it
         *
         * and the rebuild is ROUTINE, not exceptional: pba_ref_ensure is a
         * no-op unless the map is absent or pba_ref_stale (volume.c:3620),
         * and pba_ref_stale is set from the single place a recipe_addr is
         * published (vol_v3_inode_delta_put, vol_btree.c:3881/:3884). Any
         * recipe publish since the last build arms it, so writing one more
         * file is the whole trigger -- which is why this fires on a clean
         * volume with no injected fault.
         *
         * MOVING THE ENSURE IS NECESSARY BUT NOT SUFFICIENT, and the hold is
         * the other half. The ensure and the -1 are separated by the two
         * deletes and a recipe load, and the map has no lock of its own --
         * pba_ref_free and pba_ref_grow both free(v->pba_ref), so a second
         * writer is a use-after-free, not a lost update. FUSE hid that
         * behind g_io_lock (fuse_fs.c:32), a `static` in that translation
         * unit: the core cannot take it, the offline tools are not linked
         * against it, and invf-sweep runs its sweep on a worker thread
         * holding nothing. So the precondition was an accident of the mount,
         * and the offline paths were safe only by being single-writer.
         * vol_pba_ref_hold makes it structural -- and it is the DELETE that
         * has to be inside the hold, not merely the ensure, or a rebuild
         * landing between them rebuilds against a live set the pending -1
         * does not belong to (and with two sharers retiring at once the
         * second -1 floors at 0 and frees the block a second time). */
        vol_pba_ref_hold(v);
        /* the map, taken while the NAME is still there, so this recipe's own
         * contribution is in it and the -1 below has something to take */
        pba_ref_ensure(v);
        if (vol_v3_dirent_delta_del(v, pino, leaf) != 0) {
            vol_pba_ref_release(v);
            return -1;
        }
        if (vol_v3_inode_delta_delete(v, id) != 0) {
            vol_pba_ref_release(v);
            return -1;
        }
        /* WP-N1: targeted free of unlinked file data blocks.
         *
         * WP201: the ORDER is unchanged and must stay that way -- the row
         * goes first, the blocks second. Freeing first would let a failure
         * between the two leave a LIVE row pointing at freed blocks, which
         * is a bit-exactness violation and strictly worse than the leak
         * this reports. So by the time the free runs the name is already
         * gone and the unlink has happened; what is left to say is whether
         * the space came back.
         *
         * It may not: vol_v3_free_recipe_blocks returns -1 when the recipe
         * will not load or will not parse, having freed nothing. Returning
         * 0 there told the caller "gone and reclaimed" while every block the
         * recipe named stayed allocated under no reachable name -- forever.
         * Report it, and name the file, so the operator knows which reclaim
         * to chase. */
        if (in.type == INVFS_ITYP_REG &&
            vol_v3_free_recipe_blocks(v, in.recipe_addr, 0) != 0) {
            fprintf(stderr, "invarifs: unlink %s: recipe does not load or "
                    "parse, so its data blocks were NOT reclaimed; the name "
                    "is gone and the space is still allocated. Run "
                    "invf-fsck.\n", name);
            rc = -1;
        }
        /* Cascade delete container/transcode siblings if main file is
         * unlinked. Still inside the hold: this is a nested vol_v3_unlink
         * per sibling, and the hold is depth-counted over a RECURSIVE mutex,
         * so it nests correctly instead of unlocking early. */
        if (strchr(name, '!') == NULL)
            vol_delete_siblings(v, name);
        vol_pba_ref_release(v);
        return rc;
    } else {
        /* More than one name on this inode: this unlink retires a NAME, not
         * the inode, and frees nothing -- so it needs no map at all. The row
         * keeps naming its recipe and every surviving name still reaches it,
         * which is exactly the condition a map build requires. */
        if (vol_v3_dirent_delta_del(v, pino, leaf) != 0)
            return -1;
        in.nlink--;
        if (vol_v3_inode_delta_put(v, id, &in) != 0)
            return -1;
    }
    return 0;
}

int vol_v3_rename(invfs_volume *v, const char *from, const char *to)
{
    char fparent[600], fleaf[INVFS_MAX_NAME + 1];
    char tparent[600], tleaf[INVFS_MAX_NAME + 1];
    uint64_t f_id, f_pino, t_id = 0, t_pino;
    invfs_v3_inode f_in, t_in;
    const char *fn, *tn;
    int rc;

    if (!v || !from || !to || !from[0] || !to[0])
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;
    /* WP135: `to` INTRODUCES a name, so it is refused; `from` only reads
     * one, and a volume that already holds a '!' name must stay renameable
     * so its contents can be moved OFF the reserved namespace. */
    if (name_refused_internal_ns(to))
        return -1;
    if (v3_split_path(from, fparent, sizeof fparent, fleaf, sizeof fleaf) != 0)
        return -1;
    if (v3_split_path(to, tparent, sizeof tparent, tleaf, sizeof tleaf) != 0)
        return -1;
    if (vol_v3_path_lookup(v, from, &f_id) != 1)
        return -1;
    if (vol_v3_inode_get(v, f_id, &f_in) != 1)
        return -1;
    if (vol_v3_path_lookup(v, tparent, &t_pino) != 1)
        return -1;
    if (!vol_v3_path_is_dir(v, tparent))
        return -1;
    if (strcmp(fparent, tparent) == 0 && strcmp(fleaf, tleaf) == 0)
        return 0;                         /* same path: no-op */
    fn = v3_skip_slash(from);
    tn = v3_skip_slash(to);
    /* "a" -> "a/b": moving a directory inside itself */
    if (f_in.type == INVFS_ITYP_DIR) {
        size_t fl = strlen(fn);
        if (strncmp(tn, fn, fl) == 0 && tn[fl] == '/')
            return -1;
    }
    rc = vol_v3_dirent_get(v, t_pino, tleaf, &t_id);
    if (rc < 0)
        return -1;
    if (rc == 1) {
        if (vol_v3_inode_get(v, t_id, &t_in) != 1)
            return -1;
        if (t_in.type == INVFS_ITYP_DIR)
            return -2;                    /* EEXIST: dir destination */
        if (f_in.type == INVFS_ITYP_DIR)
            return -1;                    /* dir over file */
        /* the victim loses its name and a link (WP-M17: only the count
         * reaching 0 retires the row + its xattrs, so another name of a
         * hardlinked victim survives this overwrite).
         * TODO(WP-M17): POSIX says rename(old,new) where both names are
         * hardlinks to the SAME inode is a no-op; the doc is silent and
         * M6's rename mechanics would drop one name, so the pre-existing
         * (non-POSIX, non-lossy) behaviour is kept here. */
        if (t_in.nlink > 1) {
            /* A name, not the inode: frees nothing, so it needs no map. */
            if (vol_v3_dirent_delta_del(v, t_pino, tleaf) != 0)
                return -1;
            t_in.nlink--;
            if (vol_v3_inode_delta_put(v, t_id, &t_in) != 0)
                return -1;
        } else {
            /* WP unlink-takes-map-after-dirent-drop: THIS IS THE SAME DEFECT
             * AS vol_v3_unlink, and it was wrong in the same way.
             *
             * The ensure used to be taken here, AFTER
             * vol_v3_dirent_delta_del(v, t_pino, tleaf) had already dropped
             * the victim's name, and the comment said the ensure must happen
             * "while the row still names t_in.recipe_addr". Same wrong
             * invariant as the unlink: pba_ref_ensure's build reaches an
             * inode THROUGH ITS DIRENT (v3_walk_dir, below --
             * vol_v3_path_list_dir, then vol_v3_path_lookup), so once the
             * name is gone the walk cannot reach the row, whatever the row
             * still says. A rebuild here is short by exactly the
             * contribution the -1 below is about to take, and the block goes
             * out from under whoever else still names it.
             *
             * So: the map is taken while the name is still there, and the
             * name is dropped inside the same hold, exactly as in
             * vol_v3_unlink. The comma-operator
             * `pba_ref_ensure(v), vol_v3_inode_delta_delete(...)` is gone
             * because the ordering it encoded is the thing being fixed.
             */
            vol_pba_ref_hold(v);
            pba_ref_ensure(v);
            if (vol_v3_dirent_delta_del(v, t_pino, tleaf) != 0) {
                vol_pba_ref_release(v);
                return -1;
            }
            if (vol_v3_inode_delta_delete(v, t_id) != 0) {
                vol_pba_ref_release(v);
                return -1;
            }
            /* WP-N1: targeted free of overwritten destination data blocks */
            if (t_in.type == INVFS_ITYP_REG)
                vol_v3_free_recipe_blocks(v, t_in.recipe_addr, 0);
            vol_pba_ref_release(v);
        }
    }
    /* insert the new dirent BEFORE deleting the old (add-before-remove,
     * design §3): a crash between the two leaves the file under both names,
     * never under neither. */
    if (vol_v3_dirent_delta_put(v, t_pino, tleaf, f_id) != 0)
        return -1;
    if (vol_v3_path_lookup(v, fparent, &f_pino) != 1)
        return -1;
    if (vol_v3_dirent_delta_del(v, f_pino, fleaf) != 0)
        return -1;
    return 0;
}

int vol_v3_ensure_path(invfs_volume *v, const char *name)
{
    char tmp[600];
    size_t n, i;

    if (!v || !name)
        return -1;
    n = strlen(name);
    if (n >= sizeof tmp)
        return -1;
    memcpy(tmp, name, n + 1);
    for (i = 0; i < n; i++) {
        if (tmp[i] == '/') {
            char save = tmp[i];
            tmp[i] = 0;
            if (tmp[0] && !vol_v3_path_is_dir(v, tmp))
                vol_v3_mkdir(v, tmp);
            tmp[i] = save;
        }
    }
    return 0;
}

/* The shared live-inode walker.
 *
 * `strict` is the whole of the difference between this and a walker that
 * enumerates whatever it managed to read, and it exists for ONE caller
 * (vol_v3_walk_strict, below) whose answer must not be a partial view.
 *
 * A listing walk legitimately skips: its consumer wants a best-effort sweep,
 * and a row it cannot read is not something it can act on either way. The pba
 * reference map is not that. The map answers "how many LIVE recipes name this
 * block" and it is the sole gate on every data-block free -- so a row it
 * could not read is a live reference it does not hold, and a count that is
 * missing one frees a block somebody still names. There is no third answer:
 * the walk either saw every live inode or the caller must be told it did not.
 *
 * That is why this is a parameter and not a change to vol_v3_walk: the other
 * five callers (vol_btree.c:4854, :5099, :5566, vol_dedupe.c:448,
 * vol_records.c:194) each want a listing, and tightening the walk under them
 * would turn one damaged row into five unrelated failures.
 *
 * Returns 0 = every entry was visited, 1 = the callback asked to stop, -1 =
 * the walk could not be completed (a listing or a row read failed, or the
 * depth cap was hit). In strict mode a row read that fails is that last
 * case; out of it, the same read is a `continue`. */
static int v3_walk_dir(invfs_volume *v, const char *dir,
                       vol_v3_walk_cb cb, void *ctx, int depth, int strict)
{
    invfs_dirent *ents;
    int cap = 256, n, i, lrc, irc;

    if (depth > 64)
        return -1;
    /* WP135: test-only seam -- the walk itself stops (a quarantined base
     * page, an OOM in the callback). This is the state all three of the
     * v3_walk() callers failed to act on, and the one the red controls
     * need to reach without corrupting a tree. Unset in production. */
    if (invfs_vol_fault("v3_walk_dir_stop"))
        return -1;
    ents = (invfs_dirent *)malloc((size_t)cap * sizeof *ents);
    if (!ents)
        return -1;
    for (;;) {
        n = vol_v3_path_list_dir(v, dir, ents, cap);
        if (n < 0) { free(ents); return -1; }
        if (n < cap)
            break;
        cap *= 2;
        {
            void *ne = realloc(ents, (size_t)cap * sizeof *ents);
            if (!ne) { free(ents); return -1; }
            ents = (invfs_dirent *)ne;
        }
    }
    for (i = 0; i < n; i++) {
        char path[600];
        uint64_t ino;
        invfs_v3_inode in;

        int pr;
        if (dir[0])
            pr = snprintf(path, sizeof path, "%s/%s", dir, ents[i].name);
        else
            pr = snprintf(path, sizeof path, "%s", ents[i].name);
        if (pr < 0 || (size_t)pr >= sizeof path) {
            fprintf(stderr, "v3_walk_dir: path exceeds buffer capacity (%s/%s)\n",
                    dir, ents[i].name);
            if (strict) { free(ents); return -1; }
            continue;
        }
        /* Both of these distinguish three answers already -- found, absent,
         * could not read -- and the middle one is the only one a walk can
         * step over. A name whose inode could not be RESOLVED is an error
         * (vol_v3_path_lookup says so), and a row that could not be READ is
         * an error (vol_v3_inode_get says so, and volume.h is explicit that
         * a caller must not map its -1 to 0). In strict mode neither is a
         * `continue`.
         *
         * WP135: this branch is now the whole of the "which caller asked"
         * question, and the answer is that four of the six callers of a v3
         * walk still ask for the LENIENT one -- see vol_v3_walk_strict's
         * own comment, which names pba_ref_ensure as "the one caller whose
         * answer must not be a partial view of the namespace". A file list
         * for invf-verify, a name table for the mount, a live set for the
         * sweep and a reverse name lookup are all partial views too; they
         * are just partial views that get PRINTED rather than used to drop
         * a block reference, which is what made them look safe. WP135
         * moves those four to vol_v3_walk_strict.
         *
         * The fault seam below stands in for the failed RESOLUTION and then
         * defers to this same branch, so the red control measures the
         * lenient/lenient choice rather than a private copy of it. */
        lrc = invfs_vol_fault("v3_walk_dir_entry")
                  ? -1 : vol_v3_path_lookup(v, path, &ino);
        if (lrc != 1) {
            if (strict) { free(ents); return -1; }
            continue;
        }
        irc = invfs_vol_fault("v3_walk_dir_row")
                  ? -1 : vol_v3_inode_get(v, ino, &in);
        if (irc != 1) {
            if (strict) { free(ents); return -1; }
            continue;
        }
        if (cb && cb(ctx, path, ino, in.type, in.size, in.mtime) != 0) {
            free(ents);
            return 1;
        }
        if (in.type == INVFS_ITYP_DIR) {
            int sub_rc = v3_walk_dir(v, path, cb, ctx, depth + 1, strict);
            if (sub_rc != 0) {
                free(ents);
                return sub_rc;
            }
        }
    }
    free(ents);
    return 0;
}

int vol_v3_walk(invfs_volume *v, vol_v3_walk_cb cb, void *ctx)
{
    if (!v)
        return -1;
    return v3_walk_dir(v, "", cb, ctx, 0, 0);
}

/* The same walk, with every unreadable row reported instead of stepped over.
 * See the comment on v3_walk_dir for why this is a second entry point and
 * not a change to vol_v3_walk. One caller: pba_ref_ensure. */
int vol_v3_walk_strict(invfs_volume *v, vol_v3_walk_cb cb, void *ctx)
{
    if (!v)
        return -1;
    return v3_walk_dir(v, "", cb, ctx, 0, 1);
}


/* list one directory level: first path component after "dir/".
 *
 * Returns the entry count, 0 for a directory that is genuinely empty, or a
 * negative errno: -ENOMEM if the dedup set could not be allocated, -EIO if
 * the metadata scan failed. It never reports a partial listing as a count,
 * and it never reports a failure as 0 -- a caller that treated "0" as success
 * would hand FUSE an empty directory for one it could not read. */
int vol_list_dir(invfs_volume *v, const char *dir, invfs_dirent *ents, int max)
{
    /* Test-only seam (src/core/vol_fault.h), armed by INVFS_FAULT. It stands
     * in for the allocation the v2 listing did before it -- the one failure
     * on this path that cannot be arranged on purpose -- and returns the same
     * errno. Unset in production, where this costs one getenv and one pointer
     * compare. */
    if (invfs_vol_fault("vol_list_dir"))
        return -ENOMEM;

    /* The listing IS the dirent tree. The alternative was an O(n^2) dedup
     * over an inode-area scan whose source (the in-memory name index) had
     * been a no-op returning an empty set since WP-M21 -- so it reported
     * every directory as empty, which is the same "a failure reads as
     * nothing" shape this entry point is now built not to have. */
    return vol_v3_path_list_dir(v, dir, ents, max);
}


/* Replace `name` with `data`, or create it if absent.
 *
 * The write commit path used to delete by name and then create, which loses
 * the user's existing file outright when the create fails: the old record is
 * already tombstoned and the new bytes are gone with it. ENOSPC on an
 * overwrite is a normal condition, not a corner case -- the whole point of
 * this filesystem is that data survives.
 *
 * Append the new record first. The inode area is append-only and readers are
 * version-aware, so the newer record wins from the moment it lands; only then
 * is the old inode tombstoned. idx_del ignores a mismatched id, so the
 * tombstone cannot take the replacement's name down with it (the sweep has
 * relied on exactly this ordering all along).
 *
 * Returns the new inode id, or 0 with the volume untouched. */
uint64_t vol_replace_file(invfs_volume *v, const char *name,
                        const uint8_t *data, size_t len)
{
    /* WP-M6: a v3 namespace node is the dirent tree + inode row. Content
     * goes through the WP-M9 session path (WP-M21b glue).
     *
     * WP135: a USER-name boundary -- FUSE's invf_create (fuse_fs.c:1892) and
     * invf-cp (cp.c:100) are the callers -- so '!' is refused here. */
    if (name_refused_internal_ns(name)) return 0;
    return (data && len) ? vol_v3_write_bulk(v, name, data, len, NULL)
                         : vol_v3_create_node(v, name, NULL);
}


uint64_t vol_replace_file_with_meta(invfs_volume *v, const char *name,
                                    const uint8_t *data, size_t len,
                                    const invfs_meta_pub *meta)
{
    /* WP135: invf-import is the only caller (tools/invf-import.c:242), so
     * this is a user-name boundary; see vol_replace_file above. */
    if (name_refused_internal_ns(name)) return 0;
    return (data && len) ? vol_v3_write_bulk(v, name, data, len, meta)
                         : vol_v3_create_node(v, name, meta);
}


int vol_delete_file(invfs_volume *v, const char *name)
{
    return vol_v3_unlink(v, name);
}


/* Delete a file and the sibling records that belong to it. This is what an
   unlink means for a transcoded file; vol_delete_file alone strands them. */
/* remove ONE name of a multi-name (hardlinked) inode: tombstone only
 * this name's record; blocks stay alive for the surviving names.
 * nlink on surviving records is not rewritten (drift only delays block
 * retirement; fsck recount fixes it). */
int vol_unlink_name(invfs_volume *v, const char *name)
{
    return vol_v3_unlink(v, name);
}


int vol_unlink(invfs_volume *v, const char *name)
{
    return vol_v3_unlink(v, name);
}


/* ---- rename / move ---- */

int vol_rename(invfs_volume *v, const char *from, const char *to)
{
    return vol_v3_rename(v, from, to);
}


/* WP-M17: hard link on the v3 namespace. A second (parent, name) dirent
 * points at the SAME inode id; the shared row's nlink is incremented so
 * unlinking one name leaves the row (and its xattrs) for the survivors,
 * and only the last unlink frees it. No block refcounts: the data plane is
 * refcount-free reachability (WP-M15), which is why the nlink==0 gate is
 * the only inode-free condition. */
static int vol_v3_hardlink(invfs_volume *v, const char *from, const char *to)
{
    char tparent[600], tleaf[INVFS_MAX_NAME + 1];
    uint64_t id, t_pino = 0, existing = 0;
    invfs_v3_inode in;
    int rc;

    if (!v || !from || !to || !from[0] || !to[0])
        return -1;
    if (v->sb.vol_flags & VOLF_READONLY)
        return -1;                              /* EROFS */
    /* WP135: link(2) introduces `to`, so it is refused like a rename target. */
    if (name_refused_internal_ns(to))
        return -1;
    if (v3_split_path(to, tparent, sizeof tparent, tleaf, sizeof tleaf) != 0)
        return -1;
    if (vol_v3_path_lookup(v, from, &id) != 1)
        return -1;                              /* ENOENT */
    if (vol_v3_inode_get(v, id, &in) != 1)
        return -1;
    if (in.type == INVFS_ITYP_DIR)
        return -3;                              /* EPERM: dirs cannot link */
    if (vol_v3_path_lookup(v, tparent, &t_pino) != 1)
        return -1;                              /* destination parent missing */
    if (!vol_v3_path_is_dir(v, tparent))
        return -1;
    rc = vol_v3_dirent_get(v, t_pino, tleaf, &existing);
    if (rc < 0)
        return -1;
    if (rc == 1)
        return -2;                              /* EEXIST */
    if (in.nlink == 0 || in.nlink == 0xFFFFFFFFu)
        return -1;                              /* malformed / would wrap */
    /* nlink++ BEFORE the new name is visible: a crash between the two
     * leaves a count >= the name count, never a dirent whose inode is
     * under-counted and could be freed by a later unlink. */
    in.nlink++;
    if (vol_v3_inode_delta_put(v, id, &in) != 0)
        return -1;
    if (vol_v3_dirent_delta_put(v, t_pino, tleaf, id) != 0) {
        in.nlink--;                             /* roll back the count */
        vol_v3_inode_delta_put(v, id, &in);
        return -1;
    }
    return 0;
}


/* Hard link: a second NAME record sharing the same inode_id (and thus the
 * same AST/L2P blocks). LIMITATION: no block refcounts yet -- unlinking
 * EITHER name retires the shared blocks and dangles the survivor
 * (audit WP6). Portage lock files and similar transient links are fine. */
int vol_hardlink(invfs_volume *v, const char *from, const char *to)
{
    /* WP-M17: the v3 namespace tracks hardlinks through the shared inode
     * row's nlink; the v2 record-clone path is gone. */
    return vol_v3_hardlink(v, from, to);
}



/* REMOVED: vol_forget_name().
 *
 * It existed to drop a name left behind by the v2 name index when a process
 * died between the record append and the index update. That index is gone
 * (0c82a7a), so the function had no case where it could return anything but
 * 0 -- and its one caller read that 0 as "this was a ghost, safe to report
 * success", which turned every failed unlink into a success. rm(2) of a
 * nonexistent file returned 0. A stub that always agrees is not a check. */

