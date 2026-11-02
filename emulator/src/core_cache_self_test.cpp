// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <iostream>
#include <memory>
#include "core/arm/dyncom/arm_dyncom_interpreter.h"
#include "core/arm/dyncom/arm_dyncom_trans.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/memory.h"

int main() {
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
    const auto execute = [&](ARMul_State& cpu, Memory::PageTable* table, u32 expected) {
        memory.SetCurrentPageTable(table);
        cpu.Reg[15] = 0x1000; cpu.Reg[0] = 0; cpu.NumInstrsToExecute = 2;
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
    memory.SetCurrentPageTable(nullptr);
    std::cout << "cross-core decoded-cache invalidation: 17 executions passed\n";
}
