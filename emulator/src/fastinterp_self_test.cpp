// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <bit>
#include <limits>
#include "core/arm/fastinterp/arm_tick_counts.h"
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <sstream>
#include "core/arm/dyncom/arm_dyncom_interpreter.h"
#include "core/arm/dyncom/arm_dyncom_trans.h"
#include "core/arm/fastinterp/fastinterp.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core.h"
#include "fastinterp_vfp_self_test.h"
#include "test_memory.h"

int main() {
    Memory::MemorySystem memory;
    Core::Timing timing;
    auto& system = Core::System::GetInstance();
    auto old = std::make_unique<ARMul_State>(system, memory, USER32MODE);
    auto fast =
        std::make_unique<Core::FastInterp::ARM_FastInterp>(system, memory, 0, timing.GetTimer(0));
    auto table = std::make_shared<Memory::PageTable>();
    std::array<u32, 1024> code{};
    table->attributes[1] = Memory::PageType::Memory;
    table->pointers[1] = TestMemory(code);
    memory.SetCurrentPageTable(table);
    fast->SetPageTable(table);
    if (std::getenv("CARA_UNROLL_BUDGET_TEST")) {
        const char* flag = std::getenv("CARA_FASTINTERP_CHAIN_BUDGET");
        if (!flag || std::strcmp(flag, "1") != 0) {
            std::cerr << "Unroll budget oracle requires CHAIN_BUDGET=1\n";
            return 1;
        }
        const std::array<u32, 10> starts{0, 1, 2, 6, 7, 0x7fffffffu,
                                        0x80000000u, 0xffffffffu, 0xfffffffeu, 0x12345678u};
        u64 trace = 1469598103934665603ull;
        unsigned cases = 0;
        auto hash = [&](u64 value) { trace = (trace ^ value) * 1099511628211ull; };
        auto subtract_flags = [](u32 a, u32 b) {
            const u32 r = a - b;
            return (r & 0x80000000u) | (r == 0 ? 0x40000000u : 0u) |
                   (a >= b ? 0x20000000u : 0u) |
                   ((((a ^ b) & (a ^ r)) >> 3) & 0x10000000u);
        };
        for (unsigned variant = 0; variant < 3; ++variant)
            for (u32 imm : {0u, 1u, 2u, 7u})
                for (bool hot : {false, true}) {
                    const bool thumb = variant == 2, different_rn = variant == 1;
                    const unsigned ticks = thumb ? 2 : 5; // Existing fused-block cycle model.
                    code.fill(0);
                    if (thumb) {
                        const std::array<u16, 3> program{u16(0x3800 | imm), 0xD1FD, 0xE7FE};
                        std::memcpy(code.data(), program.data(), sizeof(program));
                    } else {
                        code = {0xE2500000u | (different_rn ? 1u << 16 : 0u) | imm,
                                0x1AFFFFFD, 0xEAFFFFFE};
                    }
                    fast->ClearInstructionCache();
                    fast->ResetFusionStats();
                    fast->SetReoptimizationEnabled(hot);
                    if (hot) {
                        fast->SetCPSR(0x10 | (thumb ? 0x20 : 0));
                        fast->SetPC(0x1000 | unsigned(thumb));
                        fast->SetReg(0, 0xfffffff0u);
                        fast->SetReg(1, 0xfffffff0u);
                        for (unsigned warm = 0; warm < 1001; ++warm) {
                            timing.GetTimer(0)->Advance(1);
                            fast->Run();
                            if ((warm == 998 && fast->GetFusionStats().hot_blocks_reoptimized) ||
                                (warm == 999 && fast->GetFusionStats().hot_blocks_reoptimized != 1)) {
                                std::cerr << "Unroll hot-threshold activation mismatch\n";
                                return 1;
                            }
                        }
                    }
                    for (u32 start : starts)
                        for (unsigned budget : {0u, 1u, ticks - 1, ticks, ticks + 1,
                                                15 * ticks, 16 * ticks, 16 * ticks + 1, 63u, 128u}) {
                            const u32 initial_cpsr = 0xA0000010u | (thumb ? 0x20u : 0u);
                            fast->SetCPSR(initial_cpsr);
                            fast->SetPC(0x1000 | unsigned(thumb));
                            fast->SetReg(0, different_rn ? ~start : start);
                            fast->SetReg(1, start);
                            timing.GetTimer(0)->Advance(budget);
                            const auto before = timing.GetTimer(0)->GetDowncount();
                            fast->Run();
                            u32 value = different_rn ? ~start : start, pc = 0x1000;
                            u32 cpsr = initial_cpsr;
                            s64 consumed = 0;
                            while (consumed < before) {
                                if (pc == 0x1000) {
                                    const u32 operand = different_rn ? start : value;
                                    value = operand - imm;
                                    cpsr = subtract_flags(operand, imm) | 0x10 | (thumb ? 0x20 : 0);
                                    if (!value) pc = thumb ? 0x1004 : 0x1008;
                                    consumed += ticks;
                                } else ++consumed; // Terminal B self, one guest tick.
                            }
                            if (fast->GetReg(0) != value || fast->GetReg(1) != start ||
                                fast->GetPC() != pc || fast->GetCPSR() != cpsr ||
                                before - timing.GetTimer(0)->GetDowncount() != consumed) {
                                std::cerr << "Hot/cold unroll block oracle mismatch variant=" << variant
                                          << " hot=" << hot << " imm=" << imm << " start=" << start
                                          << " budget=" << before << '\n';
                                return 1;
                            }
                            hash(value); hash(pc); hash(cpsr); hash(consumed);
                            ++cases;
                        }
                }
        std::cout << "Hot/cold ARM/Thumb unroll oracle cases=" << cases << " trace=" << trace << '\n';
        return 0;
    }
    if (std::getenv("CARA_HOT_LOOP_TRACE")) {
        code = {0xE2500001, 0x1AFFFFFD, 0xEAFFFFFE};
        fast->SetCPSR(0x10);
        fast->SetReg(0, 1000000);
        fast->SetPC(0x1000);
        const char* disable = std::getenv("CARA_HOT_LOOP_DISABLE");
        fast->SetReoptimizationEnabled(!(disable && std::strcmp(disable, "1") == 0));
        u64 trace = 1469598103934665603ull, ticks = 0;
        for (unsigned slice = 0; slice < 1100; ++slice) {
            timing.GetTimer(0)->Advance(1);
            const auto before = timing.GetTimer(0)->GetDowncount();
            fast->Run();
            const auto consumed = before - timing.GetTimer(0)->GetDowncount();
            ticks += consumed;
            trace = (trace ^ fast->GetReg(0)) * 1099511628211ull;
            trace = (trace ^ consumed) * 1099511628211ull;
        }
        std::cout << "Hot singleton trace r0=" << fast->GetReg(0) << " ticks=" << ticks
                  << " trace=" << trace << " hot=" << fast->GetFusionStats().hot_blocks_reoptimized << '\n';
        return 0;
    }
    if (std::getenv("CARA_CHAIN_SVC_TEST")) {
#ifndef NDEBUG
        std::cerr << "Unknown-SVC boundary fixture requires Release: debug kernel assertions "
                     "require a fully initialized process\n";
        return 1;
#else
        // Unknown SVC 0xff takes the real tick-flush/reload/exit path without
        // accessing a process service. This checks dispatch boundaries only;
        // process-context-changing SVCs still require target-game validation.
        const char* flag = std::getenv("CARA_FASTINTERP_CHAIN_BUDGET");
        const bool bounded = flag && std::strcmp(flag, "1") == 0;
        unsigned cases = 0;
        u64 trace = 1469598103934665603ull;
        auto hash = [&](u64 value) { trace = (trace ^ value) * 1099511628211ull; };
        for (bool thumb : {false, true})
            for (unsigned budget : {0u, 1u, 2u, 7u, 8u, 9u, 10u, 11u, 31u, 128u}) {
                code.fill(0);
                if (thumb) {
                    const std::array<u16, 4> program{0x3001, 0xDFFF, 0x3001, 0xE7FE};
                    std::memcpy(code.data(), program.data(), sizeof(program));
                } else {
                    code = {0xE2800001, 0xEF0000FF, 0xE2800001, 0xEAFFFFFE};
                }
                fast->ClearInstructionCache();
                fast->SetCPSR(0xA0000010u | (thumb ? 0x20u : 0u));
                fast->SetReg(0, 0);
                fast->SetPC(0x1000 | unsigned(thumb));
                timing.GetTimer(0)->Advance(budget);
                const s64 before = timing.GetTimer(0)->GetDowncount();
                fast->Run();
                const s64 consumed = before - timing.GetTimer(0)->GetDowncount();
                const s64 first = thumb ? 2 : 9;
                const unsigned adds = before <= 0 ? 0 : before <= first ? 1 : 2;
                const u32 pc = adds == 0 ? 0x1000 :
                               adds == 1 ? (thumb ? 0x1004 : 0x1008) :
                                           (thumb ? 0x1006 : 0x100c);
                const u32 cpsr = thumb ? (adds ? 0x30u : 0xA0000030u) : 0xA0000010u;
                const s64 expected_ticks = before <= 0 ? 0 : before <= first ? first :
                                           before <= first + 2 ? first + 2 : before;
                if (fast->GetReg(0) != adds || fast->GetPC() != pc ||
                    fast->GetCPSR() != cpsr || (bounded && consumed != expected_ticks)) {
                    std::cerr << "ARM/Thumb SVC block-budget mismatch thumb=" << thumb
                              << " budget=" << before << " consumed=" << consumed
                              << " expected=" << expected_ticks << '\n';
                    return 1;
                }
                hash(fast->GetReg(0)); hash(fast->GetPC());
                hash(fast->GetCPSR()); hash(consumed);
                ++cases;
            }
        std::cout << "ARM/Thumb real-SVC flush boundaries cases=" << cases
                  << " trace=" << trace << '\n';
        return 0;
#endif
    }
    if (std::getenv("CARA_CACHE_CONFLICT_TRACE")) {
        auto large_code = std::make_unique<std::array<u32, 32768>>();
        auto conflict_table = std::make_shared<Memory::PageTable>();
        auto backing = TestMemory(*large_code);
        for (unsigned page = 0; page < 32; ++page) {
            conflict_table->attributes[page] = Memory::PageType::Memory;
            conflict_table->pointers[page] = backing + page * 4096;
        }
        std::array<u32, 5> pcs{};
        auto index = [](u32 pc) {
            u32 hash = pc * 0x9E3779B9u;
            hash ^= hash >> 16;
            return (hash >> 4) & 4095u;
        };
        unsigned found = 0;
        for (u32 pc = 0x1000; found < pcs.size(); pc += 4)
            if (index(pc) == index(0x1000)) pcs[found++] = pc;
        for (unsigned i = 0; i < pcs.size(); ++i) {
            (*large_code)[pcs[i] / 4] = 0xE2800001; // ADD r0,r0,#1
            const s32 delta = s32(pcs[(i + 1) % pcs.size()]) - s32(pcs[i] + 12);
            (*large_code)[pcs[i] / 4 + 1] = 0xEA000000u | (u32(delta / 4) & 0xffffffu);
        }
        memory.SetCurrentPageTable(conflict_table);
        fast->SetPageTable(conflict_table);
        fast->SetCPSR(0xA0000010);
        fast->SetReg(0, 0);
        fast->SetPC(pcs[0]);
        u64 total_ticks = 0, trace = 1469598103934665603ull;
        auto hash = [&](u64 v) { trace = (trace ^ v) * 1099511628211ull; };
        for (unsigned slice = 0; slice < 512; ++slice) {
            const std::array<unsigned, 8> budgets{1, 2, 5, 6, 7, 31, 32, 128};
            timing.GetTimer(0)->Advance(budgets[slice % budgets.size()]);
            const auto before = timing.GetTimer(0)->GetDowncount();
            fast->Run();
            const auto consumed = before - timing.GetTimer(0)->GetDowncount();
            total_ticks += consumed;
            const char* budget_flag = std::getenv("CARA_FASTINTERP_CHAIN_BUDGET");
            const bool bounded = budget_flag && std::strcmp(budget_flag, "1") == 0;
            const u64 canonical_blocks = (u64(std::max<s64>(before, 0)) + 1) / 2;
            if (bounded && (u64(consumed) != canonical_blocks * 2)) {
                std::cerr << "Cache conflict canonical block-budget mismatch\n";
                return 1;
            }
            if (total_ticks != u64(fast->GetReg(0)) * 2 ||
                fast->GetPC() != pcs[fast->GetReg(0) % pcs.size()] ||
                fast->GetCPSR() != 0xA0000010) {
                std::cerr << "Cache conflict instruction/tick oracle mismatch\n";
                return 1;
            }
            hash(fast->GetReg(0)); hash(fast->GetPC()); hash(consumed);
        }
        std::cout << "Cache conflict oracle slices=512 instructions=" << fast->GetReg(0)
                  << " ticks=" << total_ticks << " trace=" << trace << '\n';
        return 0;
    }
    if (std::getenv("CARA_CACHE_PROFILE_TEST")) {
        using namespace Core::FastInterp;
        for (unsigned ways : {4u, 8u})
        for (bool enabled : {false, true}) {
            auto cache = std::make_unique<BlockCache>();
            cache->SetEightWay(ways == 8);
            cache->SetProfiling(enabled);
            std::array<u32, 9> colliding{};
            unsigned found = 0;
            auto set_index = [ways](u32 pc) {
                u32 value = pc * 0x9E3779B9u;
                value ^= value >> 16;
                return (value >> 4) & ((CACHE_SIZE / ways) - 1);
            };
            for (u32 pc = 0x1000; found < ways + 1; pc += 4)
                if (set_index(pc) == set_index(0x1000)) colliding[found++] = pc;
            for (unsigned i = 0; i < ways; ++i) {
                bool hit;
                auto* block = cache->LookupOrAllocate(colliding[i], hit);
                if (hit) return 1;
                block->inst_count = 1;
                block->end_pc = block->start_pc + 4;
            }
            // Hits must neither allocate nor change diagnostic totals.
            for (unsigned i = 0; i < ways; ++i) {
                const u32 pc = colliding[i];
                bool hit;
                if (!cache->Lookup(pc) || !cache->LookupOrAllocate(pc, hit) || !hit) return 1;
            }
            bool hit;
            auto* replaced = cache->LookupOrAllocate(colliding[ways], hit);
            if (hit) return 1;
            replaced->inst_count = 1;
            replaced->end_pc = replaced->start_pc + 4;
            auto* surviving = cache->Lookup(colliding[ways - 1]);
            if (!surviving || surviving == replaced) return 1;
            surviving->chain_target = replaced;
            cache->InvalidateRange(colliding[ways], 4);
            if (cache->Lookup(colliding[ways]) || replaced->inst_count || replaced->chain_target ||
                !surviving->chain_target || surviving->chain_target->inst_count) return 1;
            cache->InvalidateRange(0, 4); // No match still counts the call.
            const auto before_clear = cache->GetProfile();
            cache->Clear();
            unsigned resident = 0;
            cache->VisitResidentBlocks([&](const auto&) { ++resident; });
            const auto& stats = cache->GetProfile();
            if (resident || stats.allocation_misses != (enabled ? ways + 1 : 0u) ||
                stats.empty_allocations != (enabled ? ways : 0u) ||
                stats.live_replacements != (enabled ? 1u : 0u) ||
                stats.invalidation_calls != (enabled ? 2u : 0u) ||
                stats.invalidated_entries != (enabled ? 1u : 0u) ||
                stats.clears != (enabled ? 1u : 0u) ||
                stats.allocation_misses != before_clear.allocation_misses) {
                std::cerr << "Cache profile accounting/coherence mismatch\n";
                return 1;
            }
            bool new_hit;
            auto* again = cache->LookupOrAllocate(colliding[0], new_hit);
            again->inst_count = 1;
            again->end_pc = colliding[0] + 4;
            cache->SetEightWay(ways != 8);
            if (cache->Lookup(colliding[0]) || again->inst_count || again->chain_target)
                return 1;
        }
        // Run-level causes: a genuine mode mismatch and distinct page table.
        code = {0xE1A00000, 0xEAFFFFFE};
        fast->SetCPSR(0x10);
        fast->SetPC(0x1000);
        timing.GetTimer(0)->Advance(1);
        fast->Run();
        code[0] = 0xE7FEE7FE; // Thumb B self, at the same PC as cached ARM.
        fast->SetPC(0x1001);
        timing.GetTimer(0)->Advance(1);
        fast->Run();
        fast->SetPageTable(table); // Same address space must not clear.
        auto second = std::make_shared<Memory::PageTable>();
        fast->SetPageTable(second);
        fast->SetPageTable(second);
        fast->InvalidateCacheRange(0x1000, 4);
        std::ostringstream profile;
        fast->WriteBlockProfile(profile);
        const char* flag = std::getenv("CARA_FASTINTERP_CACHE_PROFILE");
        const bool enabled = flag && std::strcmp(flag, "1") == 0;
        auto field = [&](const char* name) {
            const std::string marker = std::string("\"") + name + "\":";
            const auto position = profile.str().find(marker);
            if (position == std::string::npos) return ~0ull;
            return std::stoull(profile.str().substr(position + marker.size()));
        };
        const char* way_flag = std::getenv("CARA_FASTINTERP_CACHE_WAYS");
        const unsigned expected_ways = way_flag && std::strcmp(way_flag, "8") == 0 ? 8 : 4;
        if (field("cache_ways") != expected_ways ||
            field("cache_sets") != CACHE_SIZE / expected_ways ||
            field("cache_entries") != CACHE_SIZE ||
            field("blocks_decoded") != 2 ||
            field("cache_allocation_misses") != (enabled ? 1u : 0u) ||
            field("cache_mode_redecodes") != (enabled ? 1u : 0u) ||
            field("cache_page_table_changes") != (enabled ? 2u : 0u) ||
            field("cache_clears") != (enabled ? 2u : 0u) ||
            field("cache_invalidation_calls") != (enabled ? 1u : 0u)) {
            std::cerr << "Run cache causes mismatch " << profile.str() << '\n';
            return 1;
        }
        std::cout << "Cache diagnostic off/on collisions, hit accounting, invalidation, "
                     "stale chains, clears, modes and page tables passed\n";
        return 0;
    }
    if (std::getenv("CARA_ALU_OPERANDS_TEST")) {
        // DynCom executes exactly the instruction; FastInterp decodes a real
        // block ending in a flag-neutral self-branch. Unlike Step(), Run()
        // exercises decode postpasses and the specialized handlers.
        unsigned cases = 0;
        u64 trace = 1469598103934665603ull;
        auto hash = [&](u64 value) { trace = (trace ^ value) * 1099511628211ull; };
        for (unsigned op : {4u, 13u})
            for (unsigned cond = 0; cond < 15; ++cond)
                for (unsigned nzcv = 0; nzcv < 16; ++nzcv)
                    for (unsigned form = 0; form < 9; ++form)
                        for (unsigned alias = 0; alias < 4; ++alias) {
                            const unsigned rd = alias == 1 ? 1 : alias == 2 ? 2 : 0;
                            const unsigned rn = alias == 3 ? 15 : 1;
                            const unsigned rm = alias == 3 ? 15 : 2;
                            const unsigned shift = form == 1 ? 0 : form == 2 ? 1 : 31;
                            u32 operand = form == 0 ? (1u << 25) | 0x480 :
                                          form < 4 ? (shift << 7) | rm :
                                          form < 7 ? ((form - 3) << 5) | (7u << 7) | rm :
                                          form == 7 ? (3u << 5) | rm :
                                                      (3u << 8) | (1u << 4) | rm;
                            const u32 raw = (cond << 28) | (op << 21) | (rn << 16) |
                                            (rd << 12) | operand;
                            code.fill(0xEAFFFFFE);
                            code[0] = raw;
                            const u32 cpsr = (nzcv << 28) | 0x10;
                            old->Cpsr = cpsr;
                            fast->SetCPSR(cpsr);
                            for (unsigned reg = 0; reg < 15; ++reg) {
                                const u32 value = reg == 1 ? 0xffffffffu : reg == 2 ? 1u :
                                                  reg == 3 ? 31u : 0x9e3779b9u * (reg + 1);
                                old->Reg[reg] = value;
                                fast->SetReg(reg, value);
                            }
                            old->Reg[15] = 0x1000;
                            fast->SetPC(0x1000);
                            InvalidateDynComDecodedCache();
                            old->instruction_cache.clear();
                            old->NumInstrsToExecute = 1;
                            InterpreterMainLoop(old.get(), timing.GetTimer(0).get());
                            fast->ClearInstructionCache();
                            timing.GetTimer(0)->Advance(1);
                            const auto before = timing.GetTimer(0)->GetDowncount();
                            fast->Run();
                            for (unsigned reg = 0; reg < 16; ++reg) {
                                if (old->Reg[reg] != fast->GetReg(reg)) {
                                    std::cerr << "ALU operand mismatch raw=" << std::hex << raw
                                              << " r=" << reg << " expected=" << old->Reg[reg]
                                              << " actual=" << fast->GetReg(reg) << '\n';
                                    return 1;
                                }
                                hash(fast->GetReg(reg));
                            }
                            if (fast->GetCPSR() != cpsr) {
                                std::cerr << "ALU operand live flags changed\n";
                                return 1;
                            }
                            hash(fast->GetCPSR());
                            hash(before - timing.GetTimer(0)->GetDowncount());
                            ++cases;
                        }
        // PC-writing forms stay on existing interworking handlers. Verify both
        // predicates and ARM/Thumb targets without admitting them to fast opcodes.
        for (u32 raw : {0xE1A0F001u, 0xE281F004u, 0xE28FF004u})
            for (bool taken : {false, true})
                for (bool thumb_target : {false, true}) {
                    code.fill(0xEAFFFFFE);
                    code[0] = taken ? raw : (raw & 0x0fffffffu); // failed EQ, Z=0
                    fast->ClearInstructionCache();
                    fast->SetCPSR(0xA0000010);
                    fast->SetPC(0x1000);
                    fast->SetReg(1, 0x1008 | u32(thumb_target));
                    timing.GetTimer(0)->Advance(1);
                    fast->Run();
                    const bool pc_source = raw == 0xE28FF004u;
                    const u32 target = raw == 0xE1A0F001u ? 0x1008 : 0x100c;
                    const u32 expected_cpsr = 0xA0000010u |
                        (taken && thumb_target && !pc_source ? 0x20u : 0u);
                    if (fast->GetPC() != (taken ? target : 0x1004) ||
                        fast->GetCPSR() != expected_cpsr) {
                        std::cerr << "ALU operand PC fallback mismatch\n";
                        return 1;
                    }
                    hash(fast->GetPC());
                    hash(fast->GetCPSR());
                    ++cases;
                }
        // Reuse/chaining/hot-count thresholds and narrow scheduler slices.
        // Keep the loop ADDs away from immediate-CMP pair fusion so this path
        // exercises specialized handlers repeatedly in the same cached block.
        for (unsigned prefix : {0u, 27u, 28u, 29u, 30u, 31u}) {
            code.fill(0xE1A05005);
            code[prefix] = 0xE0800081;     // ADD r0,r0,r1,LSL#1
            code[prefix + 1] = 0xE1A02000; // MOV r2,r0
            code[prefix + 2] = 0xE2820001; // ADD r0,r2,#1
            code[prefix + 3] = 0xE1A01081; // MOV r1,r1,LSL#1
            code[prefix + 4] = 0xE2911001; // ADDS r1,r1,#1
            code[prefix + 5] = 0xE2544001; // SUBS r4,r4,#1
            const s32 branch_words = -s32(prefix + 8);
            code[prefix + 6] = 0x1A000000u | (u32(branch_words) & 0xffffffu);
            code[prefix + 7] = 0xEAFFFFFE;
            fast->ClearInstructionCache();
            fast->SetCPSR(0xA0000010);
            fast->SetPC(0x1000);
            for (unsigned reg = 0; reg < 15; ++reg)
                fast->SetReg(reg, reg == 4 ? 1000000u : 0x12345678u * (reg + 1));
            for (unsigned slice = 0; slice < 1600; ++slice) {
                const std::array<unsigned, 8> budgets{1, 2, 5, 6, 7, 31, 32, 128};
                timing.GetTimer(0)->Advance(budgets[slice % budgets.size()]);
                const auto before = timing.GetTimer(0)->GetDowncount();
                fast->Run();
                for (unsigned reg = 0; reg < 16; ++reg)
                    hash(fast->GetReg(reg));
                hash(fast->GetCPSR());
                hash(before - timing.GetTimer(0)->GetDowncount());
            }
        }
        // One final real block proves every specialized opcode was activated.
        code = {0xE2810001, 0xE0812182, 0xE3A030FF, 0xE1A04281, 0xEAFFFFFE};
        fast->ClearInstructionCache();
        fast->SetCPSR(0x10);
        fast->SetPC(0x1000);
        timing.GetTimer(0)->Advance(1);
        fast->Run();
        std::ostringstream profile;
        fast->WriteBlockProfile(profile);
        const char* flag = std::getenv("CARA_FASTINTERP_ALU_OPERANDS");
        const bool enabled = flag && std::strcmp(flag, "1") == 0;
        for (const char* name : {"AddImm", "AddLslImm", "MovImm", "MovLslImm"}) {
            const std::string marker = std::string("\"") + name + "\":";
            const auto position = profile.str().find(marker);
            if (position == std::string::npos) {
                std::cerr << "ALU operand profile count missing\n";
                return 1;
            }
            const auto count = std::stoull(profile.str().substr(position + marker.size()));
            if ((count != 0) != enabled) {
                std::cerr << "ALU operand specialization activation mismatch\n";
                return 1;
            }
        }
        std::cout << "ALU operand DynCom cases=" << cases << " trace=" << trace << '\n';
        return 0;
    }
    if (std::getenv("CARA_CONTIGUOUS_TRANSFERS_TEST")) {
        using Core::FastInterp::ContiguousTransferRangesDisjoint;
        const auto maximum = std::numeric_limits<std::uintptr_t>::max();
        if (!ContiguousTransferRangesDisjoint(0x1000, 0, 16, 0x2000) ||
            !ContiguousTransferRangesDisjoint(0x1ff0, 0, 16, 0x2000) ||
            !ContiguousTransferRangesDisjoint(0x2040, 0, 16, 0x2000) ||
            ContiguousTransferRangesDisjoint(0x1ff1, 0, 16, 0x2000) ||
            ContiguousTransferRangesDisjoint(0x203f, 0, 16, 0x2000) ||
            ContiguousTransferRangesDisjoint(maximum, 1, 16, 0x2000) ||
            ContiguousTransferRangesDisjoint(maximum - 7, 0, 16, 0x2000) ||
            ContiguousTransferRangesDisjoint(0x1000, 0, 16, maximum - 7) ||
            ContiguousTransferRangesDisjoint(0x1000, 0, 0, 0x2000)) {
            std::cerr << "contiguous transfer host-range guard mismatch\n";
            return 1;
        }
        std::array<u8, 4096> page_a{}, page_b{};
        table->attributes[3] = table->attributes[4] = Memory::PageType::Memory;
        table->pointers[3] = TestMemory(page_a);
        table->pointers[4] = TestMemory(page_b);
        unsigned cases = 0;
        u64 hash = 1469598103934665603ull;
        auto record = [&](u64 value) { hash = (hash ^ value) * 1099511628211ull; };
        for (bool load : {false, true})
            for (bool before : {false, true})
                for (bool increment : {false, true})
                    for (bool writeback : {false, true})
                        for (bool user : {false, true})
                            for (u16 mask : {u16(0x000f), u16(0x0078), u16(0x0780), u16(0x7800),
                                             u16(0xf000), u16(0x00ff), u16(0x0ff0), u16(0xff00),
                                             u16(0), u16(1), u16(0x0f0f), u16(0x500f)})
                                for (unsigned rn : {0u, 2u, 7u, 12u}) {
                                    const unsigned bytes = std::popcount(mask) * 4;
                                    for (unsigned offset :
                                         {0u, 3u, 4096u - bytes, 4097u - bytes, 4095u})
                                        for (unsigned predicate : {0u, 1u, 2u}) {
                                            std::array<u8, 8192> expected_memory{};
                                            for (unsigned i = 0; i < expected_memory.size(); ++i)
                                                expected_memory[i] = u8(i * 13 + 37);
                                            std::copy_n(expected_memory.data(), 4096,
                                                        page_a.data());
                                            std::copy_n(expected_memory.data() + 4096, 4096,
                                                        page_b.data());
                                            std::array<u32, 16> expected{};
                                            for (unsigned i = 0; i < 15; ++i) {
                                                expected[i] = 0x9e3779b9u * (i + 1);
                                                fast->SetReg(i, expected[i]);
                                            }
                                            const u32 start = 0x3000 + offset;
                                            const u32 base = increment
                                                                 ? start - (before ? 4 : 0)
                                                                 : start + bytes - (before ? 0 : 4);
                                            expected[rn] = base;
                                            fast->SetReg(rn, base);
                                            const bool executes = predicate != 1;
                                            const u32 cpsr =
                                                predicate == 2 ? 0xe0000010u : 0xa0000010u;
                                            code[0] = ((predicate == 0 ? 14u : 0u) << 28) |
                                                      0x08000000u | (u32(before) << 24) |
                                                      (u32(increment) << 23) | (u32(user) << 22) |
                                                      (u32(writeback) << 21) | (u32(load) << 20) |
                                                      (rn << 16) | mask;
                                            expected[15] =
                                                0x1008; // STM's established pipeline PC source.
                                            if (executes) {
                                                unsigned position = offset;
                                                for (unsigned reg = 0; reg < 16; ++reg)
                                                    if (mask & (1u << reg)) {
                                                        if (load)
                                                            std::memcpy(&expected[reg],
                                                                        expected_memory.data() +
                                                                            position,
                                                                        4);
                                                        else
                                                            std::memcpy(expected_memory.data() +
                                                                            position,
                                                                        &expected[reg], 4);
                                                        position += 4;
                                                    }
                                                if (writeback)
                                                    expected[rn] =
                                                        increment ? base + bytes : base - bytes;
                                            }
                                            u32 expected_pc = 0x1004, expected_cpsr = cpsr;
                                            if (executes && load && (mask & 0x8000)) {
                                                expected_pc = expected[15] & ~1u;
                                                expected_cpsr |= (expected[15] & 1) ? 0x20 : 0;
                                            }
                                            expected[15] = expected_pc;
                                            fast->ClearInstructionCache();
                                            fast->SetCPSR(cpsr);
                                            fast->SetPC(0x1000);
                                            const auto tick_before =
                                                timing.GetTimer(0)->GetDowncount();
                                            fast->Step();
                                            const auto ticks =
                                                tick_before - timing.GetTimer(0)->GetDowncount();
                                            bool passed =
                                                fast->GetPC() == expected_pc &&
                                                fast->GetCPSR() == expected_cpsr &&
                                                ticks == Core::TicksForInstruction(false, code[0]);
                                            for (unsigned i = 0; i < 16; ++i) {
                                                passed &= fast->GetReg(i) == expected[i];
                                                record(fast->GetReg(i));
                                            }
                                            passed &=
                                                std::memcmp(page_a.data(), expected_memory.data(),
                                                            4096) == 0 &&
                                                std::memcmp(page_b.data(),
                                                            expected_memory.data() + 4096,
                                                            4096) == 0;
                                            if (!passed) {
                                                std::cerr
                                                    << "contiguous transfer oracle mismatch case="
                                                    << cases << " instruction=" << std::hex
                                                    << code[0] << "\n";
                                                return 1;
                                            }
                                            record(ticks);
                                            record(fast->GetCPSR());
                                            ++cases;
                                        }
                                }
        // A final word straddling guest UINT32_MAX must wrap into page zero;
        // the entire-span fast path must reject it and retain coherent byte reads.
        table->attributes[0xfffff] = table->attributes[0] = Memory::PageType::Memory;
        table->pointers[0xfffff] = TestMemory(page_a);
        table->pointers[0] = TestMemory(page_b);
        for (bool load : {false, true})
            for (bool writeback : {false, true}) {
                std::array<u8, 8192> expected_memory{};
                for (unsigned i = 0; i < expected_memory.size(); ++i)
                    expected_memory[i] = u8(i * 17 + 43);
                std::copy_n(expected_memory.data(), 4096, page_a.data());
                std::copy_n(expected_memory.data() + 4096, 4096, page_b.data());
                std::array<u32, 16> expected{};
                for (unsigned i = 0; i < 15; ++i) {
                    expected[i] = 0x10203040u + i * 977;
                    fast->SetReg(i, expected[i]);
                }
                expected[2] = 0xfffffff1;
                fast->SetReg(2, expected[2]);
                for (unsigned reg = 3; reg <= 6; ++reg) {
                    const unsigned offset = 4081 + (reg - 3) * 4;
                    if (load)
                        std::memcpy(&expected[reg], expected_memory.data() + offset, 4);
                    else
                        std::memcpy(expected_memory.data() + offset, &expected[reg], 4);
                }
                if (writeback)
                    expected[2] = 1;
                expected[15] = 0x1004;
                code[0] = 0xE8820078u | (u32(load) << 20) | (u32(writeback) << 21);
                fast->ClearInstructionCache();
                fast->SetCPSR(0xa0000010);
                fast->SetPC(0x1000);
                const auto before = timing.GetTimer(0)->GetDowncount();
                fast->Step();
                bool passed =
                    fast->GetCPSR() == 0xa0000010 && before - timing.GetTimer(0)->GetDowncount() ==
                                                         Core::TicksForInstruction(false, code[0]);
                for (unsigned reg = 0; reg < 16; ++reg) {
                    passed &= fast->GetReg(reg) == expected[reg];
                    record(fast->GetReg(reg));
                }
                passed &= std::memcmp(page_a.data(), expected_memory.data(), 4096) == 0 &&
                          std::memcmp(page_b.data(), expected_memory.data() + 4096, 4096) == 0;
                if (!passed) {
                    std::cerr << "guest address wrap transfer mismatch\n";
                    return 1;
                }
                ++cases;
            }
        std::cout
            << cases
            << " contiguous transfer scalar-oracle cases and 9 alias/wrap guards passed; trace="
            << hash << "\n";
        return 0;
    }
    if (std::getenv("CARA_ADD_CMP_TEST")) {
        static_assert(sizeof(Core::FastInterp::DecodedInst) == 20);
        auto rotate = [](u32 encoding) {
            return std::rotr(encoding & 255u, int((encoding >> 8) * 2));
        };
        auto flags = [](u32 a, u32 b) {
            const u32 d = a - b;
            return (d & 0x80000000u) | (d == 0 ? 0x40000000u : 0u) | (a >= b ? 0x20000000u : 0u) |
                   ((((a ^ b) & (a ^ d)) >> 3) & 0x10000000u);
        };
        auto passed = [](unsigned cond, u32 f) {
            bool n = f >> 31, z = (f >> 30) & 1, c = (f >> 29) & 1, v = (f >> 28) & 1;
            switch (cond) {
            case 0:
                return z;
            case 1:
                return !z;
            case 2:
                return c;
            case 3:
                return !c;
            case 4:
                return n;
            case 5:
                return !n;
            case 6:
                return v;
            case 7:
                return !v;
            case 8:
                return c && !z;
            case 9:
                return !c || z;
            case 10:
                return n == v;
            case 11:
                return n != v;
            case 12:
                return !z && n == v;
            default:
                return z || n != v;
            }
        };
        const std::array<u32, 8> edges{0,           1,           0xffffffffu, 0xfffffffeu,
                                       0x7fffffffu, 0x80000000u, 0x80000001u, 0x12345678u};
        const std::array<u32, 7> immediates{1, 255, 0x101, 0x480, 0x801, 0xcff, 0xf01};
        u64 checksum = 1469598103934665603ull;
        unsigned cases = 0;
        auto hash = [&](u64 value) { checksum = (checksum ^ value) * 1099511628211ull; };
        for (unsigned prefix : {0u, 29u, 30u, 31u})
            for (u32 a : immediates)
                for (u32 c : immediates)
                    for (u32 initial : edges)
                        for (unsigned cond = 0; cond < 14; ++cond) {
                            code.fill(0xE1A03003);
                            // Also verify ADD.S forms whose flags are killed by the following CMP.
                            code[prefix] =
                                (std::getenv("CARA_ADD_CMP_SETFLAGS") ? 0xE2910000u : 0xE2810000u) | a;
                            code[prefix + 1] = 0xE3500000 | c;
                            code[prefix + 2] = (cond << 28) | 0x0A000001; // taken -> MOV r2,#2
                            code[prefix + 3] = 0xE3A02001;
                            code[prefix + 4] = 0xEA000000;
                            code[prefix + 5] = 0xE3A02002;
                            code[prefix + 6] = 0xEAFFFFFE;
                            fast->ClearInstructionCache();
                            fast->SetCPSR(0xA0000010);
                            fast->SetPC(0x1000);
                            fast->SetReg(0, 0xdeadbeef);
                            fast->SetReg(1, initial);
                            fast->SetReg(2, 0);
                            timing.GetTimer(0)->Advance(128);
                            fast->Run();
                            u32 result = initial + rotate(a), f = flags(result, rotate(c));
                            if (fast->GetReg(0) != result || fast->GetReg(1) != initial ||
                                fast->GetReg(2) != (passed(cond, f) ? 2u : 1u) ||
                                fast->GetCPSR() != (f | 0x10) ||
                                fast->GetPC() != 0x1000 + 4 * (prefix + 6)) {
                                std::cerr
                                    << "ADD/CMP immediate differential mismatch prefix=" << prefix
                                    << " cond=" << cond << "\n";
                                return 1;
                            }
                            hash(fast->GetReg(0));
                            hash(fast->GetReg(2));
                            hash(fast->GetCPSR());
                            ++cases;
                        }
        // Repeated, observable timer boundaries. Prefixes place ADD at the last
        // real slot or immediately before it; retaining original slots matters.
        for (unsigned prefix : {0u, 28u, 29u, 30u, 31u}) {
            code.fill(0xE1A03003);
            code[prefix] = 0xE2800001;
            code[prefix + 1] = 0xE3500080;
            code[prefix + 2] = 0x3AFFFFFC;
            code[prefix + 3] = 0xEAFFFFFE;
            fast->ClearInstructionCache();
            fast->SetCPSR(0x10);
            fast->SetPC(0x1000);
            fast->SetReg(0, 0);
            for (unsigned slice = 0; slice < 128; ++slice) {
                const std::array<unsigned, 8> budgets{1, 2, 5, 6, 7, 31, 32, 128};
                timing.GetTimer(0)->Advance(budgets[slice % budgets.size()]);
                const auto before = timing.GetTimer(0)->GetDowncount();
                fast->Run();
                hash(fast->GetPC());
                hash(fast->GetReg(0));
                hash(fast->GetCPSR());
                hash(before - timing.GetTimer(0)->GetDowncount());
            }
        }
        std::ostringstream profile;
        fast->WriteBlockProfile(profile);
        const auto marker = "\"opcodes\":[" +
                            std::to_string(unsigned(Core::FastInterp::Opcode::AddCmpImmBcc)) + "]";
        const char* experiment = std::getenv("CARA_FASTINTERP_ADD_CMP_IMM");
        const bool enabled = experiment && std::strcmp(experiment, "1") == 0;
        if ((profile.str().find(marker) != std::string::npos) != enabled) {
            std::cerr << "ADD/CMP experiment activation mismatch\n";
            return 1;
        }
        std::cout << "ADD/CMP independent cases=" << cases
                  << " quantum_slices=640 trace=" << checksum << "\n";
        return 0;
    }
    if (std::getenv("CARA_VCMP_VMRS_BENCH")) {
        // Finite guest loop with a real floating compare, APSR transfer,
        // conditional branch and countdown. No guest iterations are skipped.
        code = {0xEEB40A60, 0xEEF1FA10, 0x0A000001, 0xE2500001, 0x1AFFFFFA, 0xEAFFFFFE};
        fast->SetVFPReg(0, 0x3F800000);
        fast->SetVFPReg(1, 0x40000000);
        fast->ClearInstructionCache();
        fast->SetCPSR(0x10);
        fast->SetPC(0x1000);
        fast->SetReg(0, 20000000);
        const auto start = std::chrono::steady_clock::now();
        while (fast->GetReg(0)) {
            timing.GetTimer(0)->Advance(100000);
            fast->Run();
        }
        std::cout << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                  << "\n";
        return 0;
    }
    if (std::getenv("CARA_VCMP_VMRS_TEST")) {
        static_assert(sizeof(Core::FastInterp::DecodedInst) == 20);
        auto expected_flags = [](u32 a, u32 b) {
            const bool nan_a = (a & 0x7fffffff) > 0x7f800000;
            const bool nan_b = (b & 0x7fffffff) > 0x7f800000;
            if (nan_a || nan_b)
                return 0x30000000u;
            if (a == b || ((a | b) & 0x7fffffff) == 0)
                return 0x60000000u;
            const u32 ordered_a = (a >> 31) ? ~a : (a ^ 0x80000000u);
            const u32 ordered_b = (b >> 31) ? ~b : (b ^ 0x80000000u);
            return ordered_a < ordered_b ? 0x80000000u : 0x20000000u;
        };
        auto compare = [](unsigned sd, unsigned sm, unsigned cond = 14) {
            return (cond << 28) | 0x0EB40A40u | ((sd >> 1) << 12) | ((sd & 1) << 22) | (sm >> 1) |
                   ((sm & 1) << 5);
        };
        const std::array<u32, 20> edges{0,           0x80000000u, 1,           0x80000001u,
                                        0x007fffffu, 0x807fffffu, 0x00800000u, 0x80800000u,
                                        0x3f800000u, 0xbf800000u, 0x7f7fffffu, 0xff7fffffu,
                                        0x7f800000u, 0xff800000u, 0x7fc00001u, 0xffc00001u,
                                        0x7f800001u, 0xff800001u, 0x7fffffffu, 0xffffffffu};
        u64 checksum = 1469598103934665603ull;
        unsigned cases = 0;
        auto hash = [&](u64 value) { checksum = (checksum ^ value) * 1099511628211ull; };
#ifdef __aarch64__
        u64 saved_fpcr, saved_fpsr;
        asm volatile("mrs %0, fpcr" : "=r"(saved_fpcr));
        asm volatile("mrs %0, fpsr" : "=r"(saved_fpsr));
        asm volatile("msr fpcr, %0" : : "r"(u64(0)));
#endif
        auto fixture = [&](unsigned prefix, unsigned sd, unsigned sm, u32 a, u32 b) {
            fast->SetPC(0x1000);
            fast->SetCPSR(0xa8000010);
            const u32 old_fpscr = 0xa0000000u | ((a ^ b) & 0x0fffffffu);
            fast->SetVFPSystemReg(VFP_FPSCR, old_fpscr);
            fast->SetVFPReg(sd, a);
            fast->SetVFPReg(sm, b);
            if (sd == sm)
                a = b;
#ifdef __aarch64__
            asm volatile("msr fpsr, %0" : : "r"(u64(0)));
#endif
            timing.GetTimer(0)->Advance(128);
            fast->Run();
            const u32 expected = expected_flags(a, b);
            if (fast->GetCPSR() != (expected | 0x08000010u) ||
                fast->GetVFPSystemReg(VFP_FPSCR) != ((old_fpscr & 0x0fffffffu) | expected) ||
                fast->GetVFPReg(sd) != a || fast->GetVFPReg(sm) != b ||
                fast->GetPC() != 0x1000 + (prefix + 2) * 4) {
                std::cerr << "VCMP/VMRS oracle mismatch case=" << cases << "\n";
                return false;
            }
            hash(fast->GetCPSR());
            hash(fast->GetVFPSystemReg(VFP_FPSCR));
#ifdef __aarch64__
            u64 fpsr;
            asm volatile("mrs %0, fpsr" : "=r"(fpsr));
            hash(fpsr);
#endif
            ++cases;
            return true;
        };
        auto prepare = [&](unsigned prefix, unsigned sd, unsigned sm) {
            code.fill(0xe1a03003);
            code[prefix] = compare(sd, sm);
            code[prefix + 1] = 0xeef1fa10;
            code[prefix + 2] = 0xeafffffe;
            fast->ClearInstructionCache();
        };
        auto half_bits_to_single = [](u32 bits) {
            const u32 sign = (bits & 0x8000u) << 16;
            u32 exponent = (bits >> 10) & 31u, fraction = bits & 1023u;
            if (exponent == 31)
                return sign | 0x7f800000u | (fraction << 13);
            if (!exponent) {
                if (!fraction)
                    return sign;
                int unbiased = -14;
                while (!(fraction & 1024u)) {
                    fraction <<= 1;
                    --unbiased;
                }
                return sign | (u32(unbiased + 127) << 23) | ((fraction & 1023u) << 13);
            }
            return sign | ((exponent + 112) << 23) | (fraction << 13);
        };
        prepare(0, 0, 1);
        // Exhaust all 65,536 high halves (all signs/exponents), and all low
        // halves (subnormal mantissas), with independent changing counterparts.
        for (u32 bits = 0; bits < 65536; ++bits) {
            if (!fixture(0, 0, 1, bits << 16, edges[bits % edges.size()]) ||
                !fixture(0, 0, 1, bits, edges[(bits / 7) % edges.size()]) ||
                !fixture(0, 0, 1, half_bits_to_single(bits), edges[(bits / 11) % edges.size()]))
                return 1;
        }
        for (unsigned prefix : {0u, 29u, 30u, 31u})
            for (auto layout : {std::array<unsigned, 2>{0, 1}, {31, 30}, {0, 0}}) {
                prepare(prefix, layout[0], layout[1]);
                for (u32 a : edges)
                    for (u32 b : edges)
                        if (!fixture(prefix, layout[0], layout[1], a, b))
                            return 1;
            }
        // Conditional constituents and VMRS-to-general-register are ineligible.
        for (unsigned variant = 0; variant < 3; ++variant) {
            code.fill(0xe1a03003);
            code[0] = compare(0, 1, variant == 0 ? 0 : 14);
            code[1] = variant == 1 ? 0x0ef1fa10u : variant == 2 ? 0xeef13a10u : 0xeef1fa10u;
            code[2] = 0xeafffffe;
            fast->ClearInstructionCache();
            fast->SetPC(0x1000);
            fast->SetCPSR(0x08000010);
            fast->SetVFPSystemReg(VFP_FPSCR, 0x60000005);
            fast->SetVFPReg(0, 0x3f800000);
            fast->SetVFPReg(1, 0x40000000);
            fast->SetReg(3, 0);
            timing.GetTimer(0)->Advance(128);
            fast->Run();
            const u32 expected_fpscr = variant == 0 ? 0x60000005u : 0x80000005u;
            const u32 expected_cpsr = variant == 0 ? 0x68000010u : 0x08000010u;
            if (fast->GetVFPSystemReg(VFP_FPSCR) != expected_fpscr ||
                fast->GetCPSR() != expected_cpsr ||
                (variant == 2 && fast->GetReg(3) != expected_fpscr)) {
                std::cerr << "VCMP/VMRS ineligible constituent mismatch\n";
                return 1;
            }
        }
        for (unsigned prefix : {0u, 28u, 29u, 30u, 31u}) {
            code.fill(0xe1a03003);
            code[prefix] = compare(0, 1);
            code[prefix + 1] = 0xeef1fa10;
            code[prefix + 2] = 0xe2800001;
            code[prefix + 3] = 0xeafffffb; // branch back to VCMP, real work each loop
            fast->ClearInstructionCache();
            fast->SetPC(0x1000);
            fast->SetCPSR(0x10);
            fast->SetReg(0, 0);
            for (unsigned slice = 0; slice < 128; ++slice) {
                const std::array<unsigned, 8> budgets{1, 2, 3, 4, 5, 31, 32, 128};
                fast->SetVFPReg(0, edges[slice % edges.size()]);
                fast->SetVFPReg(1, edges[(slice * 7) % edges.size()]);
                timing.GetTimer(0)->Advance(budgets[slice % budgets.size()]);
                const auto before = timing.GetTimer(0)->GetDowncount();
                fast->Run();
                hash(fast->GetPC());
                hash(fast->GetReg(0));
                hash(fast->GetCPSR());
                hash(fast->GetVFPSystemReg(VFP_FPSCR));
                hash(before - timing.GetTimer(0)->GetDowncount());
            }
        }
        std::ostringstream profile;
        fast->WriteBlockProfile(profile);
        const auto marker =
            "\"opcodes\":[" + std::to_string(unsigned(Core::FastInterp::Opcode::VcmpVmrsS)) + ",";
        const char* flag = std::getenv("CARA_FASTINTERP_VCMP_VMRS");
        const bool enabled = flag && std::strcmp(flag, "1") == 0;
        if ((profile.str().find(marker) != std::string::npos) != enabled) {
            std::cerr << "VCMP/VMRS activation mismatch\n";
            return 1;
        }
#ifdef __aarch64__
        asm volatile("msr fpcr, %0" : : "r"(saved_fpcr));
        asm volatile("msr fpsr, %0" : : "r"(saved_fpsr));
#endif
        std::cout << "VCMP/VMRS bit-oracle cases=" << cases
                  << " quantum_slices=640 trace=" << checksum << "\n";
        return 0;
    }
    auto vmul = [](unsigned d, unsigned n, unsigned m, unsigned cond = 14) {
        return (cond << 28) | 0x0E200A00 | ((d >> 1) << 12) | ((d & 1) << 22) | ((n >> 1) << 16) |
               ((n & 1) << 7) | (m >> 1) | ((m & 1) << 5);
    };
    if (std::getenv("CARA_FASTINTERP_SIMD_BENCH")) {
        code = {vmul(0, 8, 16), vmul(1, 9, 16), vmul(2, 10, 16), vmul(3, 11, 16),
                0xE2500001,     0x1AFFFFF9,     0xEAFFFFFE};
        for (unsigned i = 0; i < 64; ++i)
            fast->SetVFPReg(i, 0x3F800000 + i * 512);
        fast->ClearInstructionCache();
        fast->SetCPSR(0x10);
        fast->SetPC(0x1000);
        fast->SetReg(0, 20000000);
        const auto start = std::chrono::steady_clock::now();
        while (fast->GetReg(0)) {
            timing.GetTimer(0)->Advance(100000);
            fast->Run();
        }
        std::cout << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                  << '\n';
        fast->WriteBlockProfile(std::cerr);
        return 0;
    }
    if (std::getenv("CARA_FASTINTERP_MEMORY_BENCH")) {
        std::array<u32, 1024> source{}, destination{};
        table->attributes[3] = table->attributes[4] = Memory::PageType::Memory;
        table->pointers[3] = TestMemory(source);
        table->pointers[4] = TestMemory(destination);
        for (unsigned i = 0; i < source.size(); ++i)
            source[i] = i * 977 + 131;
        struct Fixture {
            const char* name;
            u32 load, store;
        };
        const std::array<Fixture, 6> fixtures{{
            {"conditional_bkpt", 0x31200A76, 0xE1A03003},
            {"ldm_stm_8", 0xE89107F8, 0xE88207F8},
            {"ldm_stm_3", 0xE8910038, 0xE8820038},
            {"ldm_stm_4", 0xE8910078, 0xE8820078},
            {"vldm_vstm_s16", 0xEC910A10, 0xEC820A10},
            {"vldm_vstm_d8", 0xEC910B10, 0xEC820B10},
        }};
        std::cout << "[";
        for (size_t fixture = 0; fixture < fixtures.size(); ++fixture) {
            const auto& f = fixtures[fixture];
            code = {f.load, f.store, 0xE2500001, 0x1AFFFFFB, 0xEAFFFFFE};
            fast->ClearInstructionCache();
            fast->SetCPSR(0x10);
            fast->SetPC(0x1000);
            fast->SetReg(0, 20000000);
            fast->SetReg(1, 0x3000);
            fast->SetReg(2, 0x4000);
            const auto start = std::chrono::steady_clock::now();
            while (fast->GetReg(0)) {
                timing.GetTimer(0)->Advance(100000);
                fast->Run();
            }
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (fixture && destination[0] != source[0]) {
                std::cerr << "memory benchmark result mismatch\n";
                return 1;
            }
            std::cout << (fixture ? "," : "") << "{\"name\":\"" << f.name
                      << "\",\"seconds\":" << seconds << "}";
        }
        std::cout << "]\n";
        return 0;
    }
    // Keep the original decode limits, PC-prefix slots and chaining quantum.
    // This synthetic mixed arithmetic loop records the baseline at 100% guest clock.
    {
        for (unsigned i = 0; i < 100; ++i)
            code[i] =
                vmul(i % 4, 8 + i % 4, 16) & ((i >= 40 && i < 80) ? ~0x00200000u : 0xffffffffu);
        for (unsigned i : {30u, 59u, 61u, 89u})
            code[i] = 0xED9FCA00 | (198 - i);
        code[100] = 0xE2800001;
        code[101] = 0xEAFFFF99;
        code[200] = 0x3F800000;
        for (unsigned i = 0; i < 64; ++i)
            fast->SetVFPReg(i, 0x3F800000);
        fast->SetVFPReg(16, 0x40000000);
        fast->ClearInstructionCache();
        fast->SetPC(0x1000);
        fast->SetCPSR(0x10);
        fast->SetReg(0, 0);
        const std::array<unsigned, 5> budgets{1, 7, 31, 128, 1000};
        const char* chain_flag = std::getenv("CARA_FASTINTERP_CHAIN_BUDGET");
        const bool bounded_chain = chain_flag && std::strcmp(chain_flag, "1") == 0;
        const std::array<unsigned, 5> starts{0, 30, 59, 88, 102};
        std::array<unsigned, 4> model_counts{};
        std::array<u32, 32> model_fp{};
        model_fp.fill(0x3f800000);
        model_fp[16] = 0x40000000;
        unsigned model_stage = 0;
        u32 model_r0 = 0;
        u64 trace_hash = 1469598103934665603ull;
        auto record = [&trace_hash](u64 value) {
            trace_hash = (trace_hash ^ value) * 1099511628211ull;
        };
        for (unsigned i = 0; i < 64; ++i) {
            timing.GetTimer(0)->Advance(budgets[i % budgets.size()]);
            const auto before = timing.GetTimer(0)->GetDowncount();
            fast->Run();
            if (bounded_chain) {
                // Independent block-budget oracle: these finite operations
                // multiply1*2, or accumulate2; each guest instruction is1tick.
                // Literal loads write S24=1, ADD increments r0, B closes loop.
                s64 model_ticks = 0;
                while (model_ticks < before) {
                    const unsigned begin = starts[model_stage], end = starts[model_stage + 1];
                    for (unsigned instruction = begin; instruction < end; ++instruction) {
                        if (instruction == 100) ++model_r0;
                        else if (instruction < 100) {
                            if (instruction == 30 || instruction == 59 ||
                                instruction == 61 || instruction == 89) model_fp[24] = 0x3f800000;
                            else {
                                const unsigned reg = instruction % 4;
                                const float value = instruction >= 40 && instruction < 80
                                    ? std::bit_cast<float>(model_fp[reg]) + 2.0f : 2.0f;
                                model_fp[reg] = std::bit_cast<u32>(value);
                            }
                        }
                    }
                    model_ticks += end - begin;
                    ++model_counts[model_stage];
                    model_stage = (model_stage + 1) % 4;
                }
                bool passed = fast->GetPC() == 0x1000 + starts[model_stage] * 4 &&
                              fast->GetReg(0) == model_r0 &&
                              before - timing.GetTimer(0)->GetDowncount() == model_ticks;
                for (unsigned reg = 0; reg < 32; ++reg)
                    passed &= fast->GetVFPReg(reg) == model_fp[reg];
                if (!passed) {
                    std::cerr << "SIMD independent block-budget oracle mismatch slice=" << i << '\n';
                    return 1;
                }
            }
            record(fast->GetPC());
            record(fast->GetReg(0));
            record(before - timing.GetTimer(0)->GetDowncount());
            for (unsigned r = 0; r < 32; ++r)
                record(fast->GetVFPReg(r));
        }
        if (!bounded_chain && trace_hash != 7296223465908193166ull) {
            std::cerr << "SIMD event-boundary trace mismatch: " << trace_hash << '\n';
            return 1;
        }
        std::ostringstream profile;
        fast->WriteBlockProfile(profile);
        if (bounded_chain) {
            for (unsigned stage = 0; stage < 4; ++stage) {
                const auto expected = "\"pc\":" + std::to_string(0x1000 + starts[stage] * 4) +
                    ",\"end_pc\":" + std::to_string(0x1000 + starts[stage + 1] * 4) +
                    ",\"executions\":" + std::to_string(model_counts[stage]);
                if (profile.str().find(expected) == std::string::npos) {
                    std::cerr << "SIMD model block count mismatch\n";
                    return 1;
                }
            }
        } else {
        for (const char* expected : {"\"pc\":4096,\"end_pc\":4216,\"executions\":367",
                                     "\"pc\":4216,\"end_pc\":4332,\"executions\":366",
                                     "\"pc\":4332,\"end_pc\":4448,\"executions\":366",
                                     "\"pc\":4448,\"end_pc\":4504,\"executions\":366"}) {
            if (profile.str().find(expected) == std::string::npos) {
                std::cerr << "SIMD decoded-block boundary/count mismatch\n";
                return 1;
            }
        }
        }
        std::cout << "64 mixed-budget SIMD trace slices and four block boundaries match "
                  << (bounded_chain ? "independent budget oracle" : "baseline") << '\n';
    }
#ifdef __aarch64__
    if (const char* simd = std::getenv("CARA_FASTINTERP_VFP_SIMD");
        simd && (std::strcmp(simd, "1") == 0 || std::strcmp(simd, "2") == 0 ||
                 std::strcmp(simd, "3") == 0)) {
        if (std::strcmp(simd, "3") == 0 && VerifyFastInterpFusedMultiplyAdd() != 0)
            return 1;
        const std::array<u32, 24> edges{{0,          0x80000000, 1,          0x80000001, 0x007fffff,
                                         0x807fffff, 0x00800000, 0x80800000, 0x3f800000, 0xbf800000,
                                         0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00001,
                                         0xffc00001, 0x7fa12345, 0xffa54321, 0x7fc12345, 0xffc54321,
                                         0x3eaaaaab, 0x3f000001, 0x00800001, 0x7f7ffffe}};
        struct Registers {
            unsigned d, n, m;
            bool left;
        };
        const std::array<Registers, 12> layouts{{{0, 8, 16, false},
                                                 {0, 8, 16, true},
                                                 {8, 8, 16, false},
                                                 {8, 8, 16, true},
                                                 {9, 8, 16, false},
                                                 {9, 8, 16, true},
                                                 {8, 9, 16, false},
                                                 {8, 9, 16, true},
                                                 {0, 8, 1, false},
                                                 {0, 8, 1, true},
                                                 {28, 4, 0, false},
                                                 {28, 4, 0, true}}};
        u64 fpcr_saved, fpsr_saved;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr_saved));
        asm volatile("mrs %0, fpsr" : "=r"(fpsr_saved));
        u32 random = 0x28951c17;
        unsigned count = 0;
        for (bool accumulate : {false, true})
            for (unsigned mode = 0; mode < 16; ++mode) {
                const u64 fpcr = (fpcr_saved & ~((3ull << 22) | (1ull << 24) | (1ull << 25))) |
                                 ((u64(mode & 3)) << 22) | ((u64(mode & 4)) << 22) |
                                 ((u64(mode & 8)) << 22);
                asm volatile("msr fpcr, %0" ::"r"(fpcr));
                for (const auto layout : layouts)
                    for (unsigned trial = 0; trial < 192; ++trial) {
                        std::array<u32, 64> before{}, expected{};
                        for (unsigned i = 0; i < 64; ++i) {
                            random ^= random << 13;
                            random ^= random >> 17;
                            random ^= random << 5;
                            before[i] = trial < 96 ? edges[(trial + i) % edges.size()] : random;
                        }
                        for (unsigned i = 0; i < 4; ++i) {
                            unsigned n = layout.left ? layout.m : layout.n + i,
                                     m = layout.left ? layout.n + i : layout.m;
                            unsigned cond = trial % 8 == 0 ? (i == 2 ? 0 : 14) : 14;
                            const unsigned d = layout.d + ((trial & 16)  ? (i & 1)
                                                           : (trial & 2) ? (i ^ 2)
                                                                         : i);
                            if (trial & 4) {
                                n = (n + 2) % 32;
                                m = (m + 3) % 32;
                            }
                            code[i] =
                                vmul(d, n, m, cond) & (accumulate ? ~0x00200000u : 0xffffffffu);
                        }
                        code[4] = 0xEAFFFFFE;
                        for (unsigned i = 0; i < 64; ++i)
                            fast->SetVFPReg(i, before[i]);
                        fast->ClearInstructionCache();
                        fast->SetCPSR(0xA0000010);
                        fast->SetPC(0x1000);
                        asm volatile("msr fpsr, xzr");
                        const auto scalar_tick_start = timing.GetTimer(0)->GetDowncount();
                        for (unsigned i = 0; i < 5; ++i)
                            fast->Step(); // Four multiplies and the final branch.
                        const auto expected_ticks =
                            scalar_tick_start - timing.GetTimer(0)->GetDowncount();
                        u64 expected_fpsr;
                        asm volatile("mrs %0, fpsr" : "=r"(expected_fpsr));
                        for (unsigned i = 0; i < 64; ++i)
                            expected[i] = fast->GetVFPReg(i);
                        for (unsigned i = 0; i < 64; ++i)
                            fast->SetVFPReg(i, before[i]);
                        fast->ClearInstructionCache();
                        fast->SetCPSR(0xA0000010);
                        fast->SetPC(0x1000);
                        timing.GetTimer(0)->Advance(1);
                        asm volatile("msr fpsr, xzr");
                        const auto run_tick_start = timing.GetTimer(0)->GetDowncount();
                        fast->Run();
                        const auto actual_ticks =
                            run_tick_start - timing.GetTimer(0)->GetDowncount();
                        u64 actual_fpsr;
                        asm volatile("mrs %0, fpsr" : "=r"(actual_fpsr));
                        bool ok = fast->GetPC() == 0x1010 && fast->GetCPSR() == 0xA0000010;
                        if (actual_ticks != expected_ticks || actual_fpsr != expected_fpsr) {
                            std::cerr << "SIMD tick/FPSR mismatch: ticks " << actual_ticks << '/'
                                      << expected_ticks << ", FPSR " << actual_fpsr << '/'
                                      << expected_fpsr << '\n';
                            ok = false;
                        }
                        for (unsigned i = 0; i < 64; ++i)
                            if (fast->GetVFPReg(i) != expected[i]) {
                                std::cerr << "mismatch mode=" << mode << " trial=" << trial
                                          << " d=" << layout.d << " n=" << layout.n
                                          << " m=" << layout.m << " left=" << layout.left
                                          << " reg=" << i << " expected=" << std::hex << expected[i]
                                          << " actual=" << fast->GetVFPReg(i) << '\n';
                                ok = false;
                                break;
                            }
                        if (!ok) {
                            asm volatile("msr fpcr, %0" ::"r"(fpcr_saved));
                            asm volatile("msr fpsr, %0" ::"r"(fpsr_saved));
                            return 1;
                        }
                        ++count;
                    }
            }
        asm volatile("msr fpcr, %0" ::"r"(fpcr_saved));
        asm volatile("msr fpsr, %0" ::"r"(fpsr_saved));
        std::cout << count << " SIMD versus scalar execution oracle cases passed\n";
    }
#endif
    // Invalidating any constituent word must discard a cached fused instruction.
    for (unsigned changed = 0; changed < 4; ++changed) {
        code = {vmul(0, 8, 16), vmul(1, 9, 16), vmul(2, 10, 16), vmul(3, 11, 16), 0xEAFFFFFE};
        for (unsigned i = 0; i < 64; ++i)
            fast->SetVFPReg(i, 0x3F800000);
        fast->SetVFPReg(16, 0x40000000);
        fast->SetVFPReg(17, 0x40400000);
        fast->ClearInstructionCache();
        fast->SetCPSR(0x10);
        fast->SetPC(0x1000);
        timing.GetTimer(0)->Advance(100);
        fast->Run();
        code[changed] = vmul(changed, 8 + changed, 17);
        fast->InvalidateCacheRange(0x1000 + changed * 4, 4);
        fast->SetPC(0x1000);
        timing.GetTimer(0)->Advance(100);
        fast->Run();
        for (unsigned i = 0; i < 4; ++i) {
            if (fast->GetVFPReg(i) != (i == changed ? 0x40400000u : 0x40000000u)) {
                std::cerr << "SIMD interior-word invalidation failed\n";
                return 1;
            }
        }
    }
    // A four-instruction candidate straddling separately backed code pages must
    // use the ordinary decode path and retain correct source/PC behavior.
    {
        std::array<u32, 1024> next_code{};
        table->attributes[2] = Memory::PageType::Memory;
        table->pointers[2] = TestMemory(next_code);
        for (unsigned words_before_boundary : {1u, 2u, 3u}) {
            const u32 start = 0x2000 - words_before_boundary * 4;
            for (unsigned i = 0; i < 5; ++i) {
                const unsigned word = 1024 - words_before_boundary + i;
                const u32 raw = i == 4 ? 0xEAFFFFFE : vmul(i, 8 + i, 16);
                if (word < 1024)
                    code[word] = raw;
                else
                    next_code[word - 1024] = raw;
            }
            for (unsigned i = 0; i < 64; ++i)
                fast->SetVFPReg(i, 0x3F800000);
            fast->SetVFPReg(16, 0x40000000);
            fast->ClearInstructionCache();
            fast->SetCPSR(0x10);
            fast->SetPC(start);
            timing.GetTimer(0)->Advance(100);
            fast->Run();
            bool passed = fast->GetPC() == start + 16;
            for (unsigned i = 0; i < 4; ++i)
                passed &= fast->GetVFPReg(i) == 0x40000000;
            if (!passed) {
                std::cerr << "SIMD cross-page decode failed\n";
                return 1;
            }
        }
        fast->ClearInstructionCache();
        table->attributes[2] = Memory::PageType::Unmapped;
        table->pointers[2] = nullptr;
    }
    std::cout << "7 SIMD code invalidation and code-page boundary cases passed\n";
    u32 seed = 0x4321abcd;
    auto random = [&] {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    };
    unsigned count = 0;
    auto check = [&](u32 ins, bool thumb) {
        code[0] = ins;
        const u32 cpsr = (random() & 0xF0000000) | 0x10 | (thumb ? 0x20 : 0);
        old->Cpsr = cpsr;
        fast->SetCPSR(cpsr);
        for (unsigned i = 0; i < 15; ++i) {
            old->Reg[i] = random();
            fast->SetReg(i, old->Reg[i]);
        }
        old->Reg[15] = 0x1000;
        fast->SetPC(0x1000 | thumb);
        InvalidateDynComDecodedCache();
        old->instruction_cache.clear();
        fast->ClearInstructionCache();
        old->NumInstrsToExecute = 1;
        InterpreterMainLoop(old.get(), timing.GetTimer(0).get());
        fast->Step();
        for (int i = 0; i < 16; ++i) {
            if (old->Reg[i] != fast->GetReg(i)) {
                std::cerr << "register mismatch instruction=" << std::hex << ins
                          << " thumb=" << thumb << " r" << std::dec << i << " old=" << std::hex
                          << old->Reg[i] << " fast=" << fast->GetReg(i) << '\n';
                return false;
            }
        }
        if ((old->Cpsr & 0xF0000020) != (fast->GetCPSR() & 0xF0000020)) {
            std::cerr << "flags mismatch instruction=" << std::hex << ins << " old=" << old->Cpsr
                      << " fast=" << fast->GetCPSR() << '\n';
            return false;
        }
        ++count;
        return true;
    };
    // Predicated ALU, every operation, flag-setting variant, rotated immediates and shifted
    // registers.
    for (unsigned trial = 0; trial < 18000; ++trial) {
        const u32 op = random() % 16;
        const u32 cond = random() % 15;
        const bool flag = (op >= 8 && op <= 11) || (random() & 1);
        const u32 rd = (op >= 8 && op <= 11) ? 0 : random() % 13;
        u32 ins = (cond << 28) | (op << 21) | (flag << 20) | ((random() % 13) << 16) | (rd << 12);
        if (random() & 1)
            ins |= (1u << 25) | (random() & 0xFFF);
        else
            ins |= ((random() & 31) << 7) | ((random() & 3) << 5) | (random() % 13);
        if (!check(ins, false))
            return 1;
    }
    for (unsigned trial = 0; trial < 4000; ++trial) {
        // Thumb immediate ALU and register ALU, low registers only.
        u32 ins = (random() & 1) ? (0x2000 | (random() & 0x1FFF)) : (0x4000 | (random() & 0x3FF));
        if (!check(ins, true))
            return 1;
    }
    if (const char* enabled = std::getenv("CARA_FASTINTERP_DECODE_BKPT");
        enabled && std::strcmp(enabled, "1") == 0) {
        // Legacy DynCom-compatible BKPT behavior for every condition and varied
        // immediates. A32 non-AL BKPT is not a portable hardware contract; this
        // explicitly tests the emulator's established handling of those words.
        for (unsigned trial = 0; trial < 1024; ++trial) {
            const u32 immediate = random() & 0xffff;
            const u32 ins = ((random() % 15) << 28) | 0x01200070 | ((immediate & 0xfff0) << 4) |
                            (immediate & 15);
            if (!check(ins, false))
                return 1;
        }
    }
    // Whole-block execution exercises fusion, chaining and a finite ARM loop.
    const std::array<u32, 6> loop{0xE3A00000, 0xE3A01014, 0xE0800001,
                                  0xE2511001, 0x1AFFFFFC, 0xEAFFFFFE};
    std::copy(loop.begin(), loop.end(), code.begin());
    fast->ClearInstructionCache();
    fast->SetCPSR(0x10);
    fast->SetPC(0x1000);
    timing.GetTimer(0)->Advance(2000);
    fast->Run();
    if (fast->GetReg(0) != 210 || fast->GetReg(1) != 0 || fast->GetPC() != 0x1014) {
        std::cerr << "fused loop failed sum=" << fast->GetReg(0) << " remaining=" << fast->GetReg(1)
                  << " pc=" << std::hex << fast->GetPC() << '\n';
        return 1;
    }
    // Separate backing allocations make out-of-page host reads/writes observable.
    std::array<u8, 4096> page_a{}, page_b{};
    table->attributes[3] = table->attributes[4] = Memory::PageType::Memory;
    table->pointers[3] = TestMemory(page_a);
    table->pointers[4] = TestMemory(page_b);
    page_a[4095] = 0x12;
    page_b[0] = 0x34;
    page_b[1] = 0x56;
    page_b[2] = 0x78;
    code[0] = 0xE5910000; // LDR r0, [r1], including a page crossing
    fast->ClearInstructionCache();
    fast->SetCPSR(0x10);
    fast->SetPC(0x1000);
    fast->SetReg(1, 0x3FFF);
    fast->Step();
    if (fast->GetReg(0) != 0x78563412) {
        std::cerr << "cross-page load failed\n";
        return 1;
    }
    code[0] = 0xE5810000; // STR r0, [r1]
    fast->ClearInstructionCache();
    fast->SetPC(0x1000);
    fast->SetReg(0, 0xAABBCCDD);
    fast->Step();
    if (page_a[4095] != 0xDD || page_b[0] != 0xCC || page_b[1] != 0xBB || page_b[2] != 0xAA) {
        std::cerr << "cross-page store failed\n";
        return 1;
    }
    // VFP multiple transfers: exact bit patterns, IA/DB, writeback, unaligned
    // inputs and separately allocated pages. Also verifies the burst fallback.
    unsigned transfers = 0;
    for (bool is_double : {false, true})
        for (bool load : {false, true})
            for (bool decrement : {false, true})
                for (bool writeback : {false, true}) {
                    if (decrement && !writeback)
                        continue; // P=1,W=0 is VLDR/VSTR, not VLDM/VSTM.
                    for (unsigned registers : {1u, 2u, 4u, 8u, 16u})
                        for (unsigned offset : {0u, 3u, 4000u, 4095u}) {
                            const unsigned width = is_double ? 8 : 4,
                                           first = (is_double ? 16 : 32) - registers;
                            const u32 start = 0x3000 + offset,
                                      base = decrement ? start + registers * width : start;
                            std::array<u8, 8192> expected{};
                            for (unsigned i = 0; i < 8192; ++i)
                                expected[i] = u8(i * 13 + 37);
                            std::copy_n(expected.data(), 4096, page_a.data());
                            std::copy_n(expected.data() + 4096, 4096, page_b.data());
                            std::array<u32, 64> fp{};
                            for (unsigned i = 0; i < 64; ++i) {
                                fp[i] = 0x80000000u + i * 0x01010101u;
                                fast->SetVFPReg(i, fp[i]);
                            }
                            const unsigned first_word = first * (is_double ? 2 : 1);
                            if (load)
                                std::memcpy(fp.data() + first_word, expected.data() + offset,
                                            registers * width);
                            else
                                std::memcpy(expected.data() + offset, fp.data() + first_word,
                                            registers * width);
                            const u32 vd = is_double ? (first & 15) : (first >> 1),
                                      d = is_double ? (first >> 4) : (first & 1);
                            code[0] = (decrement ? 0xED000A00 : 0xEC800A00) |
                                      (load ? 1u << 20 : 0) | (writeback ? 1u << 21 : 0) |
                                      (is_double ? 1u << 8 : 0) | (2u << 16) | (vd << 12) |
                                      (d << 22) | (registers * (width / 4));
                            fast->ClearInstructionCache();
                            fast->SetCPSR(0xA0000010);
                            fast->SetPC(0x1000);
                            fast->SetReg(2, base);
                            fast->Step();
                            bool passed =
                                fast->GetPC() == 0x1004 && fast->GetCPSR() == 0xA0000010 &&
                                fast->GetReg(2) ==
                                    (writeback ? (decrement ? start : start + registers * width)
                                               : base);
                            for (unsigned i = 0; i < 64; ++i)
                                passed &= fast->GetVFPReg(i) == fp[i];
                            passed &= std::memcmp(page_a.data(), expected.data(), 4096) == 0 &&
                                      std::memcmp(page_b.data(), expected.data() + 4096, 4096) == 0;
                            if (!passed) {
                                std::cerr
                                    << "VFP multiple transfer mismatch instruction=" << std::hex
                                    << code[0] << " address=" << start << '\n';
                                return 1;
                            }
                            ++transfers;
                        }
                }
    // LDM/STM with sparse and dense lists. Include a final word starting at the
    // last byte of a host page, so same-page checks cover the complete word.
    for (bool load : {false, true})
        for (bool writeback : {false, true})
            for (u16 mask : {u16(1), u16(0x38), u16(0x7f8), u16(0x5018), u16(0x3fff)})
                for (unsigned offset : {0u, 3u, 4067u, 4095u}) {
                    std::array<u8, 8192> expected{};
                    for (unsigned i = 0; i < 8192; ++i)
                        expected[i] = u8(i * 7 + 19);
                    std::copy_n(expected.data(), 4096, page_a.data());
                    std::copy_n(expected.data() + 4096, 4096, page_b.data());
                    std::array<u32, 15> regs{};
                    for (unsigned i = 0; i < 15; ++i) {
                        regs[i] = 0x03020100 + i * 0x01010101;
                        fast->SetReg(i, regs[i]);
                    }
                    regs[2] = 0x3000 + offset;
                    fast->SetReg(2, regs[2]);
                    unsigned bytes = 0;
                    for (unsigned i = 0; i < 15; ++i)
                        if (mask & (1u << i)) {
                            if (load)
                                std::memcpy(&regs[i], expected.data() + offset + bytes, 4);
                            else
                                std::memcpy(expected.data() + offset + bytes, &regs[i], 4);
                            bytes += 4;
                        }
                    if (writeback)
                        regs[2] = 0x3000 + offset + bytes;
                    code[0] =
                        0xE8820000 | (load ? 1u << 20 : 0) | (writeback ? 1u << 21 : 0) | mask;
                    fast->ClearInstructionCache();
                    fast->SetCPSR(0xA0000010);
                    fast->SetPC(0x1000);
                    fast->Step();
                    bool passed = fast->GetPC() == 0x1004 && fast->GetCPSR() == 0xA0000010;
                    for (unsigned i = 0; i < 15; ++i)
                        passed &= fast->GetReg(i) == regs[i];
                    passed &= std::memcmp(page_a.data(), expected.data(), 4096) == 0 &&
                              std::memcmp(page_b.data(), expected.data() + 4096, 4096) == 0;
                    if (!passed) {
                        std::cerr << "ARM multiple transfer mismatch instruction=" << std::hex
                                  << code[0] << " address=" << (0x3000 + offset) << '\n';
                        return 1;
                    }
                    ++transfers;
                }
    // Marking a real VRAM mapping cached updates the same pointer array used by
    // FastInterp. Burst transfers must take the null-pointer coherence fallback.
    // This host fixture has no renderer: device tests separately cover GPU invalidation.
    memory.RegisterPageTable(table);
    memory.MapMemoryRegion(*table, Memory::VRAM_VADDR, 4096,
                           memory.GetPhysicalRef(Memory::VRAM_PADDR));
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR, 4096, true);
    const auto tracked_page = Memory::VRAM_VADDR >> Memory::PAGE_BITS;
    if (table->GetPointerArray()[tracked_page] != nullptr ||
        table->attributes[tracked_page] != Memory::PageType::RasterizerCachedMemory) {
        std::cerr << "Rasterizer tracking did not disable the direct RAM pointer\n";
        return 1;
    }
    auto* tracked_bytes = memory.GetPhysicalPointer(Memory::VRAM_PADDR);
    for (bool is_double : {false, true})
        for (bool load : {false, true}) {
            std::array<u32, 16> expected{};
            for (unsigned i = 0; i < 16; ++i) {
                const u32 ram = 0x01020408u * (i + 1), fp = ~ram;
                std::memcpy(tracked_bytes + i * 4, &ram, 4);
                fast->SetVFPReg(i, fp);
                expected[i] = load ? ram : fp;
            }
            code[0] = (load ? 0xEC910A10 : 0xEC810A10) | (is_double ? 0x100 : 0);
            fast->ClearInstructionCache();
            fast->SetCPSR(0x10);
            fast->SetPC(0x1000);
            fast->SetReg(1, Memory::VRAM_VADDR);
            fast->Step();
            bool passed = table->GetPointerArray()[tracked_page] == nullptr;
            for (unsigned i = 0; i < 16; ++i) {
                u32 ram;
                std::memcpy(&ram, tracked_bytes + i * 4, 4);
                passed &= (load ? fast->GetVFPReg(i) : ram) == expected[i];
            }
            if (!passed) {
                std::cerr << "Tracked VFP transfer fallback failed\n";
                return 1;
            }
        }
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR, 4096, false);
    if (table->GetPointerArray()[tracked_page] == nullptr) {
        std::cerr << "Untracking did not restore the direct RAM pointer\n";
        return 1;
    }
    memory.UnmapRegion(*table, Memory::VRAM_VADDR, 4096);
    memory.UnregisterPageTable(table);
    std::cout << "4 rasterizer-tracked VFP transfer fallbacks and pointer transitions passed\n";
    std::cout << transfers << " scalar/VFP multiple transfers and host-page boundaries passed\n";
    auto context = fast->NewContext();
    fast->SaveContext(context);
    const u32 saved = fast->GetReg(7), saved_flags = fast->GetCPSR();
    fast->SetReg(7, ~saved);
    fast->SetCPSR(0x10);
    fast->LoadContext(context);
    if (fast->GetReg(7) != saved || fast->GetCPSR() != saved_flags)
        return 1;
    // Same PC in a different process must not reuse the previous decode.
    auto second = std::make_shared<Memory::PageTable>();
    std::array<u32, 1024> other{0xE3A0002A};
    second->attributes[1] = Memory::PageType::Memory;
    second->pointers[1] = TestMemory(other);
    memory.SetCurrentPageTable(second);
    fast->SetPageTable(second);
    fast->SetCPSR(0x10);
    fast->SetPC(0x1000);
    fast->Step();
    if (fast->GetReg(0) != 42)
        return 1;
    std::cout << count
              << " ARM/Thumb differential instruction cases, fused loop, cross-page RAM, context "
                 "round-trip and address-space switch passed\n";
}
