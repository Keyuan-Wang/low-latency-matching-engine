# llmes (archived)

**This monorepo is archived.** Active development continues in two independent repositories that were cloned from this tree with **full commit history** preserved:

| Repo | URL | Role |
|---|---|---|
| **llmes-orderbook** | https://github.com/Keyuan-Wang/llmes-orderbook | Pure matching engine / order book (no networking) |
| **llmes-gateway** | https://github.com/Keyuan-Wang/llmes-gateway | epoll + SPSC + binary order-entry protocol |

## What remains here

- [`PROJECT_HISTORY.md`](PROJECT_HISTORY.md) — full experiment log (Phase 1–14)
- [`server_results/`](server_results/) — remote benchmark artifacts
- Historical source tree as of the split point (`6443454`)

Do not treat this repo as the place to land new matching-core or gateway work. Use the sibling repos above.

## Split notes

- Both new repos share the same history up to `6443454`, then each has one forward “split” commit that drops the other half.
- There is **no** dependency between `llmes-orderbook` and `llmes-gateway`.
- Original readme content describing the combined system is preserved in git history and partially restated in each sibling README.
