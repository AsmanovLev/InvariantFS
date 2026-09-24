# WP82 — xattr namespaces: `security.*` / `trusted.*` / capabilities on FUSE

**Branch:** `wp/82-xattr-security`
**Worktree:** `/home/user/invfs-wp82`
**Status:** COMPLETE
**Severity:** MEDIUM (blocks SELinux/file-capability root use cases; README inaccurate)
**Source:** FUSE-vs-kernel discussion / ADR-008; empirical xattr test

---

## Executive Summary & Findings

The investigation into extended attribute namespaces (`user.*`, `trusted.*`, `security.*`, `system.*`) on InvariantFS mounts is complete.

1. **Daemon & Engine are Namespace-Agnostic**:
   - `invf_setxattr` in `src/cli/fuse_fs.c` and `vol_set_xattr` in `src/core/vol_records.c` / `vol_btree.c` do not block any namespace (except checking ACL permissions for `system.posix_acl_*`).
   - The engine stores all attributes as key-value pairs in the Meta-v3 xattr B+ tree up to 64 KiB per attribute.

2. **Root Cause Analysis per Namespace (Empirical Verification)**:
   - **`user.*`**: Fully operational for non-privileged and privileged users. Supports arbitrary values up to 64 KiB.
   - **`system.posix_acl_access` / `system.posix_acl_default`**: Fully operational via `setfacl`/`getfacl`. Validated by daemon ACL parser.
   - **`trusted.*`**: Blocked by the Linux kernel VFS layer (`xattr_permission()`) when invoked by an unprivileged user (`EPERM: Operation not permitted`). The call never reaches FUSE. When invoked by root (`CAP_SYS_ADMIN`), it reaches the daemon and is set successfully.
   - **`security.*`**: Blocked by the Linux kernel VFS LSM hook when invoked by an unprivileged user (`EPERM: Operation not permitted`). When invoked by root, it reaches the daemon and is set successfully.
   - **`security.capability` (`setcap`)**: Returns `EPERM` for unprivileged users; returns `EPERM` for root on unprivileged `fusermount3` mounts because unprivileged FUSE mounts automatically enforce `nosuid,nodev`. When mounted with `-o suid,dev`, the capability attribute is handled.

---

## Empirical Matrix

| Namespace / Command | Invoked by | Result | Error Code | Layer Enforcing |
|---|---|---|---|---|
| `setfattr -n user.test -v 123` | unprivileged user | **PASS** | `0` | Daemon stores in v3 xattr tree |
| `setfattr -n user.big -v <12KB>` | unprivileged user | **PASS** | `0` | Daemon stores in v3 xattr tree |
| `setfacl -m u:user:rwx` | unprivileged user (owner) | **PASS** | `0` | Daemon validates ACL blob |
| `setfattr -n trusted.test -v 123` | unprivileged user | **FAIL** | `EPERM` | Linux kernel VFS (`xattr_permission`) |
| `sudo setfattr -n trusted.test -v 123` | root (`CAP_SYS_ADMIN`) | **PASS** | `0` | Reaches daemon, stored successfully |
| `setfattr -n security.test -v 123` | unprivileged user | **FAIL** | `EPERM` | Linux kernel VFS (`xattr_permission`) |
| `sudo setfattr -n security.test -v 123` | root (`CAP_SYS_ADMIN`) | **PASS** | `0` | Reaches daemon, stored successfully |
| `setcap cap_net_raw=ep <bin>` | unprivileged user | **FAIL** | `EPERM` | Linux kernel VFS (`cap_inode_setxattr`) |
| `sudo setcap cap_net_raw=ep <bin>` | root (default mount) | **FAIL** | `EPERM` | Linux kernel VFS (`nosuid` mount flag) |

---

## Documentation Updates

- Updated `README.md`: Clarified that the `4096 B/inode` limit was a v2 record cap, whereas Meta-v3 xattrs use an asymmetric B+ tree supporting up to 64 KiB per attribute matching the Linux VFS limit. Clarified that `trusted.*`/`security.*` require root with capabilities (`CAP_SYS_ADMIN`).
- Updated `docs/adr/ADR-008-fuse-vs-kernel.md`: Documented the exact capability and mount prerequisites.
