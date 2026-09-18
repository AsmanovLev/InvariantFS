# WP54 — configure-guest.sh FUSE-portable (landed)

Branch: wp/54-provisioner (commit 9dfe8e5, squash-merged).

`tools/configure-guest.sh` now provisions a host-side FUSE mount without
rename-based operations: in-place rewrites instead of `sed -i` (FUSE rename
fails), `chown -R root:root` + modes for ssh material (host-FUSE files are
owned by uid 1000 and sshd StrictModes rejects them), runlevel symlinks via
python (no rename), ssh host keys generated host-side and copied in,
idempotent make.conf/shadow/inittab edits.

Verified with a scratch two-device volume + FUSE mount: shadow `root::`,
sshd/dhcpcd symlinks, host keys, ttyS0 getty, ownership/modes — 20/20
assertions; `make test` 4722/0; `test-meta-extent-walk` 6/0.
TODO: end-to-end QEMU boot confirmation of the provisioned volume.
