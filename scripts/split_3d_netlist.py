#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
split_3d_netlist.py  --  Stage 1: 3D netlist pre-processing for HeteroSTA3D
===========================================================================

HeteroSTA3D assigns every cell instance to a die by a name *suffix*:
``_top`` (top die) or ``_bottom`` (bottom die).  Open-source synthesis
(OpenROAD-flow-scripts / Yosys) emits a flat gate-level netlist with no such
suffix, so we add it here.

Two split strategies are supported (``--mode``):

  logic_split  (default, recommended)
      Combinational cells -> top die (``_top``)
      Sequential cells (flip-flops/latches) -> bottom die (``_bottom``)
      Every register-to-register path then runs:
          FF(bottom) -> HBT up -> comb cloud(top) -> HBT down -> FF(bottom)
      so it physically crosses the hybrid-bond interface twice.  That makes
      the vertical bond capacitance ``C_hbt`` (Stage 3) actually move the
      Setup/Hold numbers -- exactly what the assignment wants to study.

  duplicate
      Copy the whole design onto both dies and join them through virtual HBTs.
      Provided for completeness (the task's alternative wording); less useful
      for the C_hbt sweep because most paths stay on one die.

Outputs (into --outdir):
  design_3d.v        merged single 3D netlist (what the C++ driver reads)
  design_3d_top.v    top-die instances only      (reference / inspection)
  design_3d_bottom.v bottom-die instances only   (reference / inspection)
  placement.csv      pin_name,x,y   (synthetic placement, consumed by driver)
  split_summary.json statistics for the report

Why a synthetic placement?  HeteroSTA3D's RC extraction
(``heterosta3d_extract_rc_from_placement``) needs an (x,y) per pin.  We have no
DEF, so we generate a deterministic grid placement: cells are spread over a
square die; both dies share the same x/y footprint (face-to-face stacking),
the die is selected by the name suffix.  The driver maps ``pin_name -> x,y``
via ``heterosta3d_lookup_pin``.

Usage:
  python scripts/split_3d_netlist.py designs/gcd.v --top-module gcd \
         --outdir designs/gcd_3d
"""

import argparse
import json
import math
import os
import re
import sys

# --------------------------------------------------------------------------
# Verilog keywords that begin a NON-instance statement; everything else that
# looks like "TYPE NAME ( ... ) ;" is treated as a cell instantiation.
# --------------------------------------------------------------------------
NON_INSTANCE_KEYWORDS = {
    "module", "endmodule", "input", "output", "inout", "wire", "reg", "tri",
    "supply0", "supply1", "parameter", "localparam", "assign", "always",
    "initial", "generate", "endgenerate", "function", "endfunction", "genvar",
    "defparam", "specify", "endspecify", "`timescale", "`default_nettype",
    "`celldefine", "`endcelldefine", "primitive", "endprimitive",
}

# --------------------------------------------------------------------------
# Sequential-cell detection.  Standard-cell libraries name flip-flops/latches
# in fairly predictable ways across PDKs:
#   Nangate45 : DFF_X1, SDFF_X2, DLH_X1, ...
#   ASAP7     : DFFHQNx1_ASAP7_75t_R, ASYNC_DFFHx1_..., DHLx1_..., SDFLx1_...
#   Sky130    : sky130_fd_sc_hd__dfxtp_1, ..._dfrtp_2, ..._dlxtp_1, ..._sdfxtp
# We match on word fragments, case-insensitively, and let the user extend the
# list with --seq-regex.
# --------------------------------------------------------------------------
DEFAULT_SEQ_REGEX = r"(?i)(?:^|_)(?:s?dff|dff|dfrtp|dfxtp|dfstp|dfbbn|edff|" \
                    r"latch|dlxtp|dlrtp|dlh|dll|dhl|sdfl|sdff|dfl|dlatch|dl[hl]x|" \
                    r"async_dff|ff[_x]|reg_|sram|dffe|sdfrtp|sdfstp)"


def strip_comments(text: str) -> str:
    """Remove // line comments and /* */ block comments."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def find_top_module(text: str, top_module: str | None):
    """Return (module_name, header_str, body_str) for the chosen module."""
    # module NAME ( ports ) ;  ... endmodule
    pattern = re.compile(
        r"\bmodule\s+(\\?\S+|\w+)\s*(#\s*\(.*?\)\s*)?\((.*?)\)\s*;(.*?)\bendmodule",
        re.DOTALL,
    )
    modules = list(pattern.finditer(text))
    if not modules:
        raise ValueError("No `module ... endmodule` block found in netlist.")
    if top_module:
        for m in modules:
            if m.group(1).strip() == top_module:
                return m.group(1).strip(), m.group(0), m.group(4)
        raise ValueError(f"Top module '{top_module}' not found. "
                         f"Found: {[m.group(1).strip() for m in modules]}")
    # Default: the largest module body (the design top after flat synthesis).
    biggest = max(modules, key=lambda m: len(m.group(4)))
    return biggest.group(1).strip(), biggest.group(0), biggest.group(4)


def split_statements(body: str):
    """Split a module body into statements on top-level ';'.

    Parentheses are tracked so a ';' inside a connection list never splits a
    statement.  Returns a list of statement strings (without the ';')."""
    stmts, depth, cur = [], 0, []
    for ch in body:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        if ch == ";" and depth == 0:
            stmts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        stmts.append(tail)
    return stmts


# An instance:  TYPE  NAME ( .pin(net), ... )
INSTANCE_RE = re.compile(
    r"^\s*(\\?[\w$.\\\[\]/]+)\s+"      # 1: cell type (master)
    r"(\\?[\w$.\\\[\]/]+)\s*"           # 2: instance name
    r"\((?P<conns>.*)\)\s*$",            # 3: connection list
    re.DOTALL,
)

# A single named connection inside the list:  .PIN ( net )
CONN_RE = re.compile(r"\.\s*([\w$\\\[\].]+)\s*\(\s*(.*?)\s*\)", re.DOTALL)


def is_sequential(celltype: str, seq_re: re.Pattern) -> bool:
    return seq_re.search(celltype) is not None


def parse_instances(stmts):
    """Yield dicts for each cell instance statement; skip declarations."""
    insts = []
    for s in stmts:
        st = s.strip()
        if not st:
            continue
        first = st.split(None, 1)[0]
        if first in NON_INSTANCE_KEYWORDS or first.startswith("`"):
            # Declarations (input/output/wire/assign/...) are not instances but
            # MUST be carried through to the output netlist verbatim.
            insts.append({"raw": st, "is_instance": False})
            continue
        m = INSTANCE_RE.match(st)
        if not m:
            # Not an instance we recognise (e.g. a stray assign) -> keep raw.
            insts.append({"raw": st, "is_instance": False})
            continue
        celltype = m.group(1).strip()
        name = m.group(2).strip()
        conns_blob = m.group("conns")
        conns = [(p, n.strip()) for p, n in CONN_RE.findall(conns_blob)]
        insts.append({
            "is_instance": True,
            "celltype": celltype,
            "name": name,
            "conns": conns,
        })
    return insts


def suffix_name(name: str, suffix: str) -> str:
    """Append a die suffix, handling Verilog escaped identifiers.

    Escaped identifiers start with '\\' and run until whitespace, so the suffix
    simply attaches to the end of the token (still terminated by a space)."""
    return name + suffix


def grid_positions(n: int, die_w: float):
    """Return n (x,y) points on a square grid spanning [0, die_w]^2."""
    if n <= 0:
        return []
    cols = max(1, int(math.ceil(math.sqrt(n))))
    rows = max(1, int(math.ceil(n / cols)))
    pts = []
    for i in range(n):
        r, c = divmod(i, cols)
        x = (c + 0.5) * die_w / cols
        y = (r + 0.5) * die_w / rows
        pts.append((round(x, 3), round(y, 3)))
    return pts


def emit_instance(inst, sep="/"):
    """Render an instance back to Verilog (single line)."""
    conns = ", ".join(f".{p}({n})" for p, n in inst["conns"])
    return f"  {inst['celltype']} {inst['name']} ( {conns} );"


def main():
    ap = argparse.ArgumentParser(description="3D-split a flat Verilog netlist for HeteroSTA3D.")
    ap.add_argument("netlist", help="Input flat gate-level Verilog (.v)")
    ap.add_argument("--top-module", default=None, help="Top module name (default: largest)")
    ap.add_argument("--outdir", default=None, help="Output directory (default: <netlist>_3d)")
    ap.add_argument("--mode", choices=["logic_split", "duplicate"], default="logic_split")
    ap.add_argument("--seq-regex", default=DEFAULT_SEQ_REGEX,
                    help="Regex identifying sequential cell types")
    ap.add_argument("--die-width", type=float, default=100.0,
                    help="Die footprint width in microns for the synthetic placement")
    ap.add_argument("--pin-sep", default="/",
                    help="Separator between instance and pin in pin names (engine convention)")
    args = ap.parse_args()

    if not os.path.isfile(args.netlist):
        sys.exit(f"error: netlist not found: {args.netlist}")
    outdir = args.outdir or (os.path.splitext(args.netlist)[0] + "_3d")
    os.makedirs(outdir, exist_ok=True)
    seq_re = re.compile(args.seq_regex)

    raw = open(args.netlist, "r", encoding="utf-8", errors="replace").read()
    text = strip_comments(raw)
    modname, header, body = find_top_module(text, args.top_module)
    stmts = split_statements(body)
    items = parse_instances(stmts)
    instances = [it for it in items if it.get("is_instance")]

    if not instances:
        sys.exit("error: no cell instances parsed -- is this a gate-level netlist?")

    # ---- classify & suffix --------------------------------------------------
    top_insts, bot_insts = [], []
    for it in instances:
        if args.mode == "logic_split":
            die = "_bottom" if is_sequential(it["celltype"], seq_re) else "_top"
        else:  # duplicate handled below; placeholder
            die = "_top"
        it["die"] = die
        it["celltype"] = suffix_name(it["celltype"], die)
        it["name"] = suffix_name(it["name"], die)
        (bot_insts if die == "_bottom" else top_insts).append(it)

    if args.mode == "duplicate":
        # Re-do as a true duplication: original design on top AND bottom.
        top_insts, bot_insts = [], []
        for it in instances:
            base_type = re.sub(r"_(top|bottom)$", "", it["celltype"])
            base_name = re.sub(r"_(top|bottom)$", "", it["name"])
            for die, bucket in (("_top", top_insts), ("_bottom", bot_insts)):
                dup = {
                    "is_instance": True, "die": die,
                    "celltype": base_type + die,
                    "name": base_name + die,
                    # nets get the die suffix too so the two copies don't short;
                    # cross-die HBT nets keep a shared name (handled by driver).
                    "conns": [(p, n) for p, n in it["conns"]],
                }
                bucket.append(dup)

    all_insts = top_insts + bot_insts

    # ---- cross-die (3D) net detection --------------------------------------
    # A net touched by both a _top pin and a _bottom pin needs a vertical HBT.
    net_dies = {}
    for it in all_insts:
        for _pin, net in it["conns"]:
            net_dies.setdefault(net, set()).add(it["die"])
    cross_nets = sorted(n for n, dies in net_dies.items() if len(dies) > 1)

    # ---- synthetic placement ------------------------------------------------
    # Lay each die's instances on its own grid; both dies share [0,die_w]^2.
    top_pos = dict(zip([it["name"] for it in top_insts],
                       grid_positions(len(top_insts), args.die_width)))
    bot_pos = dict(zip([it["name"] for it in bot_insts],
                       grid_positions(len(bot_insts), args.die_width)))
    inst_pos = {**top_pos, **bot_pos}

    placement_rows = []  # (pin_name, x, y)
    for it in all_insts:
        x, y = inst_pos[it["name"]]
        for pin, _net in it["conns"]:
            placement_rows.append((f"{it['name']}{args.pin_sep}{pin}", x, y))

    # ---- write merged netlist ----------------------------------------------
    def write_netlist(path, insts, module_suffix=""):
        with open(path, "w", encoding="utf-8") as f:
            f.write("// Auto-generated by split_3d_netlist.py  (HeteroSTA3D Stage 1)\n")
            f.write(f"// mode={args.mode}  source={os.path.basename(args.netlist)}\n")
            f.write(header.split("(", 1)[0].rstrip()
                    + module_suffix + header[header.index("("):header.index(";") + 1] + "\n")
            # carry over declarations (input/output/wire/assign/...) verbatim
            for it in items:
                if not it.get("is_instance"):
                    raw_stmt = it["raw"].strip()
                    if raw_stmt:
                        f.write("  " + raw_stmt + ";\n")
            f.write("\n")
            for it in insts:
                f.write(emit_instance(it, args.pin_sep) + "\n")
            f.write("endmodule\n")

    merged = os.path.join(outdir, "design_3d.v")
    write_netlist(merged, all_insts)
    write_netlist(os.path.join(outdir, "design_3d_top.v"), top_insts, "_top")
    write_netlist(os.path.join(outdir, "design_3d_bottom.v"), bot_insts, "_bottom")

    # ---- write placement ----------------------------------------------------
    with open(os.path.join(outdir, "placement.csv"), "w", encoding="utf-8") as f:
        f.write("pin_name,x,y\n")
        for name, x, y in placement_rows:
            f.write(f"{name},{x},{y}\n")

    # ---- summary ------------------------------------------------------------
    summary = {
        "source_netlist": os.path.abspath(args.netlist),
        "top_module": modname,
        "mode": args.mode,
        "num_cells_total": len(all_insts),
        "num_cells_top": len(top_insts),
        "num_cells_bottom": len(bot_insts),
        "num_sequential_detected": sum(
            1 for it in instances if is_sequential(re.sub(r"_(top|bottom)$", "", it["celltype"]), seq_re)),
        "num_nets": len(net_dies),
        "num_cross_die_nets_hbt": len(cross_nets),
        "num_pins_placed": len(placement_rows),
        "die_width_um": args.die_width,
        "pin_sep": args.pin_sep,
    }
    with open(os.path.join(outdir, "split_summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    # ---- console report -----------------------------------------------------
    print("=" * 64)
    print(f"  3D split complete  ({args.mode})")
    print("=" * 64)
    print(f"  top module          : {modname}")
    print(f"  cells total         : {summary['num_cells_total']}")
    print(f"    -> top (_top)     : {summary['num_cells_top']}  (combinational)")
    print(f"    -> bottom(_bottom): {summary['num_cells_bottom']}  (sequential)")
    print(f"  nets total          : {summary['num_nets']}")
    print(f"  cross-die nets (HBT): {summary['num_cross_die_nets_hbt']}")
    print(f"  pins placed         : {summary['num_pins_placed']}")
    print(f"  outputs in          : {outdir}")
    if summary["num_cross_die_nets_hbt"] == 0:
        print("  WARNING: no cross-die nets -> C_hbt sweep will not affect timing.")
    print("=" * 64)


if __name__ == "__main__":
    main()
