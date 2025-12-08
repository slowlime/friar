#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>

namespace friar::util {

template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

std::expected<std::ifstream, std::error_code> open_file(std::filesystem::path &path);

std::error_code get_last_error() noexcept;

constexpr size_t compute_decimal_width(size_t v) {
    // ported from Rust's ilog10 implementation.
    constexpr size_t c1 = 0b011'00000000000000000 - 10;
    constexpr size_t c2 = 0b100'00000000000000000 - 100;
    constexpr size_t c3 = 0b111'00000000000000000 - 1000;
    constexpr size_t c4 = 0b100'00000000000000000 - 10000;

    if (v == 0) {
        return 1;
    }

    size_t width = 1;

    while (v >= 100'000) {
        v /= 100'000;
        width += 5;
    }

    width += (((v + c1) & (v + c2)) ^ ((v + c3) & (v + c4))) >> 17;

    return width;
}

inline uint32_t from_u32_le(std::span<const std::byte, 4> bytes) {
    uint32_t result = 0;

    for (size_t i = 0; i < sizeof(uint32_t); ++i) {
        result |= static_cast<uint32_t>(bytes[i]) << 8 * i;
    }

    return result;
}

inline void to_u32_le(std::span<std::byte, 4> bytes, uint32_t value) {
    for (size_t i = 0; i < sizeof(uint32_t); ++i, value >>= 8) {
        bytes[i] = std::byte(value & 0xff);
    }
}

} // namespace friar::util
