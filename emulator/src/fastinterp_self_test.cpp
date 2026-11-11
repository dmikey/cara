// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstring>
#include <iostream>
#include <memory>
#include "core/arm/fastinterp/fastinterp.h"
#include "core/arm/dyncom/arm_dyncom_interpreter.h"
#include "core/arm/dyncom/arm_dyncom_trans.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core.h"

int main() {
    Memory::MemorySystem memory;
    Core::Timing timing;
    auto& system = Core::System::GetInstance();
    auto old = std::make_unique<ARMul_State>(system, memory, USER32MODE);
    auto fast = std::make_unique<Core::FastInterp::ARM_FastInterp>(system, memory, 0, timing.GetTimer(0));
    auto table = std::make_unique<Memory::PageTable>();
    std::array<u32, 1024> code{};
    table->attributes[1] = Memory::PageType::Memory;
    table->pointers[1] = reinterpret_cast<u8*>(code.data());
    memory.SetCurrentPageTable(table.get()); fast->SetPageTable(table.get());
    u32 seed = 0x4321abcd;
    auto random = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    unsigned count = 0;
    auto check = [&](u32 ins, bool thumb) {
        code[0] = ins;
        const u32 cpsr = (random() & 0xF0000000) | 0x10 | (thumb ? 0x20 : 0);
        old->Cpsr = cpsr; fast->SetCPSR(cpsr);
        for (unsigned i = 0; i < 15; ++i) { old->Reg[i] = random(); fast->SetReg(i, old->Reg[i]); }
        old->Reg[15] = 0x1000; fast->SetPC(0x1000 | thumb);
        InvalidateDynComDecodedCache(); old->instruction_cache.clear(); fast->ClearInstructionCache();
        old->NumInstrsToExecute = 1;
        InterpreterMainLoop(old.get(), timing.GetTimer(0).get()); fast->Step();
        for (int i = 0; i < 16; ++i) {
            if (old->Reg[i] != fast->GetReg(i)) {
                std::cerr << "register mismatch instruction=" << std::hex << ins << " thumb=" << thumb << " r" << std::dec << i << " old=" << std::hex << old->Reg[i] << " fast=" << fast->GetReg(i) << '\n'; return false;
            }
        }
        if ((old->Cpsr & 0xF0000020) != (fast->GetCPSR() & 0xF0000020)) {
            std::cerr << "flags mismatch instruction=" << std::hex << ins << " old=" << old->Cpsr << " fast=" << fast->GetCPSR() << '\n';return false;
        }
        ++count; return true;
    };
    // Predicated ALU, every operation, flag-setting variant, rotated immediates and shifted registers.
    for (unsigned trial = 0; trial < 18000; ++trial) {
        const u32 op = random() % 16;
        const u32 cond = random() % 15;
        const bool flag = (op >= 8 && op <= 11) || (random() & 1);
        const u32 rd = (op >= 8 && op <= 11) ? 0 : random() % 13;
        u32 ins = (cond << 28) | (op << 21) | (flag << 20) | ((random() % 13) << 16) | (rd << 12);
        if (random() & 1) ins |= (1u << 25) | (random() & 0xFFF);
        else ins |= ((random() & 31) << 7) | ((random() & 3) << 5) | (random() % 13);
        if (!check(ins, false)) return 1;
    }
    for (unsigned trial = 0; trial < 4000; ++trial) {
        // Thumb immediate ALU and register ALU, low registers only.
        u32 ins = (random() & 1) ? (0x2000 | (random() & 0x1FFF)) : (0x4000 | (random() & 0x3FF));
        if (!check(ins, true)) return 1;
    }
    // Whole-block execution exercises fusion, chaining and a finite ARM loop.
    const std::array<u32, 6> loop{0xE3A00000, 0xE3A01014, 0xE0800001, 0xE2511001, 0x1AFFFFFC, 0xEAFFFFFE};
    std::copy(loop.begin(), loop.end(), code.begin());
    fast->ClearInstructionCache(); fast->SetCPSR(0x10); fast->SetPC(0x1000);
    timing.GetTimer(0)->Advance(2000); fast->Run();
    if (fast->GetReg(0) != 210 || fast->GetReg(1) != 0 || fast->GetPC() != 0x1014) {
        std::cerr << "fused loop failed sum=" << fast->GetReg(0) << " remaining=" << fast->GetReg(1) << " pc=" << std::hex << fast->GetPC() << '\n';return 1;
    }
    // Separate backing allocations make out-of-page host reads/writes observable.
    std::array<u8, 4096> page_a{}, page_b{};
    table->attributes[3] = table->attributes[4] = Memory::PageType::Memory;
    table->pointers[3] = page_a.data(); table->pointers[4] = page_b.data();
    page_a[4095] = 0x12; page_b[0] = 0x34; page_b[1] = 0x56; page_b[2] = 0x78;
    code[0] = 0xE5910000; // LDR r0, [r1], including a page crossing
    fast->ClearInstructionCache(); fast->SetCPSR(0x10); fast->SetPC(0x1000); fast->SetReg(1, 0x3FFF); fast->Step();
    if (fast->GetReg(0) != 0x78563412) { std::cerr << "cross-page load failed\n"; return 1; }
    code[0] = 0xE5810000; // STR r0, [r1]
    fast->ClearInstructionCache(); fast->SetPC(0x1000); fast->SetReg(0, 0xAABBCCDD); fast->Step();
    if (page_a[4095] != 0xDD || page_b[0] != 0xCC || page_b[1] != 0xBB || page_b[2] != 0xAA) { std::cerr << "cross-page store failed\n";return 1; }
    auto context = fast->NewContext(); fast->SaveContext(context);
    const u32 saved = fast->GetReg(7), saved_flags = fast->GetCPSR();
    fast->SetReg(7, ~saved); fast->SetCPSR(0x10); fast->LoadContext(context);
    if (fast->GetReg(7) != saved || fast->GetCPSR() != saved_flags) return 1;
    // Same PC in a different process must not reuse the previous decode.
    auto second = std::make_unique<Memory::PageTable>();
    std::array<u32,1024> other{0xE3A0002A}; second->attributes[1] = Memory::PageType::Memory;
    second->pointers[1] = reinterpret_cast<u8*>(other.data());
    memory.SetCurrentPageTable(second.get()); fast->SetPageTable(second.get()); fast->SetCPSR(0x10); fast->SetPC(0x1000); fast->Step();
    if (fast->GetReg(0) != 42) return 1;
    std::cout << count << " ARM/Thumb differential instruction cases, fused loop, cross-page RAM, context round-trip and address-space switch passed\n";
}
