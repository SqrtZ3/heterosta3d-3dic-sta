#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
suffix_liberty.py  --  append a die suffix to every cell name in a .lib
=======================================================================

FALLBACK utility.  Only needed if the HeteroSTA3D engine matches cell masters
by their *suffixed* name (CELL_top / CELL_bottom) rather than stripping the
suffix.  In that case the liberty libraries must also define the suffixed cell
names.  This clones a library, rewriting every `cell (NAME)` -> `cell (NAME<suffix>)`.

Usage:
  python scripts/suffix_liberty.py asap7_SS.lib _top    -o pdk/top_ss.lib
  python scripts/suffix_liberty.py asap7_FF.lib _bottom -o pdk/bot_ff.lib
"""

import argparse
import re


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("lib")
    ap.add_argument("suffix", help="e.g. _top or _bottom")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()

    text = open(args.lib, encoding="utf-8", errors="replace").read()

    # cell (NAME) {     or    cell ("NAME") {
    def repl(m):
        name = m.group("name")
        return f'{m.group("kw")}({m.group("q")}{name}{args.suffix}{m.group("q")}'

    pat = re.compile(r'(?P<kw>\bcell\s*\()\s*(?P<q>"?)(?P<name>[A-Za-z0-9_./\\]+)(?P=q)\s*')
    out = pat.sub(lambda m: f'{m.group("kw")}{m.group("q")}{m.group("name")}{args.suffix}{m.group("q")}', text)

    with open(args.out, "w", encoding="utf-8") as f:
        f.write(out)
    n = len(pat.findall(text))
    print(f"wrote {args.out}  ({n} cells suffixed with '{args.suffix}')")


if __name__ == "__main__":
    main()
