#!/usr/bin/env python3
import pandas as pd
from pathlib import Path
import math

root = Path(__file__).resolve().parents[2]
res = root / "bench" / "results"

lat = pd.read_csv(res / "phase1_baseline_raw_trials.csv")
pmc = pd.read_csv(res / "phase1_pmc_raw_trials.csv")

key_cols = ["trial_id", "scenario", "orders", "levels", "warmup_iters", "iters", "seed"]
raw = lat.merge(pmc, on=key_cols, how="inner", suffixes=("_lat", "_pmc"))

# keep one mode column to avoid confusion
if "mode_lat" in raw.columns:
    raw = raw.drop(columns=["mode_lat"])
if "mode_pmc" in raw.columns:
    raw = raw.drop(columns=["mode_pmc"])
raw["mode"] = "combined"

# derived proxy metrics from per-op counters
raw["bytes_per_op_proxy"] = 64.0 * raw["llc_miss_per_op"]
raw["ops_per_byte_proxy"] = 1.0 / raw["bytes_per_op_proxy"].replace(0.0, pd.NA)

raw_out = res / "phase1_merged_raw_trials.csv"
raw.to_csv(raw_out, index=False)

def t_critical_95(n: int) -> float:
    # two-sided 95% CI critical values
    table = {
        2: 12.706, 3: 4.303, 4: 3.182, 5: 2.776, 6: 2.571, 7: 2.447, 8: 2.365,
        9: 2.306, 10: 2.262, 11: 2.228, 12: 2.201, 13: 2.179, 14: 2.160,
        15: 2.145, 16: 2.131, 17: 2.120, 18: 2.110, 19: 2.101, 20: 2.093,
        25: 2.060, 30: 2.042,
    }
    if n <= 1:
        return float("nan")
    if n in table:
        return table[n]
    if n > 30:
        return 1.96
    nearest = max(k for k in table.keys() if k < n)
    return table[nearest]

group_keys = ["scenario", "orders", "levels", "warmup_iters", "iters", "seed"]
metric_cols = [
    "avg_ns", "p50_ns", "p95_ns", "p99_ns", "ops_s",
    "cycles_per_op", "instructions_per_op", "branches_per_op",
    "branch_misses_per_op", "llc_load_misses_per_op", "llc_store_misses_per_op",
    "cache_misses_per_op", "cpi", "branch_miss_rate", "llc_miss_per_op",
    "bytes_per_op_proxy", "ops_per_byte_proxy"
]

rows = []
for keys, g in raw.groupby(group_keys, dropna=False):
    row = {k: v for k, v in zip(group_keys, keys)}
    n = len(g)
    row["trials"] = n
    tc = t_critical_95(n)

    for m in metric_cols:
        s = pd.to_numeric(g[m], errors="coerce").dropna()
        if s.empty:
            row[f"{m}_mean"] = float("nan")
            row[f"{m}_std"] = float("nan")
            row[f"{m}_cv"] = float("nan")
            row[f"{m}_ci95_low"] = float("nan")
            row[f"{m}_ci95_high"] = float("nan")
            continue

        mean = float(s.mean())
        std = float(s.std(ddof=1)) if len(s) > 1 else 0.0
        se = std / math.sqrt(len(s)) if len(s) > 1 else 0.0
        half = (tc * se) if len(s) > 1 and math.isfinite(tc) else 0.0
        cv = (std / mean) if mean != 0.0 else float("nan")

        row[f"{m}_mean"] = mean
        row[f"{m}_std"] = std
        row[f"{m}_cv"] = cv
        row[f"{m}_ci95_low"] = mean - half
        row[f"{m}_ci95_high"] = mean + half

    rows.append(row)

agg = pd.DataFrame(rows).sort_values(["scenario", "orders", "levels"])
agg_out = res / "phase1_merged_agg.csv"
agg.to_csv(agg_out, index=False)

print(f"raw merged -> {raw_out}")
print(f"aggregated -> {agg_out}")