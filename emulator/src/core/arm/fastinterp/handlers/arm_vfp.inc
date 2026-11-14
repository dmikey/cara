// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// VFP (Floating Point) handlers: Register transfers (VmovArmToS, etc.),
//     Single precision (VaddS, VsubS, etc.), Double precision (VaddD, etc.),
//     Comparison (VcmpS, VcmpD, etc.), Conversion (VcvtSToInt, etc.),
//     Load/Store (VldrS, VstrS, VldrD, VstrD)

// Helper macros for VFP register access (bit_cast/memcpy: no aliasing UB).
// #undef'd at the bottom of this file.
#define VFP_S_READ(idx) (std::bit_cast<float>(state_.vfp_regs[(idx)]))
#define VFP_S_WRITE(idx, val) \
    (state_.vfp_regs[(idx)] = std::bit_cast<u32>(static_cast<float>(val)))
#define VFP_S_READ_U32(idx) (state_.vfp_regs[(idx)])
#define VFP_S_WRITE_U32(idx, val) (state_.vfp_regs[(idx)] = (val))

// D-registers are stored as two consecutive u32 halves in vfp_regs
#define VFP_D_READ(idx) ({ \
    double vfp_d_read_tmp_; \
    std::memcpy(&vfp_d_read_tmp_, &state_.vfp_regs[(idx) * 2], sizeof(vfp_d_read_tmp_)); \
    vfp_d_read_tmp_; \
})
#define VFP_D_WRITE(idx, val) do { \
    double vfp_d_tmp_ = (val); \
    std::memcpy(&state_.vfp_regs[(idx) * 2], &vfp_d_tmp_, sizeof(vfp_d_tmp_)); \
} while(0)

// Checks follow CHECK_COND, including its AL entry. Comparisons, conversions,
// immediates, CPU transfers and memory transfers remain scalar as specified by
// VFP11; only arithmetic and register-to-register FP moves expand into vectors.
// Bank-zero destinations still execute once, but while LEN is active they
// must observe the same guest FP controls and sticky exceptions as vector banks.
// The software engine applies that scalar-bank rule internally.
#define VFP_SHORT_VECTOR(base, is_double) do { \
    if (state_.fpscr & (7u << 16)) [[unlikely]] { \
        ExecuteVFPShortVector(base, inst->vfp.sd, inst->vfp.sn, inst->vfp.sm, is_double); \
        ADVANCE_AND_DISPATCH(); \
    } \
} while (0)

// Scalar SIMD fusion is a decode-time choice, but FPSCR may change at runtime
// inside the same cached block. Retain guest instruction order when short-vector
// mode is active: expanded register banks can alias despite scalar alias checks.
#define VFP_FUSED_SHORT_VECTOR(accumulate, gather) do { \
    if (state_.fpscr & (7u << 16)) [[unlikely]] { \
        for (unsigned i = 0; i < 4; ++i) { \
            const unsigned sd = (gather) ? inst->vfp4.sd[i] : inst->vfp.sd + i; \
            const unsigned sn = (gather) ? inst->vfp4.sn[i] : \
                (inst->vfp.flags ? inst->vfp.sm : inst->vfp.sn + i); \
            const unsigned sm = (gather) ? inst->vfp4.sm[i] : \
                (inst->vfp.flags ? inst->vfp.sn + i : inst->vfp.sm); \
            ExecuteVFPShortVector((accumulate) ? 0xee000a00u : 0xee200a00u, sd, sn, sm, false); \
        } \
        executed += inst->ticks; \
        SET_PC(GET_PC() + inst->inst_size); \
        inst += 3; \
        DISPATCH(); \
    } \
} while (0)

// VFP Register Transfer: VMOV ARM to single VFP
handle_VmovArmToS:
    CHECK_COND(VmovArmToS);
    VFP_S_WRITE_U32(inst->vfp.sd, state_.regs[inst->vfp.rt]);
    ADVANCE_AND_DISPATCH();

handle_VmovSToArm:
    CHECK_COND(VmovSToArm);
    state_.regs[inst->vfp.rt] = VFP_S_READ_U32(inst->vfp.sd);
    ADVANCE_AND_DISPATCH();

handle_VmovArmToD:
    CHECK_COND(VmovArmToD);
    state_.vfp_regs[inst->vfp.sd * 2] = state_.regs[inst->vfp.rt];
    state_.vfp_regs[inst->vfp.sd * 2 + 1] = state_.regs[inst->vfp.rt2];
    ADVANCE_AND_DISPATCH();

handle_VmovDToArm:
    CHECK_COND(VmovDToArm);
    state_.regs[inst->vfp.rt] = state_.vfp_regs[inst->vfp.sd * 2];
    state_.regs[inst->vfp.rt2] = state_.vfp_regs[inst->vfp.sd * 2 + 1];
    ADVANCE_AND_DISPATCH();

handle_Vmrs:
    CHECK_COND(Vmrs);
    if (inst->vfp.rt == 15) {
        // Move FPSCR flags to CPSR (APSR_nzcv); SetPacked masks to bits 31-28
        state_.lazy_flags.SetPacked(state_.fpscr);
    } else {
        state_.regs[inst->vfp.rt] = state_.fpscr;
    }
    ADVANCE_AND_DISPATCH();

handle_Vmsr:
    CHECK_COND(Vmsr);
    state_.fpscr = state_.regs[inst->vfp.rt];
    ADVANCE_AND_DISPATCH();

handle_VaddS:
    CHECK_COND(VaddS);
    VFP_SHORT_VECTOR(0xee300a00u, false);
    VFP_S_WRITE(inst->vfp.sd, VFP_S_READ(inst->vfp.sn) + VFP_S_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VsubS:
    CHECK_COND(VsubS);
    VFP_SHORT_VECTOR(0xee300a40u, false);
    VFP_S_WRITE(inst->vfp.sd, VFP_S_READ(inst->vfp.sn) - VFP_S_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VmulS:
    CHECK_COND(VmulS);
    VFP_SHORT_VECTOR(0xee200a00u, false);
    VFP_S_WRITE(inst->vfp.sd, VFP_S_READ(inst->vfp.sn) * VFP_S_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

// Four independent unconditional scalar VMULs, decoded only after alias checks.
handle_Vmul4S: {
    VFP_FUSED_SHORT_VECTOR(false, false);
#ifdef __aarch64__
    const auto vector = vreinterpretq_f32_u32(vld1q_u32(&state_.vfp_regs[inst->vfp.sn]));
    const auto scalar = vreinterpretq_f32_u32(vdupq_n_u32(state_.vfp_regs[inst->vfp.sm]));
    float32x4_t result;
    // Preserve operand order for NaN payload propagation; Clang may commute intrinsics.
    if (inst->vfp.flags)
        asm("fmul %0.4s, %1.4s, %2.4s" : "=w"(result) : "w"(scalar), "w"(vector));
    else
        asm("fmul %0.4s, %1.4s, %2.4s" : "=w"(result) : "w"(vector), "w"(scalar));
    vst1q_u32(&state_.vfp_regs[inst->vfp.sd], vreinterpretq_u32_f32(result));
#else
    for (unsigned i = 0; i < 4; ++i) {
        const float a = std::bit_cast<float>(state_.vfp_regs[inst->vfp.sn + i]);
        const float b = std::bit_cast<float>(state_.vfp_regs[inst->vfp.sm]);
        state_.vfp_regs[inst->vfp.sd + i] = std::bit_cast<u32>(inst->vfp.flags ? b * a : a * b);
    }
#endif
    executed += inst->ticks;
    SET_PC(GET_PC() + inst->inst_size);
    inst += 3; // Original slots remain reserved to preserve block/event boundaries.
    DISPATCH();
}

// The decoder rejects every read-after-write dependency. Gather inputs before
// writing outputs; scatter in guest instruction order for repeated destinations.
handle_Vmul4Gather: {
    VFP_FUSED_SHORT_VECTOR(false, true);
#ifdef __aarch64__
    const uint32x4_t n{state_.vfp_regs[inst->vfp4.sn[0]], state_.vfp_regs[inst->vfp4.sn[1]],
                      state_.vfp_regs[inst->vfp4.sn[2]], state_.vfp_regs[inst->vfp4.sn[3]]};
    const uint32x4_t m{state_.vfp_regs[inst->vfp4.sm[0]], state_.vfp_regs[inst->vfp4.sm[1]],
                      state_.vfp_regs[inst->vfp4.sm[2]], state_.vfp_regs[inst->vfp4.sm[3]]};
    float32x4_t result;
    // Keep NaN operand priority identical to the scalar FMUL handler.
    asm("fmul %0.4s, %1.4s, %2.4s" : "=w"(result)
        : "w"(vreinterpretq_f32_u32(n)), "w"(vreinterpretq_f32_u32(m)));
    const auto bits = vreinterpretq_u32_f32(result);
    state_.vfp_regs[inst->vfp4.sd[0]] = vgetq_lane_u32(bits, 0);
    state_.vfp_regs[inst->vfp4.sd[1]] = vgetq_lane_u32(bits, 1);
    state_.vfp_regs[inst->vfp4.sd[2]] = vgetq_lane_u32(bits, 2);
    state_.vfp_regs[inst->vfp4.sd[3]] = vgetq_lane_u32(bits, 3);
#else
    for (unsigned i = 0; i < 4; ++i) {
        const auto n = VFP_S_READ(inst->vfp4.sn[i]);
        const auto m = VFP_S_READ(inst->vfp4.sm[i]);
        VFP_S_WRITE(inst->vfp4.sd[i], n * m);
    }
#endif
    executed += inst->ticks;
    SET_PC(GET_PC() + inst->inst_size);
    inst += 3; // Original slots remain reserved to preserve block/event boundaries.
    DISPATCH();
}

handle_Vmla4S: {
    VFP_FUSED_SHORT_VECTOR(true, false);
#ifdef __aarch64__
    const auto vector = vreinterpretq_f32_u32(vld1q_u32(&state_.vfp_regs[inst->vfp.sn]));
    const auto scalar = vreinterpretq_f32_u32(vdupq_n_u32(state_.vfp_regs[inst->vfp.sm]));
    float32x4_t result = vreinterpretq_f32_u32(vld1q_u32(&state_.vfp_regs[inst->vfp.sd]));
    // Preserve operand order for NaN payload propagation; Clang may commute intrinsics.
    if (inst->vfp.flags)
        asm("fmla %0.4s, %1.4s, %2.4s" : "+w"(result) : "w"(scalar), "w"(vector));
    else
        asm("fmla %0.4s, %1.4s, %2.4s" : "+w"(result) : "w"(vector), "w"(scalar));
    vst1q_u32(&state_.vfp_regs[inst->vfp.sd], vreinterpretq_u32_f32(result));
#else
    for (unsigned i = 0; i < 4; ++i) {
        const float a = std::bit_cast<float>(state_.vfp_regs[inst->vfp.sn + i]);
        const float b = std::bit_cast<float>(state_.vfp_regs[inst->vfp.sm]);
        state_.vfp_regs[inst->vfp.sd + i] = std::bit_cast<u32>(std::bit_cast<float>(state_.vfp_regs[inst->vfp.sd + i]) + (inst->vfp.flags ? b * a : a * b));
    }
#endif
    executed += inst->ticks;
    SET_PC(GET_PC() + inst->inst_size);
    inst += 3; // Original slots remain reserved to preserve block/event boundaries.
    DISPATCH();
}

handle_Vmla4Gather: {
    VFP_FUSED_SHORT_VECTOR(true, true);
#ifdef __aarch64__
    const uint32x4_t n{state_.vfp_regs[inst->vfp4.sn[0]], state_.vfp_regs[inst->vfp4.sn[1]],
                      state_.vfp_regs[inst->vfp4.sn[2]], state_.vfp_regs[inst->vfp4.sn[3]]};
    const uint32x4_t m{state_.vfp_regs[inst->vfp4.sm[0]], state_.vfp_regs[inst->vfp4.sm[1]],
                      state_.vfp_regs[inst->vfp4.sm[2]], state_.vfp_regs[inst->vfp4.sm[3]]};
    const uint32x4_t d{state_.vfp_regs[inst->vfp4.sd[0]], state_.vfp_regs[inst->vfp4.sd[1]],
                      state_.vfp_regs[inst->vfp4.sd[2]], state_.vfp_regs[inst->vfp4.sd[3]]};
    float32x4_t result = vreinterpretq_f32_u32(d);
    // Keep NaN operand priority identical to the scalar FMADD handler.
    asm("fmla %0.4s, %1.4s, %2.4s" : "+w"(result)
        : "w"(vreinterpretq_f32_u32(n)), "w"(vreinterpretq_f32_u32(m)));
    const auto bits = vreinterpretq_u32_f32(result);
    state_.vfp_regs[inst->vfp4.sd[0]] = vgetq_lane_u32(bits, 0);
    state_.vfp_regs[inst->vfp4.sd[1]] = vgetq_lane_u32(bits, 1);
    state_.vfp_regs[inst->vfp4.sd[2]] = vgetq_lane_u32(bits, 2);
    state_.vfp_regs[inst->vfp4.sd[3]] = vgetq_lane_u32(bits, 3);
#else
    for (unsigned i = 0; i < 4; ++i) {
        const auto n = VFP_S_READ(inst->vfp4.sn[i]);
        const auto m = VFP_S_READ(inst->vfp4.sm[i]);
        VFP_S_WRITE(inst->vfp4.sd[i], VFP_S_READ(inst->vfp4.sd[i]) + n * m);
    }
#endif
    executed += inst->ticks;
    SET_PC(GET_PC() + inst->inst_size);
    inst += 3; // Original slots remain reserved to preserve block/event boundaries.
    DISPATCH();
}

handle_VdivS:
    CHECK_COND(VdivS);
    VFP_SHORT_VECTOR(0xee800a00u, false);
    VFP_S_WRITE(inst->vfp.sd, VFP_S_READ(inst->vfp.sn) / VFP_S_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VmlaS:
    CHECK_COND(VmlaS);
    VFP_SHORT_VECTOR(0xee000a00u, false);
    VFP_S_WRITE(inst->vfp.sd, VFP_S_READ(inst->vfp.sd) + VFP_S_READ(inst->vfp.sn) * VFP_S_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VmlsS:
    CHECK_COND(VmlsS);
    VFP_SHORT_VECTOR(0xee000a40u, false);
    VFP_S_WRITE(inst->vfp.sd, VFP_S_READ(inst->vfp.sd) - VFP_S_READ(inst->vfp.sn) * VFP_S_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VnmlaS:
    CHECK_COND(VnmlaS);
    VFP_SHORT_VECTOR(0xee100a40u, false);
    VFP_S_WRITE(inst->vfp.sd, -VFP_S_READ(inst->vfp.sd) - VFP_S_READ(inst->vfp.sn) * VFP_S_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VnmlsS:
    CHECK_COND(VnmlsS);
    VFP_SHORT_VECTOR(0xee100a00u, false);
    VFP_S_WRITE(inst->vfp.sd, -VFP_S_READ(inst->vfp.sd) + VFP_S_READ(inst->vfp.sn) * VFP_S_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VnmulS:
    CHECK_COND(VnmulS);
    VFP_SHORT_VECTOR(0xee200a40u, false);
    VFP_S_WRITE(inst->vfp.sd, -(VFP_S_READ(inst->vfp.sn) * VFP_S_READ(inst->vfp.sm)));
    ADVANCE_AND_DISPATCH();

handle_VabsS:
    CHECK_COND(VabsS);
    VFP_SHORT_VECTOR(0xeeb00ac0u, false);
    VFP_S_WRITE(inst->vfp.sd, std::fabs(VFP_S_READ(inst->vfp.sm)));
    ADVANCE_AND_DISPATCH();

handle_VnegS:
    CHECK_COND(VnegS);
    VFP_SHORT_VECTOR(0xeeb10a40u, false);
    VFP_S_WRITE(inst->vfp.sd, -VFP_S_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VsqrtS:
    CHECK_COND(VsqrtS);
    VFP_SHORT_VECTOR(0xeeb10ac0u, false);
    VFP_S_WRITE(inst->vfp.sd, std::sqrt(VFP_S_READ(inst->vfp.sm)));
    ADVANCE_AND_DISPATCH();

handle_VmovS:
    CHECK_COND(VmovS);
    VFP_SHORT_VECTOR(0xeeb00a40u, false);
    VFP_S_WRITE_U32(inst->vfp.sd, VFP_S_READ_U32(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

// Expanded constant precomputed at decode into vimm.lo
handle_VmovImmS:
    CHECK_COND(VmovImmS);
    VFP_S_WRITE_U32(inst->vimm.sd, inst->vimm.lo);
    ADVANCE_AND_DISPATCH();

handle_VaddD:
    CHECK_COND(VaddD);
    VFP_SHORT_VECTOR(0xee300a00u, true);
    VFP_D_WRITE(inst->vfp.sd, VFP_D_READ(inst->vfp.sn) + VFP_D_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VsubD:
    CHECK_COND(VsubD);
    VFP_SHORT_VECTOR(0xee300a40u, true);
    VFP_D_WRITE(inst->vfp.sd, VFP_D_READ(inst->vfp.sn) - VFP_D_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VmulD:
    CHECK_COND(VmulD);
    VFP_SHORT_VECTOR(0xee200a00u, true);
    VFP_D_WRITE(inst->vfp.sd, VFP_D_READ(inst->vfp.sn) * VFP_D_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VdivD:
    CHECK_COND(VdivD);
    VFP_SHORT_VECTOR(0xee800a00u, true);
    VFP_D_WRITE(inst->vfp.sd, VFP_D_READ(inst->vfp.sn) / VFP_D_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VmlaD:
    CHECK_COND(VmlaD);
    VFP_SHORT_VECTOR(0xee000a00u, true);
    VFP_D_WRITE(inst->vfp.sd, VFP_D_READ(inst->vfp.sd) + VFP_D_READ(inst->vfp.sn) * VFP_D_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VmlsD:
    CHECK_COND(VmlsD);
    VFP_SHORT_VECTOR(0xee000a40u, true);
    VFP_D_WRITE(inst->vfp.sd, VFP_D_READ(inst->vfp.sd) - VFP_D_READ(inst->vfp.sn) * VFP_D_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VnmlaD:
    CHECK_COND(VnmlaD);
    VFP_SHORT_VECTOR(0xee100a40u, true);
    VFP_D_WRITE(inst->vfp.sd, -VFP_D_READ(inst->vfp.sd) - VFP_D_READ(inst->vfp.sn) * VFP_D_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VnmlsD:
    CHECK_COND(VnmlsD);
    VFP_SHORT_VECTOR(0xee100a00u, true);
    VFP_D_WRITE(inst->vfp.sd, -VFP_D_READ(inst->vfp.sd) + VFP_D_READ(inst->vfp.sn) * VFP_D_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VnmulD:
    CHECK_COND(VnmulD);
    VFP_SHORT_VECTOR(0xee200a40u, true);
    VFP_D_WRITE(inst->vfp.sd, -(VFP_D_READ(inst->vfp.sn) * VFP_D_READ(inst->vfp.sm)));
    ADVANCE_AND_DISPATCH();

handle_VabsD:
    CHECK_COND(VabsD);
    VFP_SHORT_VECTOR(0xeeb00ac0u, true);
    VFP_D_WRITE(inst->vfp.sd, std::fabs(VFP_D_READ(inst->vfp.sm)));
    ADVANCE_AND_DISPATCH();

handle_VnegD:
    CHECK_COND(VnegD);
    VFP_SHORT_VECTOR(0xeeb10a40u, true);
    VFP_D_WRITE(inst->vfp.sd, -VFP_D_READ(inst->vfp.sm));
    ADVANCE_AND_DISPATCH();

handle_VsqrtD:
    CHECK_COND(VsqrtD);
    VFP_SHORT_VECTOR(0xeeb10ac0u, true);
    VFP_D_WRITE(inst->vfp.sd, std::sqrt(VFP_D_READ(inst->vfp.sm)));
    ADVANCE_AND_DISPATCH();

handle_VmovD:
    CHECK_COND(VmovD);
    VFP_SHORT_VECTOR(0xeeb00a40u, true);
    state_.vfp_regs[inst->vfp.sd * 2] = state_.vfp_regs[inst->vfp.sm * 2];
    state_.vfp_regs[inst->vfp.sd * 2 + 1] = state_.vfp_regs[inst->vfp.sm * 2 + 1];
    ADVANCE_AND_DISPATCH();

// Expanded constant precomputed at decode into vimm.lo/hi
handle_VmovImmD:
    CHECK_COND(VmovImmD);
    state_.vfp_regs[inst->vimm.sd * 2] = inst->vimm.lo;
    state_.vfp_regs[inst->vimm.sd * 2 + 1] = inst->vimm.hi;
    ADVANCE_AND_DISPATCH();

handle_VcmpS:
    CHECK_COND(VcmpS);
    {
        float a = VFP_S_READ(inst->vfp.sd);
        float b = VFP_S_READ(inst->vfp.sm);
        u32 flags = 0;
        if (std::isnan(a) || std::isnan(b)) {
            flags = 0x30000000;  // C=1, V=1 (unordered)
        } else if (a == b) {
            flags = 0x60000000;  // Z=1, C=1
        } else if (a < b) {
            flags = 0x80000000;  // N=1
        } else {
            flags = 0x20000000;  // C=1
        }
        state_.fpscr = (state_.fpscr & 0x0FFFFFFF) | flags;
    }
    ADVANCE_AND_DISPATCH();

handle_VcmpVmrsS:
    {
        float a = VFP_S_READ(inst->vfp.sd);
        float b = VFP_S_READ(inst->vfp.sm);
        u32 flags = 0;
        if (std::isnan(a) || std::isnan(b)) {
            flags = 0x30000000; // C=1, V=1 (unordered)
        } else if (a == b) {
            flags = 0x60000000; // Z=1, C=1
        } else if (a < b) {
            flags = 0x80000000; // N=1
        } else {
            flags = 0x20000000; // C=1
        }
        state_.fpscr = (state_.fpscr & 0x0FFFFFFF) | flags;
    }
    state_.lazy_flags.SetPacked(state_.fpscr);
    executed += inst->ticks;
    SET_PC(GET_PC() + 8);
    ++inst; // Skip the retained VMRS APSR constituent slot.
    DISPATCH();

handle_VcmpD:
    CHECK_COND(VcmpD);
    {
        double a = VFP_D_READ(inst->vfp.sd);
        double b = VFP_D_READ(inst->vfp.sm);
        u32 flags = 0;
        if (std::isnan(a) || std::isnan(b)) {
            flags = 0x30000000;  // C=1, V=1 (unordered)
        } else if (a == b) {
            flags = 0x60000000;  // Z=1, C=1
        } else if (a < b) {
            flags = 0x80000000;  // N=1
        } else {
            flags = 0x20000000;  // C=1
        }
        state_.fpscr = (state_.fpscr & 0x0FFFFFFF) | flags;
    }
    ADVANCE_AND_DISPATCH();

handle_VcmpzS:
    CHECK_COND(VcmpzS);
    {
        float a = VFP_S_READ(inst->vfp.sd);
        u32 flags = 0;
        if (std::isnan(a)) {
            flags = 0x30000000;  // C=1, V=1 (unordered)
        } else if (a == 0.0f) {
            flags = 0x60000000;  // Z=1, C=1
        } else if (a < 0.0f) {
            flags = 0x80000000;  // N=1
        } else {
            flags = 0x20000000;  // C=1
        }
        state_.fpscr = (state_.fpscr & 0x0FFFFFFF) | flags;
    }
    ADVANCE_AND_DISPATCH();

handle_VcmpzD:
    CHECK_COND(VcmpzD);
    {
        double a = VFP_D_READ(inst->vfp.sd);
        u32 flags = 0;
        if (std::isnan(a)) {
            flags = 0x30000000;  // C=1, V=1 (unordered)
        } else if (a == 0.0) {
            flags = 0x60000000;  // Z=1, C=1
        } else if (a < 0.0) {
            flags = 0x80000000;  // N=1
        } else {
            flags = 0x20000000;  // C=1
        }
        state_.fpscr = (state_.fpscr & 0x0FFFFFFF) | flags;
    }
    ADVANCE_AND_DISPATCH();

handle_VcvtSToInt:
    CHECK_COND(VcvtSToInt);
    {
        float val = VFP_S_READ(inst->vfp.sm);
        // VCVT truncates towards zero; the cast does that directly. Out-of-range/NaN
        // values rely on ARM64 fcvtzs/fcvtzu saturation, matching VFP semantics.
        s32 result;
        if (inst->vfp.flags & 1) {
            // Unsigned
            result = static_cast<s32>(static_cast<u32>(val));
        } else {
            // Signed
            result = static_cast<s32>(val);
        }
        VFP_S_WRITE_U32(inst->vfp.sd, static_cast<u32>(result));
    }
    ADVANCE_AND_DISPATCH();

handle_VcvtIntToS:
    CHECK_COND(VcvtIntToS);
    {
        u32 bits = VFP_S_READ_U32(inst->vfp.sm);
        float result;
        if (inst->vfp.flags & 1) {
            // Unsigned
            result = static_cast<float>(bits);
        } else {
            // Signed
            result = static_cast<float>(static_cast<s32>(bits));
        }
        VFP_S_WRITE(inst->vfp.sd, result);
    }
    ADVANCE_AND_DISPATCH();

handle_VcvtDToS:
    CHECK_COND(VcvtDToS);
    VFP_S_WRITE(inst->vfp.sd, static_cast<float>(VFP_D_READ(inst->vfp.sm)));
    ADVANCE_AND_DISPATCH();

handle_VcvtSToD:
    CHECK_COND(VcvtSToD);
    VFP_D_WRITE(inst->vfp.sd, static_cast<double>(VFP_S_READ(inst->vfp.sm)));
    ADVANCE_AND_DISPATCH();

handle_VcvtDToInt:
    CHECK_COND(VcvtDToInt);
    {
        double val = VFP_D_READ(inst->vfp.sm);
        // VCVT truncates towards zero; the cast does that directly. Out-of-range/NaN
        // values rely on ARM64 fcvtzs/fcvtzu saturation, matching VFP semantics.
        s32 result;
        if (inst->vfp.flags & 1) {
            // Unsigned
            result = static_cast<s32>(static_cast<u32>(val));
        } else {
            // Signed
            result = static_cast<s32>(val);
        }
        VFP_S_WRITE_U32(inst->vfp.sd, static_cast<u32>(result));
    }
    ADVANCE_AND_DISPATCH();

handle_VcvtIntToD:
    CHECK_COND(VcvtIntToD);
    {
        u32 bits = VFP_S_READ_U32(inst->vfp.sm);
        double result;
        if (inst->vfp.flags & 1) {
            // Unsigned
            result = static_cast<double>(bits);
        } else {
            // Signed
            result = static_cast<double>(static_cast<s32>(bits));
        }
        VFP_D_WRITE(inst->vfp.sd, result);
    }
    ADVANCE_AND_DISPATCH();

handle_VldrS:
    CHECK_COND(VldrS);
    {
        u32 addr = state_.regs[inst->vfp.rn] + inst->vfp.offset;
        VFP_S_WRITE_U32(inst->vfp.sd, MemRead32(addr));
    }
    ADVANCE_AND_DISPATCH();

handle_VstrS:
    CHECK_COND(VstrS);
    {
        u32 addr = state_.regs[inst->vfp.rn] + inst->vfp.offset;
        MemWrite32(addr, VFP_S_READ_U32(inst->vfp.sd));
    }
    ADVANCE_AND_DISPATCH();

handle_VldrD:
    CHECK_COND(VldrD);
    {
        u32 addr = state_.regs[inst->vfp.rn] + inst->vfp.offset;
        // Only ordinary RAM can combine the two ordered word accesses. Null
        // pointers include tracked/coherent and MMIO pages; crossings (including
        // 32-bit address wrap) retain the original word-by-word fallback.
        const u8* page = ram_burst_ ? local_page_ptrs[addr >> FASTINTERP_PAGE_BITS] : nullptr;
        if (page && (addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 7) {
            std::memcpy(&state_.vfp_regs[inst->vfp.sd * 2],
                        page + (addr & FASTINTERP_PAGE_MASK), 8);
        } else {
            state_.vfp_regs[inst->vfp.sd * 2] = MemRead32(addr);
            state_.vfp_regs[inst->vfp.sd * 2 + 1] = MemRead32(addr + 4);
        }
    }
    ADVANCE_AND_DISPATCH();

handle_VstrD:
    CHECK_COND(VstrD);
    {
        u32 addr = state_.regs[inst->vfp.rn] + inst->vfp.offset;
        u8* page = ram_burst_ ? local_page_ptrs[addr >> FASTINTERP_PAGE_BITS] : nullptr;
        if (page && (addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 7) {
            std::memcpy(page + (addr & FASTINTERP_PAGE_MASK),
                        &state_.vfp_regs[inst->vfp.sd * 2], 8);
        } else {
            MemWrite32(addr, state_.vfp_regs[inst->vfp.sd * 2]);
            MemWrite32(addr + 4, state_.vfp_regs[inst->vfp.sd * 2 + 1]);
        }
    }
    ADVANCE_AND_DISPATCH();

// vfp.sd = first Sd register, vfp.sn = count, vfp.rn = base register
// vfp.flags: bit 0 = writeback, bit 1 = DB mode
handle_VldmS:
    CHECK_COND(VldmS);
    {
        u32 count = inst->vfp.sn;
        u32 first_reg = inst->vfp.sd;
        bool writeback = inst->vfp.flags & 1;
        bool decrement = inst->vfp.flags & 2;
        u32 base = state_.regs[inst->vfp.rn];
        u32 addr = decrement ? (base - count * 4) : base;

        const u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        const u32 bytes = count * 4;
        if (ram_burst_ && page && count && first_reg + count <= state_.vfp_regs.size() &&
            (addr & FASTINTERP_PAGE_MASK) + bytes <= FASTINTERP_PAGE_MASK + 1) {
            std::memcpy(&state_.vfp_regs[first_reg], page + (addr & FASTINTERP_PAGE_MASK), bytes);
        } else {
            for (u32 i = 0; i < count; i++) {
                VFP_S_WRITE_U32(first_reg + i, MemRead32(addr));
                addr += 4;
            }
        }

        if (writeback) {
            state_.regs[inst->vfp.rn] = decrement ? (base - count * 4) : (base + count * 4);
        }
    }
    ADVANCE_AND_DISPATCH();

handle_VstmS:
    CHECK_COND(VstmS);
    {
        u32 count = inst->vfp.sn;
        u32 first_reg = inst->vfp.sd;
        bool writeback = inst->vfp.flags & 1;
        bool decrement = inst->vfp.flags & 2;
        u32 base = state_.regs[inst->vfp.rn];
        u32 addr = decrement ? (base - count * 4) : base;

        u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        const u32 bytes = count * 4;
        if (ram_burst_ && page && count && first_reg + count <= state_.vfp_regs.size() &&
            (addr & FASTINTERP_PAGE_MASK) + bytes <= FASTINTERP_PAGE_MASK + 1) {
            std::memcpy(page + (addr & FASTINTERP_PAGE_MASK), &state_.vfp_regs[first_reg], bytes);
        } else {
            for (u32 i = 0; i < count; i++) {
                MemWrite32(addr, VFP_S_READ_U32(first_reg + i));
                addr += 4;
            }
        }

        if (writeback) {
            state_.regs[inst->vfp.rn] = decrement ? (base - count * 4) : (base + count * 4);
        }
    }
    ADVANCE_AND_DISPATCH();

handle_VldmD:
    CHECK_COND(VldmD);
    {
        u32 count = inst->vfp.sn;
        u32 first_reg = inst->vfp.sd;
        bool writeback = inst->vfp.flags & 1;
        bool decrement = inst->vfp.flags & 2;
        u32 base = state_.regs[inst->vfp.rn];
        u32 addr = decrement ? (base - count * 8) : base;

        const u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        const u32 bytes = count * 8;
        if (ram_burst_ && page && count && first_reg * 2 + count * 2 <= state_.vfp_regs.size() &&
            (addr & FASTINTERP_PAGE_MASK) + bytes <= FASTINTERP_PAGE_MASK + 1) {
            std::memcpy(&state_.vfp_regs[first_reg * 2], page + (addr & FASTINTERP_PAGE_MASK), bytes);
        } else {
            for (u32 i = 0; i < count; i++) {
                state_.vfp_regs[(first_reg + i) * 2] = MemRead32(addr);
                state_.vfp_regs[(first_reg + i) * 2 + 1] = MemRead32(addr + 4);
                addr += 8;
            }
        }

        if (writeback) {
            state_.regs[inst->vfp.rn] = decrement ? (base - count * 8) : (base + count * 8);
        }
    }
    ADVANCE_AND_DISPATCH();

handle_VstmD:
    CHECK_COND(VstmD);
    {
        u32 count = inst->vfp.sn;
        u32 first_reg = inst->vfp.sd;
        bool writeback = inst->vfp.flags & 1;
        bool decrement = inst->vfp.flags & 2;
        u32 base = state_.regs[inst->vfp.rn];
        u32 addr = decrement ? (base - count * 8) : base;

        u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        const u32 bytes = count * 8;
        if (ram_burst_ && page && count && first_reg * 2 + count * 2 <= state_.vfp_regs.size() &&
            (addr & FASTINTERP_PAGE_MASK) + bytes <= FASTINTERP_PAGE_MASK + 1) {
            std::memcpy(page + (addr & FASTINTERP_PAGE_MASK), &state_.vfp_regs[first_reg * 2], bytes);
        } else {
            for (u32 i = 0; i < count; i++) {
                MemWrite32(addr, state_.vfp_regs[(first_reg + i) * 2]);
                MemWrite32(addr + 4, state_.vfp_regs[(first_reg + i) * 2 + 1]);
                addr += 8;
            }
        }

        if (writeback) {
            state_.regs[inst->vfp.rn] = decrement ? (base - count * 8) : (base + count * 8);
        }
    }
    ADVANCE_AND_DISPATCH();

#undef VFP_S_READ
#undef VFP_S_WRITE
#undef VFP_S_READ_U32
#undef VFP_S_WRITE_U32
#undef VFP_D_READ
#undef VFP_D_WRITE

#undef VFP_SHORT_VECTOR
#undef VFP_FUSED_SHORT_VECTOR
