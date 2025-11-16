#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <istream>
#include <optional>
#include <ostream>
#include <string>
#include <variant>

#include "bytecode.hpp"
#include "src/verifier.hpp"

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
        const verifier::ModuleInfo &info,
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

        // the current source line for this frame.
        uint32_t line = 0;
    };

    bytecode::Module &mod_;
    const verifier::ModuleInfo &info_;
    std::istream &input_;
    std::ostream &output_;
};

} // namespace friar::interpreter
