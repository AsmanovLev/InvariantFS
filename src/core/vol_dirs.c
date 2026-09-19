/* vol_dirs.c — virtual directories (prefix-based), rename/move,
 * hard links, replace/unlink. Split from volume.c. */

#include "volume_internal.h"


/* ---- virtual directories (prefix-based; mkdir creates an empty anchor
   file named "dir/"; nested files "dir/x" make dir visible) ---- */

/* does a directory exist? anchor "name/" or any file under "name/" */
int vol_is_dir(invfs_volume *v, const char *name)
{
    char pre[300];
    int plen;
    if (v->sb.vol_flags & VOLF_V3) return vol_v3_path_is_dir(v, name);
    if (name[0] == 0) return 1;   /* root always exists */
    plen = snprintf(pre, sizeof pre, "%s/", name);
    if (plen <= 0 || (size_t)plen >= sizeof pre) return 0;
    /* nonzero live count under "name/" -- the anchor record counts too, so
       an empty directory stays visible, and a tombstoned one does not */
    return idx_dir_live(v, pre, (size_t)plen);
}


/* create empty directory anchor "name/" */
uint64_t vol_mkdir(invfs_volume *v, const char *name)
{
    char anchor[300];
    if (v->sb.vol_flags & VOLF_V3)
        return (v->sb.vol_flags & VOLF_READONLY) ? 0 : vol_v3_mkdir(v, name);
    if (v->sb.vol_flags & VOLF_READONLY) return 0;   /* EROFS */
    if (!name || name[0] == 0 || strlen(name) > 240) return 0;
    if (vol_find(v, name) != 0) return 0;   /* plain file with same name */
    snprintf(anchor, sizeof anchor, "%s/", name);
    return vol_create_file(v, anchor, NULL, 0);
}


/* remove directory: must be empty (only the anchor, no files inside) */
int vol_rmdir(invfs_volume *v, const char *name)
{
    char anchor[300];
    int alen;
    /* H5: NOT gated on VOLF_READONLY -- a delete only frees space, and the
     * free-space latch must not weld its own exit shut. needs_recovery
     * (the true "never mutate" state) is still enforced via vol_mark_dirty
     * in the delete below. */
    if (v->sb.vol_flags & VOLF_V3) return vol_v3_rmdir(v, name);
    if (v->needs_recovery) return -1;
    if (!name || name[0] == 0) return -1;
    alen = snprintf(anchor, sizeof anchor, "%s/", name);
    if (alen <= 0 || (size_t)alen >= sizeof anchor) return -1;
    /* Live records under "name/", minus the anchor itself, are the
       directory's contents. Counting through the index also fixes the old
       scan, which accepted records already killed by a later tombstone and
       so kept an emptied directory ENOTEMPTY forever. */
    if (idx_dir_count(v, anchor, (size_t)alen) >
        (idx_get(v, anchor, (size_t)alen) ? 1u : 0u)) {
        /* Ghost tolerance: unlink-while-open (.fuse_hidden dance) can
         * leave children whose index entry outlived its record. If every
         * listed child resolves to no live name index entry, the dir is
         * de-facto empty -- scrub and remove instead of ENOTEMPTY. */
        int listed = 0, alive = 0;
        char child[600];
        invfs_dirent ents[64];
        int n = vol_list_dir(v, name, ents, 64);
        int k;
        for (k = 0; k < n; k++) {
            if (ents[k].name[0] == 0) continue;
            snprintf(child, sizeof child, "%s/%s", name, ents[k].name);
            listed++;
            if (vol_find(v, child) != 0 || vol_is_dir(v, child)) alive++;
        }
        if (alive == 0 && listed > 0 && n <= 64) {
            for (k = 0; k < n; k++) {
                if (ents[k].name[0] == 0) continue;
                snprintf(child, sizeof child, "%s/%s", name, ents[k].name);
                idx_del(v, child, (size_t)strlen(child), 0);
                idx_bump_dirs(v, child, strlen(child), -1);
            }
            v->dirty = 1;
        } else {
            return -2;   /* genuinely not empty */
        }
    }
    return vol_delete_file(v, anchor);
}


/* auto-create parent anchors for a path: "a/b/c.txt" -> "a/", "a/b/" */
int vol_ensure_path(invfs_volume *v, const char *name)
{
    char tmp[256];
    size_t n = strlen(name);
    if (v->sb.vol_flags & VOLF_V3) return vol_v3_ensure_path(v, name);
    if (n >= sizeof tmp) return -1;
    memcpy(tmp, name, n + 1);
    for (size_t i = 0; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (tmp[0] && !vol_is_dir(v, tmp))
                vol_mkdir(v, tmp);
            tmp[i] = '/';
        }
    }
    return 0;
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

    if (c->n >= c->max)
        return 1;                         /* full: stop the scan */
    if (nlen >= sizeof c->ents[0].name)
        return 0;
    if (vol_v3_inode_get(c->v, child, &in) != 1)
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
    if (v3_skip_slash(dir)[0] == 0)
        pino = INVFS_V3_ROOT_INO;
    else if (vol_v3_path_lookup(v, dir, &pino) != 1)
        return 0;
    c.v = v;
    c.ents = ents;
    c.max = max;
    c.n = 0;
    rc = vol_v3_dirent_scan(v, pino, v3_list_cb, &c);
    if (rc != 0 && rc != 1)
        return -1;
    /* the tree orders by (name_len, name); v2 listings are name-sorted and
     * the FUSE readdir caller expects that, so sort here. */
    if (c.n > 1)
        qsort(ents, (size_t)c.n, sizeof *ents, dirent_cmp);
    return c.n;
}

/* Create (or replace as an empty node) `name`, putting the inode row before
 * the dirent (a dirent must never point at a missing inode). `meta` may be
 * NULL (REG defaults). Returns the inode id, 0 on failure. Data payloads are
 * WP-M8 (recipes), so a v3 node created here is always empty. */
uint64_t vol_v3_create_node(invfs_volume *v, const char *name,
                            const invfs_meta_pub *meta)
{
    char parent[600], leaf[INVFS_MAX_NAME + 1];
    uint64_t pino, id, existing = 0;
    invfs_v3_inode in;
    int rc;

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
    in.size = 0;                          /* WP-M8: content cleared */
    memset(&in.recipe, 0, sizeof in.recipe);
    memset(in.recipe_addr, 0, sizeof in.recipe_addr);
    if (vol_v3_inode_put(v, id, &in) != 0)
        return 0;
    if (vol_v3_dirent_put(v, pino, leaf, id) != 0)
        return 0;
    if (in.type == INVFS_ITYP_DIR)
        vol_v3_dirent_put(v, id, "", id);  /* directory anchor */
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
    if (vol_v3_inode_put(v, id, &in) != 0)
        return 0;
    if (rc != 1 && vol_v3_dirent_put(v, pino, leaf, id) != 0)
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
    if (vol_v3_inode_put(v, id, &in) != 0)
        return 0;
    if (in.type == INVFS_ITYP_DIR)
        vol_v3_dirent_put(v, id, "", id);  /* ensure the anchor */
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
    if (vol_v3_inode_put(v, id, &in) != 0)
        return 0;
    if (vol_v3_dirent_put(v, id, "", id) != 0)     /* anchor */
        return 0;
    if (vol_v3_dirent_put(v, pino, leaf, id) != 0)
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
    if (vol_v3_dirent_del(v, pino, leaf) != 0)
        return -1;
    if (vol_v3_dirent_del(v, id, "") != 0)
        return -1;
    if (vol_v3_inode_delete(v, id) != 0)
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
    /* WP-M17 frozen transition (design §4): drop the dirent, then the
     * count; only nlink == 0 frees the row (and, via inode_delete, the
     * xattr keys). Name-first means a crash between the two leaves nlink
     * >= the live name count -- an inode may leak, but a live dirent can
     * never point at a freed row. */
    if (vol_v3_dirent_del(v, pino, leaf) != 0)
        return -1;
    if (in.nlink <= 1) {
        if (vol_v3_inode_delete(v, id) != 0)
            return -1;
    } else {
        in.nlink--;
        if (vol_v3_inode_put(v, id, &in) != 0)
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
        if (vol_v3_dirent_del(v, t_pino, tleaf) != 0)
            return -1;
        if (t_in.nlink > 1) {
            t_in.nlink--;
            if (vol_v3_inode_put(v, t_id, &t_in) != 0)
                return -1;
        } else if (vol_v3_inode_delete(v, t_id) != 0) {
            return -1;
        }
    }
    /* insert the new dirent BEFORE deleting the old (add-before-remove,
     * design §3): a crash between the two leaves the file under both names,
     * never under neither. */
    if (vol_v3_dirent_put(v, t_pino, tleaf, f_id) != 0)
        return -1;
    if (vol_v3_path_lookup(v, fparent, &f_pino) != 1)
        return -1;
    if (vol_v3_dirent_del(v, f_pino, fleaf) != 0)
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

static int v3_walk_dir(invfs_volume *v, const char *dir,
                       vol_v3_walk_cb cb, void *ctx, int depth)
{
    invfs_dirent *ents;
    int cap = 256, n, i;

    if (depth > 64)
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

        if (dir[0])
            snprintf(path, sizeof path, "%s/%s", dir, ents[i].name);
        else
            snprintf(path, sizeof path, "%s", ents[i].name);
        if (vol_v3_path_lookup(v, path, &ino) != 1)
            continue;
        if (vol_v3_inode_get(v, ino, &in) != 1)
            continue;
        if (cb && cb(ctx, path, ino, in.type, in.size, in.mtime) != 0) {
            free(ents);
            return 1;
        }
        if (in.type == INVFS_ITYP_DIR)
            v3_walk_dir(v, path, cb, ctx, depth + 1);
    }
    free(ents);
    return 0;
}

int vol_v3_walk(invfs_volume *v, vol_v3_walk_cb cb, void *ctx)
{
    if (!v)
        return -1;
    return v3_walk_dir(v, "", cb, ctx, 0);
}


/* list one directory level: first path component after "dir/" */
int vol_list_dir(invfs_volume *v, const char *dir, invfs_dirent *ents, int max)
{
    char pre[300];
    size_t pren;
    size_t n = 0, b;
    /* Dedup set over the entries collected so far. The old code compared each
       candidate against every entry already found, which is O(n^2) on top of
       the scan; a directory of 1600 files cost ~1.3M strcmp per listing.
       `at` is the slot in `ents`: a subdirectory is usually discovered from a
       file inside it, and its own anchor record -- the one holding its ctime
       -- may only turn up later in the walk. */
    struct dedup { struct dedup *next; size_t at; int is_dir; char name[1]; };
    struct dedup **seen = NULL;
    size_t seen_mask = 0;

    if (v->sb.vol_flags & VOLF_V3)
        return vol_v3_path_list_dir(v, dir, ents, max);
    if (dir[0] == 0) { pre[0] = 0; pren = 0; }
    else {
        int pl = snprintf(pre, sizeof pre, "%s/", dir);
        if (pl <= 0 || (size_t)pl >= sizeof pre) return 0;
        pren = (size_t)pl;
    }

    /* Walk the in-memory name index instead of re-reading the inode area.
       Windows enumerates a directory after every file creation, so this path
       ran once per created file and read every record on the volume each
       time: it was the entire cost of writing a file (10.4 ms of a 10.7 ms
       write at 1600 files), and it grew linearly with the file count.
       The index holds exactly the live names -- last-record-wins and the
       tombstone rules are applied as it is maintained -- so the separate
       tombstone post-pass that used to follow is gone too. */
    if (!v->nbuck) return 0;
    {
        size_t sc = 64;
        while (sc < v->ncount) sc *= 2;
        seen = (struct dedup **)calloc(sc, sizeof *seen);
        if (!seen) return -1;
        seen_mask = sc - 1;
    }

    for (b = 0; b <= v->nmask && n < (size_t)max; b++) {
        const name_index_entry *e;
        for (e = v->nbuck[b]; e && n < (size_t)max; e = e->next) {
            const char *rest, *slash;
            size_t nl = e->nlen, rl, flen, hb;
            char first[256];
            int is_dir;
            struct dedup *d;

            if (nl <= pren || memcmp(e->name, pre, pren) != 0) continue;
            rest = e->name + pren;
            rl = nl - pren;
            slash = (const char *)memchr(rest, '/', rl);
            if (slash) { flen = (size_t)(slash - rest); is_dir = 1; }
            else { flen = rl; is_dir = 0; }
            if (flen == 0 || flen >= sizeof first) continue;   /* "a//b" */
            memcpy(first, rest, flen);
            first[flen] = 0;
            /* skip internal '!' siblings (recipe/cover/part/jxl) */
            if (memchr(first, '!', flen)) continue;
            /* hide control-prefixed internal names (the "\x01tzb" owner) */
            if ((uint8_t)first[0] == 0x01) continue;

            hb = (size_t)(idx_hash(first, flen) & seen_mask);
            for (d = seen[hb]; d; d = d->next)
                if (d->is_dir == is_dir && strcmp(d->name, first) == 0) break;
            if (d) {
                /* The anchor record "d/" is the directory's own record; its
                   ctime is the one to report. Any other record that maps to
                   the same entry is a file underneath it. */
                if (is_dir && rl == flen + 1) ents[d->at].ctime = e->ctime;
                continue;
            }
            d = (struct dedup *)malloc(sizeof *d + flen);
            if (!d) continue;
            memcpy(d->name, first, flen + 1);
            d->is_dir = is_dir;
            d->at = n;
            d->next = seen[hb];
            seen[hb] = d;

            memcpy(ents[n].name, first, flen + 1);
            ents[n].is_dir = is_dir;
            /* Carried from the index so the caller needs no per-entry stat.
               A directory's size is meaningless; report 0 unless its own
               anchor record comes along. */
            ents[n].size = is_dir ? 0 : e->size;
            ents[n].ctime = e->ctime;
            n++;
        }
    }

    for (b = 0; b <= seen_mask; b++) {
        struct dedup *d = seen[b];
        while (d) { struct dedup *nx = d->next; free(d); d = nx; }
    }
    free(seen);
    /* Hash order is an implementation detail and would shift between mounts.
       The inode-area scan used to yield creation order; sort by name so the
       listing is at least stable. */
    if (n > 1) qsort(ents, n, sizeof *ents, dirent_cmp);
    return (int)n;
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
    uint64_t old_id, nid;

    /* WP-M6: a v3 namespace node is the dirent tree + inode row. Content
     * (recipe blobs) is WP-M8, so only the empty create/replace case is
     * supported; a data write is refused, not silently stored as v2. */
    if (v->sb.vol_flags & VOLF_V3)
        return (data && len) ? 0 : vol_v3_create_node(v, name, NULL);
    if (v->sb.vol_flags & VOLF_READONLY) return 0;   /* EROFS */
    old_id = vol_find(v, name);
    /* WP19: write-heat is the one counter a rewrite must NOT reset --
     * "rewritten often" is history the rewrite itself destroys. Capture
     * the old entries' max now (they retire below) and carry old+1 onto
     * the replacement's fresh entries (born wheat 1 in vol_create_file).
     * Read-heat resets by design: past reads say nothing about new bytes. */
    {
        uint8_t wold = old_id ? heat_file_maxw(v, old_id) : 0;
        nid = vol_create_file(v, name, data, len);
        if (nid == 0) return 0;                      /* old file still there */
        if (old_id != 0)
            heat_file_setw(v, nid, wold == 0xFF ? 0xFF : (uint8_t)(wold + 1));
    }
    if (old_id != 0) {
        /* the replacement is plain RAW, so the old file's recipe/parts/covers
           describe bytes that no longer exist under this name */
        int owns_siblings = 1;   /* unknown: scan conservatively */
        uint8_t *obuf = NULL;
        uint32_t orl = 0;
        if (meta_read_record_by_id(v, old_id, &obuf, &orl, NULL, 0, NULL) == 0) {
            owns_siblings = record_owns_siblings(obuf, orl);
            free(obuf);
        }
        vol_delete_inode(v, old_id, name);
        /* sibling deletion walks the WHOLE inode area (pread per record):
         * plain files -- everything written through FUSE -- can only have
         * '!' siblings when their AST lists children or a container algo,
         * so skip the walk */
        if (owns_siblings)
            vol_delete_siblings(v, name);
    }
    return nid;
}


uint64_t vol_replace_file_with_meta(invfs_volume *v, const char *name,
                                    const uint8_t *data, size_t len,
                                    const invfs_meta_pub *meta)
{
    uint64_t old_id, nid;

    if (v->sb.vol_flags & VOLF_V3)
        return (data && len) ? 0 : vol_v3_create_node(v, name, meta);
    if (v->sb.vol_flags & VOLF_READONLY) return 0;
    old_id = vol_find(v, name);
    {
        uint8_t wold = old_id ? heat_file_maxw(v, old_id) : 0;
        nid = vol_create_file_with_meta(v, name, data, len, meta);
        if (nid == 0) return 0;
        if (old_id != 0)
            heat_file_setw(v, nid, wold == 0xFF ? 0xFF : (uint8_t)(wold + 1));
    }
    if (old_id != 0) {
        int owns_siblings = 1;
        uint8_t *obuf = NULL;
        uint32_t orl = 0;
        if (meta_read_record_by_id(v, old_id, &obuf, &orl, NULL, 0, NULL) == 0) {
            owns_siblings = record_owns_siblings(obuf, orl);
            free(obuf);
        }
        vol_delete_inode(v, old_id, name);
        if (owns_siblings)
            vol_delete_siblings(v, name);
    }
    return nid;
}


int vol_delete_file(invfs_volume *v, const char *name)
{
    uint64_t inode_id;
    if (v->sb.vol_flags & VOLF_V3) return vol_v3_unlink(v, name);
    /* H5: allowed under the VOLF_READONLY space latch -- a delete only
     * frees blocks and appends a tombstone, moving the volume AWAY from
     * the wall. needs_recovery still refuses via vol_mark_dirty below. */
    if (v->needs_recovery) return -1;
    inode_id = vol_find(v, name);
    if (inode_id == 0)
        return -1;
    return vol_delete_inode(v, inode_id, name);
}


/* Delete a file and the sibling records that belong to it. This is what an
   unlink means for a transcoded file; vol_delete_file alone strands them. */
/* remove ONE name of a multi-name (hardlinked) inode: tombstone only
 * this name's record; blocks stay alive for the surviving names.
 * nlink on surviving records is not rewritten (drift only delays block
 * retirement; fsck recount fixes it). */
int vol_unlink_name(invfs_volume *v, const char *name)
{
    uint64_t id, pos = 0;
    uint32_t crc;
    size_t nl, tomb_size;
    uint8_t *tbuf;
    invfs_inode_rec *tomb;
    const name_index_entry *e;

    if (v->sb.vol_flags & VOLF_V3) return vol_v3_unlink(v, name);
    /* H5: allowed under the VOLF_READONLY space latch (tombstone only,
     * blocks stay alive for the surviving names -- see vol_delete_file) */
    if (v->needs_recovery) return -1;
    nl = strlen(name);
    tomb_size = INVFS_REC_HDR_LEN + nl + 1;
    e = idx_get(v, name, nl);
    if (!e) return -1;
    id = e->inode_id;
    /* The kill target is the record THIS NAME points at. Resolving the
     * position through the inode id (meta_read_record_by_id /
     * idx_get_id) is wrong here: the rename fast path's vol_hardlink has
     * just re-pointed the id index at the NEW record, which shares the
     * id -- the position-kill tombstone then named the replacement and
     * killed nothing at replay, so the old name stayed live and a later
     * retire of the shared id dropped the surviving name's mappings
     * (WP22c/F2). The name index tracks each name's own record. */
    pos = e->pos;
    /* sanity: the index entry must name a live INOD for this id (the
     * vol_forget_name ghost pattern) */
    if (!pos ||
        pos < v->inode_area_start * INVFS_BLOCK_SIZE ||
        pos + INVFS_REC_HDR_LEN > v->inode_area_pos)
        return -1;
    {
        invfs_inode_rec h;
        if (io_seek(&v->io, pos) != 0 || io_read(&v->io, &h, sizeof h) != 0)
            return -1;
        if (h.magic != INODE_REC_MAGIC || h.inode_id != id)
            return -1;
    }
    if (vol_mark_dirty(v) != 0)
        return -1;
    tbuf = (uint8_t *)calloc(1, tomb_size);
    if (!tbuf) return -1;
    tomb = (invfs_inode_rec *)tbuf;
    tomb->magic = TOMBSTONE_MAGIC;
    v->hot.tombstones++;
    tomb->rec_len = (uint32_t)tomb_size;
    tomb->inode_id = id;
    tomb->file_size = pos;                    /* v2 position-kill */
    rec_set_name(tomb, name);
    crc = invfs_crc32c(tbuf, tomb_size);
    /* Bug J: route the tombstone append through the mapper (extents own
     * records past the metadata zone on v0.3.0+ volumes) */
    {
        uint64_t tpos;
        int rc2 = vol_append_slot(v, (uint64_t)tomb_size + 4, &tpos);
        if (rc2 != 0) { free(tbuf); return rc2; }
        if (io_seek(&v->io, tpos) != 0 ||
            io_write(&v->io, tbuf, tomb_size) != 0 ||
            io_write(&v->io, &crc, 4) != 0) { free(tbuf); return -1; }
    }
    free(tbuf);
    idx_del_at(v, name, nl, pos);
    idx_bump_dirs(v, name, nl, -1);
    return 0;
}


int vol_unlink(invfs_volume *v, const char *name)
{
    int rc;
    if (v->sb.vol_flags & VOLF_V3) return vol_v3_unlink(v, name);
    /* H5: allowed under the VOLF_READONLY space latch (frees only) */
    if (v->needs_recovery) return -1;
    if (strchr(name, '!')) return vol_delete_file(v, name);   /* a sibling */
    /* capture the record first: sibling deletion is a full-area walk and
     * plain files (no children, no container algo) never have '!' siblings */
    {
        int owns_siblings = 1;
        uint8_t *obuf = NULL;
        uint32_t orl = 0;
        uint64_t id = vol_find(v, name);
        if (id && meta_read_record_by_id(v, id, &obuf, &orl, NULL, 0, NULL) == 0) {
            owns_siblings = record_owns_siblings(obuf, orl);
            free(obuf);
        }
        rc = vol_delete_file(v, name);
        if (rc == 0 && owns_siblings) vol_delete_siblings(v, name);
    }
    return rc;
}


/* ---- rename / move ---- */

/* Move ONE inode record to a new name.
 *
 * The name lives inside the inode record and the inode area is append-only,
 * so a rename is: append a copy of the record with the AST payload verbatim
 * (the data blocks are never read, rewritten or recompressed), re-key that
 * inode's L2P mappings, then tombstone the old name.
 *
 * The copy deliberately gets a NEW inode id. vol_fsck_scan kills live records
 * by inode_id alone (pass 2), so if both names shared an id, the old name's
 * tombstone would make `invf-fsck --fix` free the renamed file's blocks. */
static int rename_one(invfs_volume *v, const char *from, const char *to)
{
    uint64_t old_id, new_id, rec_pos = 0;
    invfs_inode_rec rh, *nh;
    uint8_t *old = NULL, *nrec;
    uint32_t crc;
    size_t tolen = strlen(to), fromlen = strlen(from);
    size_t old_base, body, new_rec_len, tomb_size;
    const name_index_entry *ie;

    if (tolen > INVFS_MAX_NAME) return -1;
    /* the retire tombstone carries the OLD name */
    tomb_size = INVFS_REC_HDR_LEN + fromlen + 1;
    ie = idx_get(v, from, fromlen);
    if (!ie) return -1;
    old_id = ie->inode_id;
    rec_pos = ie->pos;          /* the live record — the index tracks it */
    if (rec_pos == 0) return -1;

    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, &rh, sizeof rh) != 0) return -1;
    if (rh.rec_len < INVFS_REC_HDR_LEN + 1 ||
        rh.name_len > rh.rec_len - INVFS_REC_HDR_LEN - 1) return -1;

    /* The copy is rebuilt under the new name, so its size tracks the name
       delta (old slot name_len+1 -> tolen+1). Room for the copy AND the
       tombstone that follows it — running out between the two would leave
       the file reachable under both names. WP27 churn backstop: compact the
       dead prefix first; the compaction moves every record, so the live
       position is re-resolved after it. */
    {
        size_t new_len = rh.rec_len - (size_t)rh.name_len + tolen;
        if (v->inode_area_pos + new_len + 4 + tomb_size + 4 >
            v->inode_area_end) {
            if (inode_area_make_room(v, new_len + 4 + tomb_size + 4) != 0)
                return -3;   /* inode area full */
            ie = idx_get(v, from, fromlen);
            if (!ie) return -1;
            rec_pos = ie->pos;
            if (rec_pos == 0) return -1;
            if (io_seek(&v->io, rec_pos) != 0 ||
                io_read(&v->io, &rh, sizeof rh) != 0) return -1;
            if (rh.rec_len < INVFS_REC_HDR_LEN + 1) return -1;
        }
    }

    old = (uint8_t *)malloc(rh.rec_len);
    if (!old) return -1;
    if (io_seek(&v->io, rec_pos) != 0 ||
        io_read(&v->io, old, rh.rec_len) != 0) { free(old); return -1; }

    /* the record is variable-length: rebuild it with the new name while
     * keeping the AST payload (and INO2 ext) verbatim behind it. */
    old_base = INVFS_REC_HDR_LEN + rh.name_len + 1;
    if (old_base > rh.rec_len) { free(old); return -1; }
    body = rh.rec_len - old_base;
    new_rec_len = INVFS_REC_HDR_LEN + tolen + 1 + body;
    nrec = (uint8_t *)calloc(1, new_rec_len);
    if (!nrec) { free(old); return -1; }
    memcpy(nrec, old, INVFS_REC_HDR_LEN);
    nh = (invfs_inode_rec *)nrec;
    new_id = v->next_inode_id++;
    nh->inode_id = new_id;
    rec_set_name(nh, to);
    memcpy(nrec + INVFS_REC_HDR_LEN + tolen + 1, old + old_base, body);
    nh->rec_len = (uint32_t)new_rec_len;
    crc = invfs_crc32c(nrec, new_rec_len);
    free(old);

    /* WP27: the copied record carries the pbas verbatim -- the rename needs
     * no map re-key at all. The blocks' durability is the same as on the
     * day the original record landed, and vol_pre_record's flush ordering
     * (bitmap durable before the record that names its pbas) is kept. The
     * old entries stay until vol_delete_inode below, so a crash mid-rename
     * leaves the old name fully readable, and the delete frees nothing the
     * copy still references (the pba_ref sharer count).
     * Bug J: route the append through the mapper (extents own records past
     * the metadata zone on v0.3.0+ volumes). */
    {
        uint64_t npos;
        int rc2;
        if (vol_pre_record(v) != 0) { free(nrec); return -1; }
        rc2 = vol_append_slot(v, (uint64_t)new_rec_len + 4, &npos);
        if (rc2 != 0) { free(nrec); return rc2; }
        if (io_seek(&v->io, npos) != 0 ||
            io_write(&v->io, nrec, new_rec_len) != 0 ||
            io_write(&v->io, &crc, 4) != 0) { free(nrec); return -1; }
        idx_put(v, to, tolen, new_id, npos, nh->file_size, nh->ctime);
        idx_put_id(v, new_id, npos);
    }
    pba_ref_apply(v, nrec, (uint32_t)new_rec_len, +1);
    free(nrec);

    return vol_delete_inode(v, old_id, from);
}


/* WP47: collector for the rename walk. vol_records_walk() drives the
 * mapper-aware scan (dynamic meta extents on v0.3.0+); the callback keeps
 * the legacy per-record policy: prefix / "name!" matching, supersede
 * filtering, and first-name-wins ordering. Collection happens BEFORE any
 * rename_one append, so the walk never sees records it just wrote. */
typedef struct {
    invfs_volume *v;
    char (*names)[256];
    size_t n, cap;
    const char *from;
    const char *pre;
    size_t pren, flen;
    int dir;
    int oom;
} rename_collect_ctx;

static int rename_collect_cb(void *ctx_, uint64_t rec_pos,
                             const invfs_inode_rec *h, const uint8_t *rec)
{
    rename_collect_ctx *c = (rename_collect_ctx *)ctx_;
    char nm[257];
    size_t nl, i;
    (void)rec_pos;
    (void)rec;
    if (h->magic != INODE_REC_MAGIC) return 0;   /* tombstones */
    {
        size_t maxnl = h->rec_len > INVFS_REC_HDR_LEN + 1
                     ? h->rec_len - INVFS_REC_HDR_LEN - 1 : 0;
        if (maxnl > INVFS_MAX_NAME) maxnl = INVFS_MAX_NAME;
        nl = h->name_len < maxnl ? h->name_len : maxnl;
    }
    memcpy(nm, h->name, nl);
    nm[nl] = 0;
    if (c->dir) {
        if (nl < c->pren || strncmp(nm, c->pre, c->pren) != 0) return 0;
    } else {
        /* the file itself, plus its "name!..." siblings */
        if (nl < c->flen || strncmp(nm, c->from, c->flen) != 0) return 0;
        if (nm[c->flen] != 0 && nm[c->flen] != '!') return 0;
    }
    if (vol_find(c->v, nm) == 0) return 0;       /* superseded/tombstoned */
    for (i = 0; i < c->n; i++)
        if (strcmp(c->names[i], nm) == 0) return 0;   /* older, same name */
    if (c->n == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 16;
        void *nn = realloc(c->names, ncap * sizeof(*c->names));
        if (!nn) { c->oom = 1; return 1; }
        c->names = (char (*)[256])nn;
        c->cap = ncap;
    }
    memcpy(c->names[c->n++], nm, nl + 1);
    return 0;
}


/* Rename a file or directory.
 *
 * Transcoded files keep their payload in sibling records ("name!recipe",
 * "name!partN", "name!coverN", "name!jxl") that the read path resolves BY
 * NAME, so moving a file has to move its siblings too — otherwise the
 * bit-exact rebuild silently loses its recipe. Directories are virtual
 * (anchor "dir/" + prefix), so moving one rewrites every record under the
 * prefix, anchor included.
 *
 * Returns 0, -1 = ENOENT/EINVAL/IO, -2 = destination exists, -3 = ENOSPC.
 * The inode area is append-only, so a multi-record move that fails partway
 * leaves the already-moved records at their new names; re-running the rename
 * completes it. */
int vol_rename(invfs_volume *v, const char *from, const char *to)
{
    char (*names)[256] = NULL;
    char pre[300];
    size_t n = 0, cap = 0, pren = 0, flen, i;
    int dir, rc = 0;

    if (v->sb.vol_flags & VOLF_V3) return vol_v3_rename(v, from, to);
    if (v->sb.vol_flags & VOLF_READONLY) return -1;   /* EROFS */
    /* WP65: a rename under a live sweep checkpoint realizes the
     * checkpoint first. The rollback would decapitate the inode area at
     * the checkpoint's append pointer and discard the whole rename pair
     * -- the copy dies AND the tombstone dies, so the source name
     * resurrects. Realizing is the point of no return (invf-sweep
     * --realize), after which the pair is an ordinary post-checkpoint
     * write; refusing was the only ck_present gate on any mutator and it
     * broke package-manager atomic renames. If the realize fails the
     * rename still refuses with -4 (EBUSY). */
    if (v->ck_present) {
        uint64_t freed = 0;
        if (vol_ckp_realize(v, &freed) < 0) return -4;
    }
    if (!v || !from || !to || !from[0] || !to[0]) return -1;
    if (strcmp(from, to) == 0) return 0;
    flen = strlen(from);
    if (strlen(to) >= 240) return -1;
    /* "a" -> "a/b" would move a directory inside itself */
    if (strncmp(to, from, flen) == 0 && to[flen] == '/') return -1;

    dir = vol_is_dir(v, from);
    if (!dir && vol_find(v, from) == 0) return -1;            /* ENOENT */
    /* POSIX rename replaces an existing plain-file destination
     * silently; directory destinations stay unsupported (EEXIST). */
    if (vol_is_dir(v, to)) return -2;
    {
        uint64_t toid = vol_find(v, to);
        if (toid != 0) {
            if (dir) return -1;                    /* dir over file: ENOTDIR-ish */
            /* the victim takes its '!' siblings down with it (the
             * vol_unlink cascade): a container victim whose members were
             * only renamed in would otherwise strand them as parentless
             * live records (the chaos soak reads one as a ghost). */
            int owns_siblings = 1;
            uint8_t *obuf = NULL;
            uint32_t orl = 0;
            if (meta_read_record_by_id(v, toid, &obuf, &orl, NULL, 0,
                                       NULL) == 0) {
                owns_siblings = record_owns_siblings(obuf, orl);
                free(obuf);
            }
            if (vol_delete_file(v, to) != 0) return -1;
            if (owns_siblings) vol_delete_siblings(v, to);
        }
    }

    vol_ensure_path(v, to);   /* parents of the destination */

    /* FAST PATH: plain files without container siblings are renamed as
     * hardlink(new)+unlink_name(old) -- both are O(1) index-hint ops.
     * The legacy full-area sibling scan below only runs for directories
     * and container anchors ('!' siblings / num_children>0); on big
     * volumes it cost minutes per rename (dracut does hundreds). */
    if (!dir && !strchr(from, '!') && !strchr(to, '!')) {
        uint8_t *buf = NULL;
        uint32_t rl = 0;
        uint64_t fid = vol_find(v, from);
        int simple = 0;
        if (fid != 0 &&
            meta_read_record_by_id(v, fid, &buf, &rl, NULL, 0, NULL) == 0)
            /* Extraction containers (TARR/EXER/...) carry num_children==0
             * yet keep their payload in "name!..." siblings the read path
             * resolves BY NAME: a hardlink rename would move the anchor
             * and strand the recipe under the old name (the renamed file
             * never reads again). record_owns_siblings is the conservative
             * test; anything unknown takes the slow path. */
            simple = !record_owns_siblings(buf, rl);
        free(buf);
        if (simple) {
            if (vol_hardlink(v, from, to) != 0) return -1;
            if (vol_unlink_name(v, from) != 0) return -1;
            return 0;
        }
    }

    if (dir) { snprintf(pre, sizeof pre, "%s/", from); pren = strlen(pre); }

    /* Collect every record the move touches BEFORE appending anything:
       each rename_one extends the inode area, and a live scan would then
       walk into the records it had just written. WP47: the shared
       mapper-aware walker drives the collection (records in dynamic meta
       extents were invisible to the legacy contiguous loop). */
    {
        rename_collect_ctx c;
        int wrc;
        memset(&c, 0, sizeof c);
        c.v = v;
        c.from = from;
        c.pre = pre;
        c.pren = pren;
        c.flen = flen;
        c.dir = dir;
        wrc = vol_records_walk(v, rename_collect_cb, &c);
        names = c.names;
        n = c.n;
        cap = c.cap;
        if (wrc != 0) { free(names); return -1; }   /* OOM / IO error */
    }

    if (rc == 0 && n == 0) rc = -1;   /* nothing matched */

    for (i = 0; i < n && rc == 0; i++) {
        char nn[300];
        if (dir) snprintf(nn, sizeof nn, "%s/%s", to, names[i] + pren);
        else     snprintf(nn, sizeof nn, "%s%s", to, names[i] + flen);
        if (strlen(nn) >= 256) { rc = -1; break; }
        rc = rename_one(v, names[i], nn);
    }
    free(names);
    return rc;
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
    if (vol_v3_inode_put(v, id, &in) != 0)
        return -1;
    if (vol_v3_dirent_put(v, t_pino, tleaf, id) != 0) {
        in.nlink--;                             /* roll back the count */
        vol_v3_inode_put(v, id, &in);
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
    uint8_t *buf = NULL;
    uint32_t rl = 0, crc;
    invfs_inode_rec *nh;
    uint64_t id, pos;
    size_t nl;

    if (!v || !from || !to || !from[0] || !to[0]) return -1;
    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    /* WP-M17: the v3 namespace tracks hardlinks through the shared inode
     * row's nlink; only the v2 record-clone path below is limited. */
    if (v->sb.vol_flags & VOLF_V3)
        return vol_v3_hardlink(v, from, to);
    nl = strlen(to);
    if (nl > INVFS_MAX_NAME) return -1;

    id = vol_find(v, from);
    if (!id) return -1;                              /* ENOENT */
    {
        uint64_t toid = vol_find(v, to);
        if (toid) {
            /* stale-index self-heal: killed emerges leave table entries
             * whose latest record is already a tombstone. The id-index
             * hint always points at the newest version we wrote, so
             * liveness = the header AT THE HINT is an INOD for this id.
             * (meta_read_record_by_id alone would false-positive: it
             * happily reads OLDER same-id versions still present in the
             * append-only area.) */
            int alive = 0;
            uint64_t hp = idx_get_id(v, toid);
            if (hp >= v->inode_area_start * INVFS_BLOCK_SIZE &&
                hp + sizeof(invfs_inode_rec) <= v->inode_area_pos &&
                io_seek(&v->io, hp) == 0) {
                invfs_inode_rec hh;
                if (io_read(&v->io, &hh, sizeof(hh)) == 0 &&
                    hh.magic == INODE_REC_MAGIC &&
                    hh.inode_id == toid)
                    alive = 1;
            }
            if (alive) {
                return -2;                           /* EEXIST */
            }
        }
    }
    if (vol_is_dir(v, from)) return -3;              /* dirs can't hardlink */

    if (meta_read_record_by_id(v, id, &buf, &rl, NULL, 0, NULL) != 0)
        return -1;
    if (rl < INVFS_REC_HDR_LEN + 1) { free(buf); return -1; }

    /* the record is variable-length: rebuild it under the new name with the
     * body (AST + INO2 ext) verbatim behind it. */
    {
        const invfs_inode_rec *ob = (const invfs_inode_rec *)buf;
        size_t old_base = INVFS_REC_HDR_LEN + ob->name_len + 1;
        size_t body, new_rl;
        uint8_t *nrec;
        if (old_base > rl) { free(buf); return -1; }
        body = rl - old_base;
        new_rl = INVFS_REC_HDR_LEN + nl + 1 + body;
        nrec = (uint8_t *)calloc(1, new_rl);
        if (!nrec) { free(buf); return -1; }
        memcpy(nrec, buf, INVFS_REC_HDR_LEN);
        nh = (invfs_inode_rec *)nrec;
        rec_set_name(nh, to);
        memcpy(nrec + INVFS_REC_HDR_LEN + nl + 1, buf + old_base, body);
        nh->rec_len = (uint32_t)new_rl;
        crc = invfs_crc32c(nrec, new_rl);

        if (vol_mark_dirty(v) != 0) { free(nrec); free(buf); return -1; }
        /* Bug J: route the hardlink record append through the mapper */
        {
            int rc2 = vol_append_slot(v, (uint64_t)new_rl + 4, &pos);
            if (rc2 != 0) { free(nrec); free(buf); return rc2; }
            if (io_seek(&v->io, pos) != 0 ||
                io_write(&v->io, nrec, new_rl) != 0 ||
                io_write(&v->io, &crc, 4) != 0) {
                free(nrec); free(buf);
                return -1;
            }
        }
        idx_put(v, nh->name, nl, id, pos, nh->file_size, nh->ctime);
        idx_put_id(v, id, pos);
        /* the second name is a live child of its parent directories; without
         * this bump, unlinking it later drives parent counts negative and
         * rmdir starts refusing empty dirs ("Directory not empty") */
        idx_bump_dirs(v, to, nl, +1);
        free(nrec);
    }
    free(buf);
    return 0;
}



/* Drop a name whose backing record is already gone (index ghost left by
 * a killed process). Returns 0 if the entry was forgotten, -1 if the
 * name looks live (caller should use the normal unlink path). */
int vol_forget_name(invfs_volume *v, const char *name)
{
    uint64_t id, pos = 0;
    invfs_inode_rec hh;

    if (v->sb.vol_flags & VOLF_V3) return 0;   /* no v2 index ghosts */
    if (v->sb.vol_flags & VOLF_READONLY) return -1;
    id = vol_find(v, name);
    if (!id) return 0;                       /* already forgotten */
    pos = idx_get_id(v, id);
    if (!pos ||
        pos < v->inode_area_start * INVFS_BLOCK_SIZE ||
        pos + sizeof(hh) > v->inode_area_pos)
        return -1;
    if (io_seek(&v->io, pos) != 0 ||
        io_read(&v->io, &hh, sizeof(hh)) != 0)
        return -1;
    if (hh.magic == INODE_REC_MAGIC && hh.inode_id == id)
        return -1;                           /* record is alive */
    /* dead: drop the index entry and undo the directory child count */
    idx_del_at(v, name, strlen(name), pos);
    idx_bump_dirs(v, name, strlen(name), -1);
    vol_mark_dirty(v);
    return 0;
}
