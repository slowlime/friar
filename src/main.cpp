#include <iostream>
#include <print>
#include <variant>

#include "args.hpp"
#include "disas.hpp"
#include "idiom.hpp"
#include "interpreter.hpp"
#include "loader.hpp"
#include "util.hpp"
#include "verifier.hpp"

using namespace friar;

namespace {

int print_disas(const bytecode::Module &mod) {
    disas::disassemble(
        mod.bytecode,
        std::cout,
        disas::DisasOpts{
            .print_addr = true,
        }
    );

    return 0;
}

int print_idioms(const bytecode::Module &mod, const verifier::ModuleInfo &mod_info) {
    auto idioms = idiom::find_idioms(mod, mod_info);
    auto occur_width =
        idioms.idioms.empty() ? 1 : util::compute_decimal_width(idioms.idioms.front().occurrences);

    for (auto [instrs, occurrences] : idioms.idioms) {
        std::print("{:>{}}  ", occurrences, occur_width);
        disas::disassemble(
            instrs,
            std::cout,
            disas::DisasOpts{
                .print_addr = false,
                .instr_term = "",
                .instr_sep = "; ",
            }
        );
        std::println("");
    }

    return 0;
}

} // namespace

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

    if (args.mode == args::Mode::Disas) {
        return print_disas(*mod);
    }

    auto mod_info = verifier::verify(*mod);

    if (!mod_info) {
        auto &e = mod_info.error();
        std::println(std::cerr, "Module verification failed (at byte {:#x}): {}", e.offset, e.msg);

        return 1;
    }

    if (args.mode == args::Mode::Verify) {
        return 0;
    }

    if (args.mode == args::Mode::Idiom) {
        return print_idioms(*mod, *mod_info);
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
