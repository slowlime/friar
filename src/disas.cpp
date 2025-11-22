#include "disas.hpp"

#include <print>
#include <variant>

#include "decode.hpp"
#include "util.hpp"

using namespace friar;
using namespace friar::disas;

void friar::disas::disassemble(
    std::span<const bytecode::Instr> bc,
    std::ostream &s,
    DisasOpts opts
) {
    decode::Decoder decoder(bc);
    auto width = util::compute_decimal_width(bc.size_bytes());
    bool first = true;

    while (decoder.pos() < bc.size()) {
        decoder.next([&](const decode::Decoder::Result &result) {
            std::visit(
                util::overloaded{
                    [&](const decode::InstrStart &start) {
                        if (!first) {
                            s << opts.instr_sep;
                        }

                        first = false;

                        if (opts.print_addr) {
                            std::print(s, "{:>{}x}:  ", start.addr, width);
                        }

                        switch (start.opcode) {
                        case bytecode::Instr::Add:
                            s << "binop +";
                            break;

                        case bytecode::Instr::Sub:
                            s << "binop -";
                            break;

                        case bytecode::Instr::Mul:
                            s << "binop *";
                            break;

                        case bytecode::Instr::Div:
                            s << "binop /";
                            break;

                        case bytecode::Instr::Mod:
                            s << "binop %";
                            break;

                        case bytecode::Instr::Lt:
                            s << "binop <";
                            break;

                        case bytecode::Instr::Le:
                            s << "binop <=";
                            break;

                        case bytecode::Instr::Gt:
                            s << "binop >";
                            break;

                        case bytecode::Instr::Ge:
                            s << "binop >=";
                            break;

                        case bytecode::Instr::Eq:
                            s << "binop ==";
                            break;

                        case bytecode::Instr::Ne:
                            s << "binop !=";
                            break;

                        case bytecode::Instr::And:
                            s << "binop &&";
                            break;

                        case bytecode::Instr::Or:
                            s << "binop !!";
                            break;

                        case bytecode::Instr::Const:
                            s << "const";
                            break;

                        case bytecode::Instr::String:
                            s << "string";
                            break;

                        case bytecode::Instr::Sexp:
                            s << "sexp";
                            break;

                        case bytecode::Instr::Sti:
                            s << "sti";
                            break;

                        case bytecode::Instr::Sta:
                            s << "sta";
                            break;

                        case bytecode::Instr::Jmp:
                            s << "jmp";
                            break;

                        case bytecode::Instr::End:
                            s << "end";
                            break;

                        case bytecode::Instr::Ret:
                            s << "ret";
                            break;

                        case bytecode::Instr::Drop:
                            s << "drop";
                            break;

                        case bytecode::Instr::Dup:
                            s << "dup";
                            break;

                        case bytecode::Instr::Swap:
                            s << "swap";
                            break;

                        case bytecode::Instr::Elem:
                            s << "elem";
                            break;

                        case bytecode::Instr::LdG:
                        case bytecode::Instr::LdL:
                        case bytecode::Instr::LdA:
                        case bytecode::Instr::LdC:
                            s << "ld";
                            break;

                        case bytecode::Instr::LdaG:
                        case bytecode::Instr::LdaL:
                        case bytecode::Instr::LdaA:
                        case bytecode::Instr::LdaC:
                            s << "lda";
                            break;

                        case bytecode::Instr::StG:
                        case bytecode::Instr::StL:
                        case bytecode::Instr::StA:
                        case bytecode::Instr::StC:
                            s << "st";
                            break;

                        case bytecode::Instr::CjmpZ:
                            s << "cjmpz";
                            break;

                        case bytecode::Instr::CjmpNz:
                            s << "cjmpnz";
                            break;

                        case bytecode::Instr::Begin:
                            s << "begin";
                            break;

                        case bytecode::Instr::Cbegin:
                            s << "cbegin";
                            break;

                        case bytecode::Instr::Closure:
                            s << "closure";
                            break;

                        case bytecode::Instr::CallC:
                            s << "callc";
                            break;

                        case bytecode::Instr::Call:
                            s << "call";
                            break;

                        case bytecode::Instr::Tag:
                            s << "tag";
                            break;

                        case bytecode::Instr::Array:
                            s << "array";
                            break;

                        case bytecode::Instr::Fail:
                            s << "fail";
                            break;

                        case bytecode::Instr::Line:
                            s << "line";
                            break;

                        case bytecode::Instr::PattEqStr:
                            s << "patt =str";
                            break;

                        case bytecode::Instr::PattString:
                            s << "patt #str";
                            break;

                        case bytecode::Instr::PattArray:
                            s << "patt #array";
                            break;

                        case bytecode::Instr::PattSexp:
                            s << "patt #sexp";
                            break;

                        case bytecode::Instr::PattRef:
                            s << "patt #ref";
                            break;

                        case bytecode::Instr::PattVal:
                            s << "patt #val";
                            break;

                        case bytecode::Instr::PattFun:
                            s << "patt #fun";
                            break;

                        case bytecode::Instr::CallLread:
                            s << "call Lread";
                            break;

                        case bytecode::Instr::CallLwrite:
                            s << "call Lwrite";
                            break;

                        case bytecode::Instr::CallLlength:
                            s << "call Llength";
                            break;

                        case bytecode::Instr::CallLstring:
                            s << "call Lstring";
                            break;

                        case bytecode::Instr::CallBarray:
                            s << "call Barray";
                            break;

                        case bytecode::Instr::Eof:
                            s << "<eof>";
                            break;

                        default:
                            std::print(s, "[illop {:#02x}]", static_cast<uint8_t>(start.opcode));
                            break;
                        }
                    },

                    [&](const decode::InstrEnd &end) { s << opts.instr_term; },

                    [&](const decode::Imm32 &imm) { s << " " << imm.imm; },

                    [&](const decode::ImmVarspec &imm) {
                        s << " ";

                        switch (imm.kind) {
                        case decode::ImmVarspec::VarKind::Global:
                            s << "G(";
                            break;

                        case decode::ImmVarspec::VarKind::Local:
                            s << "L(";
                            break;

                        case decode::ImmVarspec::VarKind::Param:
                            s << "A(";
                            break;

                        case decode::ImmVarspec::VarKind::Capture:
                            s << "C(";
                            break;
                        }

                        s << imm.idx << ")";
                    },

                    [&](const decode::Error &err) { std::print(s, " [error: {}]", err.msg); },
                },
                result
            );
        });
    }
}
