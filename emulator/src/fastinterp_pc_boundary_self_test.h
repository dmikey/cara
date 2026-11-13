// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

static int VerifyFastInterpPCBoundary(Core::System& system) {
    using namespace Core::FastInterp;
    Memory::MemorySystem memory;
    Core::Timing timing;
    auto cpu = std::make_unique<ARM_FastInterp>(system, memory, 0, timing.GetTimer(0));
    auto table = std::make_shared<Memory::PageTable>();
    std::array<u32, 1024> code{};
    table->attributes[1] = Memory::PageType::Memory;
    table->pointers[1] = TestMemory(code);
    memory.SetCurrentPageTable(table);
    cpu->SetPageTable(table);

    unsigned cases = 0;
    u64 trace = 1469598103934665603ull;
    const auto put_halfword = [&](unsigned offset, u16 instruction) {
        std::memcpy(reinterpret_cast<u8*>(code.data()) + offset, &instruction, sizeof(instruction));
    };
    // The expected PC/ticks come from the guest sequence, independently of the
    // candidate decoder's end_pc. An Invalid slot has not executed; its width
    // must be excluded even after a mixed Thumb16/Thumb32 instruction stream.
    for (bool thumb : {false, true})
        for (unsigned prefix = 0; prefix <= 12; ++prefix)
            for (bool wide_invalid : {false, true})
                for (unsigned flags = 0; flags < 16; ++flags)
                    for (bool al_dispatch : {false, true}) {
                        if (!thumb && wide_invalid) continue;
                        code.fill(0);
                        unsigned offset = 0;
                        for (unsigned i = 0; i < prefix; ++i) {
                            if (thumb) {
                                put_halfword(offset, 0x3001); // ADDS r0, #1
                                offset += 2;
                            } else {
                                code[offset / 4] = 0xE2800001; // ADD r0, r0, #1
                                offset += 4;
                            }
                        }
                        if (thumb && wide_invalid) {
                            put_halfword(offset, 0xEF00);
                            put_halfword(offset + 2, 0x0A00);
                        } else if (thumb) {
                            put_halfword(offset, 0xDE00);
                        } else {
                            code[offset / 4] = 0xF0000000;
                        }

                        cpu->SetCPSR((flags << 28) | 0x10 | (thumb ? 0x20 : 0));
                        cpu->SetPC(0x1000 | unsigned(thumb));
                        cpu->SetReg(0, 0);
                        DecodeRecordSelfTestAccess::ConfigureAlDispatch(*cpu, al_dispatch);
                        auto block = std::make_unique<BasicBlock>();
                        block->start_pc = 0x1000;
                        DecodeRecordSelfTestAccess::Decode(*cpu, *block, 0x1000);
                        const unsigned invalid_size = thumb && !wide_invalid ? 2 : 4;
                        if (!block->inst_count || block->end_pc != 0x1000 + offset + invalid_size ||
                            block->instructions[block->inst_count - 1].opcode != Opcode::Invalid ||
                            block->instructions[block->inst_count - 1].inst_size != invalid_size) {
                            std::cerr << "PC boundary fixture invalid-tail decode mismatch\n";
                            return 1;
                        }
                        const u64 ticks = DecodeRecordSelfTestAccess::ExecuteOnce(*cpu, *block);
                        if (ticks != prefix || cpu->GetPC() != 0x1000 + offset ||
                            cpu->GetReg(0) != prefix) {
                            std::cerr << "Invalid-tail PC/tick mismatch thumb=" << thumb
                                      << " wide=" << wide_invalid << " prefix=" << prefix
                                      << " PC=" << std::hex << cpu->GetPC() << std::dec
                                      << " ticks=" << ticks << '\n';
                            return 1;
                        }
                        trace = (trace ^ ticks ^ cpu->GetPC() ^ cpu->GetReg(0)) * 1099511628211ull;
                        ++cases;

                        // Clear/Invalidate poison inst_count without clearing payload/end_pc.
                        // Entry into such a block must keep the caller's PC and charge no ticks.
                        block->inst_count = 0;
                        cpu->SetPC(0x1800 | unsigned(thumb));
                        const u32 cpsr = cpu->GetCPSR();
                        if (DecodeRecordSelfTestAccess::ExecuteOnce(*cpu, *block) != 0 ||
                            cpu->GetPC() != 0x1800 || cpu->GetReg(0) != prefix ||
                            cpu->GetCPSR() != cpsr) {
                            std::cerr << "Poisoned block changed entry PC/ticks/state\n";
                            return 1;
                        }
                        ++cases;
                    }
    std::cout << "PC boundary invalid-tail/poisoned cases=" << cases << " trace=" << trace << '\n';
    return 0;
}
