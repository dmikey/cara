// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

static int VerifyFastInterpBlxLink(Core::System& system) {
    // Arm DDI 0406, BLX (register): snapshot R[m] before writing LR. R14 is
    // valid; only R15 is UNPREDICTABLE. The independent oracle below also
    // checks the original predicate, NZCV, instruction-set bit and return PC.
    // https://support.arm.com/documentation/ddi0406/c/Application-Level-Architecture/Instruction-Details/Alphabetical-list-of-instructions/BLX--register-
    Memory::MemorySystem memory;
    Core::Timing timing;
    auto dyncom = std::make_unique<ARMul_State>(system, memory, USER32MODE);
    auto fast = std::make_unique<Core::FastInterp::ARM_FastInterp>(
        system, memory, 0, timing.GetTimer(0));
    auto table = std::make_shared<Memory::PageTable>();
    std::array<u32, 1024> code{};
    table->attributes[1] = Memory::PageType::Memory;
    table->pointers[1] = TestMemory(code);
    memory.SetCurrentPageTable(table);
    fast->SetPageTable(table);
    const auto condition_passed = [](unsigned cond, unsigned flags) {
        const bool n = flags & 8, z = flags & 4, c = flags & 2, v = flags & 1;
        switch (cond) {
        case 0: return z;
        case 1: return !z;
        case 2: return c;
        case 3: return !c;
        case 4: return n;
        case 5: return !n;
        case 6: return v;
        case 7: return !v;
        case 8: return c && !z;
        case 9: return !c || z;
        case 10: return n == v;
        case 11: return n != v;
        case 12: return !z && n == v;
        case 13: return z || n != v;
        default: return true; // AL; encoding 15 is excluded.
        }
    };
    unsigned cases = 0;
    for (bool thumb : {false, true})
        for (unsigned cond = 0; cond < (thumb ? 1u : 15u); ++cond)
            for (unsigned flags = 0; flags < 16; ++flags)
                for (unsigned rm : {0u, 13u, 14u})
                    for (u32 target : {0x1800u, 0x1801u})
                        for (bool al_dispatch : {false, true}) {
                            const u32 instruction = thumb ? (0x4780u | (rm << 3))
                                : ((cond << 28) | 0x012fff30u | rm);
                            code.fill(0xe1a00000);
                            code[0] = instruction;
                            const u32 cpsr = (flags << 28) | 0x10 | (thumb ? 0x20 : 0);
                            std::array<u32, 16> expected{};
                            for (unsigned r = 0; r < 15; ++r)
                                expected[r] = 0x1400 + r * 4;
                            expected[14] = 0x1c01;
                            expected[rm] = target;
                            expected[15] = 0x1000;
                            dyncom->Cpsr = cpsr;
                            fast->SetCPSR(cpsr);
                            for (unsigned r = 0; r < 16; ++r) {
                                dyncom->Reg[r] = expected[r];
                                if (r < 15) fast->SetReg(r, expected[r]);
                            }
                            // SetPC selects the starting instruction set from bit 0.
                            fast->SetPC(0x1000 | unsigned(thumb));
                            InvalidateDynComDecodedCache();
                            dyncom->instruction_cache.clear();
                            Core::FastInterp::DecodeRecordSelfTestAccess::ConfigureAlDispatch(
                                *fast, al_dispatch);
                            dyncom->NumInstrsToExecute = 1;
                            InterpreterMainLoop(dyncom.get(), timing.GetTimer(0).get());
                            const auto before = timing.GetTimer(0)->GetDowncount();
                            fast->Step();
                            const auto ticks = before - timing.GetTimer(0)->GetDowncount();
                            const bool passed = thumb || condition_passed(cond, flags);
                            u32 expected_cpsr = cpsr;
                            if (passed) {
                                expected[15] = target & ~1u;
                                expected[14] = thumb ? 0x1003 : 0x1004;
                                expected_cpsr = (cpsr & ~0x20u) | ((target & 1) << 5);
                            } else {
                                expected[15] += 4;
                            }
                            bool matched = ticks == 2 && dyncom->Cpsr == expected_cpsr &&
                                fast->GetCPSR() == expected_cpsr;
                            for (unsigned r = 0; r < 16; ++r)
                                matched &= dyncom->Reg[r] == expected[r] &&
                                    fast->GetReg(r) == expected[r];
                            if (!matched) {
                                std::cerr << "BLX link alias mismatch instruction=" << std::hex
                                          << instruction << " initial_cpsr=" << cpsr
                                          << " target=" << target << " expected_pc=" << expected[15]
                                          << " dyncom_pc=" << dyncom->Reg[15]
                                          << " fast_pc=" << fast->GetPC()
                                          << " expected_lr=" << expected[14]
                                          << " fast_lr=" << fast->GetReg(14)
                                          << " expected_cpsr=" << expected_cpsr
                                          << " fast_cpsr=" << fast->GetCPSR() << std::dec
                                          << " ticks=" << ticks << " al_dispatch=" << al_dispatch
                                          << '\n';
                                return 1;
                            }
                            ++cases;
                        }
    std::cout << cases << " ARM/Thumb BLX register link-alias cases: independent register/"
                 "predicate/NZCV/mode/return-PC oracle, DynCom and unchanged 2-tick accounting; "
                 "AL dispatch off/on passed\n";
    return 0;
}
