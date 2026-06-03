# PDK / Liberty libraries

The assignment requires a **heterogeneous** corner setup:

| Die | Corner | Why |
|-----|--------|-----|
| **Top** (combinational, `_top`) | **SS** (Slow-Slow) | worst-case slow silicon |
| **Bottom** (registers, `_bottom`) | **FF** (Fast-Fast) | best-case fast silicon |

This deliberately mixes a slow and a fast die so the analysis really is
*heterogeneous* 3D STA, not a single-corner run.

## What's in this folder

`asap7_ss_placeholder.lib`, `asap7_ff_placeholder.lib` — **placeholders only**.
They keep the example commands self-consistent when building against the CPU
**stub** (which never opens `.lib` files). They are *not* valid timing data.

## Getting real libraries

Pick **one** PDK and drop the SS/FF `.lib` files here, then point the driver at
them with `--top-lib` / `--bot-lib`.

### Option A — ASAP7 (7 nm predictive, recommended)
- Repo: https://github.com/The-OpenROAD-Project/asap7
- Use the `_SS` library as the top-die lib and the `_FF` library as the
  bottom-die lib, e.g.:
  - `asap7sc7p5t_AO_RVT_SS_nldm_*.lib`  → `--top-lib`
  - `asap7sc7p5t_AO_RVT_FF_nldm_*.lib`  → `--bot-lib`
  (Combine the AO/INVBUF/OA/SEQ/SIMPLE groups with repeated `--top-lib` flags.)

### Option B — SkyWater Sky130
- Repo: https://github.com/google/skywater-pdk (or the `sky130_fd_sc_hd` libs
  bundled with OpenROAD-flow-scripts).
- `sky130_fd_sc_hd__ss_*.lib` → `--top-lib`,  `..._ff_*.lib` → `--bot-lib`.

### Option C — HeteroSTA3D's bundled PDK
- The vendor release ships its own PDK; use those `.lib` files directly. This
  is the safest choice for first-time bring-up since they are known-compatible
  with the engine.

## Cell-name suffix consideration (important)

Stage 1 appends `_top` / `_bottom` to **cell instances *and* cell types** in the
netlist. There are two ways the engine can match those to a `.lib`:

1. **Suffix-stripping (assumed here):** the engine strips `_top`/`_bottom` and
   looks the base cell up in the die's liberty set. → use the standard ASAP7
   libs unchanged. This is the default assumption of this project.
2. **Literal match:** if the engine looks up the *suffixed* name verbatim, the
   `.lib` must also define `CELL_top` / `CELL_bottom`. In that case run
   `scripts/suffix_liberty.py` (see comment in that file) to clone each library
   with suffixed cell names.

On the very first real run, watch for `cell not found` errors; if you see them,
switch to approach (2). See `docs/API_NOTES.md`.
