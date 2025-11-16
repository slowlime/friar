#include "util.hpp"

#include <cerrno>

using namespace friar;
using namespace friar::util;

std::error_code friar::util::get_last_error() noexcept {
    return std::make_error_code(std::errc(errno));
}

std::expected<std::ifstream, std::error_code> friar::util::open_file(std::filesystem::path &path) {
    errno = 0;
    std::ifstream s(path);

    if (!s) {
        return std::unexpected(get_last_error());
    }

    return s;
}
