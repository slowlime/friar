#include "idiom.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_set>
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

template<class F>
void walk_reachable_instrs(
    const bytecode::Module &mod,
    const verifier::ModuleInfo &info,
    const F &callback
) {
    decode::Decoder decoder(mod.bytecode);
    std::vector<uint32_t> to_process;
    std::vector<bool> processed(mod.bytecode.size());

    to_process.reserve(info.procs.size());

    for (const auto &[addr, _] : info.procs) {
        to_process.push_back(addr);
    }

    while (!to_process.empty()) {
        auto addr = to_process.back();
        to_process.pop_back();

        if (processed[addr]) {
            continue;
        }

        processed[addr] = true;

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
                to_process.push_back(r->imm);
            }
        });

        callback(start, end);

        if (!is_terminal(start.opcode)) {
            to_process.push_back(end.addr);
        }
    }
}

std::unordered_set<uint32_t>
find_split_points(const bytecode::Module &mod, const verifier::ModuleInfo &info) {
    std::unordered_set<uint32_t> split_at;
    decode::Decoder decoder(mod.bytecode);

    walk_reachable_instrs(mod, info, [&](const auto &start, const auto &end) {
        if (is_jump(start.opcode)) {
            decoder.move_to(start.addr);
            decoder.next([&](const decode::Decoder::Result &result) {
                if (const auto *r = std::get_if<decode::Imm32>(&result)) {
                    split_at.insert(r->imm);
                }
            });
        }

        if (should_split_after(start.opcode)) {
            split_at.insert(end.addr);
        }
    });

    return split_at;
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

    auto split_points = find_split_points(mod, info);
    decode::Decoder decoder(mod.bytecode);

    walk_reachable_instrs(
        mod, info, [&](const decode::InstrStart &start, const decode::InstrEnd &end) {
            occurrences[get_span(end)] += 1;

            if (!split_points.contains(end.addr)) {
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
