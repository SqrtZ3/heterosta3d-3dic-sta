#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_scaled_netlist.py  --  replicate a flat netlist K times (scaling benchmark)
==============================================================================

For the Stage-4 speed-up study we need designs of *increasing size* so the
CPU-vs-GPU crossover becomes visible.  This script takes a small flat
gate-level netlist and emits a functionally-independent K-fold replica:

  * shared input ports (clk, rst, a_in, b_in) fan out to every copy;
  * every other net is renamed   <net>_c<k>   so copies never short together;
  * the primary output `result` is driven by copy 0; other copies drive a
    private  result_c<k>  wire.

The result is a valid, progressively larger gate-level netlist whose only
purpose is to scale the timing graph for benchmarking.

Usage:
  python scripts/gen_scaled_netlist.py designs/gcd_sample.v -k 32 \
         -o designs/scaled/gcd_k32.v
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from split_3d_netlist import (strip_comments, find_top_module,
                              split_statements, parse_instances)

SHARED_INPUTS = {"clk", "rst", "a_in", "b_in"}


def module_ports(header: str):
    lo, hi = header.index("("), header.index(";")
    inside = header[lo + 1:hi].rstrip(") \t\r\n")
    return [p.strip() for p in inside.split(",") if p.strip()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("netlist")
    ap.add_argument("-k", "--copies", type=int, default=8)
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--top-module", default=None)
    args = ap.parse_args()

    raw = open(args.netlist, encoding="utf-8", errors="replace").read()
    text = strip_comments(raw)
    modname, header, body = find_top_module(text, args.top_module)
    items = parse_instances(split_statements(body))
    instances = [it for it in items if it.get("is_instance")]
    ports = set(module_ports(header))

    def rename_net(net, k):
        if net in SHARED_INPUTS:
            return net
        if net == "result":
            return "result" if k == 0 else f"result_c{k}"
        return f"{net}_c{k}"

    out_insts, all_nets = [], set()
    for k in range(args.copies):
        for it in instances:
            conns = [(p, rename_net(n, k)) for p, n in it["conns"]]
            for _p, n in conns:
                if n not in ports:
                    all_nets.add(n)
            out_insts.append((it["celltype"], f"{it['name']}_c{k}", conns))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(f"// scaled x{args.copies} from {os.path.basename(args.netlist)}\n")
        f.write("module gcd (clk, rst, a_in, b_in, result);\n")
        f.write("  input clk; input rst; input a_in; input b_in;\n")
        f.write("  output result;\n")
        wires = sorted(all_nets - {"result"})
        # chunk wire declarations for readability
        for i in range(0, len(wires), 8):
            f.write("  wire " + ", ".join(wires[i:i + 8]) + ";\n")
        f.write("\n")
        for ct, name, conns in out_insts:
            cc = ", ".join(f".{p}({n})" for p, n in conns)
            f.write(f"  {ct} {name} ( {cc} );\n")
        f.write("endmodule\n")

    print(f"wrote {args.out}  (x{args.copies}, {len(out_insts)} cells, {len(wires)} wires)")


if __name__ == "__main__":
    main()
