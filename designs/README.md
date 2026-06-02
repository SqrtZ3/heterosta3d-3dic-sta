# Designs / netlists

Stage 1 (`scripts/split_3d_netlist.py`) expects a **flat gate-level Verilog**
netlist (the output of synthesis), not RTL.

## What's here

- `gcd_sample.v` — a small, hand-written Nangate45-style stand-in so the whole
  pipeline runs without a synthesis tool. **Not** the real benchmark.
- `gcd_3d/` — Stage 1 output for `gcd_sample.v` (generated).
- `scaled/` — generated scaling-benchmark netlists (Stage 4).

## Getting the real `gcd` / `aes` netlists

### Option A — OpenROAD-flow-scripts (recommended)
```bash
git clone --recursive https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts
cd OpenROAD-flow-scripts && ./build_openroad.sh
source ./env.sh
cd flow
# synthesize gcd on a chosen PDK (e.g. asap7)
make DESIGN_CONFIG=./designs/asap7/gcd/config.mk synth
# the synthesized gate-level netlist lands in:
#   flow/results/asap7/gcd/base/1_synth.v
cp results/asap7/gcd/base/1_synth.v  <this-repo>/designs/gcd.v
# for aes:  DESIGN_CONFIG=./designs/asap7/aes/config.mk
```

### Option B — Yosys (lighter weight)
```bash
yosys -p "read_verilog gcd_rtl.v; \
          synth -top gcd; \
          dfflibmap -liberty asap7_SS.lib; \
          abc -liberty asap7_SS.lib; \
          write_verilog -noattr designs/gcd.v"
```

## Then run Stage 1 on the real netlist
```bash
python scripts/split_3d_netlist.py designs/gcd.v --top-module gcd \
       --outdir designs/gcd_3d
```

If your library's flip-flops are not detected as sequential, extend the
detection regex:
```bash
python scripts/split_3d_netlist.py designs/gcd.v --top-module gcd \
       --seq-regex "(?i)(dff|dlh|sdf|latch|YOUR_FF_PREFIX)" --outdir designs/gcd_3d
```

## Note on single-pin nets (HeteroSTA3D issue #1)
HeteroSTA3D asserts `npins >= 2` per net. Synthesized netlists can contain
single-pin nets (tie cells, dangling outputs). If the engine aborts in
`spef_cuda_kernel.cu`, remove tie-offs / unconnected outputs during synthesis,
or call `heterosta3d_ignore_net()` on the offending nets. The driver already
skips empty connections (e.g. `.QN()`).
