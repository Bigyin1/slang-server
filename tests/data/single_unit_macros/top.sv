module top(input logic [3:0] a, output logic [3:0] y);
    child u_child(.a(a), .y(y));
endmodule

`ifdef TEST_UNDEF
    `undef W
`elsif TEST_REDEFINE
    `define W 16
`endif
