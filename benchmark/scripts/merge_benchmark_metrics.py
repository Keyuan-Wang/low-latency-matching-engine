#!/usr/bin/env python3
import math
import os
from pathlib import Path

import pandas as pd

root = Path(__file__).resolve().parents[2]
res = root / "benchmark" / "results"

prefix = os.getenv("OUT_PREFIX", "benchmark")
lat_path = Path(os.getenv("LAT_CSV", str(res / f"{prefix}_latency_raw_trials.csv")))
pmc_path = Path(os.getenv("PMC_CSV", str(res / f"{prefix}_pmc_raw_trials.csv")))
raw_out = Path(os.getenv("MERGED_RAW_OUT", str(res / f"{prefix}_merged_raw_trials.csv")))
agg_out = Path(os.getenv("MERGED_AGG_OUT", str(res / f"{prefix}_merged_agg.csv")))

lat = pd.DataFrame()
pmc = pd.DataFrame()
if lat_path.exists() and lat_path.stat().st_size > 0:
	lat = pd.read_csv(lat_path)
if pmc_path.exists() and pmc_path.stat().st_size > 0:
	pmc = pd.read_csv(pmc_path)

key_cols = [
	"scenario",
	"version_tag",
	"commit_sha",
	"trial_id",
	"orders",
	"levels",
	"batch_size",
	"warmup_iters",
	"iters",
	"seed",
]

if not lat.empty and not pmc.empty:
	raw = lat.merge(pmc, on=key_cols, how="inner", suffixes=("_lat", "_pmc"))
	for col in ("mode_lat", "mode_pmc"):
		if col in raw.columns:
			raw = raw.drop(columns=[col])
	raw["mode"] = "combined"
elif not lat.empty:
	raw = lat.copy()
elif not pmc.empty:
	raw = pmc.copy()
else:
	raise RuntimeError("Neither latency nor pmc CSV contains data")

if "cache_misses_per_op" in raw.columns:
	raw["bytes_per_op_proxy"] = 64.0 * raw["cache_misses_per_op"]
	raw["ops_per_byte_proxy"] = 1.0 / raw["bytes_per_op_proxy"].replace(0.0, pd.NA)
raw.to_csv(raw_out, index=False)

def t_critical_95(n: int) -> float:
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

group_keys = [
	"scenario",
	"version_tag",
	"commit_sha",
	"orders",
	"levels",
	"batch_size",
	"warmup_iters",
	"iters",
	"seed",
]
metric_cols = [
	"avg_ns", "p50_ns", "p95_ns", "p99_ns", "ops_s",
	"cycles_per_op", "instructions_per_op", "branches_per_op",
	"branch_misses_per_op", "cache_misses_per_op", "cpi", "branch_miss_rate",
	"bytes_per_op_proxy", "ops_per_byte_proxy",
]
metric_cols = [m for m in metric_cols if m in raw.columns]

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

agg = pd.DataFrame(rows).sort_values(["scenario", "version_tag", "orders", "levels", "batch_size"])
agg.to_csv(agg_out, index=False)

print(f"raw merged -> {raw_out}")
print(f"aggregated -> {agg_out}")
