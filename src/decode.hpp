#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "bytecode.hpp"
#include "src/util.hpp"

namespace friar::decode {

/// The beginning of an instruction.
struct InstrStart {
    /// The address of the opcode in the bytecode section.
    uint32_t addr;

    /// The decoded opcode.
    bytecode::Instr opcode;
};

/// The end of an instruction.
struct InstrEnd {
    /// The address of the byte following the instruction's end.
    uint32_t addr;

    //// The address of the first byte of the instruction.
    uint32_t start;

    /// The length of the instruction in bytes.
    constexpr uint32_t len() const noexcept {
        return addr - start;
    }

    /// The address of the byte following the instruction's end.
    constexpr uint32_t end() const noexcept {
        return addr;
    }
};

/// A 32-bit immediate.
struct Imm32 {
    /// The address of the first byte of the immediate.
    uint32_t addr;

    /// The value of the immediate.
    uint32_t imm;

    /// The length of the immediate in bytes.
    static constexpr uint32_t len() noexcept {
        return sizeof(uint32_t);
    }

    /// The address of the byte following the immediate's end.
    constexpr uint32_t end() const noexcept {
        return addr + len();
    }
};

/// A variable description immediate (used in load, store, and closure instructions).
struct ImmVarspec {
    /// The address of the first byte of the immediate.
    uint32_t addr;

    /// The variable kind.
    enum class VarKind : uint8_t {
        Global,
        Local,
        Param,
        Capture,
    } kind;

    /// The variable index.
    uint32_t idx;

    /// The length of the immediate in bytes.
    static constexpr uint32_t len() noexcept {
        return 1 + sizeof(uint32_t);
    }

    /// The address of the byte following the immediate's end.
    constexpr uint32_t end() const noexcept {
        return addr + len();
    }
};

/// A error while decoding bytecode.
struct Error {
    /// The address where the error occurred.
    uint32_t addr;

    /// The specific reason for this error.
    enum class Kind : uint8_t {
        // Reached the EOF prematurely.
        Eof,

        /// A varspec immediate has an unrecognized variable kind.
        IllegalVarKind,

        /// Encountered an illegal instruction.
        IllegalOp,
    } kind;

    /// A description of this error.
    std::string msg;
};

/// A bytecode decoder.
class Decoder {
public:
    using Result = std::variant<InstrStart, InstrEnd, Imm32, ImmVarspec, Error>;

    explicit Decoder(std::span<const bytecode::Instr> bytecode) : bc_(bytecode) {}

    void move_to(uint32_t addr) {
        pos_ = addr;
    }

    uint32_t pos() const noexcept {
        return pos_;
    }

    template<class F>
        requires requires(const F &listener, Result r) {
            { listener(r) } -> std::same_as<void>;
        }
    void next(const F &listener);

private:
    inline std::expected<Imm32, Error> read_imm32(std::string_view field);
    inline std::expected<ImmVarspec, Error> read_imm_varspec(bool ignore_hi);

    std::span<const bytecode::Instr> bc_;
    uint32_t pos_ = 0;
};

template<class F>
    requires requires(const F &listener, Decoder::Result r) {
        { listener(r) } -> std::same_as<void>;
    }
void Decoder::next(const F &listener) {
    if (pos_ >= bc_.size_bytes()) {
        listener(
            Error{
                .addr = pos_,
                .kind = Error::Kind::Eof,
                .msg = "encountered the EOF while reading an opcode",
            }
        );

        return;
    }

    uint32_t op_start = pos_;
    auto opcode = bc_[pos_++];

    listener(
        InstrStart{
            .addr = op_start,
            .opcode = opcode,
        }
    );

    std::expected<void, Error> r;

    switch (opcode) {
    case bytecode::Instr::Add:
    case bytecode::Instr::Sub:
    case bytecode::Instr::Mul:
    case bytecode::Instr::Div:
    case bytecode::Instr::Mod:
    case bytecode::Instr::Lt:
    case bytecode::Instr::Le:
    case bytecode::Instr::Gt:
    case bytecode::Instr::Ge:
    case bytecode::Instr::Eq:
    case bytecode::Instr::Ne:
    case bytecode::Instr::And:
    case bytecode::Instr::Or:
    case bytecode::Instr::Sti:
    case bytecode::Instr::Sta:
    case bytecode::Instr::End:
    case bytecode::Instr::Ret:
    case bytecode::Instr::Drop:
    case bytecode::Instr::Dup:
    case bytecode::Instr::Swap:
    case bytecode::Instr::Elem:
    case bytecode::Instr::PattEqStr:
    case bytecode::Instr::PattString:
    case bytecode::Instr::PattArray:
    case bytecode::Instr::PattSexp:
    case bytecode::Instr::PattRef:
    case bytecode::Instr::PattVal:
    case bytecode::Instr::PattFun:
    case bytecode::Instr::CallLread:
    case bytecode::Instr::CallLwrite:
    case bytecode::Instr::CallLlength:
    case bytecode::Instr::CallLstring:
    case bytecode::Instr::Eof:
        break;

    case bytecode::Instr::Const:
        r = read_imm32("integer constant").transform(listener);
        break;

    case bytecode::Instr::String:
        r = read_imm32("string table offset").transform(listener);
        break;

    case bytecode::Instr::Sexp:
        r = read_imm32("tag")
                .transform(listener)
                .and_then([&] { return read_imm32("member count"); })
                .transform(listener);

        break;

    case bytecode::Instr::Jmp:
    case bytecode::Instr::CjmpZ:
    case bytecode::Instr::CjmpNz:
        r = read_imm32("jump target").transform(listener);
        break;

    case bytecode::Instr::LdG:
    case bytecode::Instr::LdL:
    case bytecode::Instr::LdA:
    case bytecode::Instr::LdC:
    case bytecode::Instr::LdaG:
    case bytecode::Instr::LdaL:
    case bytecode::Instr::LdaA:
    case bytecode::Instr::LdaC:
    case bytecode::Instr::StG:
    case bytecode::Instr::StL:
    case bytecode::Instr::StA:
    case bytecode::Instr::StC:
        --pos_;
        r = read_imm_varspec(true).transform(listener);
        break;

    case bytecode::Instr::Begin:
    case bytecode::Instr::Cbegin:
        r = read_imm32("parameter count")
                .transform(listener)
                .and_then([&] { return read_imm32("local count"); })
                .transform(listener);

        break;

    case bytecode::Instr::Closure:
        r = read_imm32("call target")
                .transform(listener)
                .and_then([&] { return read_imm32("capture count"); })
                .and_then([&](auto n) {
                    listener(n);

                    std::expected<void, Error> r;

                    for (size_t i = 0; i < n.imm && r; ++i) {
                        r = read_imm_varspec(false).transform(listener);
                    }

                    return r;
                });

        break;

    case bytecode::Instr::CallC:
        r = read_imm32("argument count").transform(listener);
        break;

    case bytecode::Instr::Call:
        r = read_imm32("call target")
                .transform(listener)
                .and_then([&] { return read_imm32("argument count"); })
                .transform(listener);

        break;

    case bytecode::Instr::Tag:
        r = read_imm32("tag")
                .transform(listener)
                .and_then([&] { return read_imm32("member count"); })
                .transform(listener);

        break;

    case bytecode::Instr::Array:
    case bytecode::Instr::CallBarray:
        r = read_imm32("element count").transform(listener);
        break;

    case bytecode::Instr::Fail:
        r = read_imm32("line number")
                .transform(listener)
                .and_then([&] { return read_imm32("column numbre"); })
                .transform(listener);

        break;

    case bytecode::Instr::Line:
        r = read_imm32("line number").transform(listener);
        break;

    default:
        listener(
            Error{
                .addr = op_start,
                .kind = Error::Kind::IllegalOp,
                .msg = std::format(
                    "encountered an illegal opcode {:#02x}", static_cast<uint8_t>(opcode)
                ),
            }
        );
    }

    if (!r) {
        listener(std::move(r).error());
    }

    listener(
        InstrEnd{
            .addr = pos_,
            .start = op_start,
        }
    );
}

std::expected<Imm32, Error> Decoder::read_imm32(std::string_view field) {
    if (-1U - pos_ <= sizeof(uint32_t) || pos_ + sizeof(uint32_t) > bc_.size()) {
        pos_ = bc_.size();

        return std::unexpected(
            Error{
                .addr = pos_,
                .kind = Error::Kind::Eof,
                .msg = std::format("encountered the EOF while trying to read the {}", field),
            }
        );
    }

    Imm32 result{
        .addr = pos_,
        .imm = 0,
    };

    result.imm =
        util::from_u32_le(std::span<const std::byte, 4>(std::as_bytes(bc_.subspan(pos_, 4))));
    pos_ += sizeof(uint32_t);

    return result;
}

std::expected<ImmVarspec, Error> Decoder::read_imm_varspec(bool ignore_hi) {
    if (-1U - pos_ <= ImmVarspec::len() || pos_ + ImmVarspec::len() > bc_.size()) {
        pos_ = bc_.size();

        return std::unexpected(
            Error{
                .addr = pos_,
                .kind = Error::Kind::Eof,
                .msg = "encountered the EOF while trying to read a variable descriptor",
            }
        );
    }

    ImmVarspec result{
        .addr = pos_,
    };

    auto kind = static_cast<uint8_t>(bc_[pos_++]);

    if (ignore_hi) {
        kind &= 0xf;
    }

    switch (kind) {
    case 0:
        result.kind = ImmVarspec::VarKind::Global;
        break;

    case 1:
        result.kind = ImmVarspec::VarKind::Local;
        break;

    case 2:
        result.kind = ImmVarspec::VarKind::Param;
        break;

    case 3:
        result.kind = ImmVarspec::VarKind::Capture;
        break;

    default:
        return std::unexpected(
            Error{
                .addr = result.addr,
                .kind = Error::Kind::IllegalVarKind,
                .msg = std::format("unrecognized variable kind encoding: {:#02x}", kind),
            }
        );
    }

    result.idx =
        util::from_u32_le(std::span<const std::byte, 4>(std::as_bytes(bc_.subspan(pos_, 4))));
    pos_ += sizeof(uint32_t);

    return result;
}

} // namespace friar::decode
