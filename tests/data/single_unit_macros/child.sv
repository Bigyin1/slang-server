module child(input logic [`W-1:0] a, output logic [`W-1:0] y);
    typedef enum logic [1:0] {A, B, C} state_t;
    state_t state;
    always_comb begin
        state = a[0];
        y = a;
    end
endmodule
