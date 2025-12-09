#include "args.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <print>
#include <string_view>

using namespace friar::args;

namespace {

std::string_view usage =
    "Usage: friar [-h] [--mode=MODE] [--] <input>\n"
    "\n"
    "  <input>       A path to the Lama bytecode file to interpret.\n"
    "\n"
    "Options:\n"
    "  -h, --help    Print this help message.\n"
    "\n"
    "  -t, --time    Measure the execution time.\n"
    "\n"
    "  --mode=MODE   Select the execution mode. Available choices:\n"
    "                - disas: disassemble the bytecode and exit.\n"
    "                - verify: only perform bytecode verification.\n"
    "                - idiom: search for bytecode idioms.\n"
    "                - run: execute the bytecode (default).";

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
            if (arg == "-h" || arg == "--help") {
                std::println(std::cerr, "{}", usage);

                // NOLINTNEXTLINE(concurrency-mt-unsafe)
                exit(0);
            } else if (arg == "-t" || arg == "--time") {
                result.time = true;
            } else if (arg.starts_with("--")) {
                arg.remove_prefix(2);
                auto name = arg;
                std::optional<std::string_view> value;

                if (auto pos = arg.find('='); pos != std::string_view::npos) {
                    name = arg.substr(0, pos);
                    value = arg.substr(pos + 1);
                }

                if (name == "mode") {
                    if (!value) {
                        std::println(std::cerr, "--mode requires a value");
                        std::println(std::cerr, "{}", usage);

                        // NOLINTNEXTLINE(concurrency-mt-unsafe)
                        exit(2);
                    }

                    if (value == "disas") {
                        result.mode = Mode::Disas;
                    } else if (value == "verify") {
                        result.mode = Mode::Verify;
                    } else if (value == "idiom") {
                        result.mode = Mode::Idiom;
                    } else if (value == "run") {
                        result.mode = Mode::Run;
                    } else {
                        std::println(std::cerr, "Unrecognized mode: {}", *value);
                        std::println(std::cerr, "{}", usage);

                        // NOLINTNEXTLINE(concurrency-mt-unsafe)
                        exit(2);
                    }
                } else {
                    std::println(std::cerr, "Unrecognized option: {}", arg);
                    std::println(std::cerr, "{}", usage);

                    // NOLINTNEXTLINE(concurrency-mt-unsafe)
                    exit(2);
                }
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
