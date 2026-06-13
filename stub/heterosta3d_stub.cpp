// ===========================================================================
//  heterosta3d_stub.cpp  --  CPU reference STUB of the HeteroSTA3D C API
// ===========================================================================
//
//  PURPOSE
//  -------
//  The real HeteroSTA3D engine only runs on Linux x86-64 + NVIDIA GPU + a
//  license.  This stub implements the *same C ABI* with a small, transparent
//  CPU mini-STA so that:
//
//    * the whole pipeline (Stage 1 -> 2 -> 3 -> 4) builds and runs anywhere,
//      including Windows/WSL2 with no GPU, for development & CI;
//    * the Python plotting / report tooling can be exercised end-to-end;
//    * the timing model reacts to C_hbt the way the physics says it should
//      (more bond capacitance -> more delay -> less Setup slack), so the
//      Stage-3 curve has the right shape before you ever touch the GPU.
//
//  IT IS NOT THE REAL ENGINE.  Numbers it produces are illustrative.  For
//  graded results, link the vendor .so instead (CMake: -DH3D_USE_STUB=OFF).
//
//  Timing model (deliberately simple, but monotone & physical):
//    cell_delay = base * corner_factor + load_factor * C_load_driven
//    net_delay  = 0.69 * R_total * C_total           (Elmore)
//       R_total = res_per_um * manhattan(driver,sink) [+ hbt_r if cross-die]
//       C_total = cap_per_um * manhattan(driver,sink) [+ hbt_c if cross-die]
//    corner_factor: top die = SS (slow) = 1.5 ; bottom die = FF (fast) = 0.6
//    Setup slack = T - (clk2q + longest_comb_path + setup_time)
//    Hold  slack = (clk2q + shortest_comb_path) - hold_time
//
//  Device timing is *simulated* (the stub sleeps) to mimic the GPU trade-off
//  the advanced section studies:
//    extract: GPU pays a fixed host->device copy overhead, CPU does not.
//    update : GPU has high throughput but a fixed kernel-launch overhead.
//  => small designs favour CPU (overhead dominates); large designs favour GPU.
// ===========================================================================

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
#include "heterosta3d.h"
}

// ----------------------------------------------------------------------------
// Internal data model
// ----------------------------------------------------------------------------
namespace {

bool is_output_pinname(const std::string& p) {
    static const std::set<std::string> outs = {
        "Z", "ZN", "Y", "Q", "QN", "CO", "CON", "S", "SO", "SUM", "OUT", "O"};
    return outs.count(p) > 0;
}

bool is_seq_celltype(const std::string& ct) {
    static const std::regex re(
        R"((?:^|_)(?:s?dff|dff|dfrtp|dfxtp|dfstp|edff|latch|dlxtp|dlrtp|dlh|dll|dhl|sdfl|dfl|async_dff|dffe)(?:_|[0-9x])?)",
        std::regex::icase);
    return std::regex_search(ct, re);
}

char die_of(const std::string& celltype) {
    auto ew = [&](const char* s) {
        size_t n = std::strlen(s);
        return celltype.size() >= n &&
               celltype.compare(celltype.size() - n, n, s) == 0;
    };
    if (ew("_top")) return 't';
    if (ew("_bottom")) return 'b';
    return '-';
}

struct Pin {
    std::string full;       // inst/PIN
    int inst = -1;
    std::string pinname;
    int net = -1;
    bool is_output = false;
};
struct Inst {
    std::string name, celltype;
    char die = '-';
    bool is_seq = false;
    std::vector<int> pins;
};
struct Net {
    std::string name;
    std::vector<int> pins;
    int driver = -1;        // pin id of the single driver (-1 = primary input)
};
struct Corner {
    std::string top_set, bot_set;
    uint8_t device = HETEROSTA3D_CPU_DEVICE_ID;
    double period = 1.0;    // ns, from SDC create_clock -period
    // results
    float setup_wns = 0, setup_tns = 0, hold_wns = 0, hold_tns = 0;
    std::vector<std::array<float, 2>> slack_max, slack_min;
    // last extraction
    bool extracted = false;
};

}  // namespace

// ----------------------------------------------------------------------------
// The opaque engine struct (definition private to the stub)
// ----------------------------------------------------------------------------
struct Heterosta3D {
    std::vector<Inst> insts;
    std::vector<Pin>  pins;
    std::vector<Net>  nets;
    std::unordered_map<std::string, int> pin_id, net_id;

    // CSR connectivity (built in build_graph)
    std::vector<uintptr_t> pin2net_;
    std::vector<uintptr_t> n2p_start_, n2p_items_;

    // geometry, cached from the most recent extract_rc call
    std::vector<float> pos_x, pos_y;

    std::map<std::string, Corner> corners;

    // scratch stashed by run_sta() for the path-dump routines
    int _last_worst_ep = -1;
    std::vector<double> _amax;
    std::vector<int> _predmax;

    // pin->net distance helper (Manhattan, microns)
    float dist(int a, int b) const {
        if (a < 0 || b < 0 || a >= (int)pos_x.size() || b >= (int)pos_x.size())
            return 10.0f;
        return std::fabs(pos_x[a] - pos_x[b]) + std::fabs(pos_y[a] - pos_y[b]);
    }
};

namespace {

// ---- tiny Verilog parser (gate-level structural) -------------------------
std::string strip_comments(std::string s) {
    // Strip /* ... */ (including multi-line) and // comments in ONE manual
    // pass.  We intentionally avoid std::regex over the whole file: libstdc++'s
    // regex recurses on the match and stack-overflows on large netlists, which
    // is exactly what failed CI at the k=128 scaling size (clang/libc++ on the
    // dev machine tolerated it; g++ on the CI runner did not).
    {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*') {
                size_t e = s.find("*/", i + 2);
                i = (e == std::string::npos) ? s.size() : e + 2;
            } else if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                size_t e = s.find('\n', i + 2);
                i = (e == std::string::npos) ? s.size() : e;
            } else {
                out.push_back(s[i++]);
            }
        }
        s.swap(out);
    }
    return s;
}

std::vector<std::string> top_split_semicolons(const std::string& body) {
    std::vector<std::string> v;
    int depth = 0;
    std::string cur;
    for (char c : body) {
        if (c == '(') depth++;
        else if (c == ')') depth = std::max(0, depth - 1);
        if (c == ';' && depth == 0) { v.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    return v;
}

}  // namespace

// Burn wall-clock for `ms` milliseconds with a high-resolution spin loop.
// We deliberately do NOT use std::this_thread::sleep_for: on Windows its
// granularity is ~15 ms, which would swamp the few-ms device-timing model we
// want the driver's chrono measurement to observe.  A spin loop reproduces the
// modelled cost faithfully on any OS.
static void busy_wait_ms(double ms) {
    if (ms <= 0) return;
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile double sink = 0.0;
    for (;;) {
        double el = std::chrono::duration<double, std::milli>(
                        std::chrono::high_resolution_clock::now() - t0).count();
        if (el >= ms) break;
        sink += el;  // keep the loop from being optimised away
    }
    (void)sink;
}

// Helper to fetch-or-create a net id.
static int get_net(Heterosta3D* s, const std::string& name) {
    auto it = s->net_id.find(name);
    if (it != s->net_id.end()) return it->second;
    int id = (int)s->nets.size();
    s->net_id[name] = id;
    s->nets.push_back(Net{name, {}, -1});
    return id;
}

// ----------------------------------------------------------------------------
// C API implementation
// ----------------------------------------------------------------------------
extern "C" {

bool heterosta3d_init_license(const char*, const char*) { return true; }

Heterosta3D* heterosta3d_new(void) { return new Heterosta3D(); }
void heterosta3d_free(Heterosta3D* sta) { delete sta; }
void heterosta3d_reset(Heterosta3D* sta) {
    if (!sta) return;
    *sta = Heterosta3D();
}

bool heterosta3d_create_liberty_set_batch(Heterosta3D*, early_late_t,
                                          const char*, const char* const*,
                                          uintptr_t) {
    return true;  // stub does not parse .lib; corner factors are hard-coded
}

bool heterosta3d_create_delay_corner(Heterosta3D* sta, const char* dc_name,
                                     const char* top, const char* btm,
                                     uint8_t device_id) {
    if (!sta || !dc_name) return false;
    Corner c;
    c.top_set = top ? top : "";
    c.bot_set = btm ? btm : "";
    c.device = device_id;
    sta->corners[dc_name] = c;
    return true;
}

bool heterosta3d_read_netlist(Heterosta3D* sta, const char* path) {
    if (!sta || !path) return false;
    std::ifstream in(path);
    if (!in) { std::cerr << "[stub] cannot open " << path << "\n"; return false; }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string text = strip_comments(buf.str());

    // Largest module body, found by manual string scan (NO whole-file regex --
    // see the note in strip_comments).  For each standalone `module` keyword we
    // take the text between the header ';' and the matching `endmodule`.
    std::string body;
    {
        auto ident_char = [](char c) {
            return std::isalnum((unsigned char)c) || c == '_';
        };
        size_t best = 0, pos = 0;
        while (pos < text.size()) {
            size_t mk = text.find("module", pos);
            if (mk == std::string::npos) break;
            bool lok = (mk == 0) || !ident_char(text[mk - 1]);          // not 'endmodule'/ident
            bool rok = (mk + 6 >= text.size()) || !ident_char(text[mk + 6]);
            if (!lok || !rok) { pos = mk + 6; continue; }
            size_t semi = text.find(';', mk);
            if (semi == std::string::npos) break;
            size_t emod = text.find("endmodule", semi);
            if (emod == std::string::npos) break;
            std::string b = text.substr(semi + 1, emod - (semi + 1));
            if (b.size() > best) { best = b.size(); body = b; }
            pos = emod + 9;
        }
    }
    if (body.empty()) { std::cerr << "[stub] no module body\n"; return false; }

    static const std::set<std::string> kw = {
        "module", "endmodule", "input", "output", "inout", "wire", "reg", "tri",
        "supply0", "supply1", "parameter", "localparam", "assign", "always"};
    std::regex inst_re(R"(^\s*([\w$\\\.\[\]/]+)\s+([\w$\\\.\[\]/]+)\s*\(([\s\S]*)\)\s*$)");
    std::regex conn_re(R"(\.\s*([\w$\\\[\].]+)\s*\(\s*([^)]*?)\s*\))");

    for (auto& stmt : top_split_semicolons(body)) {
        // trim
        size_t a = stmt.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        std::string st = stmt.substr(a);
        std::string first = st.substr(0, st.find_first_of(" \t\r\n("));
        if (kw.count(first) || (!first.empty() && first[0] == '`')) continue;

        std::smatch im;
        if (!std::regex_match(st, im, inst_re)) continue;
        Inst inst;
        inst.celltype = im[1].str();
        inst.name = im[2].str();
        inst.die = die_of(inst.celltype);
        inst.is_seq = is_seq_celltype(inst.celltype);
        int inst_idx = (int)sta->insts.size();

        std::string conns = im[3].str();
        auto cb = std::sregex_iterator(conns.begin(), conns.end(), conn_re);
        auto ce = std::sregex_iterator();
        for (auto it = cb; it != ce; ++it) {
            std::string pinname = (*it)[1].str();
            std::string netname = (*it)[2].str();
            if (netname.empty()) continue;  // unconnected (e.g. .QN())
            Pin p;
            p.full = inst.name + "/" + pinname;
            p.inst = inst_idx;
            p.pinname = pinname;
            p.is_output = is_output_pinname(pinname);
            int pid = (int)sta->pins.size();
            int nid = get_net(sta, netname);
            p.net = nid;
            sta->pins.push_back(p);
            sta->pin_id[p.full] = pid;
            inst.pins.push_back(pid);
            sta->nets[nid].pins.push_back(pid);
            if (p.is_output) sta->nets[nid].driver = pid;
        }
        sta->insts.push_back(std::move(inst));
    }
    return true;
}

void heterosta3d_set_netlistdb(Heterosta3D*, struct NetlistDB*) {}

void heterosta3d_flatten_all(Heterosta3D*) {}

void heterosta3d_build_graph(Heterosta3D* sta) {
    if (!sta) return;
    // CSR pin->net
    sta->pin2net_.resize(sta->pins.size());
    for (size_t i = 0; i < sta->pins.size(); ++i)
        sta->pin2net_[i] = (uintptr_t)sta->pins[i].net;
    // CSR net->pins
    sta->n2p_start_.assign(sta->nets.size() + 1, 0);
    for (size_t n = 0; n < sta->nets.size(); ++n)
        sta->n2p_start_[n + 1] = sta->n2p_start_[n] + sta->nets[n].pins.size();
    sta->n2p_items_.clear();
    sta->n2p_items_.reserve(sta->pins.size());
    for (auto& net : sta->nets)
        for (int pid : net.pins) sta->n2p_items_.push_back((uintptr_t)pid);
}

bool heterosta3d_read_sdc(Heterosta3D* sta, const char* sdc, const char* dc) {
    if (!sta || !dc) return false;
    auto it = sta->corners.find(dc);
    if (it == sta->corners.end()) return false;
    std::ifstream in(sdc ? sdc : "");
    if (in) {
        std::stringstream b; b << in.rdbuf();
        std::string t = b.str();
        std::smatch m;
        if (std::regex_search(t, m, std::regex(R"(create_clock[\s\S]*?-period\s+([0-9.]+))")))
            it->second.period = std::stod(m[1].str());
    }
    return true;
}

bool heterosta3d_batch_read_sdc(Heterosta3D* sta, const char* const* paths,
                                uintptr_t n, const char* dc) {
    bool ok = true;
    for (uintptr_t i = 0; i < n; ++i) ok &= heterosta3d_read_sdc(sta, paths[i], dc);
    return ok;
}

void heterosta3d_zero_slew(Heterosta3D*, const char*) {}

// per-corner cached extraction parameters (keyed by dc name)
namespace {
struct ExtractParams {
    float cap_x_top, cap_y_top, res_x_top, res_y_top;
    float cap_x_btm, cap_y_btm, res_x_btm, res_y_btm;
    float hbt_r, hbt_c;
};
std::map<std::string, ExtractParams> g_params;
}  // namespace

void heterosta3d_extract_rc_from_placement(
    Heterosta3D* sta, const float* pos_x, const float* pos_y,
    const float* /*hbt_x*/, const float* /*hbt_y*/,
    float cap_x_top, float cap_y_top, float res_x_top, float res_y_top,
    float cap_x_btm, float cap_y_btm, float res_x_btm, float res_y_btm,
    float hbt_r, float hbt_c, int /*flute*/, const char* dc) {
    if (!sta || !dc) return;
    size_t np = sta->pins.size();
    sta->pos_x.assign(pos_x, pos_x + np);
    sta->pos_y.assign(pos_y, pos_y + np);
    g_params[dc] = {cap_x_top, cap_y_top, res_x_top, res_y_top,
                    cap_x_btm, cap_y_btm, res_x_btm, res_y_btm, hbt_r, hbt_c};
    if (sta->corners.count(dc)) sta->corners[dc].extracted = true;

    // Simulate parasitic-extraction cost.  On a GPU corner this is dominated
    // by the fixed host->device copy of the netlist + parasitics (the
    // "GPU 显存拷贝" overhead the advanced section asks you to reason about).
    uint8_t dev = sta->corners.count(dc) ? sta->corners[dc].device
                                         : HETEROSTA3D_CPU_DEVICE_ID;
    double ms;
    if (dev == HETEROSTA3D_CPU_DEVICE_ID) ms = np * 0.001;            // CPU
    else                                  ms = 6.0 + np * 0.0008;     // GPU H2D copy
    busy_wait_ms(ms);
}

// ---- the actual timing computation ---------------------------------------
namespace {

// Per-net R and C (kOhm, fF), including HBT contribution for cross-die nets.
void compute_net_rc(Heterosta3D* sta, const ExtractParams& p,
                    std::vector<double>& R, std::vector<double>& C,
                    std::vector<bool>& cross) {
    size_t NN = sta->nets.size();
    R.assign(NN, 0.0); C.assign(NN, 0.0); cross.assign(NN, false);
    for (size_t n = 0; n < NN; ++n) {
        Net& net = sta->nets[n];
        bool ht = false, hb = false;
        for (int pid : net.pins) {
            char d = sta->insts[sta->pins[pid].inst].die;
            if (d == 't') ht = true;
            if (d == 'b') hb = true;
        }
        cross[n] = ht && hb;
        // average driver->sink distance
        double dsum = 0; int cnt = 0;
        int drv = net.driver;
        for (int pid : net.pins) {
            if (pid == drv) continue;
            dsum += sta->dist(drv, pid);
            ++cnt;
        }
        double dist = cnt ? dsum / cnt : 1.0;
        // pick die unit values from driver's die (fallback top)
        char dd = (drv >= 0) ? sta->insts[sta->pins[drv].inst].die : 't';
        double capu = (dd == 'b') ? p.cap_x_btm : p.cap_x_top;
        double resu = (dd == 'b') ? p.res_x_btm : p.res_x_top;
        R[n] = resu * dist;
        C[n] = capu * dist;
        if (cross[n]) { R[n] += p.hbt_r; C[n] += p.hbt_c; }
    }
}

// Longest & shortest combinational-path arrival to each pin; return slacks.
void run_sta(Heterosta3D* sta, Corner& cor, const ExtractParams& p) {
    const size_t NP = sta->pins.size();
    std::vector<double> R, C; std::vector<bool> cross;
    compute_net_rc(sta, p, R, C, cross);

    const double base = 0.05, load_f = 0.003;
    auto corner_factor = [](char die) { return die == 'b' ? 0.6 : 1.5; };
    const double clk2q = 0.08 * 0.6, setup_t = 0.03, hold_t = 0.015;

    // net delay (ns): Elmore 0.69*R[kOhm]*C[fF] -> ps, /1000 -> ns
    auto net_delay = [&](int nid) { return 0.69 * R[nid] * C[nid] * 1e-3; };

    // cell delay (ns): base*corner + load*(sum of driven-net C)
    auto cell_delay = [&](int inst_idx) {
        const Inst& in = sta->insts[inst_idx];
        double cl = 0;
        for (int pid : in.pins)
            if (sta->pins[pid].is_output) cl += C[sta->pins[pid].net];
        return base * corner_factor(in.die) + load_f * cl;
    };

    // Build a pin-level DAG.  Edges:
    //   net edge:  driver_pin --net_delay--> sink_pin
    //   cell edge: input_pin --cell_delay--> output_pin   (combinational only)
    // Sources: FF Q pins (arr=clk2q), primary-input sink pins (arr=0).
    // Endpoints: FF D pins.
    std::vector<std::vector<std::pair<int, double>>> succ(NP);
    std::vector<int> indeg(NP, 0);

    for (size_t n = 0; n < sta->nets.size(); ++n) {
        int drv = sta->nets[n].driver;
        double dn = net_delay((int)n);
        for (int pid : sta->nets[n].pins) {
            if (pid == drv) continue;
            if (drv >= 0) { succ[drv].push_back({pid, dn}); indeg[pid]++; }
        }
    }
    for (size_t ii = 0; ii < sta->insts.size(); ++ii) {
        const Inst& in = sta->insts[ii];
        if (in.is_seq) continue;  // do not propagate through registers
        double cd = cell_delay((int)ii);
        for (int ip : in.pins) {
            if (sta->pins[ip].is_output) continue;
            for (int op : in.pins)
                if (sta->pins[op].is_output) { succ[ip].push_back({op, cd}); indeg[op]++; }
        }
    }

    std::vector<double> amax(NP, -1e30), amin(NP, 1e30);
    std::vector<int> predmax(NP, -1);
    std::queue<int> q;
    for (size_t pid = 0; pid < NP; ++pid) {
        const Pin& pn = sta->pins[pid];
        bool is_ff_q = sta->insts[pn.inst].is_seq && pn.is_output;
        bool prim_in = (sta->nets[pn.net].driver < 0) && !pn.is_output;
        if (indeg[pid] == 0) {
            amax[pid] = amin[pid] = is_ff_q ? clk2q : (prim_in ? 0.0 : 0.0);
            q.push((int)pid);
        }
    }
    // Kahn topo + DP
    std::vector<int> deg = indeg;
    while (!q.empty()) {
        int u = q.front(); q.pop();
        for (auto& [v, w] : succ[u]) {
            if (amax[u] + w > amax[v]) { amax[v] = amax[u] + w; predmax[v] = u; }
            if (amin[u] + w < amin[v]) { amin[v] = amin[u] + w; }
            if (--deg[v] == 0) q.push(v);
        }
    }

    // Endpoints = FF D pins.  slack.
    cor.slack_max.assign(NP, {1e30f, 1e30f});
    cor.slack_min.assign(NP, {1e30f, 1e30f});
    double T = cor.period;
    float worst_setup = 1e30f, worst_hold = 1e30f;
    double tns_setup = 0, tns_hold = 0;
    sta->_last_worst_ep = -1;  // for path dump
    for (size_t pid = 0; pid < NP; ++pid) {
        const Pin& pn = sta->pins[pid];
        bool is_ff_d = sta->insts[pn.inst].is_seq && !pn.is_output &&
                       (pn.pinname == "D" || pn.pinname == "SI" || pn.pinname == "DATA");
        if (!is_ff_d) continue;
        if (amax[pid] < -1e29) continue;  // unreachable
        double setup_slack = T - (amax[pid] + setup_t);
        double hold_slack = (amin[pid] > 1e29 ? amax[pid] : amin[pid]) - hold_t;
        cor.slack_max[pid] = {(float)setup_slack, (float)setup_slack};
        cor.slack_min[pid] = {(float)hold_slack, (float)hold_slack};
        if (setup_slack < worst_setup) { worst_setup = (float)setup_slack; sta->_last_worst_ep = (int)pid; }
        if (hold_slack < worst_hold)   worst_hold = (float)hold_slack;
        if (setup_slack < 0) tns_setup += setup_slack;
        if (hold_slack < 0) tns_hold += hold_slack;
    }
    cor.setup_wns = (worst_setup > 1e29f) ? 0.0f : worst_setup;
    cor.hold_wns  = (worst_hold > 1e29f) ? 0.0f : worst_hold;
    cor.setup_tns = (float)tns_setup;
    cor.hold_tns  = (float)tns_hold;

    sta->_amax = amax;          // stash for path dump
    sta->_predmax = predmax;
}

}  // namespace

void heterosta3d_update_delay(Heterosta3D* sta, const char* dc) {
    if (!sta || !dc || !sta->corners.count(dc)) return;
    // Delay-calculation compute share.  GPU pays a fixed kernel-launch /
    // scheduling overhead but has far higher per-pin throughput than the CPU.
    uint8_t dev = sta->corners[dc].device;
    double np = (double)sta->pins.size();
    double ms = (dev == HETEROSTA3D_CPU_DEVICE_ID) ? np * 0.003
                                                   : 2.0 + np * 0.0006;
    busy_wait_ms(ms);
}

void heterosta3d_update_arrivals(Heterosta3D* sta, const char* dc) {
    if (!sta || !dc || !sta->corners.count(dc)) return;
    run_sta(sta, sta->corners[dc], g_params[dc]);
    // Arrival-propagation compute share (graph traversal + top-k search).
    uint8_t dev = sta->corners[dc].device;
    double np = (double)sta->pins.size();
    double ms = (dev == HETEROSTA3D_CPU_DEVICE_ID) ? np * 0.004
                                                   : 2.0 + np * 0.0006;
    busy_wait_ms(ms);
}

bool heterosta3d_report_wns_tns_max(Heterosta3D* sta, float* wns, float* tns,
                                    const char* dc) {
    if (!sta || !dc || !sta->corners.count(dc)) return false;
    if (wns) *wns = sta->corners[dc].setup_wns;
    if (tns) *tns = sta->corners[dc].setup_tns;
    return true;
}
bool heterosta3d_report_wns_tns_min(Heterosta3D* sta, float* wns, float* tns,
                                    const char* dc) {
    if (!sta || !dc || !sta->corners.count(dc)) return false;
    if (wns) *wns = sta->corners[dc].hold_wns;
    if (tns) *tns = sta->corners[dc].hold_tns;
    return true;
}
void heterosta3d_report_slacks_at_max(Heterosta3D* sta, float (*slack)[2],
                                      const char* dc) {
    if (!sta || !dc || !sta->corners.count(dc)) return;
    auto& s = sta->corners[dc].slack_max;
    for (size_t i = 0; i < s.size(); ++i) { slack[i][0] = s[i][0]; slack[i][1] = s[i][1]; }
}
void heterosta3d_report_slacks_at_min(Heterosta3D* sta, float (*slack)[2],
                                      const char* dc) {
    if (!sta || !dc || !sta->corners.count(dc)) return;
    auto& s = sta->corners[dc].slack_min;
    for (size_t i = 0; i < s.size(); ++i) { slack[i][0] = s[i][0]; slack[i][1] = s[i][1]; }
}

static void dump_paths(Heterosta3D* sta, const char* file, const char* dc, bool setup) {
    if (!sta || !sta->corners.count(dc)) return;
    std::ofstream f(file);
    Corner& c = sta->corners[dc];
    f << "==========================================================\n";
    f << "  HeteroSTA3D [STUB] " << (setup ? "Setup (max)" : "Hold (min)")
      << " path report   corner=" << dc << "\n";
    f << "  clock period T = " << c.period << " ns\n";
    f << "==========================================================\n";
    f << (setup ? "  Setup WNS = " : "  Hold WNS = ")
      << (setup ? c.setup_wns : c.hold_wns) << " ns   TNS = "
      << (setup ? c.setup_tns : c.hold_tns) << " ns\n\n";
    int ep = sta->_last_worst_ep;
    if (ep >= 0 && setup) {
        f << "  Worst Setup path (endpoint -> startpoint):\n";
        int cur = ep;
        double prev = sta->_amax.size() > (size_t)ep ? sta->_amax[ep] : 0.0;
        while (cur >= 0) {
            f << "    " << std::string(sta->pins[cur].full) << "   arr="
              << (sta->_amax.size() > (size_t)cur ? sta->_amax[cur] : 0.0) << " ns\n";
            cur = (sta->_predmax.size() > (size_t)cur) ? sta->_predmax[cur] : -1;
        }
        (void)prev;
    }
    f << "\n  (STUB output -- replace with the real engine for graded runs)\n";
}

void heterosta3d_dump_paths_max_to_file(Heterosta3D* sta, uintptr_t, uintptr_t,
                                        const char* file, const char* dc) {
    dump_paths(sta, file, dc, true);
}
void heterosta3d_dump_paths_min_to_file(Heterosta3D* sta, uintptr_t, uintptr_t,
                                        const char* file, const char* dc) {
    dump_paths(sta, file, dc, false);
}

bool heterosta3d_write_spef(Heterosta3D*, const char*, const char*) { return true; }
bool heterosta3d_report_delay_sdf(Heterosta3D*, const char*, const char*) { return true; }

uintptr_t heterosta3d_batch_update_celltypes(Heterosta3D*, const char* const*,
                                             uintptr_t n) { return n; }
bool heterosta3d_ignore_net(Heterosta3D*, uintptr_t) { return true; }

uintptr_t heterosta3d_get_num_of_pins(Heterosta3D* s) { return s ? s->pins.size() : 0; }
uintptr_t heterosta3d_get_num_of_nets(Heterosta3D* s) { return s ? s->nets.size() : 0; }

const uintptr_t* heterosta3d_get_pin2net(Heterosta3D* s, bool) {
    return s ? s->pin2net_.data() : nullptr;
}
const uintptr_t* heterosta3d_get_net2pin_start(Heterosta3D* s, bool) {
    return s ? s->n2p_start_.data() : nullptr;
}
const uintptr_t* heterosta3d_get_net2pin_items(Heterosta3D* s, bool) {
    return s ? s->n2p_items_.data() : nullptr;
}

uintptr_t heterosta3d_lookup_pin(Heterosta3D* s, const char* name) {
    if (!s || !name) return SIZE_MAX;
    auto it = s->pin_id.find(name);
    return it == s->pin_id.end() ? SIZE_MAX : (uintptr_t)it->second;
}
NetlistDirection heterosta3d_query_pin_direction(Heterosta3D* s, uintptr_t pid) {
    if (!s || pid >= s->pins.size()) return Unknown;
    return s->pins[pid].is_output ? O : I;
}
const char* heterosta3d_internal_get_celltype_of_pin(Heterosta3D* s, uintptr_t pid) {
    if (!s || pid >= s->pins.size()) return "";
    return s->insts[s->pins[pid].inst].celltype.c_str();
}

}  // extern "C"
