# De-Validation Rollout Across All Branches

## Goal

Propagate exactly one change to every phase branch: **remove all correctness validation from inside `OrderBook`**, and update the related tests/benchmarks so they no longer depend on that validation.

This makes all phase baselines share the same matching-core contract (the gateway owns id validation), so cross-phase performance comparisons become apples-to-apples, as called for in `phase6_engine_handle_refactor_plan.md`.

## Scope: What Is And Is Not Propagated

This is the critical boundary for this rollout.

**Propagate (de-validation only):**

- Duplicate-`order_id` rejection on add (`DuplicateOrderId`).
- Cancel-before-insert handling: the entire `pending_cancel_ids_` mechanism and `PendingCancelExists`.
- The cancel-miss bookkeeping that feeds the pending-cancel set.
- `pending_cancel_count()` accessor and any test/benchmark assertions that depend on the above.

**Do NOT propagate (Phase 6 handle design — stays on `master`/phase6 only):**

- `OrderHandle`, `kInvalidHandle`, `AddResult::handle`.
- `OrderPool::resolve()` / `acquire()`-returns-handle.
- Handle-keyed `cancel_order(OrderHandle)` / `modify_order(OrderHandle)` APIs.
- Removal of the `id_to_order_` cancel index.
- The handle-based benchmark migration (planning-book dual-build, precomputed handles, zero-lookup timed window).

Every other branch **keeps its existing cancel/modify lookup mechanism and its id-keyed API**. The lookup mechanism is functional, not validation:

- `phase1-finale`, `phase2a`: cancel scans the books (no id index) — leave that as is.
- `phase2b`–`phase2e`: cancel uses the branch's `id_to_order_` (custom `HashTable`) — keep it.
- `phase4a`, `phase4-finale-sync`, `phase5-finale`: cancel uses `absl::flat_hash_map id_to_order_` — keep it.

So on these branches `id_to_order_` (or the book scan) survives; only the *validation* built on top of it is removed.

## Why The Reference Commit Is Not Directly Usable

On `master` the de-validation was committed together with the handle refactor in `eaa4eb6`. That commit therefore also deletes `id_to_order_`, introduces `OrderHandle`, and changes the cancel/modify signatures — all of which are out of scope here. **Do not cherry-pick `eaa4eb6` onto any branch**; it would drag the Phase 6 handle design along with it. Use `eaa4eb6` only to read which validation hunks to remove, and hand-apply the de-validation as a small semantic edit on each branch.

## Per-Branch Validation Structure

The validation is implemented slightly differently per branch, so each removal must be located in that branch's actual code. The duplicate-detection construct is the main variable.

| Local branch | Duplicate detection | Cancel-before-insert | Functional cancel lookup (KEEP) |
| --- | --- | --- | --- |
| `phase1-finale` | `active_ids_` set | `pending_cancel_ids_` | book scan |
| `phase2a` | `active_ids_` set | `pending_cancel_ids_` | book scan |
| `phase2b` | per branch (verify) | `pending_cancel_ids_` | `id_to_order_` (HashTable) |
| `phase2c` | `active_ids_` set | `pending_cancel_ids_` | `id_to_order_` (HashTable) |
| `phase2d` | `active_ids_` set | `pending_cancel_ids_` | `id_to_order_` (HashTable) |
| `phase2e` | per branch (verify) | `pending_cancel_ids_` | `id_to_order_` (HashTable) |
| `phase4a` | `id_to_order_.contains` | `pending_cancel_ids_` | `absl id_to_order_` |
| `phase4-finale-sync` | `id_to_order_.contains` | `pending_cancel_ids_` | `absl id_to_order_` |
| `phase5-finale` | `id_to_order_.contains` | `pending_cancel_ids_` | `absl id_to_order_` |

Discover the exact constructs on any branch before editing:

```bash
git switch TARGET
git grep -n -E 'pending_cancel|DuplicateOrderId|PendingCancelExists|active_ids_|id_to_order_|UnknownOrderId' \
  -- core/matching_core core/matching_core/tests benchmark
```

## Branch Inventory and Remote Mapping

Local branch names do not all match their remote (note `phase4-finale-sync` -> `origin/phase4-finale`). Several locals have no upstream set but a same-named remote exists.

| Local branch | Push to remote |
| --- | --- |
| `phase1-finale` | `origin/phase1-finale` |
| `phase2a` | `origin/phase2a` |
| `phase2b` | `origin/phase2b` |
| `phase2c` | `origin/phase2c` |
| `phase2d` | `origin/phase2d` |
| `phase2e` | `origin/phase2e` |
| `phase4a` | `origin/phase4a` |
| `phase4-finale-sync` | `origin/phase4-finale` |
| `phase5-finale` | `origin/phase5-finale` |

`master` is Phase 6 and already has both the de-validation and the handle design; it is not a rollout target.

## Branching Strategy (choose before starting)

Both modes are additive and never require `git push --force`.

- **Mode A — New comparable lineage (recommended).** For each `phaseN`, create `phaseN-devalidated` and apply the change there. Originals stay as records of the old validated contract; the new branches are the comparable lineage. Matches the plan's "preserve historical results / create a new comparable lineage" guidance.
- **Mode B — In-place additive commits.** Commit directly on each existing `phaseN` and push. Literally pushes to the existing branches but changes their meaning. Still safe in git (new commits on top, no history rewrite).

## Transformation Checklist (apply per branch)

Adapt to that branch's structure, per the validation-structure table.

### Engine (`core/matching_core/src/order_book.cpp`, `order_book.hpp`)

- `add_limit_order` / `add_market_order`:
  - Delete the `PendingCancelExists` early-return.
  - Delete the duplicate-id early-return (`active_ids_.contains(...)` or `id_to_order_.contains(...)`).
  - Keep all functional inserts the branch needs for its cancel lookup (e.g. `id_to_order_.emplace(...)`). If the branch used `active_ids_` *only* for duplicate detection, remove the `active_ids_.insert/erase` calls and the member too.
- `cancel_order(order_id)` (keep id-keyed signature):
  - Keep the functional lookup (book scan or `id_to_order_.find`).
  - On miss, delete `pending_cancel_ids_.insert(order_id)`; the function may still return `UnknownOrderId` on a true miss (harmless, off the hot path) or simply do nothing — do not feed a pending set.
- `modify_order(order_id, ...)` (keep id-keyed signature):
  - Delete `pending_cancel_ids_.erase(order_id)` and the cancel-miss-then-erase-pending dance.
  - Keep the find/erase/release + delegate-to-add flow.
- Remove the `pending_cancel_ids_` member and `pending_cancel_count()`. Remove `active_ids_` if it only served duplicate detection.
- `ErrorCode::DuplicateOrderId` / `PendingCancelExists` become unused. Leaving the enum values in place is fine (least churn); just stop returning them. Removing them is optional but then sweep all references.
- Do **not** add `OrderHandle`, do **not** remove `id_to_order_`, do **not** change cancel/modify signatures.

### Tests (`core/matching_core/tests/...`)

- Delete or rewrite cases asserting `DuplicateOrderId`, `PendingCancelExists`, cancel-before-insert behavior, and `pending_cancel_count()`.
- Keep cancel/modify tests on the id-keyed API; they should now treat ids as always-valid.

### Benchmarks (`benchmark/...`)

- These branches keep id-based tracking and id-keyed cancel/modify. No handle migration.
- Only change what the validation removal forces: drop any correctness microbenchmark that exercised duplicate/pending-cancel rejection, and remove assertions tied to the removed error codes.
- The hft/overall benchmarks already generate unique ids and target resting orders, so their core loops should need little or no change.

## Per-Branch Procedure

```bash
# 0. Safety snapshot
git switch TARGET
git tag backup/TARGET-pre-devalidation

# Mode A only:
# git switch -c TARGET-devalidated

# 1. Discover the validation constructs on this branch
git grep -n -E 'pending_cancel|DuplicateOrderId|PendingCancelExists|active_ids_|id_to_order_|UnknownOrderId' \
  -- core/matching_core core/matching_core/tests benchmark

# 2. Hand-apply the de-validation checklist above. Keep id_to_order_/scan + id API.

# 3. Verify (gate — must pass). See below.

# 4. Commit
git add -A
git commit -m "refactor(matching): drop in-core order-id validation (gateway-owned)"

# 5. Push (additive; no force)
git push -u origin TARGET:REMOTE          # Mode B; REMOTE per mapping table
# git push -u origin TARGET-devalidated     # Mode A
```

## Verification Gate (per branch, before pushing)

The branch keeps its own engine, so these are correctness/regression checks, not handle checks.

1. Build the engine, tests, and benchmarks (use that branch's CMake setup; older branches may differ from master's). An ASan build is preferred:

```bash
cmake -S . -B build-asan -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build-asan
```

2. Run the branch's test suite. All green. (Tests asserting the removed validation must already be deleted/rewritten in step 2.)

3. ASan smoke the benchmarks that branch ships, e.g.:

```bash
./build-asan/benchmark/bench_hft_macro --orders 2000 --levels 50 --batch-size 5000 --warmup-iters 1 --iters 2 --seed 123
./build-asan/benchmark/bench_overall   --orders 2000 --levels 50 --batch-size 5000 --warmup-iters 1 --iters 2 --seed 123
```

   Pass criteria: zero ASan reports and a sane `ok` count (no regression versus the branch's pre-change run). Flags use dashes: `--batch-size`, `--warmup-iters`.

## Recommended Execution Order

Do the structurally simplest removals first to settle the recipe, then the rest:

1. `phase5-finale`, `phase4-finale-sync`, `phase4a` — duplicate detection is a single `id_to_order_.contains` line; removal is minimal.
2. `phase2c`, `phase2d`, `phase2b`, `phase2e` — `active_ids_` + `pending_cancel_ids_` over a hash table.
3. `phase1-finale`, `phase2a` — `active_ids_` + `pending_cancel_ids_` over book-scan cancel.

## Push Mapping (exact)

```bash
# Mode B (in-place). Mode A: replace RHS with the *-devalidated branch name.
git push -u origin phase1-finale:phase1-finale
git push -u origin phase2a:phase2a
git push -u origin phase2b:phase2b
git push -u origin phase2c:phase2c
git push -u origin phase2d:phase2d
git push -u origin phase2e:phase2e
git push -u origin phase4a:phase4a
git push -u origin phase4-finale-sync:phase4-finale     # name differs
git push -u origin phase5-finale:phase5-finale
```

## Risks and Mitigations

- **Accidentally importing the handle design.** The single biggest risk. Never cherry-pick `eaa4eb6` or merge `master` into a phase branch; either would pull `OrderHandle` and delete `id_to_order_`. Always hand-apply the de-validation only.
- **Removing a functional lookup by mistake.** `id_to_order_` (and the book scan) is the cancel/modify mechanism, not validation — keep it. Only `active_ids_` (when used solely for duplicate detection) and `pending_cancel_ids_` are validation state to delete.
- **Changing the meaning of published snapshot branches.** Prefer Mode A, or keep the `backup/TARGET-pre-devalidation` tags so originals are restorable.
- **No force-push.** Every step is additive. A non-fast-forward rejection means something is wrong — stop and investigate rather than forcing.
- **Per-branch build drift.** Older branches may lack the gtest folder or use different CMake options; adapt the verification commands to each branch instead of assuming master's layout.

## Done Criteria

For every branch in the inventory: in-core duplicate-id and cancel-before-insert validation removed, id-keyed cancel/modify API and the branch's own lookup mechanism preserved, no `OrderHandle` introduced, tests/benchmarks no longer depend on the removed validation, the verification gate passes, and the branch is pushed to its mapped remote (or its `*-devalidated` counterpart created and pushed). At that point all phase baselines share the gateway-owned-validation contract and are comparable, while the Phase 6 handle design remains exclusive to `master`.
