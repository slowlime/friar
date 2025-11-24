#include "idiom.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>

#include "decode.hpp"

using namespace friar;
using namespace friar::idiom;
using friar::bytecode::Instr;

namespace {

bool is_jump(Instr instr) noexcept {
    switch (instr) {
    case Instr::Jmp:
    case Instr::CjmpZ:
    case Instr::CjmpNz:
        return true;

    default:
        return false;
    }
}

bool is_terminal(Instr instr) noexcept {
    switch (instr) {
    case Instr::Jmp:
    case Instr::End:
    case Instr::Ret:
    case Instr::Fail:
        return true;

    default:
        return false;
    }
}

bool should_split_after(Instr instr) noexcept {
    switch (instr) {
    case Instr::Jmp:
    case Instr::Call:
    case Instr::CallC:
    case Instr::Ret:
    case Instr::End:
    case Instr::Fail:
        return true;

    default:
        return false;
    }
}

struct Reachability {
    std::vector<bool> reachable;
    std::vector<bool> jump_targets;
};

Reachability find_reachable_instrs(const bytecode::Module &mod, const verifier::ModuleInfo &info) {
    decode::Decoder decoder(mod.bytecode);
    std::vector<uint32_t> to_process;
    to_process.reserve(info.procs.size());

    std::vector<bool> reachable(mod.bytecode.size());
    std::vector<bool> jump_targets(mod.bytecode.size());

    auto enqueue_to_process = [&](uint32_t addr) {
        if (!reachable[addr]) {
            to_process.push_back(addr);
            reachable[addr] = true;
        }
    };

    for (const auto &[addr, _] : info.procs) {
        enqueue_to_process(addr);
    }

    while (!to_process.empty()) {
        auto addr = to_process.back();
        to_process.pop_back();

        decoder.move_to(addr);
        decode::InstrStart start;
        decode::InstrEnd end;

        decoder.next([&](const decode::Decoder::Result &result) {
            if (const auto *r = std::get_if<decode::InstrStart>(&result)) {
                start = *r;
            } else if (const auto *r = std::get_if<decode::InstrEnd>(&result)) {
                end = *r;
            } else if (const auto *r = std::get_if<decode::Imm32>(&result);
                       r && is_jump(start.opcode)) {
                enqueue_to_process(r->imm);
                jump_targets[r->imm] = true;
            }
        });

        if (!is_terminal(start.opcode)) {
            enqueue_to_process(end.addr);
        }
    }

    return {
        .reachable = std::move(reachable),
        .jump_targets = std::move(jump_targets),
    };
}

template<class F>
void walk_reachable_instrs(
    const bytecode::Module &mod,
    const std::vector<bool> &reachable,
    const F &callback
) {
    decode::Decoder decoder(mod.bytecode);

    for (auto it = reachable.begin();
         it = std::find(it, reachable.end(), true), it != reachable.end();
         ++it) {

        uint32_t addr = it - reachable.begin();
        decoder.move_to(addr);
        decode::InstrStart start;
        decode::InstrEnd end;

        decoder.next([&](const decode::Decoder::Result &result) {
            if (const auto *r = std::get_if<decode::InstrStart>(&result)) {
                start = *r;
            } else if (const auto *r = std::get_if<decode::InstrEnd>(&result)) {
                end = *r;
            }
        });

        callback(start, end);
    }
}

} // namespace

Idioms friar::idiom::find_idioms(const bytecode::Module &mod, const verifier::ModuleInfo &info) {
    auto hash = [](std::span<const Instr> s) {
        size_t result = 5381;

        for (auto i : s) {
            result = result * 33 + static_cast<uint8_t>(i);
        }

        return result;
    };

    auto cmp = [](std::span<const Instr> lhs, std::span<const Instr> rhs) {
        return std::ranges::equal(lhs, rhs);
    };

    std::unordered_map<std::span<const Instr>, uint32_t, decltype(hash), decltype(cmp)> occurrences(
        16, hash, cmp
    );

    auto get_span = [&](const decode::InstrEnd &end) {
        return std::span(mod.bytecode).subspan(end.start, end.len());
    };

    auto [reachable, jump_targets] = find_reachable_instrs(mod, info);
    decode::Decoder decoder(mod.bytecode);

    walk_reachable_instrs(
        mod, reachable, [&](const decode::InstrStart &start, const decode::InstrEnd &end) {
            occurrences[get_span(end)] += 1;

            if (!jump_targets[end.addr] && !should_split_after(start.opcode)) {
                decoder.move_to(end.addr);
                decode::InstrEnd next_end;

                decoder.next([&](const decode::Decoder::Result &result) {
                    if (const auto *r = std::get_if<decode::InstrEnd>(&result)) {
                        next_end = *r;
                    }
                });

                auto two_instr_span =
                    std::span(mod.bytecode).subspan(start.addr, next_end.addr - start.addr);
                occurrences[two_instr_span] += 1;
            }
        }
    );

    std::vector<Idiom> idioms;
    idioms.reserve(occurrences.size());

    for (auto [span, n] : occurrences) {
        idioms.emplace_back(span, n);
    }

    std::ranges::sort(idioms, [](const Idiom &lhs, const Idiom &rhs) {
        auto occur_ord = lhs.occurrences <=> rhs.occurrences;

        return occur_ord > 0 ||
               (occur_ord == 0 && std::ranges::lexicographical_compare(lhs.instrs, rhs.instrs));
    });

    return {.idioms = std::move(idioms)};
}
