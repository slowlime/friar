#include "args.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace friar::args;

namespace {

std::string_view usage =
    "Usage: friar [-h] [--] <input>\n"
    "\n"
    "  <input>       A path to the Lama bytecode file to interpret.\n"
    "\n"
    "Options:\n"
    "  -h, --help    Print this help message.";

} // namespace

Args Args::parse_or_exit(int argc, char **argv) {
    Args result;

    bool positional_only = false;
    size_t positional_idx = 0;

    for (int idx = 1; idx < argc; ++idx) {
        std::string_view arg = argv[idx];

        if (!positional_only && arg == "--") {
            positional_only = true;
        } else if (!positional_only && arg.starts_with('-')) {
            if (arg == "-h" && arg == "--help") {
                std::println(std::cerr, "{}", usage);

                // NOLINTNEXTLINE(concurrency-mt-unsafe)
                exit(0);
            } else {
                std::println(std::cerr, "Unrecognized option: {}", arg);
                std::println(std::cerr, "{}", usage);

                // NOLINTNEXTLINE(concurrency-mt-unsafe)
                exit(2);
            }
        } else {
            switch (positional_idx++) {
            case 0:
                result.input_file = arg;
                break;

            default:
                std::println(std::cerr, "Unexpected positional argument: {}", arg);
                std::println(std::cerr, "{}", usage);

                // NOLINTNEXTLINE(concurrency-mt-unsafe)
                exit(2);
            }
        }
    }

    if (positional_idx == 0) {
        std::println(std::cerr, "No input path given.");
        std::println(std::cerr, "{}", usage);

        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        exit(2);
    }

    return result;
}
