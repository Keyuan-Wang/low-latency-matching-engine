#!/usr/bin/env python3
"""Grouped bar charts: latency + ops/s with error bars.

Reads from the merged agg CSV and generates a publication-quality figure
matching the Phase1-vs-Phase2a-vs-Phase2b report style.

Usage
-----
  # default: overall scenario, all versions found in the CSV
  python3 report/plot_overall_comparison.py

  # specific scenario + custom output path
  SCENARIO=lmt_rest \
    AGG_CSV=benchmark/results/compare_merged_agg.csv \
    OUT=benchmark/results/my_plot.png \
    python3 report/plot_overall_comparison.py

Environment variables (all optional)
------------------------------------
  AGG_CSV    — path to the merged agg CSV
               (default: benchmark/results/compare_merged_agg.csv)
  SCENARIO   — scenario to plot (default: overall)
  VERSIONS   — comma-separated version whitelist (default: all found in data)
  OUT        — output PNG path
               (default: benchmark/results/{scenario}_{versions}.png)
"""

import os
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parents[1]
AGG_CSV = Path(os.getenv("AGG_CSV", ROOT / "benchmark" / "results" / "compare_merged_agg.csv"))
SCENARIO = os.getenv("SCENARIO", "overall")
VERSION_WHITELIST = [v.strip() for v in os.getenv("VERSIONS", "").split(",") if v.strip()]
OUT = os.getenv("OUT", "")

# Proven colour palette from the original Phase1-vs-Phase2a-vs-Phase2b report
COLORS = ["#4C72B0", "#55A868", "#C44E52", "#9467BD", "#8C564B",
          "#E377C2", "#7F7F7F", "#BCBD22", "#17BECF", "#FF7F0E"]


# ---------------------------------------------------------------------------
# Load & filter
# ---------------------------------------------------------------------------

df = pd.read_csv(AGG_CSV)
scenario_df = df[df["scenario"] == SCENARIO].copy()

if scenario_df.empty:
    print(f"error: scenario '{SCENARIO}' not found in {AGG_CSV.name}", file=sys.stderr)
    sys.exit(1)

# Determine versions
versions = sorted(scenario_df["version_tag"].unique())
if VERSION_WHITELIST:
    versions = [v for v in versions if v in VERSION_WHITELIST]
    if not versions:
        print(f"error: no matching versions in {VERSION_WHITELIST}", file=sys.stderr)
        sys.exit(1)

n_versions = len(versions)
colors = COLORS[:n_versions]

# Pick the most common (orders, levels) combination
if "levels" in scenario_df.columns:
    mode_level = scenario_df["levels"].mode().iloc[0]
    scenario_df = scenario_df[scenario_df["levels"] == mode_level]
mode_orders = scenario_df["orders"].mode().iloc[0]

# Take the row for each version at the modal (orders, levels)
plot_df = scenario_df[scenario_df["orders"] == mode_orders].copy()
plot_df = plot_df.set_index("version_tag").loc[versions]

for col in ["avg_ns_mean", "avg_ns_std", "ops_s_mean", "ops_s_std"]:
    if col not in plot_df.columns or plot_df[col].isna().all():
        print(f"error: column '{col}' missing or all-NA", file=sys.stderr)
        sys.exit(1)


# ---------------------------------------------------------------------------
# Build figure (identical style to the original Phase1-vs-Phase2a-vs-Phase2b report)
# ---------------------------------------------------------------------------

def fmt_val(v: float) -> str:
    if v >= 1e6:
        return f"{v/1e6:.2f}M"
    if v >= 1e3:
        return f"{v/1e3:.1f}K"
    return f"{v:.1f}"


metrics = [
    ("avg latency (ns/op)", "avg_ns_mean", "avg_ns_std"),
    ("ops/s",               "ops_s_mean",  "ops_s_std"),
]

bar_width = 0.35
x = np.arange(n_versions)

fig, axes = plt.subplots(1, 2, figsize=(10, 4.5))

for ax, (label, mean_col, std_col) in zip(axes, metrics):
    means = plot_df[mean_col].values.astype(float)
    stds  = plot_df[std_col].fillna(0).values.astype(float)

    bars = ax.bar(x, means, bar_width, yerr=stds, capsize=4,
                  color=colors, edgecolor="white", error_kw={"linewidth": 1.5})
    for bar, v, s in zip(bars, means, stds):
        ax.text(bar.get_x() + bar.get_width() / 2, v + s,
                fmt_val(v), ha="center", va="bottom", fontsize=9)

    ax.set_xticks(x)
    ax.set_xticklabels(versions, fontsize=10)
    ax.set_title(label, fontsize=12, fontweight="bold")
    ax.set_yscale("log")
    ax.grid(axis="y", alpha=0.3)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

fig.suptitle(f"Throughput: {SCENARIO}", fontsize=13, fontweight="bold")
plt.tight_layout(rect=[0, 0, 1, 0.93])

# Determine output path
if OUT:
    out_path = OUT
else:
    tag = "_vs_".join(versions)
    out_path = str(ROOT / "report" / f"{SCENARIO}_{tag}.png")

Path(out_path).parent.mkdir(parents=True, exist_ok=True)
plt.savefig(out_path, dpi=150)
print(f"Saved {out_path}")
