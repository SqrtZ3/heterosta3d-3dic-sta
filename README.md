# 异构 3D IC 静态时序分析与优化（基于 HeteroSTA3D）

> 大二考核项目 · 个人编程项目 · C++ / Python
> 引擎主页：<https://heterosta3d.pkueda.org.cn/>

基于 [HeteroSTA3D](https://heterosta3d.pkueda.org.cn/) 的 C++ API，搭建面对面
混合键合（Hybrid Bonding）**异构 3D IC** 的完整静态时序分析（STA）流程，并探究
**3D 互连寄生参数（HBT 电容）**与**多设备并行**对时序结果与性能的影响。

---

## ⚡ 快速开始

**纯 Windows（无需系统编译器，用 `ziglang` 跑通整条流水线）：**
```powershell
powershell -ExecutionPolicy Bypass -File .\run_all.ps1
```

**Linux / WSL2（桩，端到端）：**
```bash
make all          # 生成 results/、report/figs/、report/report.pdf
```

**Linux / GPU 服务器（真实引擎，可提交结果）：**
```bash
make USE_STUB=0 H3D_DIR=/opt/heterosta3d CUDA_DIR=/usr/local/cuda
USE_STUB=0 TOPLIB=pdk/<真实SS>.lib BOTLIB=pdk/<真实FF>.lib DEVICES=cpu,0,1 ./run_all.sh
```

> **重要**：真实 HeteroSTA3D 仅支持 *Linux x86-64 + NVIDIA GPU(CUDA≥11) + license*。
> 本仓库自带一个实现相同 C ABI 的 **CPU 桩引擎**（`stub/`），让流水线在任何机器
> （含本机 Windows）都能编译、运行、出图——便于开发与验证。**桩产生的数值是示意性的**
> （趋势与真实物理一致）；在 GPU 服务器把 `USE_STUB=0` 链接真实 `.so` 即得可提交结果。
> 细节见 [`docs/ENVIRONMENT.md`](docs/ENVIRONMENT.md)。

---

## 📂 仓库结构

```
scripts/
  split_3d_netlist.py    阶段一：3D 网表切分（_top/_bottom 后缀 + 综合放置 + HBT 识别）
  suffix_liberty.py      （备用）给 .lib 单元名加后缀
  gen_scaled_netlist.py  阶段四：按规模复制网表（加速比扫描用）
  run_scaling.py         阶段四：跑规模扫描，收集 CPU/GPU 加速比
  plot_results.py        阶段三/四：Matplotlib/Seaborn 出图
  build_report_pdf.py    渲染实验报告 PDF（内置 CJK 字体）
src/
  h3d_wrapper.hpp        C API 的 RAII C++ 封装（内存安全核心）
  sta_driver.cpp         阶段二/三/四主驱动（single / sweep / devices）
stub/
  heterosta3d_stub.cpp   CPU 桩引擎（迷你 STA，便于无 GPU 跑通整条链路）
third_party/heterosta3d/include/heterosta3d.h   重建的 C API 头
designs/   网表与 Stage 1 产物（gcd_sample.v 为小型替身，见 designs/README.md）
pdk/       Liberty 库占位（替换为真实 ASAP7 SS/FF，见 pdk/README.md）
sdc/       时序约束（gcd.sdc）
docs/      API_NOTES.md（重建的 API 与工作流）, ENVIRONMENT.md（环境/WSL/GPU）
report/    report.md → report.pdf + figs/
results/   时序报告与实验数据（运行后生成）
Makefile · CMakeLists.txt · run_all.ps1 · run_all.sh
```

---

## 🧩 四个阶段与对应产物

| 阶段 | 内容 | 关键产物 |
|------|------|---------|
| **一** 3D 网表预处理 | 组合→`_top`(SS)，时序→`_bottom`(FF)，识别跨层网络放 HBT | `designs/gcd_3d/design_3d.v`, `placement.csv` |
| **二** C++ STA 驱动 | 官方 9 步工作流，单角 `ss_ff`，输出 WNS/TNS + 关键路径 | `results/single_corner.txt`, `setup_paths.rpt`, `hold_paths.rpt` |
| **三** C_hbt 扫描 | `C_hbt` 0.1→5.0 fF，记录 Setup/Hold WNS 并绘图 | `results/sweep.csv`, `report/figs/fig_sweep_wns.png` |
| **进阶** 多设备并行 | CPU vs GPU、规模扫描、加速比与开销分析 | `results/devices.csv`, `scaling.csv`, `fig_scaling_speedup.png` |

工作流到代码的逐步映射见 [`docs/API_NOTES.md`](docs/API_NOTES.md)。

---

## 🔬 单独运行某一步

```bash
# 阶段一
python scripts/split_3d_netlist.py designs/gcd_sample.v --top-module gcd --outdir designs/gcd_3d

# 阶段二（单角）
./sta_driver --mode single  --netlist designs/gcd_3d/design_3d.v \
    --placement designs/gcd_3d/placement.csv --sdc sdc/gcd.sdc \
    --top-lib pdk/asap7_ss_placeholder.lib --bot-lib pdk/asap7_ff_placeholder.lib --c-hbt 1.0

# 阶段三（扫描）
./sta_driver --mode sweep --c-start 0.1 --c-stop 5.0 --c-step 0.5  ...（同上参数）

# 阶段四（多设备 + 规模扫描）
./sta_driver --mode devices --devices cpu,0,1 ...（同上参数）
python scripts/run_scaling.py --driver ./sta_driver --sizes 1,2,4,8,16,32,64,128,256

# 出图 + 报告
python scripts/plot_results.py
python scripts/build_report_pdf.py
```

`./sta_driver --help` 查看全部参数。

---

## 📦 提交清单对照

- [x] **网表预处理脚本**：`scripts/split_3d_netlist.py`
- [x] **C++ 驱动 + 构建文件**：`src/`, `stub/`, `Makefile`, `CMakeLists.txt`
- [x] **实验报告 PDF**：`report/report.pdf`（源 `report/report.md`）
  - 环境配置说明、核心代码逻辑解析、阶段三图表与理论分析、进阶加速比分析
- [x] **时序报告 (txt)**：`results/setup_paths.rpt`, `results/hold_paths.rpt`

> 把 `designs/gcd.v` 换成真实综合网表、`pdk/*.lib` 换成真实 ASAP7 SS/FF、在 GPU
> 服务器 `USE_STUB=0` 重跑，即可用真实数据更新报告与时序文件后提交。
