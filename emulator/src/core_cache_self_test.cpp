// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <string>
#include <iostream>
#include <memory>
#include "core/arm/dyncom/arm_dyncom_interpreter.h"
#include "core/arm/dyncom/arm_dyncom_trans.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/memory.h"

int main(int argc, char** argv) {
    Memory::MemorySystem memory;
    Core::Timing timing;
    auto& system = Core::System::GetInstance();
    auto first = std::make_unique<ARMul_State>(system, memory, USER32MODE);
    auto second = std::make_unique<ARMul_State>(system, memory, USER32MODE);
    auto first_table = std::make_unique<Memory::PageTable>();
    auto second_table = std::make_unique<Memory::PageTable>();
    // MOV r0, #17 / MOV r0, #29; B .  Same guest PC, distinct core address spaces.
    std::array<u32, 1024> first_code{0xE3A00011, 0xEAFFFFFE};
    std::array<u32, 1024> second_code{0xE3A0001D, 0xEAFFFFFE};
    first_table->attributes[1] = second_table->attributes[1] = Memory::PageType::Memory;
    first_table->pointers[1] = reinterpret_cast<u8*>(first_code.data());
    second_table->pointers[1] = reinterpret_cast<u8*>(second_code.data());
    const auto execute = [&](ARMul_State& cpu, Memory::PageTable* table, u32 expected, u32 pc = 0x1000) {
        memory.SetCurrentPageTable(table);
        cpu.Reg[15] = pc; cpu.Reg[0] = 0; cpu.NumInstrsToExecute = 2;
        InterpreterMainLoop(&cpu, timing.GetTimer(0).get());
        if (cpu.Reg[0] != expected) {
            std::cerr << "stale cross-core decode: expected " << expected << ", got " << cpu.Reg[0] << '\n';
            return false;
        }
        return true;
    };
    InvalidateDynComDecodedCache();
    if (!execute(*first, first_table.get(), 17)) return 1;
    for (unsigned i = 0; i < 8; ++i) {
        // A reset on core two overwrites the shared arena, while core one still
        // holds an offset into it. Core one must decode its own instruction again.
        InvalidateDynComDecodedCache();
        if (!execute(*second, second_table.get(), 29)) return 1;
        if (!execute(*first, first_table.get(), 17)) return 1;
    }
    // These PCs collide in the direct-mapped lookup; a tag mismatch must fall back.
    std::array<u32, 1024> collision_code{};
    collision_code[3] = 0xE3A0002B; collision_code[4] = 0xEAFFFFFE;
    first_table->attributes[2] = Memory::PageType::Memory;
    first_table->pointers[2] = reinterpret_cast<u8*>(collision_code.data());
    for (unsigned i = 0; i < 100; ++i) {
        if (!execute(*first, first_table.get(), 43, 0x200c)) return 2;
        if (!execute(*first, first_table.get(), 17)) return 2;
    }
    if (std::getenv("CARA_DISABLE_FAST_DECODE_LOOKUP") == nullptr) {
        // Warm colliding entries, then remove the hash-map fallback. A victim hit
        // must reuse decoded data rather than translating the instructions again.
        first->instruction_cache.clear();
        if (!execute(*first, first_table.get(), 43, 0x200c) ||
            !execute(*first, first_table.get(), 17) ||
            !execute(*first, first_table.get(), 43, 0x200c) ||
            !first->instruction_cache.empty()) {
            std::cerr << "victim lookup failed to reuse a colliding block\n"; return 4;
        }
        // Leave the first PC in the victim cache before invalidating the arena.
    }
    first_code[0] = 0xE3A00047;
    InvalidateDynComDecodedCache();
    if (!execute(*first, first_table.get(), 71)) return 3;
    // ARM -> Thumb -> ARM, both inside one slice and across single-step slices.
    // Debugger polling must not change instruction-set transitions or CPSR flags.
    alignas(4) std::array<u8, 4096> interworking{};
    const u32 bx_r1 = 0xE12FFF11, mrs_r3 = 0xE10F3000;
    const u32 adds_r0 = 0xE2900007, branch = 0xEAFFFFFE;
    const u16 thumb_mov = 0x2009, thumb_bx_r2 = 0x4710;
    std::memcpy(interworking.data(), &bx_r1, sizeof(bx_r1));
    std::memcpy(interworking.data() + 0x40, &thumb_mov, sizeof(thumb_mov));
    std::memcpy(interworking.data() + 0x42, &thumb_bx_r2, sizeof(thumb_bx_r2));
    std::memcpy(interworking.data() + 0x80, &mrs_r3, sizeof(mrs_r3));
    std::memcpy(interworking.data() + 0x84, &adds_r0, sizeof(adds_r0));
    std::memcpy(interworking.data() + 0x88, &branch, sizeof(branch));
    first_table->attributes[3] = Memory::PageType::Memory;
    first_table->pointers[3] = interworking.data();
    memory.SetCurrentPageTable(first_table.get());
    for (bool single_step : {false, true}) {
        InvalidateDynComDecodedCache();
        first->Cpsr = USER32MODE;
        first->Reg[0] = 0; first->Reg[1] = 0x3041; first->Reg[2] = 0x3080;
        first->Reg[15] = 0x3000;
        for (unsigned step = 0; step < (single_step ? 5u : 1u); ++step) {
            first->NumInstrsToExecute = single_step ? 1 : 5;
            InterpreterMainLoop(first.get(), timing.GetTimer(0).get());
            if (single_step && step < 2 && (first->Cpsr & 0x20) == 0) {
                std::cerr << "Thumb state lost across execution slices\n"; return 2;
            }
        }
        if (first->Reg[0] != 16 || first->Reg[3] != USER32MODE ||
            first->Reg[15] != 0x3088 || first->Cpsr != USER32MODE) {
            std::cerr << "ARM/Thumb/CPSR mismatch\n"; return 3;
        }
    }
    // A block with sixteen arithmetic operations exercises the straight-line
    // dispatch path, in addition to the branch-heavy microbenchmark below.
    std::array<u32, 1024> arithmetic{};
    for (unsigned i = 0; i < 16; ++i) arithmetic[i] = 0xE2800001; // ADD r0, r0, #1
    arithmetic[16] = 0xEAFFFFEE; // B back to the first ADD
    first_table->attributes[4] = Memory::PageType::Memory;
    first_table->pointers[4] = reinterpret_cast<u8*>(arithmetic.data());
    InvalidateDynComDecodedCache();
    first->Reg[15] = 0x4000; first->Reg[0] = 0; first->NumInstrsToExecute = 170;
    InterpreterMainLoop(first.get(), timing.GetTimer(0).get());
    if (first->Reg[0] != 160 || first->Reg[15] != 0x4000) {
        std::cerr << "Straight-line arithmetic/branch mismatch\n"; return 4;
    }
    // All ARM rotated immediates, both incoming carry values. A repeated 64-bit
    // pattern is an independent rotation oracle, including a zero-bit rotation.
    std::array<u32, 1024> immediate_code{};
    first_table->attributes[5] = Memory::PageType::Memory;
    first_table->pointers[5] = reinterpret_cast<u8*>(immediate_code.data());
    for (unsigned operand = 0; operand < 4096; ++operand) {
        for (unsigned carry = 0; carry < 2; ++carry) {
            immediate_code[0] = 0xE3B00000 | operand; // MOVS r0, #rotated immediate
            InvalidateDynComDecodedCache();
            first->Reg[15] = 0x5000; first->Reg[0] = 0;
            first->Cpsr = USER32MODE | (carry << 29) | 0x10000000;
            first->NumInstrsToExecute = 1;
            InterpreterMainLoop(first.get(), timing.GetTimer(0).get());
            const u64 imm = operand & 255;
            const unsigned shift = (operand >> 8) * 2;
            const u32 expected = static_cast<u32>((imm | (imm << 32)) >> shift);
            const u32 expected_carry = shift ? expected >> 31 : carry;
            const u32 flags = (expected & 0x80000000) | (expected == 0 ? 0x40000000 : 0) |
                              (expected_carry << 29) | 0x10000000;
            if (first->Reg[0] != expected || first->Cpsr != (USER32MODE | flags)) {
                std::cerr << "Rotated immediate/status mismatch: " << operand << ", carry " << carry << '\n';
                return 6;
            }
        }
    }
    if (argc == 2 && std::string(argv[1]) == "--benchmark") {
        first->Reg[15] = 0x3088;
        first->NumInstrsToExecute = 20000000;
        const auto started = std::chrono::steady_clock::now();
        InterpreterMainLoop(first.get(), timing.GetTimer(0).get());
        std::cout << "20M interpreted branches: "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
                  << " seconds\n";
        first->Reg[15] = 0x4000; first->Reg[0] = 0; first->NumInstrsToExecute = 20000000;
        const auto arithmetic_started = std::chrono::steady_clock::now();
        InterpreterMainLoop(first.get(), timing.GetTimer(0).get());
        if (first->Reg[0] != 18823530) { std::cerr << "Arithmetic benchmark result mismatch\n"; return 5; }
        std::cout << "20M mixed arithmetic/branch instructions: "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - arithmetic_started).count()
                  << " seconds\n";
    }
    memory.SetCurrentPageTable(nullptr);
    std::cout << "decoded-cache, ARM/Thumb/CPSR and 8192 immediate checks passed\n";
}
