#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <variant>

#include "config.hpp"
#include "bytecode.hpp"
#include "verifier.hpp"

namespace friar::interpreter {

struct Backtrace {
    struct UserFrame {
        std::string file;
        std::optional<std::string> proc_name;
        uint32_t proc_addr;
        uint32_t line;
        uint32_t pc;
    };

    struct VmFrame {
        std::string proc_name;
    };

    using Frame = std::variant<UserFrame, VmFrame>;

    std::vector<Frame> entries;
};

class Interpreter {
public:
    struct Error {
        Backtrace backtrace;
        std::string msg;
    };

    Interpreter(
        bytecode::Module &mod,
#ifndef DYNAMIC_VERIFICATION
        const verifier::ModuleInfo &info,
#endif
        std::istream &input,
        std::ostream &output
    );

    std::expected<void, Error> run();

private:
    struct Frame {
        // the address of the procedure corresponding to the frame.
        uint32_t proc_addr;

        // the pc of the caller.
        uint32_t saved_pc;

        // the stack base of the caller.
        size_t saved_base;

        // the number of the caller's arguments.
        size_t saved_args;

#ifdef DYNAMIC_VERIFICATION
        // the number of the caller's locals.
        size_t saved_locals;
#endif

        // the current source line for this frame.
        uint32_t line = 0;

        // `true` if there's a closure object associated with this frame.
        bool is_closure = false;
    };

    bytecode::Module &mod_;

#ifndef DYNAMIC_VERIFICATION
    const verifier::ModuleInfo &info_;
#endif

    std::istream &input_;
    std::ostream &output_;
};

} // namespace friar::interpreter
