// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

static int VerifyFastInterpLdrCmpBranch(Core::System& system) {
    using namespace Core::FastInterp;
    Memory::MemorySystem memory;
    Core::Timing timing;
    auto cpu = std::make_unique<ARM_FastInterp>(system, memory, 0, timing.GetTimer(0));
    auto table = std::make_shared<Memory::PageTable>();
    std::array<u32, 1024> code{};
    std::array<u8, 4096> first{}, second{};
    for (unsigned i = 0; i < first.size(); ++i) {
        first[i] = static_cast<u8>(i * 37 + 19);
        second[i] = static_cast<u8>(i * 71 + 13);
    }
    table->attributes[1] = table->attributes[3] = table->attributes[4] = Memory::PageType::Memory;
    table->pointers[1] = TestMemory(code);
    table->pointers[3] = TestMemory(first);
    table->pointers[4] = TestMemory(second);
    memory.SetCurrentPageTable(table);
    cpu->SetPageTable(table);
    struct Device final : Memory::MMIORegion {
        unsigned reads = 0;
        bool IsValidAddress(VAddr address) override { return address == 0x6000; }
        u32 Read32(VAddr) override { ++reads; return 0x1234abcd; }
        u8 Read8(VAddr) override { return 0; }
        u16 Read16(VAddr) override { return 0; }
        u64 Read64(VAddr) override { return 0; }
        bool ReadBlock(VAddr, void*, std::size_t) override { return false; }
        void Write8(VAddr, u8) override {}
        void Write16(VAddr, u16) override {}
        void Write32(VAddr, u32) override {}
        void Write64(VAddr, u64) override {}
        bool WriteBlock(VAddr, const void*, std::size_t) override { return false; }
    };
    auto device = std::make_shared<Device>();
    memory.MapIoRegion(*table, 0x6000, 4096, device);
    memory.RegisterPageTable(table);
    memory.MapMemoryRegion(*table, Memory::VRAM_VADDR, 4096,
                           memory.GetPhysicalRef(Memory::VRAM_PADDR));
    constexpr u32 tracked_word = 0x7654fedc;
    std::memcpy(memory.GetPhysicalPointer(Memory::VRAM_PADDR), &tracked_word, sizeof(tracked_word));
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR, 4096, true);
    const auto* flag = std::getenv("CARA_FASTINTERP_LDR_CMP_BRANCH");
    const bool enabled = flag && std::strcmp(flag, "1") == 0;
    unsigned cases = 0, immediate_edges = 0, register_edges = 0;
    u64 digest = 1469598103934665603ull;
    struct Result {
        std::array<u32, 16> registers{};
        u32 cpsr{};
        u64 ticks{};
        bool operator==(const Result&) const = default;
    };
    for (const bool al : {false, true}) {
        DecodeRecordSelfTestAccess::ConfigureAlDispatch(*cpu, al);
        for (const u32 address : {0x3000u, 0x3001u, 0x3ffcu, 0x3ffdu, 0x3fffu, 0x4000u,
                                 0x6000u, u32(Memory::VRAM_VADDR)}) {
            u32 word = 0;
            if (address == 0x6000) {
                word = 0x1234abcd;
            } else if (address == Memory::VRAM_VADDR) {
                word = tracked_word;
            } else {
                for (unsigned i = 0; i < 4; ++i) {
                    const u32 at = address + i;
                    word |= u32(at < 0x4000 ? first[at - 0x3000] : second[at - 0x4000]) << (i * 8);
                }
            }
            for (const bool compare_register : {false, true})
            for (const unsigned condition : {0u, 1u, 2u, 3u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u})
            for (const u32 comparison : {word, word - 1, word + 1, 0x80000000u})
            for (const unsigned budget : {1u, 2u, 3u, 100u}) {
                code.fill(0xeafffffeu);
                code[0] = 0xe5901000u; // LDR r1, [r0]
                code[1] = compare_register ? 0xe1510002u : 0xe3510013u;
                code[2] = (condition << 28) | 0x0a000002u; // branch to 0x1018
                BasicBlock candidate{};
                candidate.start_pc = 0x1000;
                DecodeRecordSelfTestAccess::Decode(*cpu, candidate, 0x1000);
                BasicBlock scalar = candidate;
                const auto op = candidate.instructions[0].GetOpcode();
                if (op == Opcode::LdrCmpImmBcc || op == Opcode::LdrCmpRegBcc) {
                    immediate_edges += op == Opcode::LdrCmpImmBcc;
                    register_edges += op == Opcode::LdrCmpRegBcc;
                    if (candidate.inst_count < 2 || candidate.instructions[0].inst_size != 4 ||
                        candidate.instructions[1].inst_size != 8)
                        return 1;
                    scalar.instructions[0].SetOpcode(Opcode::LdrImm);
                    scalar.instructions[0].dispatch_index = unsigned(Opcode::LdrImm) +
                        (al ? unsigned(Opcode::OpcodeCount) : 0);
                }
                const auto execute = [&](BasicBlock& block) {
                    cpu->SetCPSR(0x10);
                    cpu->SetPC(0x1000);
                    for (unsigned i = 0; i < 15; ++i)
                        cpu->SetReg(i, 0x70000000u + i);
                    cpu->SetReg(0, address);
                    cpu->SetReg(2, comparison);
                    timing.GetTimer(0)->Advance(budget);
                    Result state;
                    state.ticks = DecodeRecordSelfTestAccess::ExecuteOnce(*cpu, block);
                    state.cpsr = cpu->GetCPSR();
                    for (unsigned i = 0; i < 16; ++i)
                        state.registers[i] = cpu->GetReg(i);
                    return state;
                };
                const auto reads = device->reads;
                const auto expected = execute(scalar);
                const auto actual = execute(candidate);
                if (expected != actual || actual.registers[1] != word ||
                    device->reads - reads != (address == 0x6000 ? 2u : 0u)) {
                    std::cerr << "LDR/CMP branch state, ticks or load value changed\n";
                    return 1;
                }
                for (const auto value : actual.registers)
                    digest = (digest ^ value) * 1099511628211ull;
                digest = (digest ^ actual.ticks ^ actual.cpsr) * 1099511628211ull;
                ++cases;
            }
        }
    }
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR, 4096, false);
    if ((immediate_edges != 0) != enabled || (register_edges != 0) != enabled) {
        std::cerr << "LDR/CMP test failed to exercise both eligible dispatch edges\n";
        return 1;
    }
    std::cout << "LDR/CMP cases=" << cases << " immediate=" << immediate_edges
              << " register=" << register_edges << " state_digest=" << digest << '\n';
    return 0;
}
