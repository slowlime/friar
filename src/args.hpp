#pragma once

#include <cstdint>
#include <filesystem>

namespace friar::args {

enum class Mode : uint8_t {
    Disas,
    Verify,
    Idiom,
    Run,
};

struct Args {
    std::filesystem::path input_file;
    Mode mode = Mode::Run;

    static Args parse_or_exit(int argc, char **argv);
};

} // namespace friar::args
