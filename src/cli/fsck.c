/*
 * invf-fsck: check & repair an InvariantFS volume.
 *
 * Rebuilds the L2P table and used-bitmap from the inode area (AST
 * recipes are the source of truth for block ownership). Reports:
 *   - orphaned blocks (allocated but referenced by no live AST)
 *   - missing blocks  (referenced but free)
 *   - stale L2P       (journal out of sync with the inode area)
 *   - bad inode records (CRC/length)
 *
 * Usage: invf-fsck <image> [-f|--fix] [-q] [--discard-reachable]
 *   [--list-damaged]
 *   default: read-only report; -f applies fixes (rewrites the bitmap and
 *   the superblock state=CLEAN).
 *
 *   --repair (WP20b layer-2 RS recovery) is RETIRED: it reconstructed
 *   CRC-failed shadow segments from RS(32+m2, 32) parity using a stripe
 *   map read out of the format-v2 owner records, and it is passed
 *   explicitly rather than silently ignored so an operator who asks for
 *   it is told.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "volume.h"
#include "invarifs.h"
#include "vol_spt0.h"

int main(int argc, char **argv)
{
    const char *img = NULL;
    int fix = 0, quiet = 0, repair = 0, i;
    int discard_reachable = 0, list_damaged = 0;
    int err = 0;
    invfs_volume *v;
    invfs_fsck_report rep;
    int issues;

    memset(&rep, 0, sizeof rep);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "usage: invf-fsck <image> [-f|--fix] [-q]\n"
                "                [--discard-reachable] [--list-damaged]\n"
                "  --discard-reachable  with -f: excise a quarantined key\n"
                "      range even when a live inode still needs a key inside\n"
                "      it. That DESTROYS those files' content permanently.\n"
                "      Without it, -f refuses every range it cannot prove is\n"
                "      unreachable and leaves the volume damaged and intact.\n"
                "  --list-damaged  read-only: after the report, print one\n"
                "      TAB-separated line per live file the volume cannot\n"
                "      read back fully: damaged<TAB><id><TAB><name><TAB>"
                "<reason><TAB><size>. Reasons: torn-recipe, torn-xattr,\n"
                "      torn-row, recipe-corrupt, recipe-missing,\n"
                "      recipe-incoherent. Empty list\n"
                "      with an OK verdict means nothing is damaged. Exit\n"
                "      code unchanged.\n");
            return 2;
        }
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            fprintf(stderr, "%s version %s (build %s)\n  Author: %s\n  License: %s\n",
                    argv[0], INVFS_VERSION_STRING, INVFS_BUILD_DATE, INVFS_AUTHOR_NAME, INVFS_LICENSE);
            return 0;
        }
        if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fix") == 0)
            fix = 1;
        else if (strcmp(argv[i], "--repair") == 0)
            repair = 1;
        else if (strcmp(argv[i], "--discard-reachable") == 0)
            discard_reachable = 1;
        else if (strcmp(argv[i], "--list-damaged") == 0)
            list_damaged = 1;
        else if (strcmp(argv[i], "-q") == 0)
            quiet = 1;
        else
            img = argv[i];
    }
    if (!img) {
        fprintf(stderr,
            "usage: invf-fsck <image> [-f|--fix] [--repair] [-q]\n"
            "                [--discard-reachable] [--list-damaged]\n");
        return 2;
    }
    /* The flag is a decision to destroy data, so it is only meaningful
     * attached to a repair. On its own it would parse, do nothing, and leave
     * the operator believing the volume was repaired destructively. */
    if (discard_reachable && !fix) {
        fprintf(stderr, "invf-fsck: --discard-reachable only means something "
                        "with -f: it is the decision to excise a key range that "
                        "a live file still needs. Run `invf-fsck <image> -f "
                        "--discard-reachable` if that is what you want.\n");
        return 2;
    }

    v = vol_open(img, &err);
    if (!v) {
        fprintf(stderr, "invf-fsck: cannot open %s (err %d)\n", img, err);
        return 1;
    }

    /* WP25: a degraded mount (dev0 absent) is read-only -- report mode
     * works (every structure reads from the dev1 mirror), but -f mutates:
     * the mirror would diverge with dev0 absent. Reattach dev0 first.
     *
     * WP98: this check sat BELOW the v3 branch, so on a Meta-v3 volume the
     * v3 return skipped it and `invf-fsck -f` happily ran its repair pass
     * against a read-only, degraded mount -- writing to the dev1 mirror
     * alone, which on v3 nothing detects or repairs (no DEVT sync_seq bump
     * and no mirror_resync: vol_flush returns before that tail for VOLF_META,
     * volume.c:2435). Hoisted above both branches, so the refusal is format
     * independent. Report mode is unaffected. */
    if (fix && vol_degraded(v)) {
        fprintf(stderr, "invf-fsck: %s: DEGRADED volume (dev0 absent) -- "
                "report mode only; reattach dev0 to repair\n", img);
        vol_close(v);
        return 1;
    }

    /* WP-M4: a format-v3 volume uses the metadata-v3 base tree, not the v2
     * inode-record stream. vol_fsck_scan dispatches to the v3 checker (RT30
     * root double-slot + base-tree walk).
     *
     * WP86: that walk CONTAINS an unreadable base page instead of stopping at
     * it -- the page's key range is quarantined, every other subtree stays
     * readable, and a key inside a quarantined range reads EIO. `-f` excises
     * the quarantined ranges, folds the delta back in (which restores every
     * quarantined key the delta still holds) and names what is left, which is
     * gone for good.
     *
     * The excision is the one step here that cannot be undone, and its unit
     * is a KEY RANGE, not a page: a quarantined range can hold a 0x04 recipe
     * blob or a 0x03 xattr record that a live inode -- whose own row survived
     * in a readable page -- still needs. So -f proves, per range, that no live
     * object requires a key inside it (fsck_excise_safety) and REFUSES
     * every range it cannot clear, changing nothing and leaving the volume
     * damaged and intact: a key that reads EIO is recoverable, an excised key
     * is gone. `--discard-reachable` is the operator's separate, explicit
     * decision to proceed anyway; it is the only way to get the old behaviour,
     * and it names what it is about to destroy first.
     *
     * The exit code never softens the alarm: any damage found makes this pass
     * exit 3, repair or not (the v2 -f contract -- `issues` is computed from
     * what was found, not from what was fixed). A volume that is still
     * damaged after -f is reported as DEGRADED with the reason, never as OK,
     * and a damage -f cannot repair at all says so instead of doing nothing.
     * The v2 path below is untouched.
     *
     * WP99: the walk only ever sees ONE copy of the metadata, so on a
     * two-device volume it could not see the failure that actually loses
     * data. It walks whatever the mux serves -- the dev1 mirror once dev0 is
     * known to be stale -- and prints OK, while the tree reachable from the
     * OTHER device is a rolled-back generation whose next publish drops
     * every delta record past it. So the verdict now also asks the two
     * devices directly (vol_mirror_compare, the same call the mount path
     * makes) and a stale mirror is named, never folded into a clean OK. A
     * resync the pass itself performed is reported as such. */
    if (vol_sb(v)->vol_flags & VOLF_META) {
        const invfs_superblock *sb = vol_sb(v);
        int degraded = 0;
        int mstale = -1, mstale_after = -1, mresynced = 0;
        const char *mwhy = NULL, *mwhy_after = NULL;
        (void)vol_mirror_compare(v, &mstale, &mwhy);
        if (vol_fsck_scan_ex(v, &rep, fix, discard_reachable) != 0) {
            fprintf(stderr, "invf-fsck: v3 scan failed\n");
            vol_close(v);
            return 1;
        }
        /* -f writes, so a resync pending from the open may have just run
         * (vol_commit_mirror). Ask again rather than reporting a mirror this
         * pass already put right -- but only after making sure it did: the
         * resync is a flush-time action and a report leaves the volume
         * untouched, so on a tree with no structural damage nothing would
         * have flushed and the "-f" this report points the operator at would
         * be a lie. */
        if (mstale >= 0 && fix) {
            if (vol_flush(v) != 0)
                fprintf(stderr, "invf-fsck: %s: the mirror resync flush "
                        "failed; the devices are still out of sync\n", img);
            (void)vol_mirror_compare(v, &mstale_after, &mwhy_after);
            if (mstale_after < 0)
                mresynced = 1;
        }
        if (rep.rt30_bad || rep.root_lost) {
            degraded = 1;
            fprintf(stderr, "invf-fsck: %s: CANNOT REPAIR: the base tree is "
                            "unreachable (no valid RT30 root). Only the delta "
                            "-- the writes since the last fold -- is readable; "
                            "rebuilding the base from the delta alone would "
                            "silently drop every key folded before it, so this "
                            "pass refuses. The volume is intact but unreadable: "
                            "restore it from a backup or from the original "
                            "image.\n", img);
        } else if (fix && rep.excise_refused) {
            /* The excision is the one repair step that is not reversible, so
             * it is the one that has to be proved safe first. A range a live
             * inode still needs a key from is NOT excised: the key stays on the
             * page it was on and still reads EIO, which is the one state the
             * operator can recover from. The pass changes nothing and says so,
             * which is exactly the contract the recipe check above already
             * keeps (a blob is addressed by its own hash: gone is gone). */
            degraded = 1;
            fprintf(stderr, "invf-fsck: %s: CANNOT REPAIR: %llu of %llu "
                            "quarantined key range(s) still hold keys %llu live "
                            "inode(s) need -- a recipe blob (0x04) or an xattr "
                            "record (0x03) that their own surviving rows name. "
                            "Excising a range drops every key in it, so those "
                            "files would lose their content while their names "
                            "and inode rows went on resolving: this pass made NO "
                            "change to them and left the volume damaged and "
                            "intact. Restore the image or the page, or re-run "
                            "with `-f --discard-reachable` to drop the range "
                            "anyway and lose those files for good.\n", img,
                    (unsigned long long)rep.excise_refused,
                    (unsigned long long)rep.quarantined,
                    (unsigned long long)rep.excise_blocked);
        } else if (fix && rep.quarantined && !rep.repaired) {
            degraded = 1;
            fprintf(stderr, "invf-fsck: %s: CANNOT REPAIR: %llu unreadable base "
                            "page(s) could not be quarantined away; the volume "
                            "is unchanged and still degraded\n", img,
                    (unsigned long long)rep.bad_pages);
        } else if (fix && rep.recipe_bad) {
            /* The blob is addressed by the BLAKE3 hash of its own contents.
             * There is nothing to rebuild it from, so -f does not touch
             * these files and does not claim to have: the verdict stays
             * DAMAGED and the exit code stays 3, exactly as for damage -f
             * cannot address anywhere else on this pass. */
            degraded = 1;
            fprintf(stderr, "invf-fsck: %s: CANNOT REPAIR: %llu live inode(s) "
                            "name a recipe blob the volume can no longer "
                            "produce, or a row/recipe pair that disagrees "
                            "(recipe-incoherent: row size != recipe size, an "
                            "entry out of bounds or overlapping, or a "
                            "coverage break -- each offender is named "
                            "above). A recipe is stored under the BLAKE3 "
                            "hash of its own contents, so -f has nothing "
                            "to rebuild it from and made NO change to "
                            "those files -- the content is gone or "
                            "unaddressable. Restore it from a backup or "
                            "from the original image; the other files on "
                            "this volume are unaffected and readable.\n", img,
                    (unsigned long long)rep.recipe_bad);
        }
        if (!quiet) {
            printf("InvariantFS fsck: %s\n", img);
            printf("  state:        %s\n",
                   sb->state == INVFS_STATE_CLEAN ? "CLEAN" :
                   sb->state == INVFS_STATE_DIRTY ? "DIRTY" :
                   sb->state == INVFS_STATE_RECOVERY ? "RECOVERY" : "UNKNOWN");
            printf("  format:       v%d (metadata base tree)\n", INVFS_FORMAT_VERSION);
            if (rep.rt30_bad)
                printf("  root desc:    TORN (magic/version/CRC)\n");
            printf("  root seq:     %llu\n",
                   (unsigned long long)rep.root_seq);
            printf("  pages walked: %llu\n",
                   (unsigned long long)rep.pages_walked);
            printf("  base keys:    %llu\n",
                   (unsigned long long)rep.keys);
            printf("  torn slots:   %llu\n",
                   (unsigned long long)rep.slots_torn);
            if (rep.slots_ambiguous)
                printf("  ambiguous slots: %llu (two DIFFERENT root pages "
                       "valid at the same gen -- the publish order is not "
                       "observable)\n",
                       (unsigned long long)rep.slots_ambiguous);
            if (rep.slots_same_root)
                printf("  same-root slots: %llu (both slots name the SAME "
                       "root page at the same gen -- one root, normal after "
                       "a rollback; not damage)\n",
                       (unsigned long long)rep.slots_same_root);
            printf("  bad pages:    %llu\n",
                   (unsigned long long)rep.bad_pages);
            if (rep.quarantined)
                printf("  quarantined:  %llu key range(s) -- a key inside one "
                       "reads EIO, everything else is readable\n",
                       (unsigned long long)rep.quarantined);
            /* The excision gate, in the report as well as on stderr: whether
             * -f dropped a range or refused it is the single fact an operator
             * deciding what to do next needs, and it must never be readable as
             * "repaired" from the verdict line alone. */
            if (rep.excise_refused)
                printf("  excision:     REFUSED for %llu of %llu quarantined "
                       "range(s) -- they still hold keys %llu live inode(s) "
                       "need; asked %llu live row(s)%s\n",
                       (unsigned long long)rep.excise_refused,
                       (unsigned long long)rep.quarantined,
                       (unsigned long long)rep.excise_blocked,
                       (unsigned long long)rep.excise_live,
                       rep.excise_partial ? ", PARTIAL walk" : "");
            printf("  cycles/shared: %llu\n",
                   (unsigned long long)rep.cycles);
            /* WP75: the v3 branch used to return before the v2 free-block
             * line, so free-block accounting was invisible on v3 volumes. */
            printf("  free blocks:  %llu\n",
                   (unsigned long long)vol_free_blocks_cached(v));
            if (rep.reachable_free)
                printf("  reachable-but-free pages: %llu (bitmap divergence)\n",
                       (unsigned long long)rep.reachable_free);
            /* WP118: the namespace accounting. Every line above describes the
             * metadata; this one describes the NAMES -- the check that puts
             * the number of directory entries against the link counts the
             * inode rows claim, which is the only place a volume with two
             * names on one inode (WP111b: one file's content replaced by
             * another's) can be caught. Reported, never repaired: which of
             * the colliding names is the intruder is not decidable from the
             * volume. */
            printf("  names/inodes:  %llu name(s) over %llu live inode(s)\n",
                   (unsigned long long)rep.nlink_names,
                   (unsigned long long)rep.nlink_inodes);
            if (rep.nlink_bad) {
                printf("  nlink/fan-in:  %llu inode(s) DO NOT BALANCE -- "
                       "%llu missing name(s), %llu stale dirent(s), %llu "
                       "name(s) on a dead row (each offender named above)\n",
                       (unsigned long long)rep.nlink_faults,
                       (unsigned long long)rep.nlink_missing,
                       (unsigned long long)rep.nlink_stale,
                       (unsigned long long)rep.nlink_dead);
            }
            if (rep.nlink_orphans)
                printf("  orphan rows:   %llu live inode(s) that no "
                       "directory entry names (reported; not damage, not "
                       "repairable)\n",
                       (unsigned long long)rep.nlink_orphans);
            if (!rep.nlink_bad)
                printf("  nlink/fan-in:  ok (%llu live inode(s), each with "
                       "exactly as many names as its nlink)\n",
                       (unsigned long long)rep.nlink_inodes);
            /* The other half of readability. Every line above describes the
             * tree and the names; this one asks the question the read path
             * asks -- can the content a live row names still be produced?
             * Reported, never repaired: a blob is addressed by the hash of
             * its own contents, so -f has nothing to rebuild it from. */
            if (rep.recipe_partial)
                printf("  live recipes: PARTIAL -- the walk failed, so the "
                       "count is a floor, not a total\n");
            else if (rep.recipe_bad)
                printf("  live recipes: %llu UNREADABLE of %llu live inode(s) "
                        "with content (each offender is named above)\n",
                        (unsigned long long)rep.recipe_bad,
                        (unsigned long long)rep.recipe_checked);
            else
                printf("  live recipes: ok (%llu live inode(s) with content, "
                        "every recipe blob they name loads and parses)\n",
                        (unsigned long long)rep.recipe_checked);
            if (spt0_info(v, NULL))
                printf("  save point:   %s\n",
                       rep.savepoint_bad
                       ? "live, pinning a DAMAGED base tree -- invf-rollback "
                         "refuses it; invf-fsck -f drops it"
                       : "live");
            if (rep.repaired) {
                printf("  repaired:     %llu quarantined range(s) excised; "
                       "%llu key(s) recovered from the delta\n",
                       (unsigned long long)rep.quarantined,
                       (unsigned long long)rep.keys_quarantined);
                if (rep.lost_names)
                    printf("  LOST:         %llu name(s) -- their base page is "
                           "unreadable and the data is NOT recoverable (each "
                           "one is named above)\n",
                           (unsigned long long)rep.lost_names);
                else
                    printf("  LOST:         unrecoverable, and not "
                           "attributable by name: the directory entries that "
                           "named the lost files were inside a quarantined "
                           "range too (see above)\n");
            }
            /* WP99: the two-device mirror verdict, asked of the devices
             * rather than of whatever the mux served the walk. A stale
             * device is named with the signal that saw it, because the
             * consequence is specific: a dev0 that is behind holds a
             * rolled-back root descriptor, and mounting it read-write
             * adopts that generation and drops the delta records past it. */
            if (mresynced)
                printf("  mirror:       dev%d was stale (%s); RESYNCED by this "
                       "pass, both devices now carry the same generation\n",
                       mstale, mwhy ? mwhy : "block 0");
            else if (mstale >= 0)
                printf("  mirror:       dev%d is STALE (%s) -- its block 0 is "
                       "behind the other device's. The tree walked above is "
                       "the newer copy and reads fine, but this volume is NOT "
                       "in sync: a mount that serves dev%d would adopt a "
                       "rolled-back root and drop the writes since. Reattach "
                       "both devices and run `invf-fsck -f %s` to resync "
                       "(newest state wins).\n",
                       mstale, mwhy ? mwhy : "block 0", mstale, img);
            else if (vol_ndev(v) == 2)
                printf("  mirror:       in sync\n");
            /* The verdict line keeps the v2 vocabulary (and the exact strings
             * the e2e gates grep for): OK / DAMAGED / REPAIRED. The exit code
             * is 3 whenever damage was found, REPAIRED or not -- the pass that
             * finds the damage is the one that raises the alarm. What was
             * repaired, and what is gone, is spelled out above.
             * WP99 adds MIRROR STALE, a verdict of its own for a volume whose
             * one device is behind: the tree is walkable, so DAMAGED would be
             * wrong, and OK would be the silent lie this pass exists to stop. */
            if (!rep.damaged && mstale >= 0 && !mresynced)
                printf("MIRROR STALE\n");
            else if (!rep.damaged)
                printf("OK\n");
            else if (rep.repaired)
                printf("REPAIRED\n");
            else
                printf("DAMAGED\n");
            /* WP-J damage ledger: machine-readable per-file damage list.
             * Runs after the normal report without changing it (same scan,
             * same verdict, same exit code): one TAB-separated line per
             * live file the volume cannot read back fully. Empty list +
             * OK verdict means nothing is damaged. A partial enumeration
             * fails closed on stderr and never prints a short list as
             * complete. */
            if (list_damaged) {
                invfs_damaged_file dmg[INVFS_DMG_MAX];
                int nd, k;
                memset(dmg, 0, sizeof dmg);
                nd = vol_damaged_files(v, dmg, INVFS_DMG_MAX);
                if (nd < 0) {
                    fprintf(stderr, "invf-fsck: damaged-partial: the "
                            "damage walk could not complete, so no "
                            "complete list exists\n");
                } else {
                    for (k = 0; k < nd; k++)
                        printf("damaged\t%llu\t%s\t%s\t%llu\n",
                               (unsigned long long)dmg[k].id,
                               dmg[k].name,
                               vol_damaged_kind(dmg[k].kind),
                               (unsigned long long)dmg[k].size);
                }
            }
            (void)degraded;
        }
        vol_close(v);
        return (rep.damaged || (mstale >= 0 && !mresynced)) ? 3 : 0;
    }

    /* WP-M21: CMP0/CMPS retired with online compaction. No descriptor is
     * ever armed, no staging run can be stranded. The fall-through scan
     * (below) is the only thing needed. */

    if (vol_fsck_scan(v, &rep, fix) != 0) {
        fprintf(stderr, "invf-fsck: scan failed\n");
        vol_close(v);
        return 1;
    }

    /* WP20b: layer-2 RS recovery read its stripe map out of the v2 owner
     * records and resolved each member through the L2P journal. Both are
     * gone with format v2, so on this format the pass had nothing to scan:
     * it reported zero stripes of every kind and said nothing. Say so
     * instead of accepting the flag and reporting nothing. */
    if (repair) {
        fprintf(stderr, "invf-fsck: %s: --repair (WP20b layer-2 RS "
                "recovery) is retired with format v2 -- it read its stripe "
                "map from the v2 owner records. This build has no "
                "replacement for it.\n", img);
        vol_close(v);
        return 1;
    }

    issues = (rep.orphans || rep.missing || rep.bad_recs || rep.l2p_miss ||
              rep.cut_records || rep.lost_files || rep.corrupt_files ||
              0);
    if (!quiet) {
        const invfs_superblock *sb = vol_sb(v);
        printf("InvariantFS fsck: %s\n", img);
        printf("  state:        %s\n",
               sb->state == INVFS_STATE_CLEAN ? "CLEAN" :
               sb->state == INVFS_STATE_DIRTY ? "DIRTY" :
               sb->state == INVFS_STATE_RECOVERY ? "RECOVERY" : "UNKNOWN");
        printf("  live files:   %llu\n", (unsigned long long)rep.live_files);
        printf("  l2p entries:  %llu\n", (unsigned long long)rep.l2p_entries);
        printf("  l2p misses:   %llu (AST segments with an invalid pba/extent - data lost)\n",
               (unsigned long long)rep.l2p_miss);
        printf("  cut records:  %llu (torn newest versions, fallback live)%s\n",
               (unsigned long long)rep.cut_records,
               rep.cut_records ? (fix ? " -> quarantined" :
                                  " (use -f to quarantine)") : "");
        printf("  lost files:   %llu (no readable version)%s\n",
               (unsigned long long)rep.lost_files,
               rep.lost_files ? (fix ? " -> quarantined" :
                                 " (use -f to quarantine)") : "");
        if (rep.corrupt_files)
            printf("  corrupt files: %llu (segment CRC failed)%s\n",
                   (unsigned long long)rep.corrupt_files,
                   fix ? " -> quarantined" : "");
        printf("  orphans:      %llu%s\n", (unsigned long long)rep.orphans,
               rep.orphans ? (fix ? " -> freed" : " (use -f to free)") : "");
        printf("  missing:      %llu%s\n", (unsigned long long)rep.missing,
               rep.missing ? (fix ? " -> restored" : " (use -f)") : "");
        printf("  bad records:  %llu\n", (unsigned long long)rep.bad_recs);
        printf("  free blocks:  %llu\n",
               (unsigned long long)vol_free_blocks_cached(v));
        printf("%s\n", issues ? (fix ? "REPAIRED" : "ISSUES FOUND")
                              : "OK");
    }

    vol_close(v);
    return issues ? 3 : 0;
}
