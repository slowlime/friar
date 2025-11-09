#include "loader.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <format>
#include <ios>
#include <iterator>
#include <memory>
#include <span>
#include <utility>

using namespace friar::bytecode;
using namespace friar::loader;

namespace {

std::error_code get_last_error() noexcept {
    return std::make_error_code(std::errc(errno));
}

} // namespace

Loader::Loader(std::string name, std::istream &s) : mod_{.name = std::move(name)}, s_(s) {}

std::expected<Module, Loader::Error> Loader::load() {
    // clang-format off
    return load_header()
        .and_then([this] { return load_bytecode(); })
        .transform([this] { return std::move(mod_); });
    // clang-format on
}

Loader::Error Loader::make_error(std::string msg, size_t pos) noexcept {
    return Loader::Error{
        .offset = pos,
        .msg = std::move(msg),
    };
}

Loader::Error Loader::make_eof_error(std::string_view field, size_t bytes_missing) {
    return make_error(
        std::format(
            "encountered an unexpected end of file while parsing {}: need {} more "
            "bytes",
            field,
            bytes_missing
        ),
        pos_
    );
}

std::expected<size_t, Loader::Error>
Loader::load_bytes(std::string_view field, std::span<std::byte> dst, bool allow_partial) {
    errno = 0;
    s_.read(reinterpret_cast<char *>(dst.data()), static_cast<std::streamsize>(dst.size_bytes()));
    size_t read_count = s_.gcount();
    pos_ += read_count;

    if (auto err = get_last_error(); !err) {
        return std::unexpected(make_error(
            std::format("encountered a failure while parsing {}: {}", field, err.message()), pos_
        ));
    }

    if (!allow_partial && read_count < dst.size_bytes()) {
        return std::unexpected(make_eof_error(field, dst.size_bytes() - read_count));
    }

    return read_count;
}

std::expected<uint32_t, Loader::Error> Loader::load_u32(std::string_view field) {
    auto pos = pos_;
    int32_t value = 0;

    if (auto r = load_bytes(field, std::as_writable_bytes(std::span(&value, 1))); !r) {
        return std::unexpected(std::move(r).error());
    }

    // parse as little-endian regardless of the platform.
    // though for some unfathomable reason the on-disk representation does either
    // (depending on the platform that created the file).
    if (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }

    // though there is such a thing as mixed endianness, I don't think
    // these platforms even have a C++23-capable compiler in the first place.
    static_assert(
        std::endian::native == std::endian::big || std::endian::native == std::endian::little
    );

    if (value < 0) {
        return std::unexpected(
            make_error(std::format("{} must not be negative (got {})", field, value), pos)
        );
    }

    return static_cast<uint32_t>(value);
}

std::expected<void, Loader::Error> Loader::load_header() {
    size_t strtab_size = 0;

    if (auto r = load_u32("the string table size"); r) {
        strtab_size = *r;
    } else {
        return std::unexpected(std::move(r).error());
    }

    if (auto r = load_u32("the global count"); r) {
        mod_.global_count = *r;
    } else {
        return std::unexpected(std::move(r).error());
    }

    size_t symtab_entries = 0;

    if (auto r = load_u32("the symbol table entry count"); r) {
        symtab_entries = *r;
    } else {
        return std::unexpected(std::move(r).error());
    }

    // the public symbols.
    mod_.symtab.reserve(symtab_entries);

    for (size_t i = 0; i < symtab_entries; ++i) {
        Sym sym;

        if (auto r = load_u32("a symbol table entry's address"); r) {
            sym.address = *r;
        } else {
            return std::unexpected(std::move(r).error());
        }

        if (auto r = load_u32("a symbol table entry's name"); r) {
            sym.name_offset = *r;
        } else {
            return std::unexpected(std::move(r).error());
        }

        mod_.symtab.push_back(sym);
    }

    // the string table.
    mod_.strtab.resize(strtab_size);
    auto strtab_bytes = std::as_writable_bytes(std::span(mod_.strtab.data(), strtab_size));

    if (auto r = load_bytes("the string table", strtab_bytes); !r) {
        return std::unexpected(std::move(r).error());
    }

    return {};
}

std::expected<void, Loader::Error> Loader::load_bytecode() {
    constexpr size_t buf_size = 4096;

    auto pos = pos_;
    auto buf = std::make_unique<Instr[]>(4096);

    while (true) {
        auto bytes = std::as_writable_bytes(std::span(buf.get(), buf_size));
        if (auto r = load_bytes("bytecode", bytes, true); r) {
            auto read_instrs = std::span(buf.get(), *r);
            mod_.bytecode.append_range(read_instrs);

            if (*r < buf_size) {
                break;
            }
        } else {
            return std::unexpected(std::move(r).error());
        }
    }

    auto it = std::ranges::find(mod_.bytecode, Instr::Eof);

    if (it == mod_.bytecode.end()) {
        return std::unexpected(
            make_error("no end-of-file marker found in the bytecode section", pos_)
        );
    }

    auto idx = std::distance(mod_.bytecode.begin(), it);

    if (idx != mod_.bytecode.size()) {
        return std::unexpected(make_error(
            "the end-of-file marker in the bytecode section must be the final byte in the file",
            pos + idx
        ));
    }

    return {};
}
