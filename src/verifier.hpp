#pragma once

#include "src/bytecode.hpp"

#include <expected>
#include <string>

namespace friar::verifier {

/// A verification error.
struct Error {
    /// The byte offset where the error occurred.
    size_t offset = 0;

    /// The error message.
    std::string msg;
};

/// Statically verifies the module for validity.
std::expected<void, Error> verify(bytecode::Module &mod);

} // namespace friar::verifier
