#pragma once

#include "src/bytecode.hpp"

#include <expected>
#include <string>
#include <unordered_map>

namespace friar::verifier {

constexpr uint32_t max_stack_size = 0xffff;
constexpr uint32_t max_captures = 0x7fff'ffff;
constexpr uint32_t max_param_count = 0xffff;
constexpr uint32_t max_member_count = 0xffff;
constexpr uint32_t max_elem_count = 0xfff'ffff;

/// A verification error.
struct Error {
    /// The byte offset where the error occurred.
    size_t offset = 0;

    /// The error message.
    std::string msg;
};

/// The results of bytecode analysis.
struct ModuleInfo {
    struct Proc {
        uint32_t params = 0;
        uint32_t locals = 0;
        uint32_t captures = 0;
        uint32_t stack_size = 0;
        bool is_closure = false;
    };

    std::unordered_map<uint32_t, Proc> procs;
};

/// Statically verifies the module for validity.
std::expected<ModuleInfo, Error> verify(bytecode::Module &mod);

} // namespace friar::verifier
