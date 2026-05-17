#!/usr/bin/env python3
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path
import os

root = Path(__file__).resolve().parents[2]
res = root / "bench" / "results"
plot_dir = res / "plots"
plot_dir.mkdir(parents=True, exist_ok=True)

df = pd.read_csv(res / "phase1_merged_agg.csv")
if df.empty:
    raise RuntimeError("phase1_merged_agg.csv is empty")

available_levels = sorted(df["levels"].unique().tolist())
default_level = 100 if 100 in available_levels else available_levels[0]
target_level = int(os.getenv("PLOT_LEVEL", str(default_level)))
plot_df = df[df["levels"] == target_level].copy()
if plot_df.empty:
    raise RuntimeError(f"No rows found for PLOT_LEVEL={target_level}, available={available_levels}")

# 1) p99 latency vs orders (with 95% CI error bars)
plt.figure(figsize=(9, 5))
for s, g in plot_df.groupby("scenario"):
    gg = g.sort_values("orders")
    y = gg["p99_ns_mean"]
    yerr = (gg["p99_ns_ci95_high"] - gg["p99_ns_ci95_low"]) / 2.0
    plt.errorbar(gg["orders"], y, yerr=yerr, marker="o", capsize=3, label=s)
plt.xscale("log")
plt.yscale("log")
plt.xlabel("orders")
plt.ylabel("p99 latency (ns)")
plt.title(f"Phase1 p99 latency vs book size (level={target_level})")
plt.legend()
plt.tight_layout()
plt.savefig(plot_dir / "p99_vs_orders.png", dpi=160)
plt.close()

# 2) throughput vs orders (with 95% CI error bars)
plt.figure(figsize=(9, 5))
for s, g in plot_df.groupby("scenario"):
    gg = g.sort_values("orders")
    y = gg["ops_s_mean"]
    yerr = (gg["ops_s_ci95_high"] - gg["ops_s_ci95_low"]) / 2.0
    plt.errorbar(gg["orders"], y, yerr=yerr, marker="o", capsize=3, label=s)
plt.xscale("log")
plt.ylabel("ops/s")
plt.xlabel("orders")
plt.title(f"Phase1 throughput vs book size (level={target_level})")
plt.legend()
plt.tight_layout()
plt.savefig(plot_dir / "ops_vs_orders.png", dpi=160)
plt.close()

# 3) CPI and LLC miss per op (with 95% CI error bars)
plt.figure(figsize=(9, 5))
for s, g in plot_df.groupby("scenario"):
    gg = g.sort_values("orders")
    y = gg["cpi_mean"]
    yerr = (gg["cpi_ci95_high"] - gg["cpi_ci95_low"]) / 2.0
    plt.errorbar(gg["orders"], y, yerr=yerr, marker="o", capsize=3, label=s)
plt.xscale("log")
plt.xlabel("orders")
plt.ylabel("CPI")
plt.title(f"Phase1 CPI vs book size (level={target_level})")
plt.legend()
plt.tight_layout()
plt.savefig(plot_dir / "cpi_vs_orders.png", dpi=160)
plt.close()

plt.figure(figsize=(9, 5))
for s, g in plot_df.groupby("scenario"):
    gg = g.sort_values("orders")
    y = gg["llc_miss_per_op_mean"]
    yerr = (gg["llc_miss_per_op_ci95_high"] - gg["llc_miss_per_op_ci95_low"]) / 2.0
    plt.errorbar(gg["orders"], y, yerr=yerr, marker="o", capsize=3, label=s)
plt.xscale("log")
plt.xlabel("orders")
plt.ylabel("LLC miss / op")
plt.title(f"Phase1 LLC miss per op vs book size (level={target_level})")
plt.legend()
plt.tight_layout()
plt.savefig(plot_dir / "llc_miss_per_op_vs_orders.png", dpi=160)
plt.close()

# 4) Roofline proxy: ops/s vs ops/byte with error bars
roof = plot_df.dropna(subset=["ops_per_byte_proxy_mean", "ops_s_mean"]).copy()
plt.figure(figsize=(9, 5))
for s, g in roof.groupby("scenario"):
    x = g["ops_per_byte_proxy_mean"]
    y = g["ops_s_mean"]
    xerr = (g["ops_per_byte_proxy_ci95_high"] - g["ops_per_byte_proxy_ci95_low"]) / 2.0
    yerr = (g["ops_s_ci95_high"] - g["ops_s_ci95_low"]) / 2.0
    plt.errorbar(x, y, xerr=xerr, yerr=yerr, fmt="o", capsize=3, label=s, alpha=0.85)
plt.xscale("log")
plt.yscale("log")
plt.xlabel("Operational intensity proxy (ops / byte_proxy)")
plt.ylabel("Performance (ops/s)")
plt.title(f"Phase1 Memory Roofline Proxy (level={target_level})")
plt.legend()
plt.tight_layout()
plt.savefig(plot_dir / "roofline_proxy.png", dpi=160)
plt.close()

print(f"plots -> {plot_dir}")