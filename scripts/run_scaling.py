#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_scaling.py  --  Stage 4 driver: CPU-vs-GPU speed-up versus design size
==========================================================================

Generates K-fold replicas of the base design, runs the HeteroSTA3D driver in
`devices` mode on each, and collects timing into results/scaling.csv:

    num_pins, cpu_extract_ms, gpu_extract_ms, cpu_update_ms, gpu_update_ms,
    speedup_update, speedup_total

This is the data behind the "speed-up analysis" figure.  With the bundled CPU
stub the timing is the stub's analytic model (fixed GPU launch/copy overhead +
high per-pin throughput), which reproduces the real-world crossover: GPUs lose
on tiny graphs (overhead-bound) and win on large ones (throughput-bound).
Re-run against the real .so for graded numbers.

Usage:
  python scripts/run_scaling.py --driver ./sta_driver.exe \
         --base designs/gcd_sample.v --sizes 1,2,4,8,16,32,64,128
"""

import argparse
import csv
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + "\n" + r.stderr + "\n")
        raise SystemExit(f"command failed: {' '.join(cmd)}")
    return r.stdout


def read_devices_csv(path):
    rows = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            rows[row["device"]] = row
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--driver", default="./sta_driver.exe")
    ap.add_argument("--base", default="designs/gcd_sample.v")
    ap.add_argument("--sdc", default="sdc/gcd.sdc")
    ap.add_argument("--sizes", default="1,2,4,8,16,32,64,128")
    ap.add_argument("--out", default="results/scaling.csv")
    ap.add_argument("--gpu", default="0", help="GPU device id to compare against cpu")
    ap.add_argument("--workdir", default="designs/scaled")
    args = ap.parse_args()

    sizes = [int(s) for s in args.sizes.split(",") if s]
    os.makedirs(args.workdir, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    results = []
    for k in sizes:
        scaled = os.path.join(args.workdir, f"gcd_k{k}.v")
        outdir = os.path.join(args.workdir, f"gcd_k{k}_3d")
        run([sys.executable, os.path.join(HERE, "gen_scaled_netlist.py"),
             args.base, "-k", str(k), "-o", scaled])
        run([sys.executable, os.path.join(HERE, "split_3d_netlist.py"),
             scaled, "--top-module", "gcd", "--outdir", outdir])

        summary = json.load(open(os.path.join(outdir, "split_summary.json")))
        num_pins = summary["num_pins_placed"]

        tmp_out = os.path.join(outdir, "dev_out")
        os.makedirs(tmp_out, exist_ok=True)
        run([args.driver, "--mode", "devices",
             "--netlist", os.path.join(outdir, "design_3d.v"),
             "--placement", os.path.join(outdir, "placement.csv"),
             "--sdc", args.sdc, "--devices", f"cpu,{args.gpu}",
             "--c-hbt", "1.0", "--outdir", tmp_out])

        dev = read_devices_csv(os.path.join(tmp_out, "devices.csv"))
        cpu, gpu = dev["cpu"], dev[args.gpu]
        ce, ge = float(cpu["extract_ms"]), float(gpu["extract_ms"])
        cu, gu = float(cpu["update_ms"]), float(gpu["update_ms"])
        ct, gt = ce + cu, ge + gu
        results.append({
            "num_pins": num_pins,
            "cpu_extract_ms": ce, "gpu_extract_ms": ge,
            "cpu_update_ms": cu, "gpu_update_ms": gu,
            "speedup_update": (cu / gu) if gu else 0,
            "speedup_total": (ct / gt) if gt else 0,
        })
        print(f"  k={k:4d}  pins={num_pins:6d}  "
              f"update CPU {cu:8.2f}ms  GPU {gu:8.2f}ms  "
              f"speedup {results[-1]['speedup_update']:.2f}x")

    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(results[0].keys()))
        w.writeheader()
        w.writerows(results)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
