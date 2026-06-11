#!/usr/bin/env bash
# run_all.sh -- full pipeline on Linux / WSL2.
#   Stub (default):  ./run_all.sh
#   Real engine:     USE_STUB=0 H3D_DIR=/opt/heterosta3d CUDA_DIR=/usr/local/cuda \
#                    TOPLIB=pdk/real_SS.lib BOTLIB=pdk/real_FF.lib ./run_all.sh
set -euo pipefail
cd "$(dirname "$0")"

USE_STUB="${USE_STUB:-1}"
TOPLIB="${TOPLIB:-pdk/asap7_ss_placeholder.lib}"
BOTLIB="${BOTLIB:-pdk/asap7_ff_placeholder.lib}"
DEVICES="${DEVICES:-cpu,0}"

echo "==> [1/5] Stage 1: 3D split"
make stage1

echo "==> [2/5] build (USE_STUB=$USE_STUB)"
make build USE_STUB="$USE_STUB" ${H3D_DIR:+H3D_DIR="$H3D_DIR"} ${CUDA_DIR:+CUDA_DIR="$CUDA_DIR"}

echo "==> [3/5] Stage 2: single-corner STA"
make stage2 TOPLIB="$TOPLIB" BOTLIB="$BOTLIB"

echo "==> [4/5] Stage 3 (sweep) + Stage 4 (devices + scaling)"
make stage3 TOPLIB="$TOPLIB" BOTLIB="$BOTLIB"
make stage4 TOPLIB="$TOPLIB" BOTLIB="$BOTLIB" DEVICES="$DEVICES"

echo "==> [5/6] plots"
make plots

echo "==> [6/6] build report PDF"
make report

echo "DONE. See results/, report/figs/, report/report.pdf"
