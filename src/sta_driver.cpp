// ===========================================================================
//  sta_driver.cpp  --  HeteroSTA3D 3D-STA driver (assignment Stages 2/3/4)
// ===========================================================================
//
//  Implements the official 9-step "standard 3D STA workflow":
//
//    1. heterosta3d_init_license / new          (Engine ctor)
//    2. create liberty sets   (Top=SS, Bottom=FF; each set Early+Late)
//    3. create delay corner   (ss_ff, bound to a device)
//    4. read 3D netlist       (cells carry _top/_bottom suffixes)
//    5. flatten_all + build_graph
//    6. read_sdc
//    7. extract_rc_from_placement  (pin coords + HBT coords + R/C)
//    8. update_delay + update_arrivals
//    9. report WNS/TNS (setup=max, hold=min) + dump critical paths
//
//  Three run modes (--mode):
//    single   Stage 2: one corner, print WNS/TNS, dump worst Setup/Hold paths.
//    sweep    Stage 3: sweep C_hbt, record WNS to results/sweep.csv.
//    devices  Stage 4: run the same workload on CPU vs each GPU, time it, and
//             write results/devices.csv for the speed-up analysis.
//
//  Build: against the real vendor .so, or against the bundled CPU stub
//  (-DH3D_USE_STUB).  See CMakeLists.txt / Makefile.
// ===========================================================================

#include "h3d_wrapper.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

// ----------------------------------------------------------------------------
// Configuration (populated from argv).
// ----------------------------------------------------------------------------
struct Config {
    std::string netlist   = "designs/gcd_3d/design_3d.v";
    std::string placement = "designs/gcd_3d/placement.csv";
    std::vector<std::string> top_libs;   // SS corner libs (top die)
    std::vector<std::string> bot_libs;   // FF corner libs (bottom die)
    std::string sdc       = "sdc/gcd.sdc";
    std::string outdir    = "results";
    std::string lic2d, lic3d;            // license files (optional in eval)

    std::string mode      = "single";    // single | sweep | devices
    std::string corner    = "ss_ff";
    std::string device    = "0";         // single/sweep: "cpu" or GPU index
    std::vector<std::string> devices{"cpu", "0"};  // devices mode

    h3d::RcParams rc;
    float hbt_r = 0.05f;     // kOhm, fixed
    float hbt_c = 1.0f;      // fF, fixed value for single/devices modes
    int   flute = 3;
    float die_width = 100.0f;

    // sweep range (fF)
    float c_start = 0.1f, c_stop = 5.0f, c_step = 0.5f;
};

uint8_t device_id_of(const std::string& s) {
    if (s == "cpu" || s == "CPU") return HETEROSTA3D_CPU_DEVICE_ID;
    return static_cast<uint8_t>(std::stoi(s));
}

std::string die_suffix(const std::string& celltype) {
    auto ends_with = [&](const char* suf) {
        std::string s(suf);
        return celltype.size() >= s.size() &&
               celltype.compare(celltype.size() - s.size(), s.size(), s) == 0;
    };
    if (ends_with("_top"))    return "_top";
    if (ends_with("_bottom")) return "_bottom";
    return "";
}

// ----------------------------------------------------------------------------
// Geometry: per-pin coordinates + per-cross-die-net HBT coordinates.
// ----------------------------------------------------------------------------
struct Geometry {
    std::vector<float> pos_x, pos_y;   // length = num_pins
    std::vector<float> hbt_x, hbt_y;   // length = #cross-die nets
    size_t num_pins = 0, num_cross = 0, placed = 0, missed = 0;
};

// Load placement.csv -> map pin_name -> (x,y).
std::map<std::string, std::pair<float, float>> load_placement(const std::string& path) {
    std::map<std::string, std::pair<float, float>> m;
    std::ifstream in(path);
    if (!in) { std::cerr << "warning: cannot open placement " << path << "\n"; return m; }
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string name, sx, sy;
        std::getline(ss, name, ',');
        std::getline(ss, sx, ',');
        std::getline(ss, sy, ',');
        if (name.empty()) continue;
        try { m[name] = {std::stof(sx), std::stof(sy)}; } catch (...) {}
    }
    return m;
}

// Build pin coordinates (via lookup_pin) and HBT coordinates (via topology).
Geometry build_geometry(h3d::Engine& eng, const Config& cfg) {
    Geometry g;
    g.num_pins = eng.num_pins();
    const float center = cfg.die_width * 0.5f;
    g.pos_x.assign(g.num_pins, center);
    g.pos_y.assign(g.num_pins, center);

    auto place = load_placement(cfg.placement);
    for (const auto& [name, xy] : place) {
        size_t id = eng.lookup_pin(name);
        if (id < g.num_pins) {
            g.pos_x[id] = xy.first;
            g.pos_y[id] = xy.second;
            ++g.placed;
        } else {
            ++g.missed;
        }
    }
    if (g.num_pins && g.missed > g.placed) {
        std::cerr << "warning: " << g.missed << "/" << (g.placed + g.missed)
                  << " placement pins failed lookup_pin().  Check --pin-sep / "
                     "naming convention against the vendor engine.\n";
    }

    // Cross-die nets -> HBTs, in ascending net-id order.
    const size_t num_nets = eng.num_nets();
    const uintptr_t* start = eng.net2pin_start(false);
    const uintptr_t* items = eng.net2pin_items(false);
    if (start && items) {
        for (size_t n = 0; n < num_nets; ++n) {
            bool has_top = false, has_bot = false;
            float sx = 0.0f, sy = 0.0f;
            size_t cnt = 0;
            for (uintptr_t k = start[n]; k < start[n + 1]; ++k) {
                uintptr_t pin = items[k];
                std::string suf = die_suffix(eng.celltype_of_pin(pin));
                if (suf == "_top")    has_top = true;
                if (suf == "_bottom") has_bot = true;
                if (pin < g.num_pins) { sx += g.pos_x[pin]; sy += g.pos_y[pin]; ++cnt; }
            }
            if (has_top && has_bot) {           // crosses the bond interface
                g.hbt_x.push_back(cnt ? sx / cnt : center);
                g.hbt_y.push_back(cnt ? sy / cnt : center);
            }
        }
    }
    g.num_cross = g.hbt_x.size();
    return g;
}

// ----------------------------------------------------------------------------
// Steps 1-5: licensing, liberty sets, netlist, graph (corner-independent).
// ----------------------------------------------------------------------------
void load_design(h3d::Engine& eng, const Config& cfg) {
    if (!h3d::Engine::init_license(cfg.lic2d, cfg.lic3d))
        std::cerr << "warning: init_license failed (continuing for eval build)\n";

    // Each liberty set needs BOTH early and late corners.
    // Top die -> Slow-Slow (SS).  Bottom die -> Fast-Fast (FF).
    if (!eng.create_liberty_set(EL_EARLY, "top_ss", cfg.top_libs) ||
        !eng.create_liberty_set(EL_LATE,  "top_ss", cfg.top_libs))
        std::cerr << "warning: failed to create top_ss liberty set\n";
    if (!eng.create_liberty_set(EL_EARLY, "bot_ff", cfg.bot_libs) ||
        !eng.create_liberty_set(EL_LATE,  "bot_ff", cfg.bot_libs))
        std::cerr << "warning: failed to create bot_ff liberty set\n";

    if (!eng.read_netlist(cfg.netlist)) {
        std::cerr << "error: read_netlist failed: " << cfg.netlist << "\n";
        std::exit(2);
    }
    eng.flatten_all();
    eng.build_graph();
}

// One corner: create -> sdc -> extract_rc(c_hbt) -> update -> report.
// Returns {setup, hold}.  Optionally times the heavy phases (ms).
struct CornerResult {
    h3d::WnsTns setup, hold;
    double extract_ms = 0, update_ms = 0;
};

CornerResult run_corner(h3d::Engine& eng, const Config& cfg, const Geometry& g,
                        const std::string& dc, uint8_t device_id, float c_hbt,
                        bool create = true) {
    using clk = std::chrono::high_resolution_clock;
    CornerResult r;
    if (create) {
        if (!eng.create_delay_corner(dc, "top_ss", "bot_ff", device_id))
            std::cerr << "warning: create_delay_corner(" << dc << ") failed\n";
        if (!eng.read_sdc(cfg.sdc, dc))
            std::cerr << "warning: read_sdc(" << cfg.sdc << ") failed\n";
    }

    auto t0 = clk::now();
    eng.extract_rc(g.pos_x, g.pos_y, g.hbt_x, g.hbt_y, cfg.rc, cfg.hbt_r, c_hbt,
                   cfg.flute, dc);
    auto t1 = clk::now();
    eng.update_delay(dc);
    eng.update_arrivals(dc);
    auto t2 = clk::now();

    r.extract_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.update_ms  = std::chrono::duration<double, std::milli>(t2 - t1).count();
    r.setup = eng.report_setup(dc);
    r.hold  = eng.report_hold(dc);
    return r;
}

// ----------------------------------------------------------------------------
// Mode: single (Stage 2)
// ----------------------------------------------------------------------------
int mode_single(h3d::Engine& eng, const Config& cfg, const Geometry& g) {
    const std::string dc = cfg.corner;
    CornerResult r = run_corner(eng, cfg, g, dc, device_id_of(cfg.device), cfg.hbt_c);

    std::cout << "\n================ Stage 2: single-corner STA ================\n";
    std::cout << "  corner       : " << dc << "  (Top=SS, Bottom=FF)\n";
    std::cout << "  device       : " << cfg.device << "\n";
    std::cout << "  C_hbt        : " << cfg.hbt_c << " fF   R_hbt: " << cfg.hbt_r << " kOhm\n";
    std::cout << "  pins         : " << g.num_pins << "   HBTs: " << g.num_cross << "\n";
    std::cout << "  ----------------------------------------------------------\n";
    std::cout << "  Setup  WNS = " << r.setup.wns << " ns   TNS = " << r.setup.tns << " ns\n";
    std::cout << "  Hold   WNS = " << r.hold.wns  << " ns   TNS = " << r.hold.tns  << " ns\n";
    std::cout << "============================================================\n";

    eng.dump_paths_setup(8, 1, cfg.outdir + "/setup_paths.rpt", dc);
    eng.dump_paths_hold (8, 1, cfg.outdir + "/hold_paths.rpt",  dc);

    std::ofstream s(cfg.outdir + "/single_corner.txt");
    s << "HeteroSTA3D single-corner report\n"
      << "corner=" << dc << " device=" << cfg.device
      << " C_hbt=" << cfg.hbt_c << "fF R_hbt=" << cfg.hbt_r << "kOhm\n"
      << "Setup WNS=" << r.setup.wns << " TNS=" << r.setup.tns << "\n"
      << "Hold  WNS=" << r.hold.wns  << " TNS=" << r.hold.tns  << "\n";
    std::cout << "  wrote " << cfg.outdir << "/{setup,hold}_paths.rpt, single_corner.txt\n";
    return 0;
}

// ----------------------------------------------------------------------------
// Mode: sweep (Stage 3)
// ----------------------------------------------------------------------------
int mode_sweep(h3d::Engine& eng, const Config& cfg, const Geometry& g) {
    const std::string dc = cfg.corner;
    // Build the value list: c_start, +c_step .. <= c_stop, and ensure c_stop in.
    std::vector<float> values;
    for (float c = cfg.c_start; c <= cfg.c_stop + 1e-6f; c += cfg.c_step)
        values.push_back(c);
    if (values.empty() || std::abs(values.back() - cfg.c_stop) > 1e-3f)
        values.push_back(cfg.c_stop);

    const std::string csv = cfg.outdir + "/sweep.csv";
    std::ofstream out(csv);
    out << "c_hbt_fF,setup_wns,setup_tns,hold_wns,hold_tns\n";

    std::cout << "\n================ Stage 3: C_hbt sweep ======================\n";
    std::cout << "  corner " << dc << "  device " << cfg.device
              << "  points " << values.size() << "\n";
    std::cout << "  C_hbt[fF]   Setup_WNS    Hold_WNS\n";

    bool first = true;
    for (float c : values) {
        // Create the corner once; subsequent iterations just re-extract RC.
        CornerResult r = run_corner(eng, cfg, g, dc, device_id_of(cfg.device), c,
                                    /*create=*/first);
        first = false;
        out << c << "," << r.setup.wns << "," << r.setup.tns << ","
            << r.hold.wns << "," << r.hold.tns << "\n";
        std::cout << "  " << std::setw(6) << c << "      "
                  << std::setw(9) << r.setup.wns << "   "
                  << std::setw(9) << r.hold.wns << "\n";
    }
    std::cout << "  wrote " << csv << "\n";
    std::cout << "============================================================\n";
    return 0;
}

// ----------------------------------------------------------------------------
// Mode: devices (Stage 4 -- advanced)
// ----------------------------------------------------------------------------
int mode_devices(h3d::Engine& eng, const Config& cfg, const Geometry& g) {
    const std::string csv = cfg.outdir + "/devices.csv";
    std::ofstream out(csv);
    out << "device,setup_wns,hold_wns,extract_ms,update_ms,total_ms\n";

    std::cout << "\n=========== Stage 4: multi-device timing ====================\n";
    std::cout << "  device     Setup_WNS   Hold_WNS   extract[ms]  update[ms]\n";

    std::map<std::string, double> total_ms;
    for (const auto& dev : cfg.devices) {
        const std::string dc = cfg.corner + "_" + dev;
        CornerResult r = run_corner(eng, cfg, g, dc, device_id_of(dev), cfg.hbt_c);
        double tot = r.extract_ms + r.update_ms;
        total_ms[dev] = tot;
        out << dev << "," << r.setup.wns << "," << r.hold.wns << ","
            << r.extract_ms << "," << r.update_ms << "," << tot << "\n";
        std::cout << "  " << std::setw(6) << dev << "    "
                  << std::setw(9) << r.setup.wns << "  "
                  << std::setw(9) << r.hold.wns << "   "
                  << std::setw(9) << r.extract_ms << "   "
                  << std::setw(9) << r.update_ms << "\n";
    }

    // Speed-up vs CPU baseline (if present).
    if (total_ms.count("cpu")) {
        std::cout << "  ---- speed-up vs CPU (update phase) ----\n";
        for (const auto& [dev, t] : total_ms)
            if (dev != "cpu" && t > 0)
                std::cout << "    " << dev << " : " << (total_ms["cpu"] / t) << "x\n";
    }
    std::cout << "  wrote " << csv << "\n";
    std::cout << "============================================================\n";
    return 0;
}

// ----------------------------------------------------------------------------
// Argument parsing
// ----------------------------------------------------------------------------
void usage() {
    std::cout <<
      "Usage: sta_driver [options]\n"
      "  --netlist FILE        3D netlist (default designs/gcd_3d/design_3d.v)\n"
      "  --placement FILE      pin placement csv\n"
      "  --top-lib FILE        SS .lib for top die (repeatable)\n"
      "  --bot-lib FILE        FF .lib for bottom die (repeatable)\n"
      "  --sdc FILE            SDC constraints\n"
      "  --outdir DIR          output directory (default results)\n"
      "  --lic2d / --lic3d F   license files\n"
      "  --mode M              single | sweep | devices\n"
      "  --corner NAME         delay corner name (default ss_ff)\n"
      "  --device D            cpu | 0 | 1 ...   (single/sweep)\n"
      "  --devices LIST        comma list e.g. cpu,0,1 (devices mode)\n"
      "  --c-hbt F             fixed C_hbt fF (single/devices)\n"
      "  --hbt-r F             R_hbt kOhm (default 0.05)\n"
      "  --c-start/--c-stop/--c-step  sweep range fF (default 0.1/5.0/0.5)\n"
      "  --flute N             FLUTE accuracy (default 3)\n"
      "  --die-width F         die footprint um (default 100)\n";
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> v; std::stringstream ss(s); std::string t;
    while (std::getline(ss, t, ',')) if (!t.empty()) v.push_back(t);
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    auto need = [&](int& i) -> std::string {
        if (i + 1 >= argc) { std::cerr << "missing value for " << argv[i] << "\n"; std::exit(1); }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--netlist")   cfg.netlist = need(i);
        else if (a == "--placement") cfg.placement = need(i);
        else if (a == "--top-lib")   cfg.top_libs.push_back(need(i));
        else if (a == "--bot-lib")   cfg.bot_libs.push_back(need(i));
        else if (a == "--sdc")       cfg.sdc = need(i);
        else if (a == "--outdir")    cfg.outdir = need(i);
        else if (a == "--lic2d")     cfg.lic2d = need(i);
        else if (a == "--lic3d")     cfg.lic3d = need(i);
        else if (a == "--mode")      cfg.mode = need(i);
        else if (a == "--corner")    cfg.corner = need(i);
        else if (a == "--device")    cfg.device = need(i);
        else if (a == "--devices")   cfg.devices = split_csv(need(i));
        else if (a == "--c-hbt")     cfg.hbt_c = std::stof(need(i));
        else if (a == "--hbt-r")     cfg.hbt_r = std::stof(need(i));
        else if (a == "--c-start")   cfg.c_start = std::stof(need(i));
        else if (a == "--c-stop")    cfg.c_stop = std::stof(need(i));
        else if (a == "--c-step")    cfg.c_step = std::stof(need(i));
        else if (a == "--flute")     cfg.flute = std::stoi(need(i));
        else if (a == "--die-width") cfg.die_width = std::stof(need(i));
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::cerr << "unknown option: " << a << "\n"; usage(); return 1; }
    }

    std::cout << std::fixed << std::setprecision(4);

    h3d::Engine eng;                 // RAII: freed automatically
    load_design(eng, cfg);           // steps 1-5
    Geometry g = build_geometry(eng, cfg);

    if (cfg.mode == "single")  return mode_single(eng, cfg, g);
    if (cfg.mode == "sweep")   return mode_sweep(eng, cfg, g);
    if (cfg.mode == "devices") return mode_devices(eng, cfg, g);
    std::cerr << "unknown mode: " << cfg.mode << "\n";
    return 1;
}
