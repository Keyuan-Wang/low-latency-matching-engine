#!/usr/bin/env python3
"""
Plot per-scenario latency distributions from hft_macro_scenario_cycles.csv.

Expects the per-call CSV emitted by bench_hft_macro_scenarios (mode=scenario_call).
Produces a 2×3 figure:
  row 1 — cycles distributions for add_rest / cancel_order / modify_order
  row 2 — elapsed_ns distributions for the same three op types

Usage:
  CSV=/path/to/hft_macro_scenario_cycles.csv \\
  OUT=/path/to/hft_macro_scenario_distributions.png \\
  python benchmark/scripts/plot_hft_macro_scenarios.py

Env:
  CSV          Input CSV (default: benchmark/results/hft_macro_scenario_cycles.csv)
  OUT          Output PNG (default: same dir as CSV, stem + _distributions.png)
  BINS         Histogram bar count (default: 100)
  TRIALS       Comma-separated trial_id filter (default: all)
  CLIP_PCT     Upper percentile for bin range (default: 99.5); tail mass stacks in last bar
  LOG_Y        1 = log-scaled count axis (default: 0)
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from matplotlib.gridspec import GridSpec
from matplotlib.patches import Patch

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

root = Path(__file__).resolve().parents[2]
default_csv = root / "benchmark" / "results" / "hft_macro_scenario_cycles.csv"

csv_path = Path(os.getenv("CSV", str(default_csv)))
bins = int(os.getenv("BINS", "100"))
clip_pct = float(os.getenv("CLIP_PCT", "99.5"))
log_y = os.getenv("LOG_Y", "0") == "1"

trial_filter = os.getenv("TRIALS", "").strip()
trial_ids: set[int] | None = None
if trial_filter:
	trial_ids = {int(t.strip()) for t in trial_filter.split(",") if t.strip()}

out_path = os.getenv("OUT", "").strip()
if out_path:
	out_png = Path(out_path)
else:
	out_png = csv_path.with_name(csv_path.stem + "_distributions.png")

OP_PRESETS: dict[str, dict[str, str]] = {
	"current": {
		"order": "add_rest_existing_level,add_rest_new_level,cancel_order",
		"add_rest_existing_level": "Add rest (existing level)",
		"add_rest_new_level": "Add rest (new level)",
		"cancel_order": "Cancel order",
		"add_rest_existing_level_color": "#4C78A8",
		"add_rest_new_level_color": "#72B7B2",
		"cancel_order_color": "#2CA02C",
	},
	"legacy": {
		"order": "add_rest,cancel_order,modify_order",
		"add_rest": "Add rest",
		"cancel_order": "Cancel order",
		"modify_order": "Modify order",
		"add_rest_color": "#4C78A8",
		"cancel_order_color": "#2CA02C",
		"modify_order_color": "#D62728",
	},
}


def resolve_op_layout(df: pd.DataFrame) -> tuple[list[str], dict[str, str], dict[str, str]]:
	present = set(df["op_type"].astype(str))
	if {"add_rest_existing_level", "add_rest_new_level"} & present:
		preset = OP_PRESETS["current"]
	elif "add_rest" in present or "modify_order" in present:
		preset = OP_PRESETS["legacy"]
	else:
		raise RuntimeError(f"Unrecognized op_type values: {sorted(present)}")

	op_order = [op for op in preset["order"].split(",") if op in present]
	if len(op_order) != 3:
		raise RuntimeError(f"Expected 3 measured op types, found: {op_order}")

	labels = {op: preset[op] for op in op_order}
	colors = {op: preset[f"{op}_color"] for op in op_order}
	return op_order, labels, colors

METRICS = [
	("cycles", "CPU cycles (adjusted)"),
	("elapsed_ns", "Elapsed time (ns, adjusted)"),
]


def load_calls(path: Path) -> pd.DataFrame:
	if not path.exists():
		raise FileNotFoundError(f"CSV not found: {path}")

	# Peek header for legacy aggregated format.
	header = path.read_text(encoding="utf-8", errors="replace").splitlines()[0]
	if "scenario_call" not in header and "measured" in header:
		raise ValueError(
			"Aggregated legacy CSV detected; re-run run_hft_macro_scenarios.sh "
			"to produce per-call scenario_call rows."
		)

	usecols = [
		"op_type",
		"cycles",
		"elapsed_ns",
		"commit_sha",
		"trial_id",
		"version_tag",
	]
	df = pd.read_csv(path, usecols=usecols)
	op_order, _, _ = resolve_op_layout(df)
	df = df[df["op_type"].isin(op_order)].copy()
	if trial_ids is not None:
		df = df[df["trial_id"].isin(trial_ids)]
	if df.empty:
		raise RuntimeError("No scenario_call rows after filtering")
	return df


def histogram_edges(
	values: np.ndarray, n_bins: int, upper_pct: float, *, min_hi: float | None = None
) -> np.ndarray:
	values = values[np.isfinite(values)]
	if values.size == 0:
		return np.linspace(0.0, 1.0, n_bins + 1)

	lo = float(np.min(values))
	hi = float(np.percentile(values, upper_pct))
	if min_hi is not None:
		hi = max(hi, float(min_hi))
	if hi <= lo:
		hi = lo + 1.0
	return np.linspace(lo, hi, n_bins + 1)


def compute_hist(
	values: np.ndarray, edges: np.ndarray
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
	counts, _ = np.histogram(values, bins=edges)
	centers = 0.5 * (edges[:-1] + edges[1:])
	widths = np.diff(edges)
	return centers, counts.astype(float), widths


def percentile_stats(values: np.ndarray) -> dict[str, float]:
	return {
		"n": float(values.size),
		"mean": float(np.mean(values)),
		"p50": float(np.percentile(values, 50)),
		"p95": float(np.percentile(values, 95)),
		"p99": float(np.percentile(values, 99)),
		"p999": float(np.percentile(values, 99.9)),
	}


def draw_panel(
	ax: plt.Axes,
	values: np.ndarray,
	*,
	title: str,
	xlabel: str,
	color: str,
	n_bins: int,
	clip_pct: float,
	stats: dict[str, float],
) -> None:
	edges = histogram_edges(values, n_bins, clip_pct, min_hi=stats["p999"])
	centers, counts, widths = compute_hist(values, edges)

	ax.bar(
		centers,
		counts,
		width=widths * 0.92,
		align="center",
		color=color,
		edgecolor="white",
		linewidth=0.35,
		alpha=0.92,
		zorder=2,
	)

	for pct, ls in ((50, "-"), (95, "--"), (99.9, ":")):
		key = "p50" if pct == 50 else "p95" if pct == 95 else "p999"
		line_val = stats[key]
		if edges[0] <= line_val <= edges[-1]:
			ax.axvline(
				line_val,
				color="#1F1F1F",
				linestyle=ls,
				linewidth=1.1,
				alpha=0.75,
				zorder=3,
			)

	ymax = max(float(np.max(counts)), 1.0)
	ax.set_ylim(0, ymax * 1.12)
	if log_y:
		ax.set_yscale("log")
		ax.set_ylim(max(0.9, float(np.min(counts[counts > 0])) if np.any(counts > 0) else 1.0), ymax * 1.35)

	ax.set_title(title, fontsize=12, fontweight="semibold", pad=8)
	ax.set_xlabel(xlabel)
	ax.grid(axis="y", alpha=0.28, linewidth=0.6)
	ax.grid(axis="x", alpha=0.12, linewidth=0.4)

	stats_text = (
		f"n={int(stats['n']):,}\n"
		f"mean={stats['mean']:.1f}\n"
		f"p50={stats['p50']:.1f}\n"
		f"p95={stats['p95']:.1f}\n"
		f"p99={stats['p99']:.1f}\n"
		f"p999={stats['p999']:.1f}"
	)
	ax.text(
		0.98,
		0.97,
		stats_text,
		transform=ax.transAxes,
		ha="right",
		va="top",
		fontsize=8.5,
		bbox=dict(boxstyle="round,pad=0.35", facecolor="white", edgecolor="#CCCCCC", alpha=0.92),
		zorder=4,
	)


def main() -> int:
	df = load_calls(csv_path)
	op_order, op_labels, op_colors = resolve_op_layout(df)

	meta_commit = str(df["commit_sha"].iloc[0])
	meta_tag = str(df["version_tag"].iloc[0])
	meta_trials = sorted(df["trial_id"].unique().tolist())
	meta_n = len(df)

	plt.style.use("seaborn-v0_8-whitegrid")
	plt.rcParams.update({
		"figure.dpi": 180,
		"savefig.dpi": 220,
		"font.family": "sans-serif",
		"axes.titlesize": 12,
		"axes.labelsize": 10,
		"figure.facecolor": "#FAFAFA",
		"axes.facecolor": "#FFFFFF",
	})

	fig = plt.figure(figsize=(15.5, 8.8), constrained_layout=False)
	gs = GridSpec(2, 3, figure=fig, hspace=0.34, wspace=0.22)

	for row_idx, (metric_col, row_ylabel) in enumerate(METRICS):
		for col_idx, op in enumerate(op_order):
			ax = fig.add_subplot(gs[row_idx, col_idx])
			values = df.loc[df["op_type"] == op, metric_col].to_numpy(dtype=float)
			stats = percentile_stats(values)
			draw_panel(
				ax,
				values,
				title=f"{op_labels[op]}",
				xlabel=row_ylabel,
				color=op_colors[op],
				n_bins=bins,
				clip_pct=clip_pct,
				stats=stats,
			)
			if col_idx == 0:
				row_title = "Cycles" if metric_col == "cycles" else "Elapsed ns"
				ax.set_ylabel(f"{row_title}\ncount")

	legend_handles = [
		Patch(facecolor=op_colors[op], edgecolor="white", label=op_labels[op])
		for op in op_order
	]
	legend_handles.extend([
		plt.Line2D([0], [0], color="#1F1F1F", linestyle="-", linewidth=1.2, label="p50"),
		plt.Line2D([0], [0], color="#1F1F1F", linestyle="--", linewidth=1.2, label="p95"),
		plt.Line2D([0], [0], color="#1F1F1F", linestyle=":", linewidth=1.2, label="p999"),
	])

	trial_str = ",".join(str(t) for t in meta_trials)
	fig.suptitle(
		f"hft_macro per-scenario distributions  ·  {meta_tag}@{meta_commit}  ·  "
		f"trials=[{trial_str}]  ·  calls={meta_n:,}  ·  bins={bins}  ·  "
		f"x-axis ≤ max(p{clip_pct:g}, p999)",
		fontsize=12.5,
		fontweight="bold",
		y=1.02,
	)
	fig.legend(
		handles=legend_handles,
		loc="upper center",
		ncol=6,
		frameon=True,
		bbox_to_anchor=(0.5, 1.0),
		fontsize=9,
	)
	fig.subplots_adjust(top=0.90)

	out_png.parent.mkdir(parents=True, exist_ok=True)
	fig.savefig(out_png, bbox_inches="tight", facecolor=fig.get_facecolor())
	plt.close(fig)

	print(f"read : {csv_path}")
	print(f"wrote: {out_png}")
	return 0


if __name__ == "__main__":
	sys.exit(main())
