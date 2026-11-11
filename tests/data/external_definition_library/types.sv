package external_pkg;
    typedef logic [7:0] word_t;
    function automatic word_t convert(int value);
        return word_t'(value);
    endfunction
endpackage

interface external_if;
    external_pkg::word_t data;
endinterface
