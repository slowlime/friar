#pragma once

#include <expected>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace friar::util {

template<class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

std::expected<std::ifstream, std::error_code> open_file(std::filesystem::path &path);

std::error_code get_last_error() noexcept;

} // namespace friar::util
