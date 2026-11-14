// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include "core/mmio.h"
#include "test_memory.h"

// Uses the actual decoder and Step handler. The throwing MMIO fixture checks
// ordered partial completion, not an ARM data-abort implementation.
static int VerifyFastInterpScalarDouble(Core::System& system, Core::Timing& timing) {
    struct Device final : Memory::MMIORegion {
        using Event = std::array<u32, 4>; // write, width, address, value
        std::vector<Event> events;
        u32 fail_at = 0;
        static u32 Value(u32 address, unsigned width) {
            u32 result = 0;
            for (unsigned i = 0; i < width; ++i)
                result |= u32(u8((address + i) * 13 + 37)) << (i * 8);
            return result;
        }
        u32 Read(u32 address, unsigned width) {
            const u32 value = Value(address, width);
            events.push_back({0, width, address, value});
            if (address == fail_at) throw std::runtime_error("fixture read failure");
            return value;
        }
        void Write(u32 address, u32 value, unsigned width) {
            events.push_back({1, width, address, value});
            if (address == fail_at) throw std::runtime_error("fixture write failure");
        }
        bool IsValidAddress(VAddr) override { return true; }
        u8 Read8(VAddr a) override { return u8(Read(a, 1)); }
        u16 Read16(VAddr a) override { return u16(Read(a, 2)); }
        u32 Read32(VAddr a) override { return Read(a, 4); }
        u64 Read64(VAddr) override { throw std::runtime_error("unexpected MMIO Read64"); }
        bool ReadBlock(VAddr, void*, std::size_t) override { throw std::runtime_error("unexpected MMIO block read"); }
        void Write8(VAddr a, u8 v) override { Write(a, v, 1); }
        void Write16(VAddr a, u16 v) override { Write(a, v, 2); }
        void Write32(VAddr a, u32 v) override { Write(a, v, 4); }
        void Write64(VAddr, u64) override { throw std::runtime_error("unexpected MMIO Write64"); }
        bool WriteBlock(VAddr, const void*, std::size_t) override { throw std::runtime_error("unexpected MMIO block write"); }
    };
    Memory::MemorySystem memory;
    auto table = std::make_shared<Memory::PageTable>();
    std::array<u32, 1024> code{};
    std::array<std::array<u8, 4096>, 4> pages{};
    constexpr std::array<u32, 4> page_numbers{3, 4, 0xfffff, 0};
    table->attributes[1] = Memory::PageType::Memory;
    table->pointers[1] = TestMemory(code);
    for (unsigned i = 0; i < pages.size(); ++i) {
        table->attributes[page_numbers[i]] = Memory::PageType::Memory;
        table->pointers[page_numbers[i]] = TestMemory(pages[i]);
    }
    memory.SetCurrentPageTable(table);
    memory.RegisterPageTable(table);
    memory.MapMemoryRegion(*table, Memory::VRAM_VADDR, 4096,
                           memory.GetPhysicalRef(Memory::VRAM_PADDR));
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR, 4096, true);
    const auto tracked_page = Memory::VRAM_VADDR >> Memory::PAGE_BITS;
    if (table->GetPointerArray()[tracked_page]) {
        std::cerr << "Scalar double fixture requires tracked null pointer\n";
        return 1;
    }
    auto* tracked = memory.GetPhysicalPointer(Memory::VRAM_PADDR);
    auto device = std::make_shared<Device>();
    memory.MapIoRegion(*table, 0x9000, 8192, device);
    const char* env = std::getenv("CARA_FASTINTERP_RAM_BURST");
    const bool had_env = env != nullptr;
    const std::string saved_env = env ? env : "";
    std::array<std::unique_ptr<Core::FastInterp::ARM_FastInterp>, 2> cpus;
    for (unsigned mode = 0; mode < 2; ++mode) {
        setenv("CARA_FASTINTERP_RAM_BURST", mode ? "1" : "0", 1);
        cpus[mode] = std::make_unique<Core::FastInterp::ARM_FastInterp>(system, memory, 0, timing.GetTimer(0));
        cpus[mode]->SetPageTable(table);
    }
    if (had_env) setenv("CARA_FASTINTERP_RAM_BURST", saved_env.c_str(), 1);
    else unsetenv("CARA_FASTINTERP_RAM_BURST");
    struct Result {
        std::array<u32, 16> regs{};
        std::array<u32, 64> fp{};
        std::array<std::array<u8, 4096>, 4> pages{};
        std::array<u8, 4096> tracked{};
        std::vector<Device::Event> events;
        u32 cpsr = 0, fpscr = 0;
        s64 ticks = 0;
        bool threw = false;
        bool operator==(const Result&) const = default;
    };
    // Last seven starts cover both boundary-crossing words independently;
    // wrap starts exercise guest-u32 arithmetic over separately backed pages.
    const std::array<u32, 21> addresses{0x3000,0x3001,0x3003,0x3ff8,0x3ff9,
        0x3ffa,0x3ffb,0x3ffc,0x3ffd,0x3ffe,0x3fff,0xfffffff8,0xfffffffc,
        0xffffffff,0x9000,0x9003,0x9fff,0x7000,0x7ffc,
        Memory::VRAM_VADDR,Memory::VRAM_VADDR + 3};
    unsigned cases = 0;
    for (u32 address : addresses) for (bool load : {false,true})
        for (unsigned d = 0; d < 32; ++d) for (unsigned condition : {0u,1u,14u})
            for (bool fault : {false,true}) {
                if (fault && address != 0x9000) continue;
                const bool executes = condition != 1; // Z is set below.
                std::array<Result, 2> results;
                for (unsigned mode = 0; mode < 2; ++mode) {
                    auto& cpu = *cpus[mode];
                    auto& result = results[mode];
                    for (unsigned p = 0; p < pages.size(); ++p)
                        for (unsigned i = 0; i < 4096; ++i)
                            pages[p][i] = u8(((page_numbers[p] << 12) + i) * 13 + 37);
                    for (unsigned i = 0; i < 4096; ++i) tracked[i] = u8((Memory::VRAM_VADDR+i)*13+37);
                    device->events.clear();
                    device->fail_at = fault ? address + 4 : 0;
                    for (unsigned i = 0; i < 15; ++i) cpu.SetReg(i, 0x12345600u+i);
                    for (unsigned i = 0; i < 64; ++i) cpu.SetVFPReg(i, 0x80000000u+i*0x01010101u);
                    cpu.SetVFPSystemReg(VFP_FPSCR, 0xa5c00010);
                    cpu.SetReg(2, address);
                    cpu.SetCPSR(0x60000010);
                    cpu.SetPC(0x1000);
                    code[0] = (condition << 28) | (load ? 0x0d920b00 : 0x0d820b00) |
                              ((d & 15) << 12) | ((d >> 4) << 22);
                    cpu.ClearInstructionCache();
                    const s64 before = timing.GetTimer(0)->GetDowncount();
                    try { cpu.Step(); } catch (const std::runtime_error&) { result.threw = true; }
                    result.ticks = before - timing.GetTimer(0)->GetDowncount();
                    result.cpsr = cpu.GetCPSR();
                    result.fpscr = cpu.GetVFPSystemReg(VFP_FPSCR);
                    for (unsigned i = 0; i < 16; ++i) result.regs[i] = cpu.GetReg(i);
                    for (unsigned i = 0; i < 64; ++i) result.fp[i] = cpu.GetVFPReg(i);
                    result.pages = pages;
                    std::copy_n(tracked, 4096, result.tracked.data());
                    result.events = device->events;
                    // Independent bit oracle for loads, including mapped-u32 wrap,
                    // unmapped-zero reads and the committed first word before fault.
                    bool valid = result.threw == (fault && executes) && result.cpsr == 0x60000010 &&
                                 result.fpscr == 0xa5c00010;
                    for (unsigned i = 0; i < 64; ++i) {
                        u32 expected = 0x80000000u+i*0x01010101u;
                        if (executes && load && i >= d*2 && i < d*2+2 && !(fault && i == d*2+1)) {
                            const u32 a = address + (i-d*2)*4;
                            expected = (a >= 0x7000 && a < 0x9000) ? 0 : Device::Value(a,4);
                        }
                        valid &= result.fp[i] == expected;
                    }
                    // Each MMIO word must retain the existing ordered 32-bit
                    // transaction, splitting only a word that crosses a page.
                    std::vector<Device::Event> expected_events;
                    if (executes && address >= 0x9000 && address < 0xb000) {
                        bool stop = false;
                        for (unsigned word = 0; word < 2 && !stop; ++word) {
                            const u32 a = address + word*4;
                            const unsigned width = (a & 4095) > 4092 ? 1 : 4;
                            const u32 bits = 0x80000000u+(d*2+word)*0x01010101u;
                            for (unsigned byte = 0; byte < 4; byte += width) {
                                const u32 value = load ? Device::Value(a+byte,width) :
                                    (width == 4 ? bits : ((bits >> (byte*8)) & 255));
                                expected_events.push_back({u32(!load),width,a+byte,value});
                                if (fault && a+byte == address+4) { stop = true; break; }
                            }
                        }
                    }
                    valid &= result.events == expected_events;
                    // Independent little-endian byte oracle also checks all
                    // bytes outside the selected double remain unchanged.
                    auto expected_byte = [&](u32 a) {
                        u8 value = u8(a*13+37);
                        if (executes && !load) for (unsigned byte = 0; byte < 8; ++byte)
                            if (a == address+byte) {
                                const u32 bits = 0x80000000u+(d*2+byte/4)*0x01010101u;
                                value = u8(bits >> ((byte%4)*8));
                            }
                        return value;
                    };
                    for (unsigned p = 0; p < pages.size(); ++p)
                        for (unsigned byte = 0; byte < 4096; ++byte)
                            valid &= result.pages[p][byte] == expected_byte((page_numbers[p]<<12)+byte);
                    for (unsigned byte = 0; byte < 4096; ++byte)
                        valid &= result.tracked[byte] == expected_byte(Memory::VRAM_VADDR+byte);
                    if (!result.threw) valid &= result.regs[15] == 0x1004 &&
                        result.ticks == Core::TicksForInstruction(false,code[0]);
                    else valid &= result.regs[15] == 0x1000 && result.ticks == 0;
                    for (unsigned reg = 0; reg < 15; ++reg)
                        valid &= result.regs[reg] == (reg == 2 ? address : 0x12345600u+reg);
                    if (!valid) {
                        std::cerr << "Scalar double oracle failed address=" << std::hex << address
                                  << " load=" << load << " d=" << d << " condition=" << condition
                                  << " mode=" << mode << " fault=" << fault << '\n';
                        return 1;
                    }
                }
                if (!(results[0] == results[1])) {
                    std::cerr << "Scalar double off/on mismatch address=" << std::hex << address << '\n';
                    return 1;
                }
                ++cases;
            }
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR, 4096, false);
    if (!table->GetPointerArray()[tracked_page]) {
        std::cerr << "Scalar double untracking failed to restore direct pointer\n";
        return 1;
    }
    memory.UnregisterPageTable(table);
    std::cout << cases << " scalar double off/on cases: all D registers, predication, exact bits/ticks, unaligned/cross-page/wrap, MMIO ordering/partial completion, unmapped and tracked RAM passed\n";
    return 0;
}
