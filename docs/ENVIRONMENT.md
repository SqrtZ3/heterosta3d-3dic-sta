# Environment & build options

HeteroSTA3D (the real engine) requires, per its docs:

- **Linux x86-64**
- **NVIDIA GPU**, compute capability **≥ 5.2**
- **CUDA runtime ≥ 11**, matching driver
- a valid 2D + 3D license

You have **three** ways to work with this repo. Only the first needs a GPU.

---

## 1. Real run — GPU server  (for graded numbers)

The way to get authoritative WNS/TNS and *real* speed-up numbers.

```bash
# on the server, after copying/cloning this repo
unzip heterosta3d-release-*.tar.gz -d /opt/heterosta3d     # vendor tarball
export LD_LIBRARY_PATH=/opt/heterosta3d/lib:$LD_LIBRARY_PATH

make USE_STUB=0 H3D_DIR=/opt/heterosta3d CUDA_DIR=/usr/local/cuda
make stage1
make stage2 TOPLIB=pdk/<real_SS>.lib BOTLIB=pdk/<real_FF>.lib
make stage3 TOPLIB=... BOTLIB=...
make stage4 DEVICES=cpu,0,1 TOPLIB=... BOTLIB=...
make plots
```

SSH/transfer specifics depend on your platform — typically:
`scp -r . user@server:~/heterosta3d_proj` then `ssh user@server`. Check your
GPU server's onboarding docs for the exact host, key, and queue/`srun` command.

---

## 2. Local WSL2  (development; real engine *if* GPU is compatible)

WSL2 supports CUDA via GPU pass-through, so the engine *can* run locally. Two
caveats on **this** machine (RTX 5070 Laptop):

> ⚠️ **CUDA-11 vs Blackwell.** The current HeteroSTA3D release targets **CUDA 11**
> (see GitHub issues #2/#6 asking for CUDA 12.x). The RTX 5070 is **Blackwell
> (sm_120)**, which CUDA 11 predates. The prebuilt `.so` may fail to launch
> kernels unless it embeds forward-compatible PTX that a recent driver can JIT.
> → Prefer the **GPU server** (Ampere/Turing-class) for real GPU runs, or run
> the engine's **CPU device** (`--device cpu`) locally for correctness.

> ⚠️ **WSL not yet installed** on this machine. Install it first.

```powershell
# Windows (admin PowerShell), once:
wsl --install -d Ubuntu      # then reboot, create a user
```
```bash
# inside Ubuntu/WSL2:
sudo apt update && sudo apt install -y build-essential cmake python3-pip
pip3 install pandas seaborn matplotlib
# (for real GPU) install the CUDA toolkit for WSL per NVIDIA's guide:
#   https://docs.nvidia.com/cuda/wsl-user-guide/
nvidia-smi      # should list the RTX 5070 if pass-through works

# build + run (stub first to validate, then real):
make            # stub build, runs anywhere
make all        # full stub pipeline
# real:
make USE_STUB=0 H3D_DIR=/path/to/heterosta3d CUDA_DIR=/usr/local/cuda
```

---

## 3. Plain Windows  (no compiler) — stub via `ziglang`

This machine has **no system C++ compiler**, but the project still builds and
runs end-to-end using the `ziglang` pip package (bundled clang + libc++):

```powershell
python -m pip install ziglang
# build the stub:
python -m ziglang c++ -std=c++17 -O2 -DH3D_USE_STUB -Wno-nullability-completeness `
       -I third_party/heterosta3d/include -I src `
       src/sta_driver.cpp stub/heterosta3d_stub.cpp -o sta_driver.exe
# or just run the convenience script:
powershell -ExecutionPolicy Bypass -File .\run_all.ps1
```

The Python parts (Stage 1 split, plotting, scaling orchestration) run natively
on Windows Python — no compiler needed.

---

## What this machine has (detected)

| Tool | Status |
|------|--------|
| GPU | NVIDIA RTX 5070 Laptop (Blackwell, sm_120) + AMD Radeon 610M iGPU |
| CUDA | 12.8 (Windows toolkit) — note: engine wants CUDA 11 |
| Python | 3.11 + numpy/pandas/matplotlib/seaborn |
| CMake | yes · git: yes |
| C++ compiler | **none on Windows** → use `ziglang` or WSL2 |
| WSL | not installed yet |
