# InvariantFS — Video Overview Script (draft for review)

Audience: mid-technical. Knows what a filesystem is (files, folders,
mounts). Vague on: content addressing, dedup, codecs, savepoints.
Target length: ~6 minutes. Narration EN with RU subtitles file
(`subs_en.srt` / `subs_ru.srt` rendered from the lines below).

---

## S1 — Hook (0:00–0:30)

**On screen:** black → a photo pixelating into garbage, then snapping
back to perfect. Title card: "WHAT YOU WRITE COMES BACK BYTE-IDENTICAL."

**Say:** "Every filesystem promises to keep your files. Almost none of
them check. Bit rot, a bad cable, a codec that 'improves' your archive
— and one day a file comes back... almost right. Almost right is wrong.
InvariantFS is built around one guarantee: what you write comes back
byte-identical. Every byte. Every time. And it can prove it."

## S2 — The problem (0:30–1:30)

**On screen:** split screen — left: document saved; right: same document
with one flipped byte highlighted, checksum FAIL badge.

**Say:** "Here's the enemy. Disks lie a little — a flipped bit here and
there. Compression lies more: most filesystems compress to save space
and trust the codec. If the codec has a bug, your data is gone and
nobody tells you. Backups copy the corruption. The damage compounds
silently, for years."

## S3 — The answer: content + recipe (1:30–3:00)

**On screen:** animated diagram — a file splits into colored segments;
each segment gets a hash label; a 'recipe' card lists
range → hash → codec. Reading: recipe points at segments, segments
reassemble into the file.

**Say:** "InvariantFS doesn't store files. It stores *content* — every
unique chunk of bytes, hashed, deduplicated — plus a small *recipe* per
file: which chunks, in which order, through which codec. Two identical
photos share one physical copy. Reading a song's tags never decodes the
audio — the recipe jumps straight to the bytes you asked for. And every
transcode is proven: compress, decompress, compare. If the round trip
isn't bit-exact, the bytes stay verbatim. No trust required."

## S4 — How it works: write path (3:00–4:30)

**On screen:** three zones light up in sequence — WRITE lands in RAW
(LZ4 flash) → delta entry appended → background SWEEP drains RAW into
Shadow (ZSTD, dedup merge) → fold merges metadata. Savepoint flag
planted before the sweep; rollback arrow rewinds.

**Say:** "Writes land fast, in a staging area, with light compression.
The catalogue update is appended to a log — crash-safe by construction.
Then, offline, the sweep does the heavy work: stronger compression,
deduplication across the whole volume, tidy metadata. Before every
sweep, the system plants a savepoint — one command rolls the entire
volume back to before the sweep ran. Delete a file? Its blocks are
retired and freed at the next fence — nothing is ever half-gone."

## S5 — Packs, seal, status (4:30–5:30)

**On screen:** codec pack cards slotting in (FLAC, TAR, PNG, qcow2);
parity shield closing over disk blocks; terminal showing install +
verify.

**Say:** "Special formats get special packs — FLAC audio, TAR archives,
disk images — installed from a signed registry, verified on install.
And for the paranoid, native parity seal: Reed-Solomon groups over your
data, scrubbable, healable. Today InvariantFS is in testing: releases
are published, CI is green across engine, end-to-end and soak tiers.
Not an ext4 replacement — an archive that proves itself."

## S5b — Benchmarks (5:30–6:00, PLACEHOLDER — numbers pending)

**On screen:** slate card: three bars animating to measured values +
table (corpus → InvFS vs tar|zstd). Timing reserved 30 s.

**Say (timing track):** "And the numbers are honest numbers: on a
30 MB mixed corpus — photos, sources, binaries — the stored data is
0.95× the original; tar with zstd gets 1.14×. Small volumes pay a
fixed metadata overhead — it amortizes as the volume grows. Text
batches compress 2.8×, and identical segments are stored once, however
many files name them. Full tables are linked below."

> MEASURED 2026-10-12 (`tools/bench-fs.sh`, 39 files, 31.1 MB raw):
> tar|zstd-19 = 27.4 MB (1.14×); invfs data = 32.8 MB (0.95×);
> image allocated = 77.6 MB (0.40× — 32 MB fixed metadata dominates
> this tiny corpus); TEXT batches 5.1 → 1.8 MB (2.8×). Sweep re-encoded
> 0 files (small-file lanes skipped) — the honest Bit-Exact story: no
> proven transcode, no shrink claimed. Dedup micro-bench (3× duplicated
> corpus) MEASURED 2026-10-12 (same 31 MB corpus ×4 = 124 MB logical,
> 156 files): RAW 98.2 MiB logical → 26.1 MiB physical = 3.76× (four
> identical copies share one physical copy); TEXT 20.6 → 2.2 MiB.
> Overall 0.55× logical/on-disk (fixed metadata + 91.8 MiB unclaimed —
> the documented one-sweep-lag, reclaimed by the next sweep).

## S6 — CTA (6:00–6:30)

**On screen:** install one-liner typing itself; links: releases, guides,
registry. End card.

**Say:** "One line installs it. Your bytes come back — or the volume
tells you exactly what it can't prove. That's the whole pitch:
a filesystem that would rather admit a gap than hand you a lie."
