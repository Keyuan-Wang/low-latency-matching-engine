#!/usr/bin/env python3
"""Grouped bar charts: latency + ops/s with error bars (5 trials)."""

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

versions = ["Phase1", "Phase2a", "Phase2b"]
colors = ["#4C72B0", "#55A868", "#C44E52"]

metrics = [
    ("avg latency (ns/op)", [270915.4, 198787.6, 222.5], [2658.7, 4859.0, 7.5]),
    ("ops/s",        [3691.5,    5032.9,   4.499e6], [35.8, 125.4, 150757.5]),
]

n_metrics = len(metrics)
n_versions = len(versions)
bar_width = 0.35
x = np.arange(n_versions)

fig, axes = plt.subplots(1, 2, figsize=(10, 4.5))

for idx, (ax, (label, means, stds)) in enumerate(zip(axes, metrics)):
    bars = ax.bar(x, means, bar_width, yerr=stds, capsize=4,
                  color=colors, edgecolor="white", error_kw={"linewidth": 1.5})
    for bar, v, s in zip(bars, means, stds):
        if v >= 1e6:
            txt = f"{v/1e6:.2f}M"
        elif v >= 1e3:
            txt = f"{v/1e3:.1f}K"
        else:
            txt = f"{v:.1f}"
        ax.text(bar.get_x() + bar.get_width() / 2, v + s,
                txt, ha="center", va="bottom", fontsize=9)

    ax.set_xticks(x)
    ax.set_xticklabels(versions, fontsize=10)
    ax.set_title(label, fontsize=12, fontweight="bold")
    ax.set_yscale("log")
    ax.grid(axis="y", alpha=0.3)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

fig.suptitle("Overall Throughput Benchmark: Mixed Workload",
             fontsize=13, fontweight="bold")
plt.tight_layout(rect=[0, 0, 1, 0.93])
plt.savefig("/home/wangky1998/llmes/report/overall_benchmark_comparison.png", dpi=150)
print("Saved overall_benchmark_comparison.png")
