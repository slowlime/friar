#include "interpreter.hpp"

#include "runtime.hpp"
#include <atomic>
#include <cstddef>
#include <cstring>
#include <format>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using namespace friar::interpreter;
using namespace friar;

using bytecode::Instr;

extern void *__gc_stack_top; // NOLINT(bugprone-reserved-identifier)
extern void *__gc_stack_bottom; // NOLINT(bugprone-reserved-identifier)

// dunno what these are intended for, but it doesn't like otherwise.
extern const size_t __start_custom_data = 0; // NOLINT(bugprone-reserved-identifier)
extern const size_t __stop_custom_data = 0; // NOLINT(bugprone-reserved-identifier)

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

constexpr auint unboxed_contents = static_cast<auint>(-1) >> 1;

class ValuePtr;

class Value {
public:
    static Value from_boxed_ptr(void *p) {
        return Value(reinterpret_cast<auint>(p));
    }

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

        return Value(shifted);
    }

    static Value from_int(auint v) {
        return Value(v << 1);
    }

    static Value from_bool(bool v) {
        return Value(v ? 1 : 0);
    }

    static Value box(void *v) {
        return Value(BOX(v));
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

    void *unbox() const noexcept {
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        return reinterpret_cast<void *>(UNBOX(repr_));
    }

    bool is_int() const noexcept {
        return UNBOXED(repr_);
    }

    bool is_boxed() const noexcept {
        return !UNBOXED(repr_);
    }

    lama_type get_type() const noexcept {
        return get_type_header_ptr(unbox());
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
        return TO_DATA(unbox());
    }

    sexp *to_sexp() const noexcept {
        return TO_SEXP(unbox());
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

    auint repr_ = 0;
};

class ValuePtr {
public:
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
    return get_object_field(unbox(), idx);
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
            s << to_data()->contents;
            break;

        case SEXP:
            s << "<sexp>";
            break;
        }
    }
}

} // namespace

Interpreter::Interpreter(
    bytecode::Module &mod,
    const verifier::ModuleInfo &info,
    std::istream &input,
    std::ostream &output
)
    : mod_(mod), info_(info), input_(input), output_(output) {}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::expected<void, Interpreter::Error> Interpreter::run() {
    UniqueRunnerGuard _guard;

    std::vector<Frame> frames;
    std::vector<auint> stack;
    const auto *bc = mod_.bytecode.data();

    // globals.
    stack.resize(mod_.global_count, 0);

    // per-frame registers.
    uint32_t pc = -1;
    uint32_t base = mod_.global_count;
    uint32_t args = 0;

    auto read_u32 = [&] {
        auto b0 = static_cast<uint32_t>(bc[pc++]);
        auto b1 = static_cast<uint32_t>(bc[pc++]);
        auto b2 = static_cast<uint32_t>(bc[pc++]);
        auto b3 = static_cast<uint32_t>(bc[pc++]);

        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    };

    auto top_nth = [&](size_t n) {
        return ValuePtr(static_cast<auint *>(__gc_stack_bottom) - static_cast<ptrdiff_t>(n + 1));
    };

    auto pop_n = [&](size_t n) {
        __gc_stack_bottom = static_cast<void *>(static_cast<auint *>(__gc_stack_bottom) - n);
    };

    auto push = [&](Value v) {
        top_nth(0) = v;
        __gc_stack_bottom = static_cast<void *>(static_cast<auint *>(__gc_stack_bottom) + 1);
    };

    auto global = [&](uint32_t m) { return ValuePtr(static_cast<auint *>(__gc_stack_top) + m); };

    auto local = [&](uint32_t m) {
        return ValuePtr(static_cast<auint *>(__gc_stack_top) + base + m);
    };

    auto arg = [&](uint32_t m) {
        return ValuePtr(static_cast<auint *>(__gc_stack_top) + base - args + m);
    };

    auto capture = [&](uint32_t m) {
        auto closure = Value::from_repr(static_cast<auint *>(__gc_stack_top)[base + args - 1]);

        return closure.field(m + 1);
    };

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

    // the address to call.
    uint32_t call_target = 0;

enter_frame: {
    const auto *proc = &info_.procs.at(call_target);
    auto new_size = static_cast<uint64_t>(base) + proc->locals + proc->stack_size;

    if (new_size > max_stack_size) [[unlikely]] {
        return std::unexpected(make_error("stack overflow"));
    }

    if (stack.size() < new_size) {
        stack.resize(new_size, 0);
    }

    frames.push_back(
        Frame{
            .proc_addr = call_target,
            .saved_pc = pc,
            .saved_base = base,
            .saved_args = args,
        }
    );

    pc = call_target;
    base = static_cast<auint *>(__gc_stack_bottom) - static_cast<auint *>(__gc_stack_top);
    args = proc->params + static_cast<uint32_t>(proc->is_closure);

    __gc_stack_top = static_cast<void *>(stack.data());
    __gc_stack_bottom = static_cast<void *>(stack.data() + base + proc->locals);
}

    while (true) {
        switch (bc[pc++]) {
        case Instr::Add: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_auint();
                auto rhs = top_nth(0).get().get_auint();
                pop_n(2);
                push(Value::from_int(lhs + rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot add {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Sub: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_auint();
                auto rhs = top_nth(0).get().get_auint();
                pop_n(2);
                push(Value::from_int(lhs - rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot subtract {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Mul: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_auint();
                auto rhs = top_nth(0).get().get_auint();
                pop_n(2);
                push(Value::from_int(lhs * rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot multiply {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Div: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_aint();
                auto rhs = top_nth(0).get().get_aint();
                pop_n(2);

                if (rhs == 0) [[unlikely]] {
                    return std::unexpected(make_error("division by zero"));
                }

                push(Value::from_int(lhs / rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot divide {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Mod: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_aint();
                auto rhs = top_nth(0).get().get_aint();
                pop_n(2);

                if (rhs == 0) [[unlikely]] {
                    return std::unexpected(
                        make_error("division by zero while taking the remainder")
                    );
                }

                push(Value::from_int(lhs % rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot take the remainder of {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Lt: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_aint();
                auto rhs = top_nth(0).get().get_aint();
                pop_n(2);
                push(Value::from_bool(lhs < rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Le: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_aint();
                auto rhs = top_nth(0).get().get_aint();
                pop_n(2);
                push(Value::from_bool(lhs <= rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Gt: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_aint();
                auto rhs = top_nth(0).get().get_aint();
                pop_n(2);
                push(Value::from_bool(lhs > rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Ge: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_aint();
                auto rhs = top_nth(0).get().get_aint();
                pop_n(2);
                push(Value::from_bool(lhs >= rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Eq: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_aint();
                auto rhs = top_nth(0).get().get_aint();
                pop_n(2);
                push(Value::from_bool(lhs == rhs));
            } else if (top_nth(1).get().is_int() || top_nth(0).get().is_int()) {
                pop_n(2);
                push(Value::from_bool(false));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Ne: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_aint();
                auto rhs = top_nth(0).get().get_aint();
                pop_n(2);
                push(Value::from_bool(lhs != rhs));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot compare {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::And: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_auint();
                auto rhs = top_nth(0).get().get_auint();
                pop_n(2);
                push(Value::from_bool(lhs != 0 && rhs != 0));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot perform boolean AND for {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Or: {
            if (top_nth(1).get().is_int() && top_nth(0).get().is_int()) {
                auto lhs = top_nth(1).get().get_auint();
                auto rhs = top_nth(0).get().get_auint();
                pop_n(2);
                push(Value::from_bool(lhs != 0 || rhs != 0));
            } else [[unlikely]] {
                return std::unexpected(make_error(
                    "cannot perform boolean OR for {} and {}",
                    top_nth(1).get().type_to_string(),
                    top_nth(0).get().type_to_string()
                ));
            }

            break;
        }

        case Instr::Const: {
            auto k = read_u32();
            push(Value::from_int(static_cast<aint>(k)));

            break;
        }

        case Instr::String: {
            auto s = read_u32();
            auto sv = mod_.strtab_entry_at(s);
            auto *v = get_object_content_ptr(alloc_string(sv.length() + 1));
            push(Value::box(v));
            // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
            strcpy(TO_DATA(v)->contents, sv.data());

            break;
        }

        case Instr::Sexp: {
            auto s = read_u32();
            auto n = read_u32();
            auto tag = mod_.strtab_entry_at(s);
            auto *v = get_object_content_ptr(alloc_sexp(n));
            TO_SEXP(v)->tag = reinterpret_cast<auint>(tag.data());

            for (size_t i = 0; i < n; ++i) {
                get_object_field(v, i) = *top_nth(n - i - 1);
            }

            pop_n(n);
            push(Value::box(v));

            break;
        }

        case Instr::Sti: {
            // the STI instruction is never emitted by the Lama compiler.
            std::unreachable();
        }

        case Instr::Sta: {
            auto aggregate_v = top_nth(2);
            auto idx_v = top_nth(1);
            auto v = top_nth(0);

            if (!aggregate_v.get().is_aggregate()) [[unlikely]] {
                return std::unexpected(
                    make_error("cannot index {}", aggregate_v.get().type_to_string())
                );
            }

            if (!idx_v.get().is_int()) [[unlikely]] {
                return std::unexpected(
                    make_error("index must be an integer, got {}", idx_v.get().type_to_string())
                );
            }

            auto idx = idx_v.get().get_aint();
            auto *aggregate_data = aggregate_v.get().to_data();

            if (aint len = LEN(aggregate_data->data_header); idx < 0 || idx >= len) [[unlikely]] {
                return std::unexpected(
                    make_error("index {} out of range for an aggregate of length {}", idx, len)
                );
            }

            switch (aggregate_v.get().get_type()) {
            case ARRAY:
            case STRING:
                get_object_field(aggregate_data, static_cast<size_t>(idx)) = v;
                break;

            case SEXP:
                get_sexp_field(aggregate_v.get().to_sexp(), static_cast<size_t>(idx)) = v;
                break;

            default:
                std::unreachable();
            }

            pop_n(3);
            push(v);

            break;
        }

        case Instr::Jmp: {
            auto l = read_u32();
            pc = l;

            break;
        }

        case Instr::End:
        case Instr::Ret: {
            auto v = *top_nth(0);
            auto &frame = frames.back();
            __gc_stack_bottom =
                static_cast<void *>(static_cast<auint *>(__gc_stack_top) + base - args);

            if (frame.saved_pc == -1U) [[unlikely]] {
                return {};
            }

            push(v);
            pc = frame.saved_pc;
            base = frame.saved_base;
            args = frame.saved_args;
            frames.pop_back();

            break;
        }

        case Instr::Drop: {
            pop_n(1);

            break;
        }

        case Instr::Dup: {
            push(*top_nth(0));

            break;
        }

        case Instr::Swap: {
            auto lhs = *top_nth(1);
            auto rhs = *top_nth(0);
            pop_n(2);
            push(rhs);
            push(lhs);
        }

        case Instr::Elem: {
            auto aggregate_v = top_nth(1);
            auto idx_v = top_nth(0);

            if (!aggregate_v.get().is_aggregate()) [[unlikely]] {
                return std::unexpected(
                    make_error("cannot index {}", aggregate_v.get().type_to_string())
                );
            }

            if (!idx_v.get().is_int()) [[unlikely]] {
                return std::unexpected(
                    make_error("index must be an integer, got {}", idx_v.get().type_to_string())
                );
            }

            auto idx = idx_v.get().get_aint();
            auto *aggregate_data = aggregate_v.get().to_data();

            if (aint len = LEN(aggregate_data->data_header); idx < 0 || idx >= len) [[unlikely]] {
                return std::unexpected(
                    make_error("index {} out of range for an aggregate of length {}", idx, len)
                );
            }

            pop_n(2);

            switch (aggregate_v.get().get_type()) {
            case ARRAY:
            case STRING:
                push(get_object_field(aggregate_data, static_cast<size_t>(idx)));
                break;

            case SEXP:
                push(get_sexp_field(aggregate_v.get().to_sexp(), static_cast<size_t>(idx)));
                break;

            default:
                std::unreachable();
            }

            break;
        }

        case Instr::LdG: {
            auto m = read_u32();
            push(global(m));

            break;
        }

        case Instr::LdL: {
            auto m = read_u32();
            push(local(m));

            break;
        }

        case Instr::LdA: {
            auto m = read_u32();
            push(arg(m));

            break;
        }

        case Instr::LdC: {
            auto m = read_u32();
            push(capture(m));

            break;
        }

        case Instr::LdaG:
        case Instr::LdaL:
        case Instr::LdaA:
        case Instr::LdaC:
            std::unreachable();

        case Instr::StG: {
            auto m = read_u32();
            global(m) = *top_nth(0);

            break;
        }

        case Instr::StL: {
            auto m = read_u32();
            local(m) = *top_nth(0);

            break;
        }

        case Instr::StA: {
            auto m = read_u32();
            arg(m) = *top_nth(0);

            break;
        }

        case Instr::StC: {
            auto m = read_u32();
            capture(m) = *top_nth(0);

            break;
        }

        case Instr::CjmpZ: {
            auto l = read_u32();
            auto cond = *top_nth(0);

            if (!cond.is_int()) [[unlikely]] {
                return std::unexpected(make_error(
                    "wrong branch condition type: expected integer, got {}", cond.type_to_string()
                ));
            }

            if (cond.get_auint() == 0) {
                pc = l;
            }

            break;
        }

        case Instr::CjmpNz: {
            auto l = read_u32();
            auto cond = *top_nth(0);

            if (!cond.is_int()) [[unlikely]] {
                return std::unexpected(make_error(
                    "wrong branch condition type: expected integer, got {}", cond.type_to_string()
                ));
            }

            if (cond.get_auint() != 0) {
                pc = l;
            }

            break;
        }

        case Instr::Begin:
        case Instr::Cbegin:
            // read a, n.
            read_u32();
            read_u32();

            break;

        case Instr::Closure: {
            auto l = read_u32();
            auto n = read_u32();
            auto *closure = get_object_content_ptr(alloc_closure(n + 1));
            push(Value::box(closure));
            get_object_field(closure, 0) = Value::from_int(static_cast<auint>(l));

            for (size_t i = 0; i < n; ++i) {
                auto kind = static_cast<uint8_t>(bc[pc++]);
                auto m = read_u32();
                auto field = get_object_field(closure, i + 1);

                switch (kind) {
                case 0:
                    field = *global(m);
                    break;

                case 1:
                    field = *local(m);
                    break;

                case 2:
                    field = *arg(m);
                    break;

                case 3:
                    field = *capture(m);
                    break;

                default:
                    std::unreachable();
                }
            }

            break;
        }

        case Instr::CallC: {
            auto n = read_u32();
            auto closure = *top_nth(n);

            if (!closure.is_closure()) [[unlikely]] {
                return std::unexpected(make_error("cannot call {}", closure.type_to_string()));
            }

            auto l = closure.field(0).get().get_auint();
            const auto &proc = info_.procs.at(l);

            if (proc.params != n) [[unlikely]] {
                return std::unexpected(
                    make_error("the function expected {} arguments, got {}", proc.params, n)
                );
            }

            call_target = l;

            goto enter_frame;
        }

        case Instr::Call: {
            auto l = read_u32();
            // read n.
            read_u32();

            call_target = l;

            goto enter_frame;
        }

        case Instr::Tag: {
            auto s = read_u32();
            auto n = read_u32();
            auto v = *top_nth(0);

            auto expected_tag = mod_.strtab_entry_at(s);

            pop_n(1);

            if (v.is_sexp()) {
                auto *sexp = v.to_sexp();
                auto actual_tag = mod_.strtab_entry_at(Value::from_repr(sexp->tag).get_auint());

                push(Value::from_bool(LEN(sexp->data_header) == n && expected_tag == actual_tag));
            } else {
                push(Value::from_bool(false));
            }

            break;
        }

        case Instr::Array: {
            auto n = read_u32();
            auto v = *top_nth(0);

            pop_n(1);

            if (v.is_array()) {
                push(Value::from_bool(LEN(v.to_data()->data_header) == n));
            } else {
                push(Value::from_bool(false));
            }

            break;
        }

        case Instr::Fail: {
            auto ln = read_u32();
            auto col = read_u32();
            // the scrutinee.
            pop_n(1);

            return std::unexpected(make_error("match failure at L{}:{}", ln, col));
        }

        case Instr::Line: {
            auto ln = read_u32();
            frames.back().line = ln;

            break;
        }

        case Instr::PattEqStr: {
            auto lhs = *top_nth(1);
            auto rhs = *top_nth(0);
            pop_n(2);

            if (lhs.is_string() && rhs.is_string()) {
                push(
                    Value::from_bool(strcmp(lhs.to_data()->contents, rhs.to_data()->contents) == 0)
                );
            } else {
                push(Value::from_bool(false));
            }

            break;
        }

        case Instr::PattString: {
            auto v = *top_nth(0);
            pop_n(1);
            push(Value::from_bool(v.is_string()));

            break;
        }

        case Instr::PattArray: {
            auto v = *top_nth(0);
            pop_n(1);
            push(Value::from_bool(v.is_array()));

            break;
        }

        case Instr::PattSexp: {
            auto v = *top_nth(0);
            pop_n(1);
            push(Value::from_bool(v.is_sexp()));

            break;
        }

        case Instr::PattRef: {
            auto v = *top_nth(0);
            pop_n(1);
            push(Value::from_bool(v.is_boxed()));

            break;
        }

        case Instr::PattVal: {
            auto v = *top_nth(0);
            pop_n(1);
            push(Value::from_bool(!v.is_boxed()));

            break;
        }

        case Instr::PattFun: {
            auto v = *top_nth(0);
            pop_n(1);
            push(Value::from_bool(v.is_closure()));

            break;
        }

        case Instr::CallLread: {
            aint v = 0;
            input_ >> v;
            push(Value::from_int(v));

            break;
        }

        case Instr::CallLwrite: {
            auto v = *top_nth(0);

            if (!v.is_int()) {
                return std::unexpected(
                    make_error("cannot write {} (expected integer)", v.type_to_string())
                );
            }

            pop_n(1);
            output_ << v.get_aint() << '\n';
            push(Value());

            break;
        }

        case Instr::CallLlength: {
            auto v = *top_nth(0);

            if (!v.is_aggregate()) {
                return std::unexpected(
                    make_error("cannot get the length of {}", v.type_to_string())
                );
            }

            aint len = 0;

            switch (v.get_type()) {
            case ARRAY:
            case STRING:
            case SEXP:
                len = LEN(v.to_data()->data_header);
                break;

            case CLOSURE:
                std::unreachable();
            }

            pop_n(1);
            push(Value::from_int(len));

            break;
        }

        case Instr::CallLstring: {
            auto v = *top_nth(0);
            auto s = v.stringify();
            auto *r = get_object_content_ptr(alloc_string(s.size() + 1));
            pop_n(1);
            push(Value::box(r));
            // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
            strcpy(TO_DATA(r)->contents, s.data());

            break;
        }

        case Instr::CallBarray: {
            auto n = read_u32();
            auto *v = get_object_content_ptr(alloc_array(n));

            for (size_t i = 0; i < n; ++i) {
                get_object_field(v, i) = *top_nth(n - i - 1);
            }

            pop_n(n);
            push(Value::box(v));

            break;
        }

        case Instr::Eof:
            std::unreachable();
        }
    }
}
