/* ===========================================================================
 *  heterosta3d.h  --  Reconstructed C API header for HeteroSTA3D
 * ===========================================================================
 *
 *  HeteroSTA3D is a GPU-accelerated static-timing-analysis engine for
 *  face-to-face hybrid-bonded heterogeneous 3D ICs.
 *      Project page : https://heterosta3d.pkueda.org.cn/
 *      API reference: https://heterosta3d.pkueda.org.cn/documentation/api-reference
 *
 *  WHY THIS FILE EXISTS
 *  --------------------
 *  The official library ships its own <heterosta3d.h> inside the release
 *  tarball (v1.1-20260129, Linux x86-64 + CUDA).  That header is the ground
 *  truth.  This file is a faithful re-declaration of the public C ABI as
 *  documented on the API-reference page, so that:
 *
 *    1. The C++ driver in this repository compiles and is type-checked even
 *       on a machine that does not have the vendor tarball unpacked yet.
 *    2. The bundled CPU *stub* (stub/heterosta3d_stub.cpp) can implement the
 *       exact same ABI, letting you build & run the whole pipeline locally
 *       (e.g. inside WSL2) with no GPU and no license.
 *
 *  >>> ON THE REAL MACHINE: delete / shadow this header with the vendor's
 *      official heterosta3d.h (point the include path at the tarball's
 *      include/ directory) so you always link against the authoritative
 *      declarations.  See CMakeLists.txt -> H3D_USE_STUB option.
 *
 *  All signatures below are transcribed verbatim from the published API
 *  reference.  Comments marked [ASSUMPTION] flag semantics the public docs
 *  leave implicit; verify them against the vendor header / run_cpu.cpp on the
 *  first real run.
 * ========================================================================= */

#ifndef HETEROSTA3D_H
#define HETEROSTA3D_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   /* uintptr_t */

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 *  Opaque handles
 * ------------------------------------------------------------------------ */

/* Main engine instance. Internals are private to the library. */
typedef struct Heterosta3D Heterosta3D;

/* External netlist database (declared in the vendor's netlistdb.h).
 * Only needed if you build the netlist programmatically instead of from
 * Verilog; the standard flow uses heterosta3d_read_netlist() and never
 * touches this type. */
struct NetlistDB;

/* --------------------------------------------------------------------------
 *  Enumerations & constants
 * ------------------------------------------------------------------------ */

/* Early/Late timing split. A liberty *set* must contain BOTH an Early and a
 * Late corner (min and max operating conditions). */
typedef enum {
    EL_EARLY = 0,   /* min / fast condition  -> drives Hold (min) analysis  */
    EL_LATE  = 1,   /* max / slow condition  -> drives Setup (max) analysis */
} early_late_t;

/* Pin direction as reported by heterosta3d_query_pin_direction(). */
typedef enum {
    I       = 0,    /* input  */
    O       = 1,    /* output */
    Unknown = 2,    /* undetermined */
} NetlistDirection;

/* Sentinel device id selecting the CPU back-end for a delay corner.
 * Any other value (0, 1, 2, ...) selects the corresponding CUDA GPU. */
#define HETEROSTA3D_CPU_DEVICE_ID UINT8_MAX   /* == 255 */

/* --------------------------------------------------------------------------
 *  1. Lifecycle / licensing
 * ------------------------------------------------------------------------ */

/* Validate the 2D and 3D license files. Must succeed before heavy APIs run.
 * Returns true on success. Pass NULL/"" if your build is unlicensed-eval. */
bool heterosta3d_init_license(const char *lic_2d, const char *lic_3d);

/* Allocate / free an engine instance. */
Heterosta3D *heterosta3d_new(void);
void         heterosta3d_free(Heterosta3D *sta);

/* Reset analysis state (keeps nothing of the previous design). */
void         heterosta3d_reset(Heterosta3D *sta);

/* --------------------------------------------------------------------------
 *  2. Liberty sets   &   3. Delay corners
 * ------------------------------------------------------------------------ */

/* Create (or extend) a named liberty set for ONE early/late side.
 * Call twice per set -- once with EL_EARLY, once with EL_LATE -- so the set
 * carries both corners. `lib_paths` is an array of `num_paths` .lib file
 * paths that get merged into the set. */
bool heterosta3d_create_liberty_set_batch(Heterosta3D       *sta,
                                          early_late_t       el,
                                          const char        *set_name,
                                          const char *const *lib_paths,
                                          uintptr_t          num_paths);

/* Combine a top-die liberty set and a bottom-die liberty set into a named
 * delay corner, bound to a specific compute device.
 *   device_id == HETEROSTA3D_CPU_DEVICE_ID -> run this corner on the CPU
 *   device_id == 0,1,2,...                  -> run on that CUDA GPU
 * [ASSUMPTION] The engine routes each cell instance to top_libset or
 * btm_libset by its `_top` / `_bottom` name suffix. */
bool heterosta3d_create_delay_corner(Heterosta3D *sta,
                                     const char  *dc_name,
                                     const char  *top_libset_name,
                                     const char  *btm_libset_name,
                                     uint8_t      device_id);

/* --------------------------------------------------------------------------
 *  4. Netlist  &  5. Timing graph
 * ------------------------------------------------------------------------ */

/* Read a (possibly multi-file-merged) gate-level Verilog netlist.
 * Cell instances MUST carry a `_top` or `_bottom` suffix so the engine can
 * assign each to a die. Returns true on success. */
bool heterosta3d_read_netlist(Heterosta3D *sta, const char *verilog_path);

/* Inject a pre-built NetlistDB instead of reading Verilog (advanced). */
void heterosta3d_set_netlistdb(Heterosta3D *sta, struct NetlistDB *netlistdb);

/* Flatten the hierarchy, then build the internal timing graph.
 * Call flatten_all() before build_graph(). */
void heterosta3d_flatten_all(Heterosta3D *sta);
void heterosta3d_build_graph(Heterosta3D *sta);

/* --------------------------------------------------------------------------
 *  6. Constraints (SDC)
 * ------------------------------------------------------------------------ */

/* Apply one SDC file to a delay corner. */
bool heterosta3d_read_sdc(Heterosta3D *sta,
                          const char  *sdc_path,
                          const char  *dc_name);

/* Apply several SDC files to a delay corner in one call. */
bool heterosta3d_batch_read_sdc(Heterosta3D       *sta,
                                const char *const *paths,
                                uintptr_t          num_sdc,
                                const char        *dc_name);

/* Force all slews to zero for a corner (useful for ideal-clock sanity runs). */
void heterosta3d_zero_slew(Heterosta3D *sta, const char *dc_name);

/* --------------------------------------------------------------------------
 *  7. 3D RC parasitic extraction
 * ------------------------------------------------------------------------ */

/* Build RC parasitics from a placement.
 *
 *   pos_x, pos_y : per-PIN coordinates, length == heterosta3d_get_num_of_pins(),
 *                  indexed by internal pin id (see lookup_pin()). [ASSUMPTION:
 *                  units are microns; same x/y footprint is shared by both dies
 *                  in a face-to-face stack, the die is chosen by name suffix.]
 *   hbt_x, hbt_y : per-HBT (hybrid-bonding terminal) coordinates -- one entry
 *                  per cross-die net. [ASSUMPTION: ordered by ascending net id
 *                  of the 3D nets; confirm against run_cpu.cpp.]
 *   unit_cap_*_top / unit_res_*_top : per-unit-length wire C (fF/um) and
 *                  R (kOhm/um) along x and y for the TOP die.
 *   unit_cap_*_btm / unit_res_*_btm : same for the BOTTOM die.
 *   hbt_r, hbt_c : resistance (kOhm) and capacitance (fF) of ONE vertical
 *                  hybrid-bond connection (the knob swept in Stage 3).
 *   flute_accuracy : Steiner-tree (FLUTE) accuracy level for RC topology.
 *   dc_name        : the delay corner this parasitic model is attached to.
 */
void heterosta3d_extract_rc_from_placement(Heterosta3D *sta,
                                           const float *pos_x,
                                           const float *pos_y,
                                           const float *hbt_x,
                                           const float *hbt_y,
                                           float unit_cap_x_top, float unit_cap_y_top,
                                           float unit_res_x_top, float unit_res_y_top,
                                           float unit_cap_x_btm, float unit_cap_y_btm,
                                           float unit_res_x_btm, float unit_res_y_btm,
                                           float hbt_r, float hbt_c,
                                           int   flute_accuracy,
                                           const char *dc_name);

/* --------------------------------------------------------------------------
 *  8. Delay / arrival propagation
 * ------------------------------------------------------------------------ */

/* Run in this order, per corner. */
void heterosta3d_update_delay(Heterosta3D *sta, const char *dc_name);
void heterosta3d_update_arrivals(Heterosta3D *sta, const char *dc_name);

/* --------------------------------------------------------------------------
 *  9. Timing reports
 * ------------------------------------------------------------------------ */

/* Worst negative slack / total negative slack.
 *   _max -> Setup (late) analysis
 *   _min -> Hold  (early) analysis
 * Writes results through the wns/tns pointers. Returns true on success. */
bool heterosta3d_report_wns_tns_max(Heterosta3D *sta, float *wns, float *tns,
                                    const char *dc_name);
bool heterosta3d_report_wns_tns_min(Heterosta3D *sta, float *wns, float *tns,
                                    const char *dc_name);

/* Per-endpoint slack. `slack[i][0]` / `slack[i][1]` hold the rise/fall (or
 * the two transition) slacks for endpoint i; the caller allocates an array of
 * heterosta3d_get_num_of_pins() rows. */
void heterosta3d_report_slacks_at_max(Heterosta3D *sta, float (*slack)[2],
                                      const char *dc_name);
void heterosta3d_report_slacks_at_min(Heterosta3D *sta, float (*slack)[2],
                                      const char *dc_name);

/* --------------------------------------------------------------------------
 *  Path dumping & file output
 * ------------------------------------------------------------------------ */

/* Dump the worst timing paths to a text report.
 *   num_paths : number of distinct endpoints to report
 *   nworst    : number of worst paths per endpoint
 *   _max -> Setup paths, _min -> Hold paths */
void heterosta3d_dump_paths_max_to_file(Heterosta3D *sta,
                                        uintptr_t num_paths, uintptr_t nworst,
                                        const char *file_path, const char *dc_name);
void heterosta3d_dump_paths_min_to_file(Heterosta3D *sta,
                                        uintptr_t num_paths, uintptr_t nworst,
                                        const char *file_path, const char *dc_name);

/* Write extracted parasitics / computed delays to standard EDA formats. */
bool heterosta3d_write_spef(Heterosta3D *sta, const char *spef_path,
                            const char *dc_name);
bool heterosta3d_report_delay_sdf(Heterosta3D *sta, const char *sdf_path,
                                  const char *dc_name);

/* --------------------------------------------------------------------------
 *  Design modification
 * ------------------------------------------------------------------------ */

/* Re-map cell types in bulk (e.g. resize / re-VT). Returns #cells updated. */
uintptr_t heterosta3d_batch_update_celltypes(Heterosta3D       *sta,
                                             const char *const *celltypes,
                                             uintptr_t          num_cells);

/* Drop a net from analysis (e.g. a problematic single-pin net). */
bool heterosta3d_ignore_net(Heterosta3D *sta, uintptr_t net_id);

/* --------------------------------------------------------------------------
 *  Netlist topology queries
 * ------------------------------------------------------------------------ */

uintptr_t heterosta3d_get_num_of_pins(Heterosta3D *sta);
uintptr_t heterosta3d_get_num_of_nets(Heterosta3D *sta);

/* CSR-style net<->pin connectivity. With use_cuda==true the returned pointer
 * lives in device memory. Use use_cuda==false for host inspection. */
const uintptr_t *heterosta3d_get_pin2net(Heterosta3D *sta, bool use_cuda);
const uintptr_t *heterosta3d_get_net2pin_start(Heterosta3D *sta, bool use_cuda);
const uintptr_t *heterosta3d_get_net2pin_items(Heterosta3D *sta, bool use_cuda);

/* Name <-> id helpers. */
uintptr_t        heterosta3d_lookup_pin(Heterosta3D *sta, const char *pin_name);
NetlistDirection heterosta3d_query_pin_direction(Heterosta3D *sta, uintptr_t pin_id);
const char      *heterosta3d_internal_get_celltype_of_pin(Heterosta3D *sta,
                                                          uintptr_t pin_id);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* HETEROSTA3D_H */
