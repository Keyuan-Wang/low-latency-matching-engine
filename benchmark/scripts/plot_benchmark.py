#!/usr/bin/env python3
import os
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

root = Path(__file__).resolve().parents[2]
res = Path(os.getenv("RESULTS_DIR", str(root / "benchmark" / "results")))
plot_dir = Path(os.getenv("PLOT_OUT_DIR", str(res / "plots")))
plot_dir.mkdir(parents=True, exist_ok=True)

prefix = os.getenv("OUT_PREFIX", "benchmark")
agg_path = Path(os.getenv("AGG_CSV", str(res / f"{prefix}_merged_agg.csv")))
df = pd.read_csv(agg_path)
if df.empty:
	raise RuntimeError(f"{agg_path.name} is empty")

x_col = os.getenv("X_COL", "orders")
group_col = os.getenv("GROUP_COL", "scenario")
metrics = [m.strip() for m in os.getenv("PLOT_METRICS", "p99_ns,ops_s,cpi,cache_misses_per_op").split(",") if m.strip()]
logx = os.getenv("LOGX", "1") == "1"

scenario_filter = os.getenv("SCENARIOS", "").strip()
if scenario_filter:
	keep = {s.strip() for s in scenario_filter.split(",") if s.strip()}
	df = df[df["scenario"].isin(keep)]

version_filter = os.getenv("VERSIONS", "").strip()
if version_filter:
	keep = {v.strip() for v in version_filter.split(",") if v.strip()}
	df = df[df["version_tag"].isin(keep)]

if df.empty:
	raise RuntimeError("No rows left after filtering")

level_filter = os.getenv("PLOT_LEVEL", "").strip()
if level_filter:
	df = df[df["levels"] == int(level_filter)]
if df.empty:
	raise RuntimeError("No rows left after PLOT_LEVEL filtering")

plt.style.use("seaborn-v0_8-whitegrid")
plt.rcParams.update({
	"figure.dpi": 220,
	"axes.titlesize": 12,
	"axes.labelsize": 10,
	"legend.fontsize": 9,
	"lines.linewidth": 2.2,
	"lines.markersize": 7.0,
})

def plot_metric(metric: str) -> None:
	mean_col = f"{metric}_mean"
	low_col = f"{metric}_ci95_low"
	high_col = f"{metric}_ci95_high"
	if mean_col not in df.columns:
		return

	fig, ax = plt.subplots(figsize=(10.0, 6.0))
	for group_name, g in df.groupby(group_col):
		gg = g.sort_values(x_col)
		y = gg[mean_col]
		yerr = None
		if low_col in gg.columns and high_col in gg.columns:
			yerr = (gg[high_col] - gg[low_col]) / 2.0
		label = str(group_name)
		if "version_tag" in gg.columns:
			version = str(gg["version_tag"].iloc[0])
			label = f"{label}:{version}"
		ax.errorbar(gg[x_col], y, yerr=yerr, marker="o", capsize=4, label=label)

	if logx:
		ax.set_xscale("log")
	ax.set_xlabel(x_col)
	ax.set_ylabel(metric)
	ax.set_title(f"{metric} vs {x_col}")
	ax.grid(True, which="both", alpha=0.3)
	ax.legend(loc="best", frameon=True)
	fig.tight_layout()
	fig.savefig(plot_dir / f"{metric}_vs_{x_col}.png")
	plt.close(fig)

for metric in metrics:
	plot_metric(metric)

print(f"plots -> {plot_dir}")
