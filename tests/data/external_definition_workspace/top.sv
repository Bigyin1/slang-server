`include "defs.svh"
module top;
    collision second();
    external_only first();
    external_if bus();
    external_pkg::word_t value;
    assign value = external_pkg::convert(`LIB_WIDTH);
`ifdef LIB_WIDTH
    localparam int WIDTH = `LIB_WIDTH;
`endif
`ifdef INHERITED_WIDTH
    localparam int INHERITED = `INHERITED_WIDTH;
`endif
endmodule
