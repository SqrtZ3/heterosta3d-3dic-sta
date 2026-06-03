# ===========================================================================
#  gcd.sdc  --  timing constraints for the gcd 3D design
# ===========================================================================
#  Clock: 1 GHz (period 1.0 ns), as suggested by the assignment.
#  Switch to 2.0 for 500 MHz.  The C++ driver passes this file to
#  heterosta3d_read_sdc() for each delay corner.
# ===========================================================================

set clk_period 1.0

# Primary clock on the top-level clk port.
create_clock -name clk -period $clk_period [get_ports clk]

# Model clock uncertainty / jitter (setup and hold).
set_clock_uncertainty -setup 0.05 [get_clocks clk]
set_clock_uncertainty -hold  0.02 [get_clocks clk]

# I/O timing budget: inputs arrive 30% into the cycle, outputs need 30%.
set_input_delay  -clock clk [expr 0.30 * $clk_period] [remove_from_collection [all_inputs] [get_ports clk]]
set_output_delay -clock clk [expr 0.30 * $clk_period] [all_outputs]

# Reasonable drive / load assumptions.
set_load 0.005 [all_outputs]
