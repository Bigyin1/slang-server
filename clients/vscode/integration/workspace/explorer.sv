module explorer_cell #(parameter int WIDTH = 8) (input logic [WIDTH-1:0] value);
endmodule

module explorer_a;
    explorer_cell #(.WIDTH(8)) narrow('0);
    explorer_cell #(.WIDTH(16)) wide('0);
    external_only external_child();
endmodule

module explorer_b;
endmodule
