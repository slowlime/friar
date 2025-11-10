#include "verifier.hpp"

#include <algorithm>
#include <format>
#include <utility>
#include <variant>

#include "util.hpp"

using namespace friar;
using namespace friar::verifier;
using bytecode::Instr;
using util::overloaded;

namespace {

constexpr uint32_t max_stack_height = 0x7fff'ffff;
constexpr uint32_t max_captures = 0x7fff'ffff;

class Verifier {
public:
    explicit Verifier(bytecode::Module &mod) : mod_(mod) {
        verified_.resize(mod_.bytecode.size());
    }

    std::expected<void, Error> verify() {
        compute_last_strtab_entry();

        return verify_symtab().and_then([this] { return verify_bytecode(); });
    }

private:
    void compute_last_strtab_entry() {
        auto r = std::ranges::find_last(mod_.strtab, '\0');

        if (r.begin() == mod_.strtab.end()) {
            last_strtab_entry_ = 0;
        } else {
            last_strtab_entry_ = std::distance(mod_.strtab.begin(), r.begin());
        }
    }

    std::expected<void, Error> verify_symtab() {
        for (const auto &sym : mod_.symtab) {
            if (sym.address > mod_.bytecode.size()) {
                return std::unexpected(Error(
                    sym.offset,
                    std::format(
                        "the symbol points to address {:#x} which is beyond the size of the "
                        "bytecode ({:#x})",
                        sym.address,
                        mod_.bytecode.size()
                    )
                ));
            }

            if (auto r = verify_strtab_entry(sym.name, sym.offset); !r) {
                return std::unexpected(Error(
                    r.error().offset,
                    std::format("the symbol has an illegal name: {}", r.error().msg)
                ));
            }

            if (auto it = mod_.symtab_map.insert({mod_.strtab_entry_at(sym.name), sym.address});
                !it.second) {

                return std::unexpected(Error(
                    sym.address,
                    std::format("the symbol named `{}` is defined multiple times", it.first->first)
                ));
            }
        }

        return {};
    }

    std::expected<void, Error> verify_strtab_entry(uint32_t offset, uint32_t pos) const {
        if (offset >= mod_.strtab.size()) {
            return std::unexpected(Error(
                pos,
                std::format(
                    "a string table offset {:#x} is out of bounds for the string table of size "
                    "{:#x}",
                    offset,
                    mod_.strtab.size()
                )
            ));
        }

        if (offset > last_strtab_entry_) {
            return std::unexpected(Error(
                pos,
                std::format(
                    "a string at offset {:#x} in the string table is not NUL-terminated", offset
                )
            ));
        }

        return {};
    }

    struct TopLevelInstrVerifyReq {
        bool main = false;
    };

    struct BodyInstrVerifyReq {
        uint32_t proc_addr = 0;
        uint32_t stack_height = 0;
    };

    struct VerifyReq {
        uint32_t addr = 0;
        std::variant<TopLevelInstrVerifyReq, BodyInstrVerifyReq> kind;
    };

    struct BytecodeInfo {
        enum Kind : uint8_t {
            Proc,
            Body,
            Eof,
            Unknown,
        } kind;

        uint32_t proc_addr = 0;
        uint32_t stack_height = 0;
    };

    struct ProcInfo {
        uint32_t params = 0;
        uint32_t locals = 0;
        uint32_t captures = 0;
        uint32_t stack_size = 0;
        bool is_closure = false;
    };

    struct Closure {
        uint32_t addr = 0;
        uint32_t target_addr = 0;
        uint32_t captures = 0;
    };

    struct Call {
        uint32_t addr = 0;
        uint32_t target_addr = 0;
        uint32_t args = 0;
    };

    using PostValidateReq = std::variant<Closure, Call>;

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    std::expected<void, Error> verify_bytecode() {
        while (!to_verify_.empty()) {
            auto req = to_verify_.back();
            to_verify_.pop_back();

            auto r = std::visit(
                overloaded{
                    [&](TopLevelInstrVerifyReq &kind) {
                        return verify_top_level_instr(req.addr, kind.main);
                    },
                    [&](BodyInstrVerifyReq &kind) {
                        return verify_body_instr(req.addr, kind.proc_addr, kind.stack_height);
                    }
                },
                req.kind
            );

            if (!r) {
                return r;
            }
        }

        for (const auto &req : post_validate_reqs_) {
            auto r = std::visit(
                overloaded{
                    [&](const Closure &req) -> std::expected<void, Error> {
                        if (req.target_addr >= bc_.size()) {
                            return std::unexpected(Error(
                                req.addr,
                                std::format(
                                    "the closure instantiation refers to address {:#x}, which is "
                                    "out of bounds for the bytecode section of size {:#x}",
                                    req.target_addr,
                                    bc_.size()
                                )
                            ));
                        }

                        auto it = procs_.find(req.target_addr);

                        if (it == procs_.end()) {
                            return std::unexpected(Error(
                                req.addr,
                                std::format(
                                    "the closure instantiation refers to address {:#x}, which is "
                                    "not a procedure definition",
                                    req.target_addr
                                )
                            ));
                        }

                        if (req.captures < it->second.captures) {
                            return std::unexpected(Error(
                                req.addr,
                                std::format(
                                    "the closure instantiation captures {} variables while the "
                                    "procedure needs at least {}",
                                    req.captures,
                                    it->second.captures
                                )
                            ));
                        }

                        return {};
                    },

                    [&](const Call &req) -> std::expected<void, Error> {
                        if (req.target_addr >= bc_.size()) {
                            return std::unexpected(Error(
                                req.addr,
                                std::format(
                                    "the call refers to address {:#x}, which is out of bounds for "
                                    "the bytecode section of size {:#x}",
                                    req.target_addr,
                                    bc_.size()
                                )
                            ));
                        }

                        auto it = procs_.find(req.target_addr);

                        if (it == procs_.end()) {
                            return std::unexpected(Error(
                                req.addr,
                                std::format(
                                    "the call refers to address {:#x}, which is not a procedure "
                                    "definition",
                                    req.target_addr
                                )
                            ));
                        }

                        if (it->second.is_closure) {
                            return std::unexpected(Error(
                                req.addr,
                                "a closure cannot be called directly, as the call does not capture "
                                "variables"
                            ));
                        }

                        if (req.args != it->second.params) {
                            return std::unexpected(Error(
                                req.addr,
                                std::format(
                                    "the call has a wrong number of arguments: the procedure "
                                    "expects {}, got {}",
                                    it->second.params,
                                    req.args
                                )
                            ));
                        }

                        return {};
                    },
                },
                req
            );

            if (!r) {
                return r;
            }
        }

        return {};
    }

    std::expected<void, Error> verify_top_level_instr(uint32_t addr, bool main) {
        if (addr >= bc_.size()) {
            return std::unexpected(
                Error(addr, "no end-of-file marker found in the bytecode section")
            );
        }

        switch (verified_[addr].kind) {
        case BytecodeInfo::Proc:
        case BytecodeInfo::Eof:
            return {};

        case BytecodeInfo::Body:
        case BytecodeInfo::Unknown:
            break;
        }

        uint32_t op_addr = addr;

        switch (auto instr = bc_[addr++]) {
        case Instr::Cbegin:
            if (main) {
                return std::unexpected(Error(
                    op_addr,
                    "the first procedure must not close over variables, but it's declared with "
                    "CBEGIN"
                ));
            }

            [[fallthrough]];

        case Instr::Begin: {
            uint32_t params = 0;
            uint32_t locals = 0;

            if (auto r = read_u32("the parameter count", addr); r) {
                params = *r;
            } else {
                return std::unexpected(std::move(r).error());
            }

            if (auto r = read_u32("the local count", addr); r) {
                locals = *r;
            } else {
                return std::unexpected(std::move(r).error());
            }

            procs_.insert(
                {op_addr,
                 ProcInfo{
                     .params = params,
                     .locals = locals,
                     .captures = 0,
                     .is_closure = instr == Instr::Cbegin,
                 }}
            );

            verified_[op_addr] = BytecodeInfo{
                .kind = BytecodeInfo::Proc,
                .proc_addr = op_addr,
            };

            to_verify_.emplace_back(op_addr, BodyInstrVerifyReq{.proc_addr = op_addr});

            break;
        }

        case Instr::Eof:
            if (main) {
                return std::unexpected(Error(op_addr, "no main procedure definition found"));
            }

            verified_[op_addr] = BytecodeInfo{
                .kind = BytecodeInfo::Eof,
            };

            break;

        default:
            return std::unexpected(Error(
                op_addr,
                std::format(
                    "encountered an illegal top-level bytecode byte {:#02x}", uint8_t(instr)
                )
            ));
        }

        return {};
    }

    std::expected<void, Error>
    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    verify_body_instr(uint32_t addr, uint32_t proc_addr, uint32_t stack_height) {
        if (addr >= bc_.size()) {
            return std::unexpected(Error(
                addr, "encountered the end of the file unexpectedly while verifying the bytecode"
            ));
        }

        auto &info = verified_[addr];

        switch (info.kind) {
        case BytecodeInfo::Body:
            if (info.proc_addr != proc_addr) {
                return std::unexpected(Error(
                    addr,
                    std::format(
                        "an instruction is part of multiple procedure definitions (at {:#x} and "
                        "{:#x})",
                        info.proc_addr,
                        proc_addr
                    )
                ));
            }

            if (info.stack_height != stack_height) {
                return std::unexpected(Error(
                    addr,
                    std::format(
                        "detected unbalanced static stack heights: {} and {}",
                        info.stack_height,
                        stack_height
                    )
                ));
            }

            return {};

        case BytecodeInfo::Proc:
        case BytecodeInfo::Eof:
        case BytecodeInfo::Unknown:
            break;
        }

        auto &proc = procs_[info.proc_addr];
        info.stack_height = stack_height;
        info.proc_addr = proc_addr;
        proc.stack_size = std::max(proc.stack_size, info.stack_height);

        auto op_addr = addr;
        auto instr = bc_[addr++];

        auto check_stack = [&](size_t pops, size_t pushes) -> std::expected<void, Error> {
            if (info.stack_height < pops) {
                return std::unexpected(Error(
                    op_addr,
                    std::format(
                        "not enough operands on the stack: expected at least "
                        "{}, have {}",
                        pops,
                        info.stack_height
                    )
                ));
            }

            if (max_stack_height - info.stack_height < pushes) {
                return std::unexpected(Error(
                    op_addr,
                    std::format("exceeded the maximum static stack height of {}", max_stack_height)
                ));
            }

            info.stack_height += pushes - pops;
            proc.stack_size = std::max(proc.stack_size, info.stack_height);

            return {};
        };

        auto check_varspec = [&](Varspec varspec) -> std::expected<void, Error> {
            switch (varspec.kind) {
            case Varspec::Global:
                if (varspec.idx >= mod_.global_count) {
                    return std::unexpected(Error(
                        varspec.addr,
                        std::format(
                            "the global index {} is out of bounds: the module only has {}",
                            varspec.idx,
                            mod_.global_count
                        )
                    ));
                }

                return {};

            case Varspec::Local:
                if (varspec.idx >= proc.locals) {
                    return std::unexpected(Error(
                        varspec.addr,
                        std::format(
                            "the local index {} is out of bounds: the procedure only has {}",
                            varspec.idx,
                            proc.locals
                        )
                    ));
                }

                return {};

            case Varspec::Param:
                if (varspec.idx >= proc.params) {
                    return std::unexpected(Error(
                        varspec.addr,
                        std::format(
                            "the parameter index {} is out of bounds: the procedure only has {}",
                            varspec.idx,
                            proc.params
                        )
                    ));
                }

                return {};

            case Varspec::Capture:
                if (varspec.idx >= max_captures) {
                    return std::unexpected(Error(
                        varspec.addr,
                        std::format(
                            "the captured variable index {} is too large: the maximum is {}",
                            varspec.idx,
                            max_captures
                        )
                    ));
                }

                proc.captures = std::max(proc.captures, varspec.idx + 1);

                return {};
            }
        };

        auto check_jmp_target = [&](uint32_t l, uint32_t l_addr) -> std::expected<void, Error> {
            if (l >= bc_.size()) {
                return std::unexpected(Error(
                    l_addr,
                    std::format(
                        "the jump target {:#x} is out of bounds for the bytecode section of size "
                        "{:#x}",
                        l,
                        bc_.size()
                    )
                ));
            }

            auto instr = bc_[l];

            if (instr == Instr::Begin || instr == Instr::Cbegin) {
                return std::unexpected(Error(
                    l_addr,
                    std::format(
                        "the jump target {:#x} refers to the beginning of a procedure declaration",
                        l
                    )
                ));
            }

            if (instr == Instr::Eof) {
                return std::unexpected(Error(
                    l_addr, std::format("the jump target {:#x} refers to the end-of-file marker", l)
                ));
            }

            to_verify_.emplace_back(
                l,
                BodyInstrVerifyReq{
                    .proc_addr = info.proc_addr,
                    .stack_height = info.stack_height,
                }
            );

            return {};
        };

        std::expected<void, Error> r;
        bool continue_path = true;

        switch (instr) {
        case Instr::Add:
        case Instr::Sub:
        case Instr::Mul:
        case Instr::Div:
        case Instr::Mod:
        case Instr::Lt:
        case Instr::Le:
        case Instr::Gt:
        case Instr::Ge:
        case Instr::Eq:
        case Instr::Ne:
        case Instr::And:
        case Instr::Or:
            r = check_stack(2, 1);
            break;

        case Instr::Const:
            r = read_u32("integer constant", addr, true).and_then([&](auto) {
                return check_stack(0, 1);
            });

            break;

        case Instr::String: {
            auto s_addr = addr;

            r = read_u32("string table offset", addr)
                    .and_then([&](auto s) { return verify_strtab_entry(s, s_addr); })
                    .and_then([&] { return check_stack(0, 1); });

            break;
        }

        case Instr::Sexp: {
            auto s_addr = addr;

            r = read_u32("string table offset", addr).and_then([&](auto s) {
                return read_u32("sexp member count", addr).and_then([&](auto n) {
                    return verify_strtab_entry(s, s_addr).and_then([&] {
                        return check_stack(n, 1);
                    });
                });
            });

            break;
        }

        case Instr::Sti:
            r = check_stack(2, 1);
            break;

        case Instr::Sta:
            r = check_stack(3, 1);
            break;

        case Instr::Jmp: {
            auto l_addr = addr;

            continue_path = false;
            r = read_u32("jump target", addr).and_then([&](auto l) {
                return check_jmp_target(l, l_addr);
            });

            break;
        }

        case Instr::End:
        case Instr::Ret:
            continue_path = false;
            r = check_stack(1, 1);
            break;

        case Instr::Drop:
            r = check_stack(1, 0);
            break;

        case Instr::Dup:
            r = check_stack(1, 2);
            break;

        case Instr::Swap:
            r = check_stack(2, 2);
            break;

        case Instr::Elem:
            r = check_stack(2, 1);
            break;

        case Instr::Ld:
        case Instr::Lda:
            r = read_varspec(--addr, true).and_then(check_varspec).and_then([&] {
                return check_stack(0, 1);
            });

            break;

        case Instr::St:
            r = read_varspec(--addr, true).and_then(check_varspec).and_then([&] {
                return check_stack(1, 1);
            });

            break;

        case Instr::CjmpZ:
        case Instr::CjmpNz: {
            auto l_addr = addr;

            r = read_u32("jump target", addr)
                    .and_then([&](auto l) { return check_jmp_target(l, l_addr); })
                    .and_then([&] { return check_stack(1, 0); });

            break;
        }

        case Instr::Begin:
            r = std::unexpected(Error(
                op_addr,
                std::format(
                    "encountered a BEGIN instruction nested inside a procedure declared at {:#x}",
                    info.proc_addr
                )
            ));

            break;

        case Instr::Cbegin:
            r = std::unexpected(Error(
                op_addr,
                std::format(
                    "encountered a CBEGIN instruction nested inside a procedure declared at {:#x}",
                    info.proc_addr
                )
            ));

            break;

        case Instr::Closure:
            r = read_u32("call target", addr).and_then([&](auto l) {
                return read_u32("captured variable count", addr).and_then([&](auto n) {
                    std::expected<void, Error> result;

                    while (result) {
                        result = read_varspec(addr, false).and_then([&](auto varspec) {
                            return check_varspec(varspec);
                        });
                    }

                    result = result.and_then([&] { return check_stack(0, 1); });

                    if (result) {
                        post_validate_reqs_.emplace_back(
                            Closure{
                                .addr = op_addr,
                                .target_addr = l,
                                .captures = n,
                            }
                        );
                    }

                    return result;
                });
            });

            break;

        case Instr::CallC:
            r = read_u32("argument count", addr).and_then([&](auto n) {
                return check_stack(n + 1, 1);
            });

            break;

        case Instr::Call:
            r = read_u32("call target", addr).and_then([&](auto l) {
                return read_u32("argument count", addr).and_then([&](auto n) {
                    auto result = check_stack(n, 1);

                    if (result) {
                        post_validate_reqs_.emplace_back(
                            Call{
                                .addr = op_addr,
                                .target_addr = l,
                                .args = n,
                            }
                        );
                    }

                    return result;
                });
            });

            break;

        case Instr::Tag: {
            auto s_addr = addr;

            r = read_u32("string table offset", addr).and_then([&](auto s) {
                return read_u32("member count", addr).and_then([&](auto n) {
                    return verify_strtab_entry(s, s_addr).and_then([&] {
                        return check_stack(1, 1);
                    });
                });
            });

            break;
        }

        case Instr::Array:
            r = read_u32("element count", addr).and_then([&](auto n) { return check_stack(1, 1); });

            break;

        case Instr::Fail:
            continue_path = false;
            r = read_u32("line number", addr).and_then([&](auto ln) {
                return read_u32("column number", addr).and_then([&](auto col) {
                    return check_stack(1, 0);
                });
            });

            break;

        case Instr::Line:
            r = read_u32("line number", addr).transform([](auto) {});
            break;

        case Instr::PattEqStr:
        case Instr::PattString:
        case Instr::PattArray:
        case Instr::PattSexp:
        case Instr::PattRef:
        case Instr::PattVal:
        case Instr::PattFun:
            r = check_stack(1, 1);
            break;

        case Instr::CallLread:
            r = check_stack(0, 1);
            break;

        // NOLINTNEXTLINE(bugprone-branch-clone)
        case Instr::CallLwrite:
            r = check_stack(1, 1);
            break;

        case Instr::CallLlength:
            r = check_stack(1, 1);
            break;

        case Instr::CallLstring:
            r = check_stack(1, 1);
            break;

        case Instr::CallBarray:
            r = read_u32("element count", addr).and_then([&](auto n) { return check_stack(n, 1); });
            break;

        case Instr::Eof:
            return std::unexpected(Error(
                op_addr,
                "encountered an unexpected end-of-file marker inside a procedure definition"
            ));
        }

        if (!r) {
            return r;
        }

        if (instr == Instr::End) {
            to_verify_.emplace_back(addr, TopLevelInstrVerifyReq{.main = false});
        } else if (continue_path) {
            to_verify_.emplace_back(
                addr,
                BodyInstrVerifyReq{
                    .proc_addr = info.proc_addr,
                    .stack_height = info.stack_height,
                }
            );
        }

        return {};
    }

    std::expected<uint32_t, Error>
    read_u32(std::string_view field, uint32_t &addr, bool allow_negative = false) {
        if (-1U - addr <= sizeof(uint32_t) || addr + sizeof(uint32_t) >= bc_.size()) {
            return std::unexpected(Error(
                addr,
                std::format(
                    "encountered the end of the file unexpectedly while trying to read the {}",
                    field
                )
            ));
        }

        uint32_t result = 0;

        for (size_t i = 0; i < sizeof(uint32_t); ++i) {
            result |= static_cast<uint32_t>(bc_[addr + i]) << 8 * i;
        }

        if (!allow_negative && (result >> 31) != 0) {
            return std::unexpected(
                Error(addr, std::format("the value {:#x} is too large for the {}", result, field))
            );
        }

        addr += sizeof(uint32_t);

        return result;
    }

    struct Varspec {
        enum Kind : uint8_t {
            Global,
            Local,
            Param,
            Capture,
        } kind;

        uint32_t addr = 0;
        uint32_t idx = 0;
    };

    std::expected<Varspec, Error> read_varspec(uint32_t &addr, bool ignore_hi) {
        constexpr size_t size = 1 + sizeof(uint32_t);

        if (-1U - addr <= size || addr + size >= bc_.size()) {
            return std::unexpected(Error(
                addr,
                "encountered the end of the file unexpectedly while trying to read a variable "
                "descriptor"
            ));
        }

        auto kind = static_cast<uint8_t>(bc_[addr]);
        Varspec result{.addr = addr};

        if (ignore_hi) {
            kind &= 0xf;
        }

        switch (kind) {
        case 0:
            result.kind = Varspec::Global;
            break;

        case 1:
            result.kind = Varspec::Local;
            break;

        case 2:
            result.kind = Varspec::Param;
            break;

        case 3:
            result.kind = Varspec::Capture;
            break;

        default:
            return std::unexpected(
                Error(addr, std::format("unrecognized variable kind encoding: {:#02x}", kind))
            );
        }

        for (size_t i = 0; i < sizeof(uint32_t); ++i) {
            result.idx |= static_cast<uint32_t>(bc_[addr + i + 1]) << 8 * i;
        }

        return result;
    }

    bytecode::Module &mod_;
    std::vector<Instr> &bc_ = mod_.bytecode;

    size_t last_strtab_entry_ = 0;
    std::vector<VerifyReq> to_verify_{{.addr = 0, .kind = TopLevelInstrVerifyReq{.main = true}}};
    std::vector<BytecodeInfo> verified_;
    std::unordered_map<uint32_t, ProcInfo> procs_;
    std::vector<PostValidateReq> post_validate_reqs_;
};

} // namespace

namespace friar::verifier {

std::expected<void, Error> verify(bytecode::Module &mod) {
    return Verifier(mod).verify();
}

} // namespace friar::verifier
