# Contributing to InvariantFS

Thanks for your interest. This document covers the two things worth knowing
before you open a pull request: the licensing terms for contributed code, and
the one technical rule that is not negotiable.

---

## Licensing and the relicensing grant

InvariantFS is released under **GPL-2.0-only**. Contributions are accepted under
that license.

In addition, by submitting a contribution you confirm that:

1. You are the author of the contribution, or you otherwise have the right to
   submit it under these terms.
2. You grant the project owner a **perpetual, worldwide, irrevocable,
   royalty-free right to relicense your contribution under any terms**,
   including permissive licenses such as MIT or Apache-2.0, and including
   proprietary licenses.
3. You retain copyright in your contribution. This grant is a license to the
   project owner, not a transfer of ownership.

**Why this exists.** Under plain GPL-2.0, every contributor holds copyright in
their patches, so relicensing the project later would require tracking down and
getting agreement from every single one — which in practice freezes the license
permanently. This grant keeps the option open. It is the same mechanism a CLA
provides, stated inline rather than as a separate signed document.

If you are not comfortable with clause 2, say so in your pull request. A
contribution can still be accepted GPL-2.0-only; it just means that code becomes
a constraint on any future relicensing, and it may be kept isolated or declined
on that basis.

### Sign-off

Every commit must carry a `Signed-off-by` line certifying the above:

```
git commit -s -m "your message"
```

which appends:

```
Signed-off-by: Your Name <your.email@example.com>
```

Use a real name and a reachable email address.

---

## The invariant is not negotiable

InvariantFS exists to guarantee one property:

> **What is written comes back bit-for-bit identical.**

No optimization, however large the space saving, is worth weakening this. A
change that makes reconstruction *probably* correct is a change that will be
rejected.

Concretely, if your pull request touches a codec, a transcode, or the AST/recipe
path:

- **`tests.ps1` must stay green.** All 72 assertions. It is the regression net
  for the invariant and there is no exception for "obviously safe" changes.
- **A new transcode must verify itself.** Compress, decompress, compare against
  the original with BLAKE3, and fall back to storing the original when the
  comparison fails. Look at how `FLACR`, `TARR`, `GZR`, and `PNGR` handle this
  in `src/volume.c` — every one of them has an explicit bail-out path, and yours
  needs one too.
- **Never widen a guard to make a test pass.** If a bit-exactness check is
  failing, the transform is wrong, not the check.
- **Containers keep their original bytes.** Members are byte-range windows into
  the stored original, never re-serialized copies. This is what keeps archive
  checksums and signatures intact.

Please add a test for new functionality. Numbering scheme and conventions are in
`doc/18-test-coverage.md` §8.

---

## Practical notes

- Match the surrounding style: C99, no compiler-specific attributes in
  `invarifs.h` (it must build under both MSVC and GCC), 4-space indent.
- On-disk structures are little-endian and `#pragma pack(1)`. Any change to a
  struct in `src/invarifs.h` is a format change — call it out explicitly in the
  PR description, since it may invalidate existing images.
- Don't use PowerShell's `>` redirect for binary output in tests; it corrupts
  bytes. Use `cmd /c` or `[IO.File]::ReadAllBytes`.
- The design documents in `doc/` are working documents and some describe intended
  rather than implemented behaviour. `doc/18-test-coverage.md` is the most
  accurate picture of what actually exists.

## Reporting bugs

For anything touching the invariant — a file that does not read back identical —
please include the input file if you can share it, the exact tool invocation,
and the output of `invf-verify --deep`. Those reports get priority over
everything else.
