# Why this project

> One page. Everything else (ADRs, WPs, benchmarks) is judged against it.

## The pain the project was born from

**Personal and concrete.** There is a small handful of raw images compressed with zstd/xz,
totaling **no more than a few terabytes**. They need to be **packed onto a single 2 TB disk**
and then lived with: found, read, mounted, updated, rolled back.

Not “we need a filesystem”. What is needed is for these few terabytes to **fit**
and for **nothing to be lost silently** in the process.

## Two requirements, and the second is no less important

1. **Space.** The content of the images largely repeats (the same systems,
   packages, files in different wrappers), but each image is compressed independently — so
   block-level dedupe (ZFS/btrfs/duperemove) does not work on them: it sees different
   compressed bytes of the same thing. What is needed is dedupe **by content through compression**,
   otherwise 2 TB will not be enough.
2. **Confidence.** “There is a risk that something somewhere will go wrong.” This is not distrust
   of the formats — it is not wanting to learn about a loss a year later. So what is needed is not hope,
   but **verifiability**: every file can be checked for bit-exactness, any
   operation — proved, not assumed.

Hence the invariant in the README: `what you write comes back byte-identical`. It is not
an end in itself; it is a way to achieve (2) given that for the sake of (1) we allow ourselves to
**transform** data on disk (compress, decompose, deduplicate).

## What follows from this for priorities

| primary | secondary |
|---|---|
| through-compression: see the content of a `.zst`/`.xz`/qcow2 image and deduplicate it | dynamic extents, size ladder (this is about space, but after dedupe works at all) |
| verifiability: verify/scrub, proven reversibility, no “silent” corruption | speed, profiles, tuning |
| the ability to **answer beforehand** “will it fit” (measurement of unique content) | conveniences, computed views (ADR-013) |
| clarity: remove dead layers and false promises (otherwise there is no confidence) | rewriting in another language |

## The criterion for any decision

> Does this help fit my images into 2 TB **and** be confident that nothing was
> lost? If not — it is secondary, and it has no right to complicate the primary.
