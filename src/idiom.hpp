#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "bytecode.hpp"
#include "verifier.hpp"

namespace friar::idiom {

struct Idiom {
    std::span<const bytecode::Instr> instrs;
    uint32_t occurrences = 0;
};

struct Idioms {
    /// Sorted in descending order.
    std::vector<Idiom> idioms;
};

Idioms find_idioms(const bytecode::Module &mod, const verifier::ModuleInfo &info);

} // namespace friar::idiom
