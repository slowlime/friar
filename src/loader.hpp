#pragma once

#include <cstddef>
#include <expected>
#include <istream>
#include <span>
#include <string>
#include <string_view>

#include "bytecode.hpp"

namespace friar::loader {

/// Loads a Lama bytecode module from an input stream.
class Loader {
public:
    /// A loading error.
    struct Error {
        /// The byte offset where the error occurred.
        size_t offset = 0;

        /// The error message.
        std::string msg;
    };

    Loader(std::string name, std::istream &s);

    /// Loads a module from the input stream.
    ///
    /// This method must be called no more than once.
    std::expected<bytecode::Module, Error> load();

private:
    Error make_error(std::string msg, size_t pos) noexcept;
    Error make_eof_error(std::string_view field, size_t bytes_missing);

    std::expected<size_t, Error>
    load_bytes(std::string_view field, std::span<std::byte> dst, bool allow_partial = false);
    std::expected<uint32_t, Error> load_u32(std::string_view field);

    std::expected<void, Error> load_header();
    std::expected<void, Error> load_bytecode();

    bytecode::Module mod_;
    std::istream &s_;
    size_t pos_ = 0;
};

} // namespace friar::loader
