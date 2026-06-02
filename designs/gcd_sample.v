/*
 * gcd_sample.v  --  SMALL stand-in gate-level netlist (Nangate45-style names)
 *
 * This is NOT the real OpenROAD `gcd` netlist.  It is a compact, structurally
 * valid gate-level design used to exercise the full HeteroSTA3D pipeline
 * (Stage 1 split -> Stage 2 STA -> Stage 3 sweep) on a machine without a
 * synthesis tool installed.
 *
 * It deliberately has the shape that matters for this assignment:
 *   - several D flip-flops (DFF_X1)  -> become the BOTTOM die after the split
 *   - a combinational cloud feeding them -> becomes the TOP die
 *   - every register-to-register path runs reg(bottom) -> logic(top) -> reg(bottom)
 *
 * Replace with the real netlist (designs/gcd.v) for graded runs -- see
 * designs/README.md for the OpenROAD-flow-scripts / Yosys recipe.
 *
 * Cell names use the Nangate45 OpenCell library convention.
 */
module gcd (clk, rst, a_in, b_in, result);
  input  clk;
  input  rst;
  input  a_in;
  input  b_in;
  output result;

  wire n_a, n_b, n_r;            // FF outputs (state)
  wire d_a, d_b, d_r;            // FF inputs  (next state)
  wire t1, t2, t3, t4, t5, t6;   // combinational nets
  wire t7, t8, t9, t10;

  // ---------------- sequential (will -> _bottom die) ----------------
  DFF_X1   reg_a   ( .D(d_a), .CK(clk), .Q(n_a), .QN() );
  DFF_X1   reg_b   ( .D(d_b), .CK(clk), .Q(n_b), .QN() );
  DFF_X1   reg_r   ( .D(d_r), .CK(clk), .Q(n_r), .QN() );

  // ---------------- combinational (will -> _top die) ----------------
  // next-state logic forming reg -> logic -> reg paths
  XOR2_X1  g1 ( .A(n_a),  .B(n_b),  .Z(t1) );
  AND2_X1  g2 ( .A(n_a),  .B(n_b),  .ZN(t2) );
  INV_X1   g3 ( .A(t2),             .ZN(t3) );
  OR2_X1   g4 ( .A(t1),   .B(t3),   .ZN(t4) );
  NAND2_X1 g5 ( .A(t4),   .B(n_r),  .ZN(t5) );
  NOR2_X1  g6 ( .A(t5),   .B(a_in), .ZN(t6) );
  MUX2_X1  g7 ( .A(t6),   .B(n_a),  .S(rst), .Z(d_a) );

  XOR2_X1  g8  ( .A(n_b), .B(t1),   .Z(t7) );
  AND2_X1  g9  ( .A(t7),  .B(t4),   .ZN(t8) );
  MUX2_X1  g10 ( .A(t8),  .B(n_b),  .S(rst), .Z(d_b) );

  OR2_X1   g11 ( .A(t2),  .B(t8),   .ZN(t9) );
  AND2_X1  g12 ( .A(t9),  .B(b_in), .ZN(t10) );
  BUF_X1   g13 ( .A(t10),           .Z(d_r) );

  // primary output
  BUF_X2   g14 ( .A(n_r),           .Z(result) );
endmodule
