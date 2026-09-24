# Benchmark Corpus — Ubuntu LTS base cloud images

Reproducible, checksum-verified corpus for the InvariantFS VM-image / QCOW2
benchmark. Every image is an **official, unmodified Ubuntu cloud image**: its
bytes are traceable to a published URL and to that release's own `SHA256SUMS`
file, so a third party can re-download and confirm the exact bytes the results
were measured on.

- **Assembled:** 2026-09-24
- **Host:** Fedora Linux `7.2.5-200.fc44.x86_64`, XFS over `/dev/sdb1`
- **Local corpus dir:** `/run/media/user/a3277147-c47a-4b90-a3ec-6536c5d8c724/invfs-bench-corpus/`
- **Manifest next to the images:** `SHA256SUMS` + `fetch.sh` (same directory)

---

## 1. The three images

| # | File | Ubuntu | Build serial | File size (bytes) | Virtual | SHA256 (verified) |
|---|------|--------|--------------|------------------:|--------:|-------------------|
| 1 | `ubuntu-22.04-server-cloudimg-amd64.img` | 22.04.5 LTS (jammy) | `20260913` | 735,388,672 | 2.2 GiB | `9144540e8af7637d258b50dbabe82ce1aa6752c9574fedfb048270da0e087899` |
| 2 | `ubuntu-24.04-server-cloudimg-amd64.img` | 24.04.4 LTS (noble) | `20260826` | 624,829,952 | 3.5 GiB | `d0fe84bb5f80853425fa6be28e2c106f30104c3cfe8611933f2e65c9b63f0e30` |
| 3 | `ubuntu-26.04-server-cloudimg-amd64.img` | 26.04.1 LTS (resolute) | `20260918` | 864,411,136 | 3.5 GiB | `4908fb59ccd4e87ae4e8e973b7ef56f535448eacb24a87fd787270c0048987bc` |

All three are qcow2 with internal zlib compression (`qemu-img check`: no errors).

---

## 2. Sources and published checksums

Each `SHA256` above is the value in the corresponding upstream `SHA256SUMS`
(verified locally with `sha256sum -c`):

| File | Image URL | Published SHA256SUMS |
|------|-----------|----------------------|
| 22.04 | <https://cloud-images.ubuntu.com/releases/22.04/release/ubuntu-22.04-server-cloudimg-amd64.img> | <https://cloud-images.ubuntu.com/releases/22.04/release/SHA256SUMS> |
| 24.04 | <https://cloud-images.ubuntu.com/noble/20260826/noble-server-cloudimg-amd64.img> | <https://cloud-images.ubuntu.com/noble/20260826/SHA256SUMS> |
| 26.04 | <https://cloud-images.ubuntu.com/releases/26.04/release/ubuntu-26.04-server-cloudimg-amd64.img> | <https://cloud-images.ubuntu.com/releases/26.04/release/SHA256SUMS> |

Provenance cross-checks (read from inside each image):

- `/etc/cloud/build.info` → `serial` matches the table (`20260913`, `20260826`,
  `20260918`).
- `/etc/os-release` → `Ubuntu 22.04.5 LTS`, `Ubuntu 24.04.4 LTS`,
  `Ubuntu 26.04.1 LTS`.

**24.04 note.** `releases/24.04/release/` is a moving target: it currently
points at build `20260911` (`sha256 612b2c0c…`). The corpus deliberately pins
the dated build `20260826` (`sha256 d0fe84bb…`) so the bytes are immutable.

---

## 3. Excluded image (why the corpus is not the old one)

The pre-existing `megapolos-installer/vm/images/ubuntu2204-base.qcow2`
(735,051,776 B, `sha256 bc56dd7e…`) was **not** used:

- its internal `build.info` says `serial 20260829`, but
- the official 20260829 image is 734,966,784 B / `sha256 46c966c6…`, and
- the file is 84,992 B larger and hashes differently.

It was modified after download (repacked), so it cannot be traced to a published
checksum. The corpus uses a fresh, verifiable download instead. Its sibling
`ubuntu2404-base.qcow2` was pristine (`sha256 d0fe84bb…` = official 20260826) and
is byte-identical to corpus image #2.

---

## 4. Reproduce

```bash
cd /run/media/user/a3277147-c47a-4b90-a3ec-6536c5d8c724/invfs-bench-corpus
bash fetch.sh          # downloads from the URLs above, then runs sha256sum -c
```

Expected output ends with three `OK` lines. `fetch.sh` is idempotent and resumes
partial downloads (`curl -C -`).

---

## 5. Status

**Benchmark not yet run.** The InvariantFS side is blocked on the current fix
wave — the e2e helper link lists (WP74) and the Meta-v3 sweep transcode/batch
parity gap — because the benchmark exercises the sweep and qcow2 containerpack
paths. Corpus is ready and frozen; results will be appended here (or in
`QCOW2-COMPRESSION-BENCHMARK.md`) once the fixes land.

> Reproducibility posture: the images and their checksums give **exact**
> byte-level provenance. Timings/RSS in the eventual results are
> **approximate** (single host, shared XFS-over-USB volume); the space/ratio
> numbers are deterministic for a fixed image set and code revision.
