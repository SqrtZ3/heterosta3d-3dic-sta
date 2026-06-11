# run_all.ps1 -- full pipeline on Windows using ziglang (no system compiler needed)
#   Usage:  powershell -ExecutionPolicy Bypass -File .\run_all.ps1
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

Write-Host "==> [0/6] ensure python deps" -ForegroundColor Cyan
python -m pip install --quiet ziglang pandas seaborn matplotlib numpy reportlab

Write-Host "==> [1/5] Stage 1: 3D split" -ForegroundColor Cyan
python scripts/split_3d_netlist.py designs/gcd_sample.v --top-module gcd --outdir designs/gcd_3d

Write-Host "==> [2/5] build driver (stub) with zig c++" -ForegroundColor Cyan
python -m ziglang c++ -std=c++17 -O2 -DH3D_USE_STUB -Wno-nullability-completeness `
    -I third_party/heterosta3d/include -I src `
    src/sta_driver.cpp stub/heterosta3d_stub.cpp -o sta_driver.exe

New-Item -ItemType Directory -Force -Path results | Out-Null

Write-Host "==> [3/5] Stage 2: single-corner STA" -ForegroundColor Cyan
./sta_driver.exe --mode single --netlist designs/gcd_3d/design_3d.v `
    --placement designs/gcd_3d/placement.csv --sdc sdc/gcd.sdc `
    --top-lib pdk/asap7_ss_placeholder.lib --bot-lib pdk/asap7_ff_placeholder.lib `
    --c-hbt 1.0 --device 0 --outdir results

Write-Host "==> [4/5] Stage 3 (sweep) + Stage 4 (devices + scaling)" -ForegroundColor Cyan
./sta_driver.exe --mode sweep --netlist designs/gcd_3d/design_3d.v `
    --placement designs/gcd_3d/placement.csv --sdc sdc/gcd.sdc `
    --c-start 0.1 --c-stop 5.0 --c-step 0.5 --device 0 --outdir results
./sta_driver.exe --mode devices --netlist designs/gcd_3d/design_3d.v `
    --placement designs/gcd_3d/placement.csv --sdc sdc/gcd.sdc `
    --devices cpu,0,1 --c-hbt 1.0 --outdir results
python scripts/run_scaling.py --driver ./sta_driver.exe --base designs/gcd_sample.v `
    --sdc sdc/gcd.sdc --sizes 1,2,4,8,16,32,64,128,256 --out results/scaling.csv

Write-Host "==> [5/6] plots" -ForegroundColor Cyan
python scripts/plot_results.py

Write-Host "==> [6/6] build report PDF" -ForegroundColor Cyan
python scripts/build_report_pdf.py report/report.md report/report.pdf

Write-Host "DONE. See results/, report/figs/, report/report.pdf" -ForegroundColor Green
