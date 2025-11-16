#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace friar::bytecode {

/// An enumeration of instruction opcodes.
enum class Instr : uint8_t {
    Add = 0x01, // `BINOP +`.
    Sub = 0x02, // `BINOP -`.
    Mul = 0x03, // `BINOP *`.
    Div = 0x04, // `BINOP /`.
    Mod = 0x05, // `BINOP %`.
    Lt = 0x06, // `BINOP <`.
    Le = 0x07, // `BINOP <=`.
    Gt = 0x08, // `BINOP >`.
    Ge = 0x09, // `BINOP >=`.
    Eq = 0x0a, // `BINOP ==`.
    Ne = 0x0b, // `BINOP !=`.
    And = 0x0c, // `BINOP &&`.
    Or = 0x0d, // `BINOP !!`.

    Const = 0x10, // `CONST k`.
    String = 0x11, // `STRING s`.
    Sexp = 0x12, // `SEXP s n`.
    Sti = 0x13, // `STI`.
    Sta = 0x14, // `STA`.
    Jmp = 0x15, // `JMP l`.
    End = 0x16, // `END`.
    Ret = 0x17, // `RET`.
    Drop = 0x18, // `DROP`.
    Dup = 0x19, // `DUP`.
    Swap = 0x1a, // `SWAP`.
    Elem = 0x1b, // `ELEM`.

    LdG = 0x20, // `LD G(m)`.
    LdL = 0x21, // `LD L(m)`.
    LdA = 0x22, // `LD A(m)`.
    LdC = 0x23, // `LD C(m)`.
    LdaG = 0x30, // `LDA G(m)`.
    LdaL = 0x31, // `LDA L(m)`.
    LdaA = 0x32, // `LDA A(m)`.
    LdaC = 0x33, // `LDA C(m)`.
    StG = 0x40, // `ST G(m)`.
    StL = 0x41, // `ST L(m)`.
    StA = 0x42, // `ST A(m)`.
    StC = 0x43, // `ST C(m)`.

    CjmpZ = 0x50, // `CJMPz l`.
    CjmpNz = 0x51, // `CJMPnz`.
    Begin = 0x52, // `BEGIN a n`.
    Cbegin = 0x53, // `CBEGIN a n`.
    Closure = 0x54, // `CLOSURE l n V(m)...`.
    CallC = 0x55, // `CALLC n`.
    Call = 0x56, // `CALL l n`.
    Tag = 0x57, // `TAG s n`.
    Array = 0x58, // `ARRAY n`.
    Fail = 0x59, // `FAIL ln col`.
    Line = 0x5a, // `LINE ln`.

    PattEqStr = 0x60, // `PATT =str`.
    PattString = 0x61, // `PATT #string`.
    PattArray = 0x62, // `PATT #array`.
    PattSexp = 0x63, // `PATT #sexp`.
    PattRef = 0x64, // `PATT #ref`.
    PattVal = 0x65, // `PATT #val`.
    PattFun = 0x66, // `PATT #fun`.

    CallLread = 0x70, // `CALL Lread`.
    CallLwrite = 0x71, // `CALL Lwrite`.
    CallLlength = 0x72, // `CALL Llength`.
    CallLstring = 0x73, // `CALL Lstring`.
    CallBarray = 0x74, // `CALL Barray`.

    Eof = 0xff, // End-of-file marker.
};

/// A public symbol declaration.
struct Sym {
    /// A byte offset in the file where this symbol is defined.
    size_t offset = 0;

    /// An address in the bytecode.
    uint32_t address = 0;

    /// The name associated with this symbol; stored as an offset into the string table.
    uint32_t name = 0;
};

/// A Lama bytecode module.
struct Module {
    /// The name of the module.
    std::string name;

    /// The number of globals used by the module.
    uint32_t global_count = 0;

    /// The symbol table.
    std::vector<Sym> symtab;

    /// The symbol table, represented as a map.
    ///
    /// Initialized during module verification.
    std::unordered_map<std::string_view, uint32_t> symtab_map;

    /// The string table.
    std::vector<char> strtab;

    /// The offset of the bytecode section in the file.
    uint32_t bytecode_offset = 0;

    /// The program bytecode (includes the end-of-file marker).
    std::vector<Instr> bytecode;

    std::string_view strtab_entry_at(uint32_t offset) {
        return &strtab.at(offset);
    }
};

} // namespace friar::bytecode
