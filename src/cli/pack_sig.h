/* pack_sig.h — pack manifest signatures v1 (WP203).
 *
 * Trust model, in one paragraph: a codecpack is code the daemon executes,
 * and `invfs-pack install` fetches it from wherever the operator points
 * it. v1 signs the MANIFEST (which carries an `integrity` line binding
 * every helper file's bytes), verifies at INSTALL time, refuses a BAD
 * signature always, and prompts on a MISSING one. Load-time re-verify is
 * deliberately absent: the daemon loads from host roots and volume images
 * it already trusts, and re-checking there would break every unsigned
 * in-tree pack for no new guarantee (see the install-gate justification
 * in docs/guides/CLI-USAGE.md).
 *
 * File formats (all text, all strict — see pack_sig.c):
 *   <pack>/manifest.sig   `sig-ed25519 <pk-hex64> <sig-hex128>\n` (one line)
 *   <keyring>/<name>.pub  `ed25519-pub <pk-hex64>\n`
 *   <keyring>/<name>.sec  `ed25519-sec <sk-hex128>\n` (created 0600)
 *   manifest line         `integrity = <blake3-hex64>`
 *
 * Keyring location (exactly one default + one override, no more):
 *   $INVFS_KEYRING, else $HOME/.config/invfs/keys
 */
#ifndef INVFS_PACK_SIG_H
#define INVFS_PACK_SIG_H

#include <stddef.h>

/* Verify outcome: OK = signed and trusted; UNSIGNED = no manifest.sig;
 * BAD = refuse (bad sig, tampered bytes, malformed envelope, or no
 * trusted key). BAD is sticky: no flag overrides it. */
typedef enum {
    PACK_SIG_OK = 0,
    PACK_SIG_UNSIGNED = 1,
    PACK_SIG_BAD = 2
} pack_sig_status;

/* Verify the pack directory at <packdir> against <keyring_dir>.
 * err (>=256 bytes recommended) carries the human reason on BAD.
 * Returns PACK_SIG_* above. Never prompts, never installs. */
pack_sig_status pack_sig_verify_dir(const char *packdir,
                                    const char *keyring_dir,
                                    char *err, size_t errcap);

/* Sign <packdir> in place with the secret key in <secfile>:
 * recompute the integrity binding, rewrite the manifest, then write
 * manifest.sig. 0 on success, -1 with err set on any failure. */
int pack_sig_sign_dir(const char *packdir, const char *secfile,
                      char *err, size_t errcap);

/* Generate <name>.pub + <name>.sec (0600) under <keyring_dir>.
 * 0 on success, -1 with err set. */
int pack_sig_keygen(const char *keyring_dir, const char *name,
                    char *err, size_t errcap);

/* Resolve the keyring: $INVFS_KEYRING, else $HOME/.config/invfs/keys.
 * 0 on success, -1 with err set (e.g. HOME unset). */
int pack_sig_default_keyring(char *out, size_t outcap,
                             char *err, size_t errcap);

/* Prompt hook for the missing-signature question. Returns nonzero iff
 * the operator answered yes. The stdio implementation reads one line
 * from stdin (EOF/closed = no). Unit tests inject scripted answers. */
typedef int (*pack_sig_prompt_fn)(const char *packname, void *ctx);
int pack_sig_prompt_stdio(const char *packname, void *ctx);

/* Install gate. Composition with the existing flags (locked WP203):
 *   BAD sig          -> refuse (rc 1), whatever the flags say.
 *   missing + --skip -> proceed silently-ish (one stderr note).
 *   missing + -y     -> proceed with an UNSIGNED warning.
 *   missing + -n     -> dry-run: report the unsigned state, install
 *                       nothing (rc 0). -n never installs, so "-n aborts"
 *                       holds by construction.
 *   missing, interactive -> prompt `Install unsigned pack ...? [y/N]`;
 *                       anything but y* aborts (rc 1); closed stdin
 *                       aborts too, naming the skip flag.
 * auto_yes = -y given; dry_run = -n given; skip_flag =
 * --skip-signature-verification given. err carries the refusal reason.
 * Returns 0 (proceed) or 1 (refuse). */
int pack_sig_gate_install(const char *packdir, const char *packname,
                          const char *keyring_dir,
                          int auto_yes, int dry_run, int skip_flag,
                          pack_sig_prompt_fn prompt, void *ctx,
                          char *err, size_t errcap);

/* Recompute the helper-content binding of <packdir> (sorted walk over
 * every regular file except manifest and manifest.sig — both are covered
 * directly by ed25519 over the manifest's exact bytes; symlinks hash as
 * link:<relpath>-><target> without being followed). hex_out must hold
 * 65+ bytes. 0 on success, -1 with err set. */
int pack_sig_integrity_compute(const char *packdir,
                               char hex_out[65],
                               char *err, size_t errcap);

#endif /* INVFS_PACK_SIG_H */
