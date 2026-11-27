# Exit codes: one meaning per code, and the one rule that matters

The rule that matters is not the number. It is:

> **A checker never exits 0 after it has found damage, and never exits 0 for a
> check it did not perform.**

Every code below is a consequence of that sentence. The kind of damage is
always named in the text ("CORRUPT: <name>", "DAMAGED", "damaged<TAB>…"), so a
script that needs the *kind* reads the text; a script that needs "is anything
wrong" tests `!= 0`.

## `invf-verify`, `invf-fsck`, `invf-sweep`, `invf-rollback`

| code | meaning | who returns it |
|---|---|---|
| 0 | clean **at the level that was performed** | all four |
| 1 | the check could not run, or could not complete — **no claim either way** | all four |
| 2 | **content** damage: a file does not read back | `invf-verify --deep`, `invf-verify --list-damaged` |
| 3 | **structure** damage: superblock, zone table, bitmap, parity drift, or a damage verdict that a repair pass could not clear | `invf-verify` (pre-checks, `--verify-seal`), `invf-fsck` verdict `DAMAGED` |

`2` and `3` are different claims and both are non-zero: a volume with one torn
file in `/home` is code 2; a volume whose root tree is unreachable is code 3.
When both are true, structure wins.

## Why `invf-fsck` returns 3 where `invf-verify --list-damaged` returns 2

They answer different questions, and that is deliberate rather than an
inconsistency:

- `invf-fsck` reports a **repair outcome**: `OK` / `DAMAGED` / `REPAIRED`, and
  its code means "damage remains, with or without `-f`". A content tear it
  cannot cut is exactly that, so it is 3.
- `invf-verify --list-damaged` reports a **finding list** and mutates nothing,
  so its code says what was found: content damage, 2.

The lists themselves are identical, byte for byte, because both call
`vol_damaged_ledger()`. `tools/test-contentcut.sh` leg B2 asserts that.

## The level, stated in the verdict

`invf-verify` without `--deep` reads the superblock, the zone table and the
bitmap and **no file content at all**. It therefore exits 0 on a volume that has
an unreadable file — correctly, for the level it performed — and its verdict line
says so:

```
OK: <img> is a valid InvariantFS volume (STRUCTURE ONLY: no file content was read;
    use --deep to read every live file)
```

This sentence is load-bearing. Before it existed, the same sentence was printed
after a full read and after no read at all, and a volume with a torn file got a
bare green from the tool named *verify*. `tools/test-contentcut.sh` leg B1 fails
if the sentence ever disappears, if shallow stops exiting 0 on a structurally
valid volume, or if `--deep` ever calls itself structure-only.

## Unknown filesystems and other tools

`invf-absorb`, `invf-convert`, `invf-import` use the same 0/1/2/3 vocabulary with
`2` reused for "refused by name to act on this input" where no damage exists —
their text always says which, and `mkfs --absorb` refuses rather than half-doing
it. If a tool's code needs a fourth meaning, it gets a fourth code in *this*
table, not a silently reinterpreted 1.
