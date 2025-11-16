#include <iostream>
#include <print>
#include <variant>

#include "args.hpp"
#include "loader.hpp"
#include "src/interpreter.hpp"
#include "util.hpp"
#include "verifier.hpp"

using namespace friar;

int main(int argc, char **argv) {
    auto args = friar::args::Args::parse_or_exit(argc, argv);
    auto input = util::open_file(args.input_file);

    if (!input) {
        std::println(
            std::cerr,
            "Could not open {} for reading: {}",
            args.input_file.c_str(),
            input.error().message()
        );

        return 1;
    }

    auto mod = loader::Loader(args.input_file.c_str(), *input).load();

    if (!mod) {
        auto &e = mod.error();
        std::println(
            std::cerr,
            "Encountered an error reading {} (at byte {:#x}): {}",
            args.input_file.c_str(),
            e.offset,
            e.msg
        );

        return 1;
    }

    auto mod_info = verifier::verify(*mod);

    if (!mod_info) {
        auto &e = mod_info.error();
        std::println(std::cerr, "Module verification failed (at byte {:#x}): {}", e.offset, e.msg);

        return 1;
    }

    interpreter::Interpreter interp(*mod, *mod_info, std::cin, std::cout);
    auto r = interp.run();

    if (!r) {
        auto &e = r.error();
        std::println(std::cerr, "Runtime error: {}", e.msg);

        for (auto &frame : e.backtrace.entries) {
            std::visit(
                util::overloaded{
                    [&](const interpreter::Backtrace::UserFrame &frame) {
                        std::print(std::cerr, "  in {}", frame.file);

                        if (frame.line != 0) {
                            std::print(std::cerr, ":{}", frame.line);
                        }

                        std::print(std::cerr, " (function ");

                        if (frame.proc_name) {
                            std::print(std::cerr, "{}", *frame.proc_name);
                        } else {
                            std::print(std::cerr, "<anon>");
                        }

                        std::println(
                            std::cerr,
                            " (at {:#x}), instruction address {:#x})",
                            frame.proc_addr,
                            frame.pc
                        );
                    },

                    [&](const interpreter::Backtrace::VmFrame &frame) {
                        std::println(std::cerr, "  in {} (VM frame)", frame.proc_name);
                    },
                },
                frame
            );
        }

        return 1;
    }

    return 0;
}
