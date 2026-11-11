// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <chrono>
#include <cstring>
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
    // CMP followed by conditional B/BL must preserve flags, both branch outcomes,
    // link semantics, and exact stopping at either instruction boundary.
    const auto condition = [](unsigned c,u32 f) {
        const bool n=f>>31, z=(f&0x40000000)!=0, carry=(f&0x20000000)!=0, v=(f&0x10000000)!=0;
        switch(c) {
        case 0:return z; case 1:return !z; case 2:return carry; case 3:return !carry;
        case 4:return n; case 5:return !n; case 6:return v; case 7:return !v;
        case 8:return carry&&!z; case 9:return !carry||z; case 10:return n==v;
        case 11:return n!=v; case 12:return !z&&(n==v); case 13:return z||(n!=v);
        default:return true;
        }
    };
    std::array<u32,1024> compare_code{};
    compare_code[2]=0xE3A01001; compare_code[3]=0xEAFFFFFE;
    compare_code[4]=0xE3A01002; compare_code[5]=0xEAFFFFFE;
    first_table->attributes[8]=Memory::PageType::Memory;
    first_table->pointers[8]=reinterpret_cast<u8*>(compare_code.data());
    memory.SetCurrentPageTable(first_table.get());
    for (u32 lhs:{0u,1u,0xffffffffu,0x80000000u,0x7fffffffu})
    for (u32 rhs:{0u,1u,255u}) for (unsigned cmp_cond:{14u,0u})
    for (unsigned branch_cond=0;branch_cond<15;++branch_cond) for (bool link:{false,true})
    for (unsigned budget:{0u,1u,2u,3u}) {
        compare_code[0]=(cmp_cond<<28)|0x03500000|rhs;
        compare_code[1]=(branch_cond<<28)|0x0A000001|(link?0x01000000:0);
        InvalidateDynComDecodedCache();
        first->Cpsr=USER32MODE|0x20000000; first->Reg[0]=lhs;
        first->Reg[1]=0x12345678; first->Reg[14]=0x11223344;
        first->Reg[15]=0x8000; first->NumInstrsToExecute=budget;
        const u32 result=lhs-rhs;
        const u32 cmp_flags=(result&0x80000000)|(result==0?0x40000000:0)|
            (lhs>=rhs?0x20000000:0)|(((lhs^rhs)&(lhs^result)&0x80000000)?0x10000000:0);
        const u32 flags=(budget&&cmp_cond==14)?cmp_flags:0x20000000;
        const bool taken=condition(branch_cond,flags);
        const u32 expected_pc=budget==0?0x8000:budget==1?0x8004:
            budget==2?(taken?0x8010:0x8008):(taken?0x8014:0x800c);
        const unsigned count=InterpreterMainLoop(first.get(),timing.GetTimer(0).get());
        if (count!=budget || first->Reg[15]!=expected_pc || first->Reg[0]!=lhs ||
            first->Reg[1]!=(budget==3?(taken?2u:1u):0x12345678) ||
            first->Reg[14]!=((budget>=2&&taken&&link)?0x8008:0x11223344) ||
            first->Cpsr!=(USER32MODE|flags)) {
            std::cerr << "CMP/branch dispatch state or instruction boundary mismatch\n"; return 10;
        }
    }
    std::cout << "CMP/B/BL conditions, flags, links and exact instruction budgets passed\n";
    // Load/compare/branch chains retain memory reads, predication, links, and
    // exact stopping boundaries; a load into PC must continue through DISPATCH.
    std::array<u32,1024> load_code{}, load_data{};
    load_code[3]=0xE3A01001; load_code[4]=0xEAFFFFFE;
    load_code[5]=0xE3A01002; load_code[6]=0xEAFFFFFE;
    first_table->attributes[9]=first_table->attributes[10]=Memory::PageType::Memory;
    first_table->pointers[9]=reinterpret_cast<u8*>(load_code.data());
    first_table->pointers[10]=reinterpret_cast<u8*>(load_data.data());
    for (u32 lhs:{0u,1u,0xffffffffu,0x80000000u,0x7fffffffu})
    for (u32 rhs:{0u,1u,255u}) for (unsigned load_cond:{14u,0u})
    for (unsigned branch_cond=0;branch_cond<15;++branch_cond) for (bool link:{false,true})
    for (unsigned budget:{0u,1u,2u,3u,4u}) {
        load_data[0]=lhs;
        load_code[0]=(load_cond<<28)|0x05920000; // LDR r0, [r2]
        load_code[1]=0xE3500000|rhs;
        load_code[2]=(branch_cond<<28)|0x0A000001|(link?0x01000000:0);
        InvalidateDynComDecodedCache();
        first->Cpsr=USER32MODE|0x20000000; first->Reg[0]=0x12345678;
        first->Reg[1]=0x12345678; first->Reg[2]=0xa000; first->Reg[14]=0x11223344;
        first->Reg[15]=0x9000; first->NumInstrsToExecute=budget;
        const u32 value=(budget&&load_cond==14)?lhs:0x12345678;
        const u32 result=value-rhs;
        const u32 cmp_flags=(result&0x80000000)|(result==0?0x40000000:0)|
            (value>=rhs?0x20000000:0)|(((value^rhs)&(value^result)&0x80000000)?0x10000000:0);
        const u32 flags=budget>=2?cmp_flags:0x20000000;
        const bool taken=condition(branch_cond,flags);
        const u32 expected_pc=budget==0?0x9000:budget==1?0x9004:budget==2?0x9008:
            budget==3?(taken?0x9014:0x900c):(taken?0x9018:0x9010);
        const unsigned count=InterpreterMainLoop(first.get(),timing.GetTimer(0).get());
        if (count!=budget || first->Reg[15]!=expected_pc || first->Reg[0]!=value ||
            first->Reg[1]!=(budget==4?(taken?2u:1u):0x12345678) ||
            first->Reg[14]!=((budget>=3&&taken&&link)?0x900c:0x11223344) ||
            first->Cpsr!=(USER32MODE|flags)) {
            std::cerr << "LDR/CMP/branch dispatch state or boundary mismatch\n"; return 11;
        }
    }
    load_code[0]=0xE592F000; // LDR pc, [r2]
    load_data[0]=0x9014; InvalidateDynComDecodedCache();
    first->Cpsr=USER32MODE; first->Reg[2]=0xa000; first->Reg[1]=0;
    first->Reg[15]=0x9000; first->NumInstrsToExecute=2;
    if (InterpreterMainLoop(first.get(),timing.GetTimer(0).get())!=2 ||
        first->Reg[1]!=2 || first->Reg[15]!=0x9018) return 12;
    std::cout << "LDR/CMP/branch and load-to-PC dispatch checks passed\n";
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
