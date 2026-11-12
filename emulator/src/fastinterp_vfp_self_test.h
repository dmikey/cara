// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/common_types.h"
#ifdef __aarch64__
#include <array>
#include <bit>
#include <cstdint>
#include <iostream>
#include <arm_neon.h>
static int VerifyFastInterpFusedMultiplyAdd() {
    const std::array<u32, 24> edge{0,          0x80000000, 1,          0x80000001, 0x007fffff,
                                   0x807fffff, 0x00800000, 0x80800000, 0x3f800000, 0xbf800000,
                                   0x7f7fffff, 0xff7fffff, 0x7f800000, 0xff800000, 0x7fc00001,
                                   0xffc00001, 0x7fa12345, 0xffa54321, 0x7fc12345, 0xffc54321,
                                   0x3eaaaaab, 0x3f000001, 0x00800001, 0x7f7ffffe};
    u64 saved_fpcr, saved_fpsr;
    asm volatile("mrs %0, fpcr" : "=r"(saved_fpcr));
    asm volatile("mrs %0, fpsr" : "=r"(saved_fpsr));
    unsigned count = 0;
    u32 random = 0x9a72b31d;
    auto rng = [&] {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return random;
    };
    for (unsigned mode = 0; mode < 16; ++mode) {
        const u64 fpcr = (saved_fpcr & ~((3ull << 22) | (1ull << 24) | (1ull << 25))) |
                         (u64(mode & 3) << 22) | (u64(mode & 4) << 22) | (u64(mode & 8) << 22);
        asm volatile("msr fpcr, %0" ::"r"(fpcr));
        for (unsigned trial = 0; trial < 24 * 24 * 24 + 65536; ++trial) {
            u32 a, n, m;
            if (trial < 24 * 24 * 24) {
                a = edge[trial % 24];
                n = edge[trial / 24 % 24];
                m = edge[trial / 24 / 24];
            } else {
                a = rng();
                n = rng();
                m = rng();
            }
            float scalar;
            const float af = std::bit_cast<float>(a), nf = std::bit_cast<float>(n),
                        mf = std::bit_cast<float>(m);
            asm volatile("msr fpsr, xzr");
            asm volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(scalar) : "w"(nf), "w"(mf), "w"(af));
            u64 scalar_fpsr;
            asm volatile("mrs %0, fpsr" : "=r"(scalar_fpsr));
            float32x4_t vector = vreinterpretq_f32_u32(vdupq_n_u32(a));
            const auto nv = vreinterpretq_f32_u32(vdupq_n_u32(n)),
                       mv = vreinterpretq_f32_u32(vdupq_n_u32(m));
            asm volatile("msr fpsr, xzr");
            asm volatile("fmla %0.4s, %1.4s, %2.4s" : "+w"(vector) : "w"(nv), "w"(mv));
            u64 vector_fpsr;
            asm volatile("mrs %0, fpsr" : "=r"(vector_fpsr));
            const u32 actual = vgetq_lane_u32(vreinterpretq_u32_f32(vector), 0),
                      expected = std::bit_cast<u32>(scalar);
            if (actual != expected || scalar_fpsr != vector_fpsr) {
                std::cerr << "Mismatch mode=" << mode << " trial=" << trial << " a/n/m=" << std::hex
                          << a << '/' << n << '/' << m << " scalar=" << expected
                          << " vector=" << actual << " FPSR=" << scalar_fpsr << '/' << vector_fpsr
                          << '\n';
                asm volatile("msr fpcr, %0" ::"r"(saved_fpcr));
                asm volatile("msr fpsr, %0" ::"r"(saved_fpsr));
                return 1;
            }
            ++count;
        }
    }
    asm volatile("msr fpcr, %0" ::"r"(saved_fpcr));
    asm volatile("msr fpsr, %0" ::"r"(saved_fpsr));
    std::cout << count << " Cartesian and random FMADD/FMLA value/FPSR cases passed\n";
    return 0;
}
#endif
