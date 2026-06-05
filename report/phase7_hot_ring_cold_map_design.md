# Phase 7: Hot Ring Buffer + Cold Map Design

## Motivation

The Phase 6a window-isolated `perf record` shows `add_limit_order` consuming 55.3% of all RunOp cycles, with `std::map`'s `get_or_create` (`lower_bound` + `try_emplace` + RB-tree rebalance) accounting for 28%. This is the single largest hot-path bottleneck.

Phase 7 replaces `std::map` on the near-best hot path with an O(1) ring-buffer index, while retaining a `std::map` cold path for out-of-window prices.

## Overall Structure

```
CachedSideBook<IsAsk>
├── RingBuffer<IsAsk> hot_     16-slot circular array covering [best, best±15]
└── std::map<price, unique_ptr<PriceLevel>, PriceCompare<IsAsk>> cold_
```

`CachedSideBook` exposes the same interface as the original `SideBook`: `empty()`, `best_price()`, `best_level()`, `get_or_create(price)`, `erase_best()`. The matching logic in `OrderBook` requires no changes.

### Ownership Model

- Each `PriceLevel` is held by a `std::unique_ptr` and exists in exactly one location at any time: either a ring slot or a cold map node.
- Migration (evict / promote) uses `std::move` to transfer ownership. The pointee address never changes, so `Order::parent_level` remains valid across migrations.
- Per-level heap allocation frequency (one `new` on creation, one `delete` on destruction) matches the `std::map<price, PriceLevel>` baseline, ensuring a fair benchmark comparison. Allocator optimization is deferred to Phase 8.

## RingBuffer

### Data Layout

```cpp
struct Slot {
    int64_t                price;   // kNoPrice when slot is vacant
    unique_ptr<PriceLevel> level;   // nullptr when slot is vacant
};

int64_t   best_price_;     // cached best price
size_t    anchor_;         // physical slot index of the best price
MaskType  live_mask_;      // bit i set iff slots_[i] is occupied
array<Slot, 16> slots_;
```

`MaskType` is selected at compile time via a `uint_from_size<RingSize>` trait to match the ring width exactly: `RingSize=16` maps to `uint16_t`, `RingSize=32` to `uint32_t`, etc. This eliminates the need for a separate validity mask (`kValid`) — every bit in `live_mask_` is a meaningful slot bit, and standard bit-rotation functions like `std::rotr` operate at the correct width natively.

16 slots × 24 bytes = 384 bytes, plus anchor / best_price / live_mask totals roughly 406 bytes — fits entirely within L1 cache.

### Three Core Primitives

**`rank(price)`**: directed distance from best to price. 0 = at best, positive = worse direction, negative = strictly better than best (triggers re-anchor). Ask side: `price - best_price_`. Bid side: `best_price_ - price`.

**`in_hot_window(r)`**: whether rank `r` falls in `[0, RingSize)`. Implemented as `(uint64_t)r < RingSize` — a negative `r` casts to a huge unsigned value, so one comparison rejects both `r < 0` and `r >= RingSize`.

**`idx_of(r)`**: maps rank to a physical slot index. `(anchor_ + (size_t)r) & kMask`. Because RingSize is a power of two, negative ranks wrap correctly via two's-complement: `(size_t)(-d) & 15 == (16 - d) % 16`.

### Mutating Operations

| Operation | Semantics | State updated |
|---|---|---|
| `insert(idx, price, level)` | Install a new level at slot | write price + move level + set live bit |
| `remove(idx)` | Destroy an empty level in place | set price to kNoPrice + reset level + clear live bit |
| `evict(idx)` | Transfer level ownership to caller | set price to kNoPrice + exchange level to nullptr + clear live bit |

All three operations use `static_cast<MaskType>(1) << idx` for bit manipulation, ensuring the shift width matches `live_mask_` exactly and avoids implicit widening/truncation.

All three operations maintain the guarantee: **a non-live slot always has `price == kNoPrice`**. This is the implementation basis of Invariant 3.

### `next_live_offset()`

Starting from anchor, finds the offset of the next live slot in the "worse" direction. Returns -1 if the ring is empty.

Implementation: right-rotate `live_mask_` by `anchor_` positions (bringing the anchor bit to bit 0), then `countr_zero`. Because `MaskType` is exactly `RingSize` bits wide, `std::rotr` performs a natural N-bit rotation — no manual shift-and-or, no validity mask:

```cpp
auto rotated = std::rotr(live_mask_, static_cast<int>(anchor_));
return rotated ? std::countr_zero(rotated) : -1;
```

Earlier versions used `uint64_t` for `live_mask_`, which required a hand-rolled RingSize-bit rotation and a `kValid` mask to strip stray high bits. The `MaskType` approach eliminates both.

## CachedSideBook

### Three Invariants

#### Invariant 1: Hot–Cold Strict Ordering

> For all live hot prices `p_h` and all cold prices `p_c`: `rank(p_h) < rank(p_c)`.

Every live hot price is strictly more aggressive than every cold price.

**Consequence**: as long as `live_mask_ != 0`, the global best is in the ring. `best_price()` and `best_level()` read the anchor slot directly. The bit-scan in `erase_best` finds the true global next-best without consulting the cold map.

**Maintained by**:
- `reanchor_to()`: the `d` evicted wrap-around slots have rank ≥ RingSize in the new frame, worse than all remaining hot slots (rank ∈ [0, RingSize−d)) — ordering holds after insertion into cold.
- `cold_get_or_create()`: only reached for rank ≥ RingSize, so the inserted cold price is always worse than every current hot price.
- `erase_best()` cold-best promotion: the promoted price was the most aggressive in cold; all remaining cold prices are still worse — ordering holds.

#### Invariant 2: Cold Prices Lie Strictly Outside the Hot Window

> For all prices `p` in `cold_`: `in_hot_window(rank(p)) == false`, i.e. `rank(p) ≥ RingSize`.

**Consequence**: `get_or_create` cannot find the same price in both hot and cold simultaneously. Same-price level splitting is impossible.

**Maintained by**:
- `cold_get_or_create()`: only called when rank ≥ RingSize; the inserted price satisfies the invariant immediately.
- `reanchor_to()` eviction: evicted slots have rank ≥ RingSize in the new frame (they were pushed out of the window); they satisfy the invariant upon cold insertion.
- `erase_best()`: after removing the best and advancing the anchor, the window slides toward the "worse" direction. Cold prices that previously lay outside the window may now fall inside it. The trailing `promote_cold()` loop pulls every such price back into the ring until `cold_.begin()` is outside the window again.

This is the most critical maintenance point for Invariant 2. Without the promote step, `get_or_create` would create a new hot level at a price that already exists in cold — causing same-price level splitting, which leads to under-fills and price-time priority inversion.

#### Invariant 3: Live Bit ↔ Slot Content Consistency

> Bit `i` of `live_mask_` is set iff `slots_[i].price != kNoPrice` and `slots_[i].level != nullptr`.

Equivalently: **a non-live slot always has `price == kNoPrice`**.

**Consequence**: the hit test `slot.price == query_price` in `get_or_create` is safe and unambiguous. A vacant slot has `price == kNoPrice`, which cannot equal any legitimate price, so false positives are impossible. After a re-anchor, non-evicted slots need no additional validation because their stored prices remain correct in the new frame (see the re-anchor section for the proof).

**Maintained by**: `insert`, `remove`, and `evict` are the only writers of slot state; each one atomically updates all three components (price, level, live bit).

### Lemma: Empty Hot ⟹ Empty Cold

Cold grows only through two paths: `cold_get_or_create` (requires a defined best, i.e. non-empty hot) and `reanchor_to` eviction (requires non-empty hot). Meanwhile, `erase_best` promotes cold-best to hot whenever the ring empties but cold is non-empty. Therefore "hot empty, cold non-empty" is an unreachable state.

**Consequence**: the `hot_.empty()` branch of `get_or_create` (empty-book first insertion) does not need to inspect the cold map.

### `get_or_create(price)` — Primary Hot Path

```
hot_.empty()?  ──yes──>  set_anchor(0, price), materialize(0, price)
       │no
  r = rank(price)
  in_hot_window(r)?  ──yes──>  idx = idx_of(r)
       │no                      slot.price == price?  ──yes──>  return slot.level (hit)
       │                              │no
       │                        materialize(idx, price) (vacant slot, create level)
  r < 0?  ──yes──>  reanchor_to(price)
       │no
  cold_get_or_create(price)
```

Instruction budget for the hit case: `sub` (rank) → `cmp` (in_hot_window) → `add`+`and` (idx_of) → `load price` → `cmp` → `load level.get()` → ret. Approximately 8–10 instructions, 2 loads, 2 predictable branches. No malloc, no hash, no tree walk.

### `erase_best()` — Cold Path

Precondition: the best level has already been drained by the matching loop.

**Phase 1: bit-scan for next best**

```
remove(anchor)               // destroy the empty best level, clear live bit
loop:
    off = next_live_offset()
    off < 0?  ──yes──>  cold empty? ──yes──> reset(), return (fully empty)
         │                    │no
         │              promote cold-best as new anchor, break
    set_anchor(idx, slot[idx].price)
    slot[idx].level->empty()?  ──no──>  break (found true next best)
         │yes
    remove(idx)              // ghost-empty level, clean up and keep scanning
```

Non-live slots between the old and new anchor need no processing: by Invariant 3 their price is kNoPrice, so they cannot interfere with the new frame. This is asymmetric with re-anchor handling — re-anchor must evict wrap-around slots, but erase_best's forward scan passes harmlessly over vacant slots.

**Phase 2: promote-on-advance**

After the window slides toward the "worse" direction, the most aggressive cold price may now fall inside the new window. The loop promotes cold entries until `cold_.begin()` is outside the window again. In the typical case, the cold head is well beyond the window and the loop exits after a single comparison.

### `reanchor_to(price)` — Better Price Arrives

`d = -rank(price)`: the new price is `d` ticks better than the current best.

**d ≥ RingSize (large jump)**: the entire ring is invalidated. `flush_all_to_cold()` evicts all live slots, then the anchor is reset.

**d < RingSize (common case, usually d = 1)**:

The `d` slots that must be evicted are the **wrap-around slots** — those at the far (worst) end of the old window that fall outside the new window.

Proof for the ask side:

- Old window covers prices `[best, best+15]`
- New window covers `[best−d, best+15−d]`
- Prices no longer in window: `[best+16−d, best+15]`, exactly `d` prices
- Their physical ring indices: `(anchor + RingSize − k) & kMask` for k = 1..d

The remaining **N−d non-wrap-around slots** need no processing. Their stored prices remain correct in the new frame because anchor movement only changes the starting point of `idx_of`, not the meaning of already-stored prices. This is the correctness foundation of the "lazy except O(1) boundary" design.

The new best occupies the k=d position. That slot is evicted first (if live), then materialized with the new level.

Ghost-empty levels (drained by cancels but still live in the ring) are `remove`d directly during eviction — they do not enter cold.

### Cancel Path — Zero Additional Overhead

`cancel_order` does not touch `CachedSideBook`. It locates the level via `Order::parent_level`, calls `PriceLevel::erase`, and releases the pool slot.

A cancel may drain a level to empty without cleaning up the ring slot (creating a "ghost-empty" level). This is safe:

- The matching loop detects the ghost via `best_level().empty()` and calls `erase_best()` to clean up.
- A ghost's price is at least as aggressive as the true next best. If the ghost price does not satisfy the crossing condition, the true next best certainly does not either — `break` is correct. If the ghost price does satisfy crossing, the inner loop finds the level empty, falls through to `erase_best()`, which cleans up the ghost and continues matching.

Therefore cancel does not need to modify the ring's `live_mask_` or slot state, maintaining O(1) with zero overhead from the ring structure.

## Hot Path Cost Comparison

| Operation | Share | std::map baseline | ring + cold |
|---|---|---|---|
| `get_or_create` (in-window hit) | ~48% | `try_emplace`: tree walk + rebalance | `rank` + `idx_of` + 1 price comparison |
| `best_price()` / `best_level()` | matching loop | `begin()->first/second` | field / array read |
| `cancel_order` | ~46% | does not touch SideBook | does not touch SideBook |
| `erase_best` | ~2% | `erase(begin())`: tree rebalance | bit-scan + promote |
| `get_or_create` (out of window) | ~3% | same as baseline | `std::map` (off hot path) |
