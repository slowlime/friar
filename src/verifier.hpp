#pragma once

#include "src/bytecode.hpp"

#include <expected>
#include <string>
#include <unordered_map>

namespace friar::verifier {

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
std::expected<void, Error> verify(bytecode::Module &mod);

} // namespace friar::verifier
