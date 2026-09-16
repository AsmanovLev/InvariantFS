# WP<N>-<slug> — <one-line title>

**Branch:** `wp/<N>-<slug>`
**Worktree:** `/tmp/invfs-wp<N>`
**Severity:** <HIGH | MEDIUM | LOW | N/A>
**Source:** <AUDIT finding #X | INCIDENTS.md entry | user report | …>
**Estimated effort:** <hours>

---

## Scope

<One paragraph: what this WP changes.>

<Bugs / fixes, one paragraph each. Use Bug A, Bug B, … so the patch
commits can reference them.>

---

## Bug A — <name>

**File:** `<path>`
**Function:** `<name>`
**CWE:** <CWE-NNN if applicable>

<Description of the bug, with a code excerpt if it helps.>

**Fix:**

```c
// pseudocode or actual patch
```

---

## Bug B — <name>

**File:** `<path>`

...

---

## Validation

1. **Unit tests:** `make test` — must pass.
2. **Crafted inputs:** `<command>` — must return error, not crash.
3. **Fuzz:** `make fuzz` — must run N iterations cleanly.
4. **E2E:** `<which gate>` — listed in Coordination notes below.

---

## Deliverables

- Patch series (one commit per bug).
- New test file `<path>` (if any).
- `<INCIDENTS.md | AUDIT.md | README.md>` update: <what changes>.

---

## Out of scope (do NOT touch)

- <list, so reviewers don't ask "did you also fix X?">

---

## Coordination notes

- Subagent ID for this WP: `wp<N>-<slug>`
- Pass via `INVFS_E2E_AGENT=wp<N>-<slug>` when invoking e2e.
- E2E gates to run:
  - `bash tools/run-e2e.sh tools/test-XXX.sh`
- Dependencies: <other WPs that must land first, or "none">.