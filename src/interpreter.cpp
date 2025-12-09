#include "interpreter.hpp"

#include "config.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime.hpp"
#include "src/util.hpp"
#include "src/verifier.hpp"

using namespace friar::interpreter;
using namespace friar;

using bytecode::Instr;

extern "C" void *__gc_stack_top; // NOLINT(bugprone-reserved-identifier)
extern "C" void *__gc_stack_bottom; // NOLINT(bugprone-reserved-identifier)

namespace {

constexpr uint32_t max_stack_size = 0x7fff'ffffU;

class UniqueRunnerGuard {
public:
    UniqueRunnerGuard() {
        if (running.exchange(true)) {
            throw std::runtime_error("detected multiple concurrent interpreter instances");
        }
    }

    ~UniqueRunnerGuard() noexcept {
        running = false;
    }

private:
    static std::atomic<bool> running;
};

std::atomic<bool> UniqueRunnerGuard::running = false;

class GcGuard {
public:
    GcGuard() noexcept {
        __init();
    }

    ~GcGuard() noexcept {
        __shutdown();
    }
};

constexpr auint unboxed_contents = static_cast<auint>(-1) >> 1;

class ValuePtr;

class Value {
public:
    static Value from_repr(auint repr) {
        return Value(repr);
    }

    static Value from_int(aint v) {
        // clear the two high bits and shift left by 1.
        auto masked = static_cast<auint>(v) & (unboxed_contents >> 1);
        auto shifted = masked << 1;

        if (v < 0) {
            // restore the sign bit.
            shifted |= static_cast<auint>(1) << (sizeof(auint) * 8 - 1);
        };

        return Value(shifted | 1);
    }

    static Value from_int(auint v) {
        return Value(BOX(v));
    }

    static Value from_bool(bool v) {
        return Value(v ? BOX(1) : BOX(0));
    }

    static Value from_ptr(void *p) {
        return Value(reinterpret_cast<auint>(p));
    }

    constexpr Value() noexcept = default;

    auint to_repr() const noexcept {
        return repr_;
    }

    aint get_aint() const noexcept {
        return static_cast<aint>(repr_) >> 1;
    }

    auint get_auint() const noexcept {
        return repr_ >> 1;
    }

    void *get_ptr() const noexcept {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        return reinterpret_cast<void *>(repr_);
    }

    bool is_int() const noexcept {
        return UNBOXED(repr_);
    }

    bool is_boxed() const noexcept {
        return !UNBOXED(repr_);
    }

    lama_type get_type() const noexcept {
        return get_type_header_ptr(get_obj_header_ptr(get_ptr()));
    }

    bool is_closure() const noexcept {
        return is_boxed() && get_type() == CLOSURE;
    }

    bool is_sexp() const noexcept {
        return is_boxed() && get_type() == SEXP;
    }

    bool is_string() const noexcept {
        return is_boxed() && get_type() == STRING;
    }

    bool is_array() const noexcept {
        return is_boxed() && get_type() == ARRAY;
    }

    bool is_aggregate() const noexcept {
        if (!is_boxed()) {
            return false;
        }

        switch (get_type()) {
        case ARRAY:
        case STRING:
        case SEXP:
            return true;

        case CLOSURE:
            return false;
        }

        std::unreachable();
    }

    ValuePtr field(size_t idx) const noexcept;

    data *to_data() const noexcept {
        return TO_DATA(get_ptr());
    }

    sexp *to_sexp() const noexcept {
        return TO_SEXP(get_ptr());
    }

    auint len() const noexcept {
        return LEN(to_data()->data_header);
    }

    std::string_view type_to_string() const noexcept {
        if (is_int()) {
            return "integer";
        }

        switch (get_type()) {
        case ARRAY:
            return "array";

        case CLOSURE:
            return "function";

        case STRING:
            return "string";

        case SEXP:
            return "sexp";
        }

        std::unreachable();
    }

    std::string stringify() const noexcept {
        std::ostringstream s;
        stringify_to(s);

        return std::move(s).str();
    }

    void stringify_to(std::ostream &s) const noexcept;

private:
    explicit Value(auint v) noexcept : repr_(v) {}

    auint repr_ = BOX(0);
};

class ValuePtr {
public:
    ValuePtr() = default;

    explicit ValuePtr(auint *ptr) : ptr_(ptr) {}

    ValuePtr(const ValuePtr &other) = default;
    ValuePtr &operator=(const ValuePtr &other) = default;

    const ValuePtr &operator=(Value v) const noexcept {
        set(v);

        return *this;
    }

    Value get() const noexcept {
        return Value::from_repr(*ptr_);
    }

    operator Value() const noexcept {
        return get();
    }

    Value operator*() const noexcept {
        return get();
    }

    void set(Value v) const noexcept {
        *ptr_ = v.to_repr();
    }

    auint *ptr() const noexcept {
        return ptr_;
    }

private:
    auint *ptr_ = nullptr;
};

ValuePtr get_object_field(void *contents, size_t idx) noexcept {
    return ValuePtr(static_cast<auint *>(contents) + idx);
}

ValuePtr get_object_field(data *p, size_t idx) noexcept {
    return ValuePtr(reinterpret_cast<auint *>(p->contents) + idx);
}

ValuePtr get_sexp_field(sexp *p, size_t idx) noexcept {
    return ValuePtr(reinterpret_cast<auint *>(p->contents) + idx);
}

ValuePtr Value::field(size_t idx) const noexcept {
    return get_object_field(get_ptr(), idx);
}

void Value::stringify_to(std::ostream &s) const noexcept {
    if (is_int()) {
        s << get_aint();
    } else {
        switch (get_type()) {
        case ARRAY: {
            auto n = len();
            s << "[";

            for (size_t i = 0; i < n; ++i) {
                if (i > 0) {
                    s << ", ";
                }

                field(i).get().stringify_to(s);
            }

            s << "]";

            break;
        }

        case CLOSURE:
            s << "<function>";
            break;

        case STRING:
            s << '"' << to_data()->contents << '"';
            break;

        case SEXP:
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            s << reinterpret_cast<const char *>(to_sexp()->tag);
            auto n = len();

            if (n > 0) {
                s << " (";

                for (size_t i = 0; i < n; ++i) {
                    if (i > 0) {
                        s << ", ";
                    }

                    get_sexp_field(to_sexp(), i).get().stringify_to(s);
                }

                s << ")";
            }

            break;
        }
    }
}

} // namespace

Interpreter::Interpreter(
    bytecode::Module &mod,
#ifndef DYNAMIC_VERIFICATION
    const verifier::ModuleInfo &info,
#endif
    std::istream &input,
    std::ostream &output
)
    : mod_(mod),
#ifndef DYNAMIC_VERIFICATION
      info_(info),
#endif
      input_(input), output_(output) {
}

#ifdef DYNAMIC_VERIFICATION
template<class T>
using DynamicExpected = std::expected<T, Interpreter::Error>;
#else
template<class T>
using DynamicExpected = T;
#endif

// NOLINTNEXTLINE(readability-function-cognitive-complexity, readability-function-size)
std::expected<void, Interpreter::Error> Interpreter::run() {
    UniqueRunnerGuard _unique_guard;

    std::vector<Frame> frames;
    std::vector<auint> stack;
    std::span<const Instr> bc = mod_.bytecode;

    // globals + 2 dummy `main` arguments.
    stack.resize(mod_.global_count + 2, BOX(0));

    // per-frame registers.
    uint32_t pc = -1;
    uint32_t args = 2; // `main` takes 2 arguments.
    uint32_t base = mod_.global_count + args;

#ifdef DYNAMIC_VERIFICATION
    uint32_t locals = 0;
#endif

    // initialize the GC (use a virtual stack).
    __gc_stack_top = static_cast<void *>(stack.data());
    __gc_stack_bottom = static_cast<void *>(stack.data() + base);
    GcGuard _gc_guard;

    auto backtrace = [&] {
        Backtrace result;
        auto current_frame_pc = pc;

        for (auto it = frames.rbegin(); it != frames.rend();
             current_frame_pc = it->saved_pc, ++it) {

            Backtrace::UserFrame frame;
            frame.file = mod_.name;
            frame.proc_addr = it->proc_addr;
            frame.line = it->line;
            frame.pc = current_frame_pc;
            result.entries.emplace_back(std::move(frame));
        }

        return result;
    };

    auto make_error = [&]<class... Args>(std::format_string<Args...> s, Args &&...args) {
        return Error{
            .backtrace = backtrace(),
            .msg = std::format(s, std::forward<Args>(args)...),
        };
    };

    auto read_u32_at = [&](uint32_t addr, bool allow_neg = false) -> DynamicExpected<uint32_t> {
#ifdef DYNAMIC_VERIFICATION
        if (addr + 4 > bc.size_bytes()) {
            return std::unexpected(make_error(
                "trying to read a 32-bit immediate at {:#x} would go beyond the size of the bytes "
                "({:#x})",
                addr,
                bc.size_bytes()
            ));
        }
#endif
        std::span<const std::byte, 4> bytes(std::as_bytes(bc.subspan(addr, 4)));
        auto result = util::from_u32_le(bytes);

#ifdef DYNAMIC_VERIFICATION
        if (!allow_neg && result >> 31) {
            return std::unexpected(
                make_error("the 32-bit immediate {:#x} at {:#x} is too large", result, addr)
            );
        }
#endif

        return result;
    };

    auto read_u32 = [&](bool allow_neg = false) {
        auto result = read_u32_at(pc, allow_neg);

#ifdef DYNAMIC_VERIFICATION
        if (result) {
#endif
            pc += sizeof(uint32_t);
#ifdef DYNAMIC_VERIFICATION
        }
#endif

        return result;
    };

    auto stack_size = [&] -> size_t {
        return static_cast<auint *>(__gc_stack_bottom) - static_cast<auint *>(__gc_stack_top);
    };

    auto top_nth = [&](auto n) -> DynamicExpected<ValuePtr> {
#ifdef DYNAMIC_VERIFICATION
        if (n > 0 && static_cast<size_t>(n) >= stack_size()) {
            return std::unexpected(make_error(
                "trying to access stack value #{}, "
                "which is out of range for the stack of size {}",
                n,
                stack_size()
            ));
        }
#endif

        return ValuePtr(static_cast<auint *>(__gc_stack_bottom) - static_cast<ptrdiff_t>(n + 1));
    };

    auto pop_n = [&](size_t n) -> DynamicExpected<void> {
#ifdef DYNAMIC_VERIFICATION
        if (n > stack_size()) {
            return std::unexpected(make_error(
                "trying to pop {} stack values, "
                "which is out of range for the stack of size {}",
                n,
                stack_size()
            ));
        }
#endif

        __gc_stack_bottom = static_cast<void *>(static_cast<auint *>(__gc_stack_bottom) - n);

#ifdef DYNAMIC_VERIFICATION
        return {};
#endif
    };

    auto push = [&](Value v) -> DynamicExpected<void> {
#ifdef DYNAMIC_VERIFICATION
        auto new_size = stack_size() + 1;

        if (new_size > max_stack_size) [[unlikely]] {
            return std::unexpected(make_error("stack overflow"));
        }

        if (new_size > stack.size()) {
            stack.push_back(v.to_repr());

            __gc_stack_top = static_cast<void *>(stack.data());
            __gc_stack_bottom = static_cast<void *>(stack.data() + new_size);
        } else {
            *top_nth(-1) = v;
            __gc_stack_bottom = static_cast<void *>(static_cast<auint *>(__gc_stack_bottom) + 1);
        }
#else
        top_nth(-1) = v;
        __gc_stack_bottom = static_cast<void *>(static_cast<auint *>(__gc_stack_bottom) + 1);
#endif

#ifdef DYNAMIC_VERIFICATION
        return {};
#endif
    };

    auto global = [&](uint32_t m) -> DynamicExpected<ValuePtr> {
#ifdef DYNAMIC_VERIFICATION
        if (m >= mod_.global_count) {
            return std::unexpected(make_error(
                "trying to access global #{}, but there are only {} globals declared",
                m,
                mod_.global_count
            ));
        }
#endif

        return ValuePtr(static_cast<auint *>(__gc_stack_top) + m);
    };

    auto local = [&](uint32_t m) -> DynamicExpected<ValuePtr> {
#ifdef DYNAMIC_VERIFICATION
        if (m >= locals) {
            return std::unexpected(make_error(
                "trying to access local #{}, but there are only {} locals declared", m, locals
            ));
        }
#endif

        return ValuePtr(static_cast<auint *>(__gc_stack_top) + base + m);
    };

    auto arg = [&](uint32_t m) -> DynamicExpected<ValuePtr> {
#ifdef DYNAMIC_VERIFICATION
        if (m >= args) {
            return std::unexpected(make_error(
                "trying to access argument #{}, but there are only {} arguments", m, args
            ));
        }
#endif

        return ValuePtr(static_cast<auint *>(__gc_stack_top) + base - args + m);
    };

    auto capture = [&](uint32_t m) -> DynamicExpected<ValuePtr> {
#ifdef DYNAMIC_VERIFICATION
        if (!frames.back().is_closure) {
            return std::unexpected(make_error(
                "trying to access a captured variable "
                "when there's no closure associated with the frame"
            ));
        }
#endif

        auto closure = Value::from_repr(static_cast<auint *>(__gc_stack_top)[base - args - 1]);

#ifdef DYNAMIC_VERIFICATION
        if (auto len = closure.len() - 1; m >= len) {
            return std::unexpected(make_error(
                "trying to access capture #{}, "
                "but there are only {} variables captured by the closure",
                m,
                len
            ));
        }
#endif

        return closure.field(m + 1);
    };

#ifdef DYNAMIC_VERIFICATION
    auto check_strtab = [&](uint32_t s) -> DynamicExpected<std::string_view> {
        if (s >= mod_.strtab.size()) {
            return std::unexpected(make_error(
                "string table offset {:#x} is out of range for the string table of size {:#x}",
                s,
                mod_.strtab.size()
            ));
        }

        auto it = std::find(mod_.strtab.begin() + s, mod_.strtab.end(), '\0');

        if (it == mod_.strtab.end()) {
            return std::unexpected(
                make_error("string starting at {:#x} in the string table is not NUL-terminated", s)
            );
        }

        return std::string_view(mod_.strtab.begin() + s, it);
    };

    auto check_jmp = [&](uint32_t l) -> DynamicExpected<void> {
        if (l >= bc.size()) {
            return std::unexpected(make_error(
                "address {:#x} points outside the bytecode section of size {:#x}", l, bc.size()
            ));
        }

        switch (bc[l]) {
        case Instr::Begin:
        case Instr::Cbegin:
            return std::unexpected(make_error("address {:#x} must not point to BEGIN/CBEGIN", l));

        default:
            break;
        }

        return {};
    };

    auto check_begin = [&](uint32_t l) -> DynamicExpected<void> {
        if (l >= bc.size()) {
            return std::unexpected(make_error(
                "address {:#x} points outside the bytecode section of size {:#x}", l, bc.size()
            ));
        }

        switch (auto op = bc[l]) {
        case Instr::Begin:
        case Instr::Cbegin:
            break;

        default:
            return std::unexpected(make_error(
                "address {:#x} must point to BEGIN/CBEGIN, got {:#02x}", l, static_cast<uint8_t>(op)
            ));
        }

        size_t op_size = 1 + 2 * sizeof(uint32_t);

        if (l + op_size > bc.size()) {
            return std::unexpected(
                make_error("address {:#x} must point to a valid BEGIN/CBEGIN instruction", l)
            );
        }

        return {};
    };
#else
    auto check_strtab = [&](uint32_t s) { return mod_.strtab_entry_at(s); };
    auto check_jmp = [](uint32_t l) {};
    auto check_begin = [](uint32_t l) {};
#endif

    // the address to call.
    uint32_t call_target = 0;
    bool call_closure = false;

#ifdef DYNAMIC_VERIFICATION
    bool is_main = true;
#endif

enter_frame:
    frames.push_back(
        Frame{
            .proc_addr = call_target,
            .saved_pc = pc,
            .saved_base = base,
            .saved_args = args,

#ifdef DYNAMIC_VERIFICATION
            .saved_locals = locals,
#endif

            .is_closure = call_closure,
        }
    );

    pc = call_target;

#ifdef DYNAMIC_VERIFICATION
    switch (bc[pc]) {
    case Instr::Begin:
    case Instr::Cbegin:
        break;

    default:
        return std::unexpected(make_error(
            "expected BEGIN or CBEGIN at {:#x}, got {:#02x}", pc, static_cast<uint8_t>(bc[pc])
        ));
    }
#endif

    while (true) {
#if INTERPRETER_TRACE
        std::print(std::cerr, "[{:#x}] op = {:#02x} ", pc, uint8_t(bc[pc]));

#if INTERPRETER_TRACE >= 2
        std::print(std::cerr, "stack = [");

        for (size_t i = 0; i < stack.size(); ++i) {
            if (__gc_stack_bottom == stack.data() + i) {
                if (i == 0) {
                    std::print(std::cerr, "| ");
                } else {
                    std::print(std::cerr, " | ");
                }
            } else if (i > 0) {
                std::print(std::cerr, ", ");
            }

            std::print(std::cerr, "{:#x}", stack[i]);
            std::print(
                std::cerr,
                " ({} `{}`)",
                Value::from_repr(stack[i]).type_to_string(),
                Value::from_repr(stack[i]).stringify()
            );
        }

        std::print(std::cerr, "]");
#else
        std::print(
            std::cerr,
            "stack height = {} ({} max, {} allocated)",
            static_cast<auint *>(__gc_stack_bottom) - static_cast<auint *>(__gc_stack_top),
            stack.size(),
            stack.capacity()
        );
#endif

        std::println(std::cerr, "");

#endif

#ifdef DYNAMIC_VERIFICATION

#define PROPAGATE_DYNEXP_T(T, V, EXPR)                                                             \
    T V;                                                                                           \
    do {                                                                                           \
        if (auto _r = (EXPR)) {                                                                    \
            V = *std::move(_r);                                                                    \
        } else {                                                                                   \
            return std::unexpected(std::move(_r).error());                                         \
        }                                                                                          \
    } while (false)

#define PROPAGATE_DYNEXP(V, EXPR) PROPAGATE_DYNEXP_T(decltype((EXPR))::value_type, V, EXPR)

#define PROPAGATE_DYNEXP_VOID(EXPR)                                                                \
    do {                                                                                           \
        if (auto _r = (EXPR); !_r) {                                                               \
            return std::unexpected(std::move(_r).error());                                         \
        }                                                                                          \
    } while (false)

#else

#define PROPAGATE_DYNEXP(V, EXPR) auto V = (EXPR)
#define PROPAGATE_DYNEXP_T(T, V, EXPR) T V = (EXPR)
#define PROPAGATE_DYNEXP_VOID(EXPR) EXPR

#endif

#ifdef DYNAMIC_VERIFICATION
        if (pc >= bc.size()) {
            return std::unexpected(make_error(
                "the PC ({:#x}) is outside the bytecode section of size {:#x}", pc, bc.size()
            ));
        }
#endif

        switch (bc[pc++]) {
        case Instr::Add: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_auint();
                auto rhs = v0.get().get_auint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_int(lhs + rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot add {} and {}", v1.get().type_to_string(), v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Sub: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_auint();
                auto rhs = v0.get().get_auint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_int(lhs - rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot subtract {} and {}",
                    v1.get().type_to_string(),
                    v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Mul: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_auint();
                auto rhs = v0.get().get_auint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_int(lhs * rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot multiply {} and {}",
                    v1.get().type_to_string(),
                    v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Div: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_aint();
                auto rhs = v0.get().get_aint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));

                if (rhs == 0) [[unlikely]] {
                    return std::unexpected(make_error("division by zero"));
                }

                PROPAGATE_DYNEXP_VOID(push(Value::from_int(lhs / rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot divide {} and {}", v1.get().type_to_string(), v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Mod: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_aint();
                auto rhs = v0.get().get_aint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));

                if (rhs == 0) [[unlikely]] {
                    return std::unexpected(
                        make_error("division by zero while taking the remainder")
                    );
                }

                PROPAGATE_DYNEXP_VOID(push(Value::from_int(lhs % rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot take the remainder of {} and {}",
                    v1.get().type_to_string(),
                    v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Lt: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_aint();
                auto rhs = v0.get().get_aint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(lhs < rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}", v1.get().type_to_string(), v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Le: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_aint();
                auto rhs = v0.get().get_aint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(lhs <= rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}", v1.get().type_to_string(), v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Gt: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_aint();
                auto rhs = v0.get().get_aint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(lhs > rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}", v1.get().type_to_string(), v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Ge: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_aint();
                auto rhs = v0.get().get_aint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(lhs >= rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}", v1.get().type_to_string(), v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Eq: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_aint();
                auto rhs = v0.get().get_aint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(lhs == rhs)));
            } else if (v1.get().is_int() || v0.get().is_int()) {
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(false)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}", v1.get().type_to_string(), v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Ne: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_aint();
                auto rhs = v0.get().get_aint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(lhs != rhs)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}", v1.get().type_to_string(), v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::And: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_auint();
                auto rhs = v0.get().get_auint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(lhs != 0 && rhs != 0)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot perform boolean AND for {} and {}",
                    v1.get().type_to_string(),
                    v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Or: {
            PROPAGATE_DYNEXP(v1, top_nth(1));
            PROPAGATE_DYNEXP(v0, top_nth(0));

            if (v1.get().is_int() && v0.get().is_int()) {
                auto lhs = v1.get().get_auint();
                auto rhs = v0.get().get_auint();
                PROPAGATE_DYNEXP_VOID(pop_n(2));
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(lhs != 0 || rhs != 0)));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot perform boolean OR for {} and {}",
                    v1.get().type_to_string(),
                    v0.get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Const: {
            PROPAGATE_DYNEXP(k, read_u32(true));
            PROPAGATE_DYNEXP_VOID(push(Value::from_int(static_cast<aint>(k))));

            break;
        }

        case Instr::String: {
            PROPAGATE_DYNEXP(s, read_u32());
            PROPAGATE_DYNEXP(sv, check_strtab(s));
            auto *v = get_object_content_ptr(alloc_string(sv.length()));
            PROPAGATE_DYNEXP_VOID(push(Value::from_ptr(v)));
            // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
            strcpy(TO_DATA(v)->contents, sv.data());

            break;
        }

        case Instr::Sexp: {
            PROPAGATE_DYNEXP(s, read_u32());
            PROPAGATE_DYNEXP(n, read_u32());
            PROPAGATE_DYNEXP(tag, check_strtab(s));
            auto *v = get_object_content_ptr(alloc_sexp(n));
            TO_SEXP(v)->tag = reinterpret_cast<auint>(tag.data());

            if (n > verifier::max_member_count) {
                return std::unexpected(make_error(
                    "too many sexp members: expected at most {}, got {}",
                    verifier::max_member_count,
                    n
                ));
            }

            for (size_t i = 0; i < n; ++i) {
                PROPAGATE_DYNEXP_T(Value, elem, top_nth(n - i - 1));
                get_sexp_field(TO_SEXP(v), i) = elem;
            }

            PROPAGATE_DYNEXP_VOID(pop_n(n));
            PROPAGATE_DYNEXP_VOID(push(Value::from_ptr(v)));

            break;
        }

        case Instr::Sta: {
            PROPAGATE_DYNEXP_T(Value, aggregate, top_nth(2));
            PROPAGATE_DYNEXP_T(Value, idx_v, top_nth(1));
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));

            if (!aggregate.is_aggregate()) [[unlikely]] {
                return std::unexpected(make_error("cannot index {}", aggregate.type_to_string()));
            }

            if (!idx_v.is_int()) [[unlikely]] {
                return std::unexpected(
                    make_error("index must be an integer, got {}", idx_v.type_to_string())
                );
            }

            auto idx = idx_v.get_aint();
            auto *aggregate_data = aggregate.to_data();

            if (aint len = static_cast<aint>(aggregate.len()); idx < 0 || idx >= len) [[unlikely]] {
                return std::unexpected(
                    make_error("index {} out of range for an aggregate of length {}", idx, len)
                );
            }

            switch (aggregate.get_type()) {
            case ARRAY:
                get_object_field(aggregate_data, static_cast<size_t>(idx)) = v;
                break;

            case STRING: {
                if (!v.is_int()) [[unlikely]] {
                    return std::unexpected(make_error(
                        "cannot assign {} at index {} into string (expected integer)",
                        v.type_to_string(),
                        idx
                    ));
                }

                auto c = v.get_aint();

                if (c < 0 || c > 0xff) [[unlikely]] {
                    return std::unexpected(make_error(
                        "cannot assign {} at index {} into string: does not fit into a byte", c, idx
                    ));
                }

                aggregate_data->contents[idx] = static_cast<char>(c);

                break;
            }

            case SEXP:
                get_sexp_field(aggregate.to_sexp(), static_cast<size_t>(idx)) = v;
                break;

            default:
                std::unreachable();
            }

            PROPAGATE_DYNEXP_VOID(pop_n(3));
            PROPAGATE_DYNEXP_VOID(push(v));

            break;
        }

        case Instr::Jmp: {
            PROPAGATE_DYNEXP(l, read_u32());
            PROPAGATE_DYNEXP_VOID(check_jmp(l));
            pc = l;

            break;
        }

        case Instr::End:
        case Instr::Ret: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            auto &frame = frames.back();
            __gc_stack_bottom = static_cast<void *>(
                static_cast<auint *>(__gc_stack_top) + base - args - (frame.is_closure ? 1 : 0)
            );

            if (frame.saved_pc == -1U) [[unlikely]] {
                return {};
            }

            PROPAGATE_DYNEXP_VOID(push(v));
            pc = frame.saved_pc;
            base = frame.saved_base;
            args = frame.saved_args;

#ifdef DYNAMIC_VERIFICATION
            locals = frame.saved_locals;
#endif

            frames.pop_back();

            break;
        }

        case Instr::Drop: {
            PROPAGATE_DYNEXP_VOID(pop_n(1));

            break;
        }

        case Instr::Dup: {
            PROPAGATE_DYNEXP_VOID(push(*top_nth(0)));

            break;
        }

        case Instr::Swap: {
            PROPAGATE_DYNEXP_T(Value, lhs, top_nth(1));
            PROPAGATE_DYNEXP_T(Value, rhs, top_nth(0));
            PROPAGATE_DYNEXP_VOID(pop_n(2));
            PROPAGATE_DYNEXP_VOID(push(rhs));
            PROPAGATE_DYNEXP_VOID(push(lhs));
        }

        case Instr::Elem: {
            PROPAGATE_DYNEXP_T(Value, aggregate, top_nth(1));
            PROPAGATE_DYNEXP_T(Value, idx_v, top_nth(0));

            if (!aggregate.is_aggregate()) [[unlikely]] {
                return std::unexpected(make_error("cannot index {}", aggregate.type_to_string()));
            }

            if (!idx_v.is_int()) [[unlikely]] {
                return std::unexpected(
                    make_error("index must be an integer, got {}", idx_v.type_to_string())
                );
            }

            auto idx = idx_v.get_aint();
            auto *aggregate_data = aggregate.to_data();

            if (aint len = static_cast<aint>(aggregate.len()); idx < 0 || idx >= len) [[unlikely]] {
                return std::unexpected(
                    make_error("index {} out of range for an aggregate of length {}", idx, len)
                );
            }

            PROPAGATE_DYNEXP_VOID(pop_n(2));

            switch (aggregate.get_type()) {
            case ARRAY:
                PROPAGATE_DYNEXP_VOID(
                    push(get_object_field(aggregate_data, static_cast<size_t>(idx)))
                );
                break;

            case STRING:
                PROPAGATE_DYNEXP_VOID(
                    push(Value::from_int(static_cast<auint>(aggregate_data->contents[idx])))
                );
                break;

            case SEXP:
                PROPAGATE_DYNEXP_VOID(
                    push(get_sexp_field(aggregate.to_sexp(), static_cast<size_t>(idx)))
                );
                break;

            default:
                std::unreachable();
            }

            break;
        }

        case Instr::LdG: {
            PROPAGATE_DYNEXP(m, read_u32());
            PROPAGATE_DYNEXP_T(Value, v, global(m));
            PROPAGATE_DYNEXP_VOID(push(v));

            break;
        }

        case Instr::LdL: {
            PROPAGATE_DYNEXP(m, read_u32());
            PROPAGATE_DYNEXP_T(Value, v, local(m));
            PROPAGATE_DYNEXP_VOID(push(v));

            break;
        }

        case Instr::LdA: {
            PROPAGATE_DYNEXP(m, read_u32());
            PROPAGATE_DYNEXP_T(Value, v, arg(m));
            PROPAGATE_DYNEXP_VOID(push(v));

            break;
        }

        case Instr::LdC: {
            PROPAGATE_DYNEXP(m, read_u32());
            PROPAGATE_DYNEXP_T(Value, v, capture(m));
            PROPAGATE_DYNEXP_VOID(push(v));

            break;
        }

        case Instr::StG: {
            PROPAGATE_DYNEXP(m, read_u32());
            PROPAGATE_DYNEXP(g, global(m));
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            g = v;

            break;
        }

        case Instr::StL: {
            PROPAGATE_DYNEXP(m, read_u32());
            PROPAGATE_DYNEXP(l, local(m));
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            l = v;

            break;
        }

        case Instr::StA: {
            PROPAGATE_DYNEXP(m, read_u32());
            PROPAGATE_DYNEXP(a, arg(m));
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            a = v;

            break;
        }

        case Instr::StC: {
            PROPAGATE_DYNEXP(m, read_u32());
            PROPAGATE_DYNEXP(c, capture(m));
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            c = v;

            break;
        }

        case Instr::CjmpZ: {
            PROPAGATE_DYNEXP(l, read_u32());
            PROPAGATE_DYNEXP_VOID(check_jmp(l));
            PROPAGATE_DYNEXP_T(Value, cond, top_nth(0));

            if (!cond.is_int()) [[unlikely]] {
                return std::unexpected(make_error(
                    "wrong branch condition type: expected integer, got {}", cond.type_to_string()
                ));
            }

            if (cond.get_auint() == 0) {
                pc = l;
            }

            PROPAGATE_DYNEXP_VOID(pop_n(1));

            break;
        }

        case Instr::CjmpNz: {
            PROPAGATE_DYNEXP(l, read_u32());
            PROPAGATE_DYNEXP_VOID(check_jmp(l));
            PROPAGATE_DYNEXP_T(Value, cond, top_nth(0));

            if (!cond.is_int()) [[unlikely]] {
                return std::unexpected(make_error(
                    "wrong branch condition type: expected integer, got {}", cond.type_to_string()
                ));
            }

            if (cond.get_auint() != 0) {
                pc = l;
            }

            PROPAGATE_DYNEXP_VOID(pop_n(1));

            break;
        }

        case Instr::Begin:
        case Instr::Cbegin: {
            PROPAGATE_DYNEXP(params, read_u32());
            PROPAGATE_DYNEXP(local_imm, read_u32());

#ifdef DYNAMIC_VERIFICATION
            locals = local_imm;
#else
            auto locals = local_imm;
#endif

#ifdef DYNAMIC_VERIFICATION
            if (params > verifier::max_param_count) {
                return std::unexpected(make_error(
                    "too many parameters: expected at most {}, got {}",
                    verifier::max_param_count,
                    params
                ));
            }

            if (is_main) {
                if (params != 2) {
                    return std::unexpected(
                        make_error("the main procedure must have 2 parameters, got {}", params)
                    );
                }

                if (bc[pc - 1] == Instr::Cbegin) {
                    return std::unexpected(
                        make_error("the main procedure must be declared with BEGIN")
                    );
                }
            }
#endif
            uint32_t proc_stack_size = params >> 16;
            params &= 0xffff;

            base = stack_size();
            auto new_size = static_cast<uint64_t>(base) + locals + proc_stack_size;

            if (new_size > max_stack_size) [[unlikely]] {
                return std::unexpected(make_error("stack overflow"));
            }

            if (stack.size() < new_size) {
                stack.resize(new_size, BOX(0));
            }

            args = params;
            __gc_stack_top = static_cast<void *>(stack.data());
            __gc_stack_bottom = static_cast<void *>(stack.data() + base + locals);

#if INTERPRETER_TRACE
            std::println(
                std::cerr,
                "calling {:#x} ({}{} args, {} locals, {} values pre-allocated)",
                call_target,
                args,
                frames.back().is_closure ? " + 1" : "",
                locals,
                proc_stack_size
            );
#endif

            break;
        }

        case Instr::Closure: {
            PROPAGATE_DYNEXP(l, read_u32());
            PROPAGATE_DYNEXP_VOID(check_begin(l));
            PROPAGATE_DYNEXP(n, read_u32());
            auto *closure = get_object_content_ptr(alloc_closure(n + 1));
            PROPAGATE_DYNEXP_VOID(push(Value::from_ptr(closure)));
            get_object_field(closure, 0) = Value::from_int(static_cast<auint>(l));

            for (size_t i = 0; i < n; ++i) {
                auto kind = static_cast<uint8_t>(bc[pc++]);
                PROPAGATE_DYNEXP(m, read_u32());
                auto field = get_object_field(closure, i + 1);

                switch (kind) {
                case 0: {
                    PROPAGATE_DYNEXP_T(Value, v, global(m));
                    field = v;
                    break;
                }

                case 1: {
                    PROPAGATE_DYNEXP_T(Value, v, local(m));
                    field = v;
                    break;
                }

                case 2: {
                    PROPAGATE_DYNEXP_T(Value, v, arg(m));
                    field = v;
                    break;
                }

                case 3: {
                    PROPAGATE_DYNEXP_T(Value, v, capture(m));
                    field = v;
                    break;
                }

                default:
#ifdef DYNAMIC_VERIFICATION
                    return std::unexpected(
                        make_error("unknown variable kind encoding: {:#02x}", kind)
                    );
#else
                    std::unreachable();
#endif
                }
            }

            break;
        }

        case Instr::CallC: {
            PROPAGATE_DYNEXP(n, read_u32());
            PROPAGATE_DYNEXP_T(Value, closure, top_nth(n));

            if (!closure.is_closure()) [[unlikely]] {
                return std::unexpected(make_error("cannot call {}", closure.type_to_string()));
            }

            auto l = closure.field(0).get().get_auint();

            // read the low word of the first immediate: the high word stores the stack size.
            PROPAGATE_DYNEXP(params, read_u32_at(l + 1));
            params &= 0xffff;

            if (params != n) [[unlikely]] {
                return std::unexpected(
                    make_error("the function expected {} arguments, got {}", params, n)
                );
            }

            call_target = l;
            call_closure = true;

#ifdef DYNAMIC_VERIFICATION
            is_main = false;
#endif

            goto enter_frame;
        }

        case Instr::Call: {
            PROPAGATE_DYNEXP(l, read_u32());
            PROPAGATE_DYNEXP_VOID(check_begin(l));

#ifdef DYNAMIC_VERIFICATION
            if (bc[l] == Instr::Cbegin) {
                return std::unexpected(make_error(
                    "cannot call a CBEGIN-declared procedure at {:#x} "
                    "without creating a closure first",
                    l
                ));
            }

            PROPAGATE_DYNEXP(n, read_u32());

            // read the low word of the first immediate: the high word stores the stack size.
            PROPAGATE_DYNEXP(params, read_u32_at(l + 1));
            params &= 0xffff;

            if (params != n) [[unlikely]] {
                return std::unexpected(
                    make_error("the function expected {} arguments, got {}", params, n)
                );
            }
#else
            // read n.
            read_u32();
#endif

            call_target = l;
            call_closure = false;

#ifdef DYNAMIC_VERIFICATION
            is_main = false;
#endif

            goto enter_frame;
        }

        case Instr::Tag: {
            PROPAGATE_DYNEXP(s, read_u32());
            PROPAGATE_DYNEXP(n, read_u32());
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            PROPAGATE_DYNEXP(expected_tag, check_strtab(s));
            PROPAGATE_DYNEXP_VOID(pop_n(1));

            if (v.is_sexp()) {
                auto *sexp = v.to_sexp();
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                auto *actual_tag = reinterpret_cast<char *>(sexp->tag);

                PROPAGATE_DYNEXP_VOID(push(
                    Value::from_bool(LEN(sexp->data_header) == n && expected_tag == actual_tag)
                ));
            } else {
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(false)));
            }

            break;
        }

        case Instr::Array: {
            PROPAGATE_DYNEXP(n, read_u32());
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));

            PROPAGATE_DYNEXP_VOID(pop_n(1));

            if (v.is_array()) {
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(LEN(v.to_data()->data_header) == n)));
            } else {
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(false)));
            }

            break;
        }

        case Instr::Fail: {
            PROPAGATE_DYNEXP(ln, read_u32());
            PROPAGATE_DYNEXP(col, read_u32());
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            // the scrutinee.
            PROPAGATE_DYNEXP_VOID(pop_n(1));

            return std::unexpected(
                make_error("match failure for {} at L{}:{}", v.stringify(), ln, col)
            );
        }

        case Instr::Line: {
            PROPAGATE_DYNEXP(ln, read_u32());
            frames.back().line = ln;

            break;
        }

        case Instr::PattEqStr: {
            PROPAGATE_DYNEXP_T(Value, lhs, top_nth(1));
            PROPAGATE_DYNEXP_T(Value, rhs, top_nth(0));
            PROPAGATE_DYNEXP_VOID(pop_n(2));

            if (lhs.is_string() && rhs.is_string()) {
                PROPAGATE_DYNEXP_VOID(push(
                    Value::from_bool(strcmp(lhs.to_data()->contents, rhs.to_data()->contents) == 0)
                ));
            } else {
                PROPAGATE_DYNEXP_VOID(push(Value::from_bool(false)));
            }

            break;
        }

        case Instr::PattString: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            PROPAGATE_DYNEXP_VOID(pop_n(1));
            PROPAGATE_DYNEXP_VOID(push(Value::from_bool(v.is_string())));

            break;
        }

        case Instr::PattArray: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            PROPAGATE_DYNEXP_VOID(pop_n(1));
            PROPAGATE_DYNEXP_VOID(push(Value::from_bool(v.is_array())));

            break;
        }

        case Instr::PattSexp: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            PROPAGATE_DYNEXP_VOID(pop_n(1));
            PROPAGATE_DYNEXP_VOID(push(Value::from_bool(v.is_sexp())));

            break;
        }

        case Instr::PattRef: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            PROPAGATE_DYNEXP_VOID(pop_n(1));
            PROPAGATE_DYNEXP_VOID(push(Value::from_bool(v.is_boxed())));

            break;
        }

        case Instr::PattVal: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            PROPAGATE_DYNEXP_VOID(pop_n(1));
            PROPAGATE_DYNEXP_VOID(push(Value::from_bool(!v.is_boxed())));

            break;
        }

        case Instr::PattFun: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            PROPAGATE_DYNEXP_VOID(pop_n(1));
            PROPAGATE_DYNEXP_VOID(push(Value::from_bool(v.is_closure())));

            break;
        }

        case Instr::CallLread: {
            aint v = 0;
            output_ << " > " << std::flush;
            input_ >> v;
            PROPAGATE_DYNEXP_VOID(push(Value::from_int(v)));

            break;
        }

        case Instr::CallLwrite: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));

            if (!v.is_int()) {
                return std::unexpected(
                    make_error("cannot write {} (expected integer)", v.type_to_string())
                );
            }

            PROPAGATE_DYNEXP_VOID(pop_n(1));
            output_ << v.get_aint() << '\n';
            PROPAGATE_DYNEXP_VOID(push(Value()));

            break;
        }

        case Instr::CallLlength: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));

            if (!v.is_aggregate()) {
                return std::unexpected(
                    make_error("cannot get the length of {}", v.type_to_string())
                );
            }

            aint len = static_cast<aint>(v.len());
            PROPAGATE_DYNEXP_VOID(pop_n(1));
            PROPAGATE_DYNEXP_VOID(push(Value::from_int(len)));

            break;
        }

        case Instr::CallLstring: {
            PROPAGATE_DYNEXP_T(Value, v, top_nth(0));
            auto s = v.stringify();
            auto *r = get_object_content_ptr(alloc_string(s.size()));
            PROPAGATE_DYNEXP_VOID(pop_n(1));
            PROPAGATE_DYNEXP_VOID(push(Value::from_ptr(r)));
            // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
            strcpy(TO_DATA(r)->contents, s.data());

            break;
        }

        case Instr::CallBarray: {
            PROPAGATE_DYNEXP(n, read_u32());

            if (n > verifier::max_elem_count) [[unlikely]] {
                return std::unexpected(make_error(
                    "too many array elements: expected at most {}, got {}",
                    verifier::max_elem_count,
                    n
                ));
            }

            auto *v = get_object_content_ptr(alloc_array(n));

            for (size_t i = 0; i < n; ++i) {
                PROPAGATE_DYNEXP_T(Value, elem, top_nth(n - i - 1));
                get_object_field(v, i) = elem;
            }

            PROPAGATE_DYNEXP_VOID(pop_n(n));
            PROPAGATE_DYNEXP_VOID(push(Value::from_ptr(v)));

            break;
        }

        case Instr::Sti: // the STI/LDA instructions are never emitted by the Lama compiler.
        case Instr::LdaG:
        case Instr::LdaL:
        case Instr::LdaA:
        case Instr::LdaC:
        case Instr::Eof:
        default:
#ifdef DYNAMIC_VERIFICATION
            return std::unexpected(make_error(
                "illegal operation at {:#x}: {:#02x}", pc - 1, static_cast<uint8_t>(bc[pc - 1])
            ));
#else
            std::unreachable();
#endif
        }

#ifdef DYNAMIC_VERIFICATION
        PROPAGATE_DYNEXP_VOID(check_jmp(pc));
#endif
    }
}
