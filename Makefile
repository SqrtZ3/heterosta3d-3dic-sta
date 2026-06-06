# ===========================================================================
#  Makefile  --  HeteroSTA3D 3D-STA driver (assignment deliverable)
# ===========================================================================
#
#  Two build flavours, selected by USE_STUB:
#
#    make                 # USE_STUB=1 (default): build against the bundled CPU
#                         # stub -- compiles & runs anywhere (WSL2/Linux/macOS),
#                         # no GPU, no license.  Great for development.
#
#    make USE_STUB=0 \    # build against the REAL vendor library for graded runs
#         H3D_DIR=/path/to/heterosta3d-release \
#         CUDA_DIR=/usr/local/cuda
#
#  Convenience pipeline targets:
#    make stage1   # 3D-split the netlist            (Python)
#    make stage2   # single-corner STA               (C++)
#    make stage3   # C_hbt sweep                      (C++)
#    make stage4   # multi-device speed-up + scaling  (C++/Python)
#    make plots    # render Stage 3/4 figures         (Python)
#    make all      # everything end-to-end
# ===========================================================================

CXX       ?= g++
CXXFLAGS  ?= -std=c++17 -O2 -Wall -Wextra
PYTHON    ?= python3
BIN        = sta_driver

USE_STUB  ?= 1

# ---- vendor SDK locations (only used when USE_STUB=0) ----------------------
H3D_DIR   ?= /opt/heterosta3d
H3D_INC   ?= $(H3D_DIR)/include
H3D_LIB   ?= $(H3D_DIR)/lib
CUDA_DIR  ?= /usr/local/cuda

# ---- design / corner inputs (override on the command line as needed) ------
NETLIST   ?= designs/gcd_3d/design_3d.v
PLACEMENT ?= designs/gcd_3d/placement.csv
SDC       ?= sdc/gcd.sdc
TOPLIB    ?= pdk/asap7_ss_placeholder.lib
BOTLIB    ?= pdk/asap7_ff_placeholder.lib
RESULTS   ?= results
BASEV     ?= designs/gcd_sample.v
DEVICES   ?= cpu,0

SRCS       = src/sta_driver.cpp

ifeq ($(USE_STUB),1)
  SRCS    += stub/heterosta3d_stub.cpp
  CXXFLAGS += -DH3D_USE_STUB
  INCLUDES  = -Ithird_party/heterosta3d/include -Isrc
  LDFLAGS   =
else
  INCLUDES  = -I$(H3D_INC) -Isrc
  LDFLAGS   = -L$(H3D_LIB) -lheterosta3d -L$(CUDA_DIR)/lib64 -lcudart \
              -Wl,-rpath,$(H3D_LIB)
endif

.PHONY: all build stage1 stage2 stage3 stage4 plots report clean realclean

build: $(BIN)

$(BIN): $(SRCS) src/h3d_wrapper.hpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRCS) -o $(BIN) $(LDFLAGS)

stage1:
	$(PYTHON) scripts/split_3d_netlist.py $(BASEV) --top-module gcd --outdir designs/gcd_3d

stage2: $(BIN)
	@mkdir -p $(RESULTS)
	./$(BIN) --mode single --netlist $(NETLIST) --placement $(PLACEMENT) \
	         --sdc $(SDC) --top-lib $(TOPLIB) --bot-lib $(BOTLIB) \
	         --c-hbt 1.0 --device 0 --outdir $(RESULTS)

stage3: $(BIN)
	@mkdir -p $(RESULTS)
	./$(BIN) --mode sweep --netlist $(NETLIST) --placement $(PLACEMENT) \
	         --sdc $(SDC) --top-lib $(TOPLIB) --bot-lib $(BOTLIB) \
	         --c-start 0.1 --c-stop 5.0 --c-step 0.5 --device 0 --outdir $(RESULTS)

stage4: $(BIN)
	@mkdir -p $(RESULTS)
	./$(BIN) --mode devices --netlist $(NETLIST) --placement $(PLACEMENT) \
	         --sdc $(SDC) --top-lib $(TOPLIB) --bot-lib $(BOTLIB) \
	         --devices $(DEVICES) --c-hbt 1.0 --outdir $(RESULTS)
	$(PYTHON) scripts/run_scaling.py --driver ./$(BIN) --base $(BASEV) \
	         --sdc $(SDC) --sizes 1,2,4,8,16,32,64,128,256 --out $(RESULTS)/scaling.csv

plots:
	$(PYTHON) scripts/plot_results.py --sweep $(RESULTS)/sweep.csv \
	         --scaling $(RESULTS)/scaling.csv --outdir report/figs

report:
	$(PYTHON) scripts/build_report_pdf.py report/report.md report/report.pdf

all: stage1 build stage2 stage3 stage4 plots report
	@echo "==== pipeline complete; see $(RESULTS)/, report/figs/, report/report.pdf ===="

clean:
	rm -f $(BIN) $(BIN).exe *.o

realclean: clean
	rm -rf $(RESULTS)/* report/figs/* designs/gcd_3d designs/scaled
