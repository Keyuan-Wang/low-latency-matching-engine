#!/usr/bin/env python3
"""
Version-aware benchmark comparison plots.

Reads the aggregated CSV (produced by merge_benchmark_metrics.py) and
generates comparison charts showing how performance evolved across
different version_tag values for each scenario and metric.

Plot types:
	1. Line chart  — for each (scenario x metric), plot the metric against
	 the x-axis variable (default: orders), with one line per version_tag.
	 CI error bars are shown when available.
	2. Bar chart   — for each scenario, at a specific (orders, levels) config,
	 plot all metrics as a grouped bar per version_tag. Metrics are
	 normalised relative to the first version so the direction of change
	 is immediately visible.

Usage (env vars):
	AGG_CSV       Input aggregated CSV path (default: benchmark/results/*_merged_agg.csv)
	SCENARIOS     Comma-separated scenario filter (default: all)
	PLOT_METRICS  Comma-separated metrics to plot (default: p99_ns,ops_s,cpi,cache_misses_per_op)
	X_COL         X-axis column (default: orders)
	PLOT_LEVEL    Filter by levels value (default: first available)
	FIXED_ORDERS  For the bar chart, fix orders to this value (default: none)
	PLOT_OUT_DIR  Output directory for PNG files (default: benchmark/results/plots)
	LOGX          Use log x-axis (default: 1)
"""

import os
import math
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import numpy as np


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

root = Path(__file__).resolve().parents[2]
res = Path(os.getenv("RESULTS_DIR", str(root / "benchmark" / "results")))
plot_dir = Path(os.getenv("PLOT_OUT_DIR", str(res / "plots")))
plot_dir.mkdir(parents=True, exist_ok=True)

prefix = os.getenv("OUT_PREFIX", "benchmark")
agg_path = Path(os.getenv("AGG_CSV", str(res / f"{prefix}_merged_agg.csv")))

df = pd.read_csv(agg_path)
if df.empty:
	raise RuntimeError(f"{agg_path.name} is empty")

# Supported metrics (latency + PMC) and their display names / units
METRIC_META = {
	"p99_ns":         {"label": "p99 latency",      "unit": "ns",       "lower_better": True},
	"p95_ns":         {"label": "p95 latency",      "unit": "ns",       "lower_better": True},
	"p50_ns":         {"label": "p50 latency",      "unit": "ns",       "lower_better": True},
	"avg_ns":         {"label": "Avg latency",      "unit": "ns",       "lower_better": True},
	"ops_s":          {"label": "Throughput",       "unit": "ops/s",    "lower_better": False},
	"cpi":            {"label": "CPI",              "unit": "",         "lower_better": True},
	"cycles_per_op":  {"label": "Cycles/op",        "unit": "",         "lower_better": True},
	"instructions_per_op": {"label": "Instructions/op", "unit": "",     "lower_better": False},
	"branches_per_op":     {"label": "Branches/op",  "unit": "",        "lower_better": False},
	"branch_misses_per_op": {"label": "Branch misses/op", "unit": "",  "lower_better": True},
	"cache_misses_per_op": {"label": "Cache misses/op", "unit": "",    "lower_better": True},
	"branch_miss_rate":    {"label": "Branch miss rate", "unit": "",   "lower_better": True},
}

# Metrics available in this dataset (intersection of METRIC_META with actual columns)
base_cols = set(c.rsplit("_", 1)[0] for c in df.columns if c.endswith("_mean"))
available_metrics = sorted(base_cols & set(METRIC_META.keys()))

target_metrics = os.getenv("PLOT_METRICS", "").strip()
if target_metrics:
	wanted = [m.strip() for m in target_metrics.split(",") if m.strip()]
	plot_metrics = [m for m in wanted if m in available_metrics]
else:
	plot_metrics = available_metrics

if not plot_metrics:
	raise RuntimeError(f"No valid metrics found. Available: {available_metrics}")

# Scenario filter
scenario_filter = os.getenv("SCENARIOS", "").strip()
if scenario_filter:
	keep = {s.strip() for s in scenario_filter.split(",") if s.strip()}
	df = df[df["scenario"].isin(keep)]
if df.empty:
	raise RuntimeError("No rows left after scenario filtering")

# Levels filter
level_filter = os.getenv("PLOT_LEVEL", "").strip()
if level_filter:
	df = df[df["levels"] == int(level_filter)]
elif "levels" in df.columns:
	# Pick the most common level
	level_filter = str(df["levels"].mode().iloc[0])
	df = df[df["levels"] == int(level_filter)]

# X-axis column
x_col = os.getenv("X_COL", "orders")
if x_col not in df.columns:
	x_col = "orders"  # fallback

logx = os.getenv("LOGX", "1") == "1"

# Fixed orders for bar chart
fixed_orders = os.getenv("FIXED_ORDERS", "").strip()

# Autodetect versions
versions = sorted(df["version_tag"].unique())
n_versions = len(versions)

if n_versions < 1:
	raise RuntimeError("No version_tag values found")

# ---------------------------------------------------------------------------
# Style
# ---------------------------------------------------------------------------

colours = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd",
		   "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf"]
markers = ["o", "s", "D", "^", "v", "p", "*", "h", "X", "<"]

plt.style.use("seaborn-v0_8-whitegrid")
plt.rcParams.update({
	"figure.dpi": 200,
	"axes.titlesize": 13,
	"axes.labelsize": 11,
	"legend.fontsize": 9,
	"lines.linewidth": 2.0,
	"lines.markersize": 6.5,
	"figure.figsize": (10.0, 6.0),
})


def ci_error(gg: pd.DataFrame, metric: str) -> pd.Series | None:
	"""Return symmetric CI half-width, or None if CI columns are missing."""
	low = f"{metric}_ci95_low"
	high = f"{metric}_ci95_high"
	if low in gg.columns and high in gg.columns:
		raw = (gg[high] - gg[low]) / 2.0
		return raw.where(raw.notna(), 0.0)
	return None


# ---------------------------------------------------------------------------
# 1. Line charts: metric vs x_col, one line per version_tag
# ---------------------------------------------------------------------------

def make_line_plots() -> list[Path]:
	files: list[Path] = []

	for scenario, sg in df.groupby("scenario", sort=True):
		for metric in plot_metrics:
			mean_col = f"{metric}_mean"
			if mean_col not in sg.columns:
				continue

			fig, ax = plt.subplots()
			for i, (ver, vg) in enumerate(sorted(sg.groupby("version_tag"))):
				gg = vg.sort_values(x_col)
				x = gg[x_col]
				y = gg[mean_col]
				yerr = ci_error(gg, metric)
				colour = colours[i % len(colours)]
				marker = markers[i % len(markers)]

				ax.errorbar(x, y, yerr=yerr, fmt=f"-{marker}",
							color=colour, capsize=3, label=ver, alpha=0.9)

			meta = METRIC_META.get(metric, {})
			ylabel = meta.get("label", metric)
			unit = meta.get("unit", "")
			if unit:
				ylabel = f"{ylabel} ({unit})"

			ax.set_xlabel(x_col)
			ax.set_ylabel(ylabel)
			ax.set_title(f"{scenario}: {meta.get('label', metric)} vs {x_col}")

			if logx:
				ax.set_xscale("log")
			if metric in ("ops_s",) or "ns" in metric:
				ax.set_yscale("log")

			ax.grid(True, which="both", alpha=0.3)
			ax.legend(loc="best", frameon=True, title="version")
			fig.tight_layout()

			path = plot_dir / f"{scenario}_{metric}_vs_{x_col}.png"
			fig.savefig(path)
			plt.close(fig)
			files.append(path)

	return files


# ---------------------------------------------------------------------------
# 2. Bar chart: side-by-side versions at a fixed (orders, levels) config
# ---------------------------------------------------------------------------

def make_bar_chart() -> list[Path]:
	if not fixed_orders:
		return []

	bar_df = df[df[x_col] == int(fixed_orders)].copy()
	if bar_df.empty:
		print(f"  (no data for {x_col}={fixed_orders}, skipping bar chart)")
		return []

	files: list[Path] = []

	for scenario, sg in bar_df.groupby("scenario", sort=True):
		if "levels" in sg.columns:
			sg = sg[sg["levels"] == int(level_filter)]
		if sg.empty:
			continue

		# Find metrics with at least one non-NA value
		active_metrics = []
		for m in plot_metrics:
			mc = f"{m}_mean"
			if mc in sg.columns and sg[mc].notna().sum() >= 1:
				active_metrics.append(m)

		if not active_metrics:
			continue

		fig, axes = plt.subplots(
			nrows=1, ncols=len(active_metrics),
			figsize=(5.0 * len(active_metrics), 4.5),
			squeeze=False,
		)

		for ax, metric in zip(axes[0], active_metrics):
			mc = f"{metric}_mean"
			low = f"{metric}_ci95_low"
			high = f"{metric}_ci95_high"

			# Sort versions deterministically
			rows = sg.sort_values("version_tag")
			x_pos = np.arange(len(rows))
			ver_labels = rows["version_tag"].tolist()
			values = rows[mc].values

			has_ci = low in rows.columns and high in rows.columns
			yerr = None
			if has_ci:
				yerr_lo = rows[mc].values - rows[low].values
				yerr_hi = rows[high].values - rows[mc].values
				yerr = np.vstack([yerr_lo, yerr_hi])
				# Replace NaN errors with 0
				yerr = np.nan_to_num(yerr)

			bar_colours = [colours[i % len(colours)] for i in range(len(rows))]
			bars = ax.bar(x_pos, values, yerr=yerr, capsize=4,
						  color=bar_colours, width=0.55, edgecolor="white", linewidth=0.5)

			# Value labels on top of bars
			for bar, val in zip(bars, values):
				if not np.isnan(val):
					ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
							f"{val:.3g}" if abs(val) < 1e6 else f"{val:.3e}",
							ha="center", va="bottom", fontsize=7)

			meta = METRIC_META.get(metric, {})
			ylabel = meta.get("label", metric)
			unit = meta.get("unit", "")
			if unit:
				ylabel = f"{ylabel} ({unit})"
			ax.set_ylabel(ylabel)
			ax.set_title(meta.get("label", metric))
			ax.set_xticks(x_pos)
			ax.set_xticklabels(ver_labels, fontsize=8)
			ax.grid(True, axis="y", alpha=0.3)

		fig.suptitle(f"{scenario} (orders={fixed_orders}, levels={level_filter})",
					 fontsize=14, y=1.02)
		fig.tight_layout()

		path = plot_dir / f"{scenario}_bar_vs_{x_col}.png"
		fig.savefig(path, bbox_inches="tight")
		plt.close(fig)
		files.append(path)

	return files


# ---------------------------------------------------------------------------
# 3. Summary: percent change heatmap  (version_n vs version_0)
# ---------------------------------------------------------------------------

def make_heatmap() -> list[Path]:
	if n_versions < 2:
		return []

	files: list[Path] = []

	for scenario, sg in df.groupby("scenario", sort=True):
		base = sg[sg["version_tag"] == versions[0]]
		if base.empty:
			continue

		rows = []
		for ver in versions[1:]:
			ver_df = sg[sg["version_tag"] == ver]
			if ver_df.empty:
				continue
			row = {"version": ver}
			for m in plot_metrics:
				mc = f"{m}_mean"
				if mc not in sg.columns:
					continue
				bv = base[mc].values
				vv = ver_df[mc].values
				if len(bv) and len(vv) and not (np.isnan(bv).all() or np.isnan(vv).all()):
					b_mean = np.nanmean(bv)
					v_mean = np.nanmean(vv)
					if b_mean != 0:
						row[m] = (v_mean - b_mean) / abs(b_mean) * 100.0
			rows.append(row)

		if not rows:
			continue

		hm = pd.DataFrame(rows).set_index("version")
		# Drop all-NaN columns
		hm = hm.dropna(axis=1, how="all")
		if hm.empty:
			continue

		# Keep only requested plot metrics
		keep_cols = [c for c in hm.columns if c in plot_metrics]
		hm = hm[keep_cols]
		if hm.empty:
			continue

		# Rename columns for display
		label_map = {m: METRIC_META.get(m, {}).get("label", m) for m in hm.columns}
		hm = hm.rename(columns=label_map)

		n_rows = len(hm)
		n_cols = len(hm.columns)
		cell_w = 1.8
		cell_h = 0.95
		fig, ax = plt.subplots(figsize=(max(6, n_cols * cell_w + 2), max(2.5, n_rows * cell_h + 1.2)))
		im = ax.imshow(hm.values, cmap="RdYlGn_r", aspect="auto", vmin=-20, vmax=20)

		ax.set_xticks(range(n_cols))
		ax.set_xticklabels(hm.columns, fontsize=8, rotation=25, ha="right")
		ax.xaxis.tick_top()
		ax.set_yticks(range(n_rows))
		ax.set_yticklabels(hm.index, fontsize=9)

		# Border around cells so adjacent similar-colour cells are distinguishable
		for r in range(n_rows):
			for c in range(n_cols):
				ax.add_patch(plt.Rectangle((c - 0.5, r - 0.5), 1, 1,
				                           fill=False, edgecolor="white", lw=1.2))

		# Annotate cells
		for r in range(n_rows):
			for c in range(n_cols):
				val = hm.values[r, c]
				if not np.isnan(val):
					ax.text(c, r, f"{val:+.1f}%", ha="center", va="center",
							fontsize=7.5, fontweight="bold",
							color="white" if abs(val) > 12 else "black")

		ax.set_title(f"{scenario}: % change vs {versions[0]}", fontsize=11, pad=18)
		plt.colorbar(im, ax=ax, label="% change vs baseline", shrink=0.85, pad=0.02)
		fig.tight_layout()

		path = plot_dir / f"{scenario}_pct_change_heatmap.png"
		fig.savefig(path, dpi=200, bbox_inches="tight")
		plt.close(fig)
		files.append(path)

	return files


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

print(f"Versions: {versions}")
print(f"Scenarios: {sorted(df['scenario'].unique())}")
print(f"Metrics: {plot_metrics}")
print(f"Level filter: {level_filter}")
print(f"X column: {x_col}")

line_files = make_line_plots()
print(f"Line charts: {len(line_files)} plots")

bar_files = make_bar_chart()
if bar_files:
	print(f"Bar charts: {len(bar_files)} plots")

heat_files = make_heatmap()
if heat_files:
	print(f"Heatmaps: {len(heat_files)} plots")

print(f"All outputs -> {plot_dir}")
