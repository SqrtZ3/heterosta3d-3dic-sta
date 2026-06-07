#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
plot_results.py  --  Stage 3 & Stage 4 figures (Matplotlib + Seaborn)
=====================================================================

Reads:
  results/sweep.csv     (C_hbt sweep:   c_hbt_fF, setup_wns, hold_wns, ...)
  results/scaling.csv   (size scaling:  num_pins, *_ms, speedup_*)

Writes PNGs into report/figs/:
  fig_sweep_wns.png      Setup/Hold WNS vs C_hbt
  fig_scaling_speedup.png  GPU speed-up vs design size (crossover)
  fig_devices_time.png   extract/update time vs size, CPU vs GPU

Usage:  python scripts/plot_results.py
"""

import argparse
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns

sns.set_theme(style="whitegrid", context="talk")


def plot_sweep(sweep_csv, outdir):
    if not os.path.isfile(sweep_csv):
        print(f"skip sweep: {sweep_csv} not found")
        return
    df = pd.read_csv(sweep_csv)
    fig, ax1 = plt.subplots(figsize=(9, 5.5))
    ax1.plot(df["c_hbt_fF"], df["setup_wns"], "o-", color="#d1495b",
             label="Setup WNS", linewidth=2.2, markersize=7)
    ax1.set_xlabel("HBT vertical capacitance  $C_{hbt}$  (fF)")
    ax1.set_ylabel("Setup WNS (ns)", color="#d1495b")
    ax1.tick_params(axis="y", labelcolor="#d1495b")
    ax1.axhline(0, color="grey", linestyle="--", linewidth=1)

    ax2 = ax1.twinx()
    ax2.grid(False)
    ax2.plot(df["c_hbt_fF"], df["hold_wns"], "s-", color="#2e86ab",
             label="Hold WNS", linewidth=2.2, markersize=7)
    ax2.set_ylabel("Hold WNS (ns)", color="#2e86ab")
    ax2.tick_params(axis="y", labelcolor="#2e86ab")

    plt.title("Stage 3: Timing slack vs 3D bond capacitance $C_{hbt}$")
    fig.tight_layout()
    out = os.path.join(outdir, "fig_sweep_wns.png")
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print("wrote", out)


def plot_scaling(scaling_csv, outdir):
    if not os.path.isfile(scaling_csv):
        print(f"skip scaling: {scaling_csv} not found")
        return
    df = pd.read_csv(scaling_csv)

    # (1) speed-up vs size
    fig, ax = plt.subplots(figsize=(9, 5.5))
    ax.plot(df["num_pins"], df["speedup_update"], "o-", color="#3a7d44",
            linewidth=2.4, markersize=8, label="update-phase speed-up")
    if "speedup_total" in df:
        ax.plot(df["num_pins"], df["speedup_total"], "^--", color="#9b6a6c",
                linewidth=2, markersize=7, label="end-to-end speed-up")
    ax.axhline(1.0, color="red", linestyle="--", linewidth=1.4,
               label="break-even (GPU = CPU)")
    ax.set_xscale("log")
    ax.set_xlabel("design size (number of pins)")
    ax.set_ylabel("GPU speed-up  ($t_{CPU}/t_{GPU}$)")
    ax.set_title("Stage 4: GPU speed-up vs design size")
    ax.legend(fontsize=12)
    fig.tight_layout()
    out = os.path.join(outdir, "fig_scaling_speedup.png")
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print("wrote", out)

    # (2) raw times
    fig, ax = plt.subplots(figsize=(9, 5.5))
    ax.plot(df["num_pins"], df["cpu_update_ms"], "o-", label="CPU update", color="#1b264f")
    ax.plot(df["num_pins"], df["gpu_update_ms"], "s-", label="GPU update", color="#e07a5f")
    ax.plot(df["num_pins"], df["gpu_extract_ms"], "^--", label="GPU extract (H2D copy)",
            color="#81b29a")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("design size (number of pins)")
    ax.set_ylabel("time (ms)")
    ax.set_title("Stage 4: runtime breakdown, CPU vs GPU")
    ax.legend(fontsize=12)
    fig.tight_layout()
    out = os.path.join(outdir, "fig_devices_time.png")
    fig.savefig(out, dpi=150)
    plt.close(fig)
    print("wrote", out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweep", default="results/sweep.csv")
    ap.add_argument("--scaling", default="results/scaling.csv")
    ap.add_argument("--outdir", default="report/figs")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    plot_sweep(args.sweep, args.outdir)
    plot_scaling(args.scaling, args.outdir)


if __name__ == "__main__":
    main()
