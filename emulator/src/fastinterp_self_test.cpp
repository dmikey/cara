// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
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
        const std::array<Fixture, 5> fixtures{{
            {"conditional_bkpt", 0x31200A76, 0xE1A03003},
            {"ldm_stm_8", 0xE89107F8, 0xE88207F8},
            {"ldm_stm_3", 0xE8910038, 0xE8820038},
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
        u64 trace_hash = 1469598103934665603ull;
        auto record = [&trace_hash](u64 value) {
            trace_hash = (trace_hash ^ value) * 1099511628211ull;
        };
        for (unsigned i = 0; i < 64; ++i) {
            timing.GetTimer(0)->Advance(budgets[i % budgets.size()]);
            const auto before = timing.GetTimer(0)->GetDowncount();
            fast->Run();
            record(fast->GetPC());
            record(fast->GetReg(0));
            record(before - timing.GetTimer(0)->GetDowncount());
            for (unsigned r = 0; r < 32; ++r)
                record(fast->GetVFPReg(r));
        }
        if (trace_hash != 7296223465908193166ull) {
            std::cerr << "SIMD event-boundary trace mismatch: " << trace_hash << '\n';
            return 1;
        }
        std::ostringstream profile;
        fast->WriteBlockProfile(profile);
        for (const char* expected : {"\"pc\":4096,\"end_pc\":4216,\"executions\":367",
                                     "\"pc\":4216,\"end_pc\":4332,\"executions\":366",
                                     "\"pc\":4332,\"end_pc\":4448,\"executions\":366",
                                     "\"pc\":4448,\"end_pc\":4504,\"executions\":366"}) {
            if (profile.str().find(expected) == std::string::npos) {
                std::cerr << "SIMD decoded-block boundary/count mismatch\n";
                return 1;
            }
        }
        std::cout << "64 mixed-budget SIMD trace slices and four block boundaries match baseline\n";
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
