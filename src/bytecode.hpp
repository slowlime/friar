#pragma once

#include <cstdint>
#include <string>
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

    Ld = 0x20, // `LD V(m)`.
    Lda = 0x30, // `LDA V(m)`.
    St = 0x40, // `ST V(m)`.

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
    /// An address in the bytecode.
    uint32_t address = 0;

    /// The name associated with this symbol; stored as an offset into the string table.
    uint32_t name_offset = 0;
};

/// A Lama bytecode module.
struct Module {
    /// The name of the module.
    std::string name;

    /// The number of globals used by the module.
    uint32_t global_count = 0;

    /// The symbol table.
    std::vector<Sym> symtab;

    /// The string table.
    std::vector<char> strtab;

    /// The program bytecode (does not include the end-of-file marker).
    std::vector<Instr> bytecode;
};

} // namespace friar::bytecode
