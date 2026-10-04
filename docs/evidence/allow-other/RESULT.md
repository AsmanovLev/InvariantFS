# WP227 — the `allow_other` experiment

Same volume (`invf-mkfs v.img 2` + `invf-import` of two files), same boot kernel
(`6.12.111+deb13-amd64`), mounted twice. Only the mount option differs.

## Without `-o allow_other`

    root      read : cat: .../probe.txt: Permission denied
    nobody    read : cat: .../probe.txt: Permission denied
    nobody    exec : setpriv: failed to execute .../run.sh: Permission denied
    nobody    ls   : ls: cannot access '.../mnt': Permission denied

## With `-o allow_other`

    root      read : hello-from-invfs
    nobody    read : hello-from-invfs
    nobody    exec : ran-as-nobody
    nobody    ls   : probe.txt run.sh

`nobody` = uid 65534. Mounted as root via `sudo`, so in the first case even the
invoking shell's uid 1000 was refused — the mount was private to uid 0.

## The kernel question

    CONFIG_FUSE_FS=y            CONFIG_FUSE_DAX=y
    CONFIG_FUSE_PASSTHROUGH=y   CONFIG_FUSE_ALLOW_ALL  -- NOT SET
    CONFIG_FUSE_POSIX_ACL       -- NOT SET

`CONFIG_FUSE_ALLOW_ALL` being absent does **not** make `-o allow_other` fail for
root; it gates *unprivileged* users requesting it. Verified by mounting, not by
reasoning — the host mount line came back `...,allow_other,max_read=1048576`.

Note `CONFIG_FUSE_POSIX_ACL=n` is worth its own look: `tools/test-acl.sh`
exercises POSIX ACLs, and a kernel without `FUSE_POSIX_ACL` will not enforce
them. Not investigated here; recorded as a question, not a finding.
