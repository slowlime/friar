#pragma once

#include <filesystem>

namespace friar::args {

struct Args {
    std::filesystem::path input_file;

    static Args parse_or_exit(int argc, char **argv);
};

} // namespace friar::args
