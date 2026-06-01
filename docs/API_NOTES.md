# HeteroSTA3D API notes (reconstructed)

Source: <https://heterosta3d.pkueda.org.cn/documentation> (get-started +
API reference) and the project's GitHub issues. The authoritative header ships
inside the vendor release tarball; `third_party/heterosta3d/include/heterosta3d.h`
in this repo is a faithful re-declaration so the code type-checks and the stub
can mirror the ABI.

## The standard 9-step 3D STA workflow

| Step | Call(s) | Notes |
|------|---------|-------|
| 1 | `heterosta3d_init_license`, `heterosta3d_new` | licenses are process-global |
| 2 | `heterosta3d_create_liberty_set_batch` | **call twice per set** — once `EL_EARLY`, once `EL_LATE` |
| 3 | `heterosta3d_create_delay_corner` | binds top+bottom libsets to a `device_id` |
| 4 | `heterosta3d_read_netlist` | cell instances **must** carry `_top`/`_bottom` |
| 5 | `heterosta3d_flatten_all` → `heterosta3d_build_graph` | order matters |
| 6 | `heterosta3d_read_sdc` | per delay corner |
| 7 | `heterosta3d_extract_rc_from_placement` | pin coords + HBT coords + R/C |
| 8 | `heterosta3d_update_delay` → `heterosta3d_update_arrivals` | order matters, per corner |
| 9 | `heterosta3d_report_wns_tns_max/min`, `*_dump_paths_*` | max = Setup, min = Hold |

## Key semantics

- **early/late**: `EL_EARLY` (min, fast) feeds **Hold**; `EL_LATE` (max, slow)
  feeds **Setup**. A liberty *set* must contain both.
- **Heterogeneous corner**: top libset = SS, bottom libset = FF, combined into
  one delay corner (e.g. `ss_ff`).
- **device_id**: `HETEROSTA3D_CPU_DEVICE_ID` (== `UINT8_MAX` == 255) selects the
  CPU back-end; `0,1,2,…` select CUDA GPUs. This is the hook for the Stage-4
  CPU-vs-GPU and multi-GPU experiments.
- **`max` vs `min` reports**: `report_wns_tns_max` = Setup (late) WNS/TNS;
  `report_wns_tns_min` = Hold (early). Same split for `dump_paths_*` and
  `report_slacks_at_*`.

## `extract_rc_from_placement` parameters

```c
void heterosta3d_extract_rc_from_placement(
    Heterosta3D *sta,
    const float *pos_x, const float *pos_y,   // per-PIN, length = get_num_of_pins()
    const float *hbt_x, const float *hbt_y,   // per cross-die net (HBT)
    float unit_cap_x_top, unit_cap_y_top,     // top-die wire C  [fF/um]
    float unit_res_x_top, unit_res_y_top,     // top-die wire R  [kOhm/um]
    float unit_cap_x_btm, unit_cap_y_btm,     // bottom-die wire C
    float unit_res_x_btm, unit_res_y_btm,     // bottom-die wire R
    float hbt_r, float hbt_c,                  // one vertical bond: R [kOhm], C [fF]
    int   flute_accuracy,                      // Steiner-tree accuracy
    const char *dc_name);
```

- `pos_x/pos_y` are indexed by internal pin id. Build the array by calling
  `heterosta3d_lookup_pin(name)` for each placed pin and writing at that index.
  (Our driver does exactly this from `placement.csv`.)
- `hbt_x/hbt_y` give one coordinate per **cross-die net**. Our driver discovers
  cross-die nets from the topology (`get_net2pin_*` + `internal_get_celltype_of_pin`,
  checking for both `_top` and `_bottom` pins on the net) in ascending net-id
  order, and uses the net's centroid as the HBT location.
- **`hbt_c` is the Stage-3 sweep knob.** Raising it adds load capacitance to
  the cross-die net and series RC through the bond → more delay.

## Assumptions to confirm on first real run (search code for `[ASSUMPTION]`)

1. **Suffix → liberty mapping.** We assume the engine strips `_top`/`_bottom`
   and looks the base cell up in the appropriate die's libset, so standard
   ASAP7 libs work unchanged. If you hit `cell not found`, suffix the libs with
   `scripts/suffix_liberty.py` (see `pdk/README.md`).
2. **HBT array ordering** = ascending cross-die net id. If the vendor
   `run_cpu.cpp` example uses a different convention, adjust `build_geometry()`
   in `src/sta_driver.cpp`.
3. **Pin-name separator** (`inst/pin`). If `lookup_pin` misses most pins, change
   `--pin-sep` in Stage 1 and the driver to match the engine's convention; the
   driver prints a warning when >50% of placement lookups fail.

## Relevant GitHub issues

- **#1** `npins >= 2` assertion on single-pin nets → see `designs/README.md`.
- **#3** "no reg-to-reg path" → WNS/TNS come back empty if the design has no
  register-to-register paths. Our `logic_split` mode guarantees every
  reg→reg path crosses the bond (FF_bottom → logic_top → FF_bottom), so paths
  always exist and are sensitive to `C_hbt`.
- **#2 / #6** requests for CUDA 12.x builds → the current release targets
  CUDA 11; see `docs/ENVIRONMENT.md` for the Blackwell (RTX 50-series) caveat.
