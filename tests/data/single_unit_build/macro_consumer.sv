module macro_consumer;
    logic [`SHARED_WIDTH-1:0] data;
    $static_assert($bits(data) == 8, "shared width changed");
endmodule
