# WP-M1-decisions — recommendations for `design-meta-v3.md` §18 open decisions

**Status:** planning record; input to WP-M1 and the first engine WPs.
**Scope:** recommendation + rationale only. Where the design doc is silent,
that is stated rather than invented.

---

## D1 — Delta representation

**Recommendation: append-log + in-memory index** (replay at mount).

Rationale: §2–§4 already describe the delta as "a single coalescing stream
keyed by namespace key, with an in-memory index", replayed at mount, and
§16 lists "delta append/index" as a unit test. A persisted on-disk tree
would duplicate the base B+-tree and its COW/publish machinery for the
recent tier, and §11 rejects a single in-place tree precisely to keep the
base immutable and reads lock-free. The cost of the recommendation is mount
O(delta), which §7 accepts and bounds via fold cadence; the persisted-tree
alternative buys O(1) mount at the cost of a second tree — not justified
for the deployment profile (§14, read-mostly/archive).

**Risk to track:** delta bounding (mount latency, §17). Fold must keep the
log small enough that replay stays bounded; this is a fold-trigger concern
(D2).

---

## D2 — Fold trigger

**Recommendation: delta size/age threshold, with the sweep as a secondary
trigger.** Concretely: fold when the delta exceeds a byte threshold or a
record count, or when its oldest record exceeds an age; the sweep also
requests a fold.

Rationale: §5 says the trigger is "delta size/age threshold, or the
sweep", so this recommendation follows the doc rather than inventing one.
Bounding is required for the D1 mount-latency risk and for reclaim (§8).
The design doc does **not** give numeric thresholds; this decisions doc
deliberately does not invent them — the implementing WP must pick them
from measured replay latency and record them in its WP doc.

---

## D3 — Base page size

**Recommendation: 4 KiB** (the §12 default), with the 16 KiB variant left
as a measured follow-up.

Rationale: §12 says "default 4 KiB; 16 KiB variant evaluated". 4 KiB
matches `INVFS_BLOCK_SIZE` (`invarifs.h:20`), so the page ↔ block mapping
is 1:1 and the WP-M2 allocator and existing block-layer idioms apply
without a sub-block addressing scheme. WP-M1's RT30 descriptor already
carries `page_size`, so a later 16 KiB switch is a mount-parsed field, not
a structural change. The design doc gives **no measurement** favouring
16 KiB; choosing it now would be speculative.

---

## D4 — Reclaim mechanism

**Recommendation: reachability diff against the single save point**
(mark from the current base root and from the pinned save-point base root;
free base pages reachable from neither; free delta segments below the
save-point `delta_end`).

Rationale: §8 says base pages unreachable from the current base and not
pinned by a save point are freed, "with a single save point this is a
reachability diff … or epoch-based retention — no general refcount tree".
Because §6 allows exactly **one** save point at a time, the pinned set is
one extra root, so the diff needs one extra bitmap and no global refcount
graph. Epoch retention needs a retained-set registry comparable to today's
`\x01reten` mechanism (`volume.h:555`), which §10 deletes; the diff keeps
reclaim refcount-free, matching §8 and §16's "refcount-free reclaim
(reachability) checks".

**Risk to track:** §17 flags "reclaim without refcounts"; the diff must be
re-run after each fold and must not free a page still referenced by an
in-flight reader of the old base (§9 says readers drain naturally — the
implementing WP must define when an old base's pages become freeable).

---

## D5 — Save-point shape

**Recommendation: `{base_root, delta_end}`** (the §6/§12 shape).

Rationale: §6 specifies exactly `{base_root, delta_end}` and §12 the
descriptor `{base_root, delta_end, flags}`. A delta-position-only save
point cannot survive a fold: after a fold the delta is reset and the
pre-fold state lives only in the pinned base, so `delta_end` alone would
roll back to an empty delta against the wrong base. The pinned base root is
what makes rollback-after-fold well-defined.

**Note:** the design doc is silent on how a save point interacts with a
**second** fold while pinned (the first fold's pre-fold base is replaced
by a newer one). §6 permits folds while a save point is live and says the
pre-fold base is retained; the implementing WP must make the pinned root
the one captured **at save-point creation**, not the latest pre-fold root.

---

## D6 — Recovery claim timing (not requested, recorded for completeness)

The design doc is explicit (§7.4): the power-loss claim is **not** made
until `tools/test-flakey.sh` crash-injection soak passes. No decision is
needed here beyond deferring the claim.
