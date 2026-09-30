/* vol_repair.c — WP20b layer-2 (RS) repair, invf-fsck --repair.
 *
 * The pass itself is gone. It reconstructed CRC-failed shadow segments
 * from RS(32+m2, 32) parity by walking the live v2 inode records and
 * resolving each member through the L2P journal; both of those are
 * format-v2 structures, retired with the format. On this build the pass
 * had nothing to scan -- it reported zero stripes of every kind, which
 * is indistinguishable from "the repair succeeded".
 *
 * `invf-fsck --repair` therefore refuses and says why (src/cli/fsck.c)
 * rather than accepting the flag and reporting nothing.
 */
