# ADR-006: Universal Delta-Lossless Decomposition

## Context

InvariantFS guarantees an absolute **Bit-Exactness Invariant**: every byte written to the filesystem must be reproduced byte-for-byte upon read, cat, or export.

Historically, InvariantFS has relied on two strategies when dealing with already-compressed formats (GZIP, ZIP, PNG, QCOW2 with compressed clusters, JXL, etc.):

1. **Re-encode with exact reproduction** (`gzrepro.c` / `vol_create_gz_file`): Attempts to brute-force compressor parameters (`level`, `memLevel`, `windowBits`, `strategy`) across a discrete grid. If a bit-exact stream matches, only parameters are saved.
2. **Refuse / Store Verbatim**: If a format uses arbitrary compression implementations (such as `zlib-ng`, `libdeflate`, custom Huffman trees, or QCOW2 with `QCOW_OFLAG_COMPRESSED`), parameter brute-force fails. InvariantFS historically **declined decomposition** (falling back to storing the entire monolithic file in RAW or generic ZSTD).

Storing already-compressed archives as monolithic files creates severe penalties:
- **No deduplication:** Content inside the archive cannot be deduplicated against identical uncompressed files or other container instances.
- **Shannon entropy wall:** High-entropy compressed streams compress poorly under generic ZSTD (typically 0% to 3% savings).
- **Inability to perform nested decomposition:** Inner filesystems (e.g. `ext4fs` or partitions inside a `qcow2`) cannot be traversed.

## Decision: Delta-Lossless Decomposition

Instead of choosing between binary verbatim storage or risky non-exact re-compression, InvariantFS introduces **Universal Delta-Lossless Compression**:

1. **Member Stream (Canonical Decoded Data):**
   - The container or stream is fully decompressed into canonical, uncompressed member bytes (`diskimg` for QCOW2, extracted members for ZIP/GZ).
   - The uncompressed member bytes are stored, deduplicated, and compressed using modern, high-ratio algorithms (e.g., ZSTD-19, PPMd, BCJ).

2. **Deterministic Baseline Re-encoder:**
   - A standardized, deterministic reference compressor (e.g. standard `zlib` at a canonical level/window configuration) re-compresses the member data upon rebuild.

3. **Compact Delta Patch in Recipe:**
   - The diff between the original compressed stream and the deterministic re-compressed stream is computed.
   - For formats where the structure differs (different Huffman tree or match selection), the recipe stores a compact byte patch or bit-stream delta.
   - For cases where the stream is arbitrary or proprietary, the recipe carries either a delta patch or the original stream as recipe gap bytes, while exposing the uncompressed member for deduplication and nested analysis.

4. **Rebuild Invariant:**
   - Upon rebuild or reading the container:
     $$\text{Original Stream} = \text{DeterministicRecompress}(\text{DecodedMember}) \oplus \text{RecipeDelta}$$
   - Or, when stored via MRMP seekable maps:
     The member data is exposed for random I/O and deduplication, while the exact byte-level container layout is guaranteed by the recipe mapping.

## Consequences

- **Bit-exactness is mathematically guaranteed:** The reconstruction reproduces 100% identical SHA-256 hashes.
- **Containers are unlocked:** `qcow2` images with compressed clusters, compressed tarballs, and custom archives can now be fully decomposed into guest disk images and inner filesystems.
- **Cross-VM deduplication enabled:** Identical OS libraries and files inside different compressed VMs can now be deduplicated.
