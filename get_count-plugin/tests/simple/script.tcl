yosys plugin -i ../../get_count.so
yosys -import

read_verilog -icells design.v

set n [get_count t:\$_BUF_]
puts "BUF count: $n"
if {$n != "5"} {
    error "Invalid count"
}

set n [get_count t:\$_NOT_]
puts "NOT count: $n"
if {$n != "3"} {
    error "Invalid count"
}

