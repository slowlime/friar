#pragma once

#include <ostream>
#include <span>
#include <string_view>

#include "bytecode.hpp"

namespace friar::disas {

struct DisasOpts {
    bool print_addr = false;
    std::string_view instr_sep = "\n";
};

void disassemble(std::span<const bytecode::Instr> bc, std::ostream &s, DisasOpts opts = {});

} // namespace
