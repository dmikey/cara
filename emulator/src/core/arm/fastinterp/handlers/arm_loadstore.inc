// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

// ARM Load/Store handlers: register/scaled-offset (Ldr, LdrB, LdrH, LdrSB,
//     LdrSH, Str, StrB, StrH), immediate-offset fast paths (LdrImm, StrImm,
//     LdrBImm, StrBImm, LdrHImm, StrHImm), PC-relative literal (LdrLit),
//     doubleword (Ldrd, Strd), and block transfer (Ldm, Stm)

handle_Ldr: {
    CHECK_COND(Ldr);
    // rn==15 is only reachable behind a PcSetup prefix (regs[15] = pipeline
    // PC): word literals fold to LdrLit at decode, and the Thumb byte/half
    // literal alignment is baked into the decoded offset.
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(ShiftNoCarry(state_.regs[inst->ls.rm], inst->ls.shift_type,
                                                inst->ls.shift_imm));
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }

    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }

    state_.regs[inst->rd] = MemRead32(effective_addr);
    executed += inst->ticks;

    // If destination is PC, it's a branch
    if (inst->rd == 15) {
        u32 new_pc = state_.regs[15];
        state_.thumb_mode = new_pc & 1;
        SET_PC(new_pc & ~1u);
        goto chain;
    }

    SET_PC(GET_PC() + inst->inst_size);
    DISPATCH();
}

handle_LdrB: {
    CHECK_COND(LdrB);
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(ShiftNoCarry(state_.regs[inst->ls.rm], inst->ls.shift_type,
                                                inst->ls.shift_imm));
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }

    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }

    state_.regs[inst->rd] = MemRead8(effective_addr);
    ADVANCE_AND_DISPATCH();
}

handle_LdrH: {
    CHECK_COND(LdrH);
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(state_.regs[inst->ls.rm]);
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }

    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }

    state_.regs[inst->rd] = MemRead16(effective_addr);
    ADVANCE_AND_DISPATCH();
}

handle_LdrSB: {
    CHECK_COND(LdrSB);
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(state_.regs[inst->ls.rm]);
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }

    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }

    state_.regs[inst->rd] = static_cast<u32>(static_cast<s8>(MemRead8(effective_addr)));
    ADVANCE_AND_DISPATCH();
}

handle_LdrSH: {
    CHECK_COND(LdrSH);
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(state_.regs[inst->ls.rm]);
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }

    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }

    state_.regs[inst->rd] = static_cast<u32>(static_cast<s16>(MemRead16(effective_addr)));
    ADVANCE_AND_DISPATCH();
}

handle_Str: {
    CHECK_COND(Str);
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(ShiftNoCarry(state_.regs[inst->ls.rm], inst->ls.shift_type,
                                                inst->ls.shift_imm));
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }

    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }

    MemWrite32(effective_addr, state_.regs[inst->rd]);
    ADVANCE_AND_DISPATCH();
}

handle_StrB: {
    CHECK_COND(StrB);
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(ShiftNoCarry(state_.regs[inst->ls.rm], inst->ls.shift_type,
                                                inst->ls.shift_imm));
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }

    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }

    MemWrite8(effective_addr, static_cast<u8>(state_.regs[inst->rd]));
    ADVANCE_AND_DISPATCH();
}

handle_StrH: {
    CHECK_COND(StrH);
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(state_.regs[inst->ls.rm]);
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }

    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }

    MemWrite16(effective_addr, static_cast<u16>(state_.regs[inst->rd]));
    ADVANCE_AND_DISPATCH();
}

// Specialized immediate-offset load/store (decode guarantees: offset mode,
// immediate offset, rd != 15, rn != 15)

// One guard for two 16-byte extents, followed by the original EIGHT ordered
// scalar memory operations. Never load-four/store-four or memcpy the region.
handle_LdrStrFourPairsImm: {
    CHECK_COND(LdrStrFourPairsImm);
    const u32 read_address = state_.regs[inst[0].ls.rn] + inst[0].ls.offset;
    const u32 write_address = state_.regs[inst[1].ls.rn] + inst[1].ls.offset;
    const auto ro = read_address & FASTINTERP_PAGE_MASK;
    const auto wo = write_address & FASTINTERP_PAGE_MASK;
    const auto* read_page = local_page_ptrs[read_address >> FASTINTERP_PAGE_BITS];
    auto* write_page = local_page_ptrs[write_address >> FASTINTERP_PAGE_BITS];
    u64 ticks = 0;
    for (unsigned i = 0; i < 8; ++i) ticks += inst[i].ticks;
    const s64 budget = timer->GetDowncount();
    const bool direct = !stepping_ && !halted_ && budget > 0 &&
        executed <= static_cast<u64>(budget) && ticks <= static_cast<u64>(budget) - executed &&
        page_table_ && page_table_->attributes[read_address >> FASTINTERP_PAGE_BITS] == Memory::PageType::Memory &&
        page_table_->attributes[write_address >> FASTINTERP_PAGE_BITS] == Memory::PageType::Memory &&
        read_page && write_page && !(read_address & 3) && !(write_address & 3) &&
        ro <= FASTINTERP_PAGE_MASK - 15 && wo <= FASTINTERP_PAGE_MASK - 15;
    if (direct) {
        const auto* read_ptr = read_page + ro;
        auto* write_ptr = write_page + wo;
        const auto disjoint = [](const void* ptr, std::size_t bytes,
                                 const void* object, std::size_t object_bytes) {
            const auto p = reinterpret_cast<std::uintptr_t>(ptr);
            const auto q = reinterpret_cast<std::uintptr_t>(object);
            const auto max = std::numeric_limits<std::uintptr_t>::max();
            return p <= max - bytes && q <= max - object_bytes &&
                   (p + bytes <= q || q + object_bytes <= p);
        };
        const auto safe = [&](const void* ptr) {
            return disjoint(ptr,16,this,sizeof(*this)) &&
                disjoint(ptr,16,block,sizeof(*block)) &&
                disjoint(ptr,16,page_table_.get(),sizeof(*page_table_)) &&
                disjoint(ptr,16,local_page_ptrs,
                         (std::size_t(1) << (32 - FASTINTERP_PAGE_BITS)) * sizeof(*local_page_ptrs));
        };
        // Current guest-code page aliases also decline, including another VA
        // mapped onto the same host code bytes. Original scalar handler remains
        // responsible for all tracked/coherent/MMIO/invalidation paths.
        const auto* code_page = local_page_ptrs[GET_PC() >> FASTINTERP_PAGE_BITS];
        if (safe(read_ptr) && safe(write_ptr) &&
            code_page && (GET_PC() & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 31 &&
            disjoint(write_ptr,16,code_page,std::size_t(1) << FASTINTERP_PAGE_BITS)) {
            if (ldr_str_four_pairs_census_) ++ldr_str_four_pairs_fused_;
            for (unsigned pair = 0; pair < 4; ++pair) {
                std::memcpy(&state_.regs[inst->rd], read_ptr + pair * 4, 4);
                executed += inst->ticks;
                SET_PC(GET_PC() + inst->inst_size);
                ++inst;
                std::memcpy(write_ptr + pair * 4, &state_.regs[inst->rd], 4);
                executed += inst->ticks;
                SET_PC(GET_PC() + inst->inst_size);
                ++inst;
            }
            --inst; // DISPATCH advances to the first unconsumed retained slot.
            DISPATCH();
        }
    }
    if (ldr_str_four_pairs_census_) ++ldr_str_four_pairs_fallback_;
    goto handle_LdrImm_al; // no architectural effect has occurred yet
}

handle_LdrImm: {
    CHECK_COND(LdrImm);
    MEM_READ32(state_.regs[inst->ls.rn] + inst->ls.offset, state_.regs[inst->rd]);
    ADVANCE_AND_DISPATCH();
}

// Decoder-proven ARM AL immediate offset, no writeback, SP base, non-PC dest.
// Keep the original memory macro and constituent bookkeeping unchanged.
handle_LdrSpImm: {
    CHECK_COND(LdrSpImm);
    MEM_READ32(state_.regs[13] + inst->ls.offset, state_.regs[inst->rd]);
    ADVANCE_AND_DISPATCH();
}

handle_StrImm: {
    CHECK_COND(StrImm);
    MEM_WRITE32(state_.regs[inst->ls.rn] + inst->ls.offset, state_.regs[inst->rd]);
    ADVANCE_AND_DISPATCH();
}

handle_LdrBImm: {
    CHECK_COND(LdrBImm);
    MEM_READ8(state_.regs[inst->ls.rn] + inst->ls.offset, state_.regs[inst->rd]);
    ADVANCE_AND_DISPATCH();
}

handle_StrBImm: {
    CHECK_COND(StrBImm);
    MEM_WRITE8(state_.regs[inst->ls.rn] + inst->ls.offset, state_.regs[inst->rd]);
    ADVANCE_AND_DISPATCH();
}

handle_LdrHImm: {
    CHECK_COND(LdrHImm);
    u16 val;
    MEM_READ16(state_.regs[inst->ls.rn] + inst->ls.offset, val);
    state_.regs[inst->rd] = val; // zero-extend (matches generic LdrH)
    ADVANCE_AND_DISPATCH();
}

handle_StrHImm: {
    CHECK_COND(StrHImm);
    MEM_WRITE16(state_.regs[inst->ls.rn] + inst->ls.offset, state_.regs[inst->rd]);
    ADVANCE_AND_DISPATCH();
}

// LDR literal: ls.offset holds the absolute literal-pool address,
// precomputed at decode from the pipeline-adjusted PC. rd==15 is a branch
// (ARM jump tables use LDR PC, [PC, #imm]).
handle_LdrLit: {
    CHECK_COND(LdrLit);
    u32 value;
    MEM_READ32(static_cast<u32>(inst->ls.offset), value);
    state_.regs[inst->rd] = value;
    executed += inst->ticks;
    if (inst->rd == 15) {
        state_.thumb_mode = value & 1;
        SET_PC(value & ~1u);
        goto chain;
    }
    SET_PC(GET_PC() + inst->inst_size);
    DISPATCH();
}

// ----------------------------------------------------------------------------
// Doubleword Load/Store
// ----------------------------------------------------------------------------

handle_Ldrd: {
    CHECK_COND(Ldrd);
    // rn==15 (LDRD literal) arrives behind a PcSetup prefix with the
    // alignment baked into the decoded offset
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(state_.regs[inst->ls.rm]);
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }
    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }
    state_.regs[inst->rd] = MemRead32(effective_addr);
    state_.regs[inst->ls.rt2] = MemRead32(effective_addr + 4);
    executed += inst->ticks;
    // If Rt2 == PC it's a branch (decode rejects this; kept for safety)
    if (inst->ls.rt2 == 15) {
        u32 new_pc = state_.regs[15];
        state_.thumb_mode = new_pc & 1;
        SET_PC(new_pc & ~1u);
        goto chain;
    }
    SET_PC(GET_PC() + inst->inst_size);
    DISPATCH();
}

handle_Strd: {
    CHECK_COND(Strd);
    u32 addr = state_.regs[inst->ls.rn];
    s32 offset = inst->ls.offset;
    if (inst->ls.rm != 0xFF) {
        offset = static_cast<s32>(state_.regs[inst->ls.rm]);
        if (!(inst->ls.flags & 0x01)) {
            offset = -offset; // U==0 => subtract register offset
        }
    }
    u32 effective_addr;
    switch (inst->ls.mode) {
    case AddrMode::Offset:
        effective_addr = addr + offset;
        break;
    case AddrMode::PreIndex:
        effective_addr = addr + offset;
        state_.regs[inst->ls.rn] = effective_addr;
        break;
    case AddrMode::PostIndex:
        effective_addr = addr;
        state_.regs[inst->ls.rn] = addr + offset;
        break;
    }
    MemWrite32(effective_addr, state_.regs[inst->rd]);
    MemWrite32(effective_addr + 4, state_.regs[inst->ls.rt2]);
    ADVANCE_AND_DISPATCH();
}

// ----------------------------------------------------------------------------
// Load/Store Multiple
// ----------------------------------------------------------------------------

handle_Ldm: {
    CHECK_COND(Ldm);
    u32 addr = state_.regs[inst->lsm.rn];
    u16 reg_list = inst->lsm.reg_list;
    bool writeback = (inst->lsm.flags & 0x01) != 0;  // W bit (bit 0)

    // count/base_off/wb_delta precomputed at decode (fold increment/before)
    u32 current = addr + inst->lsm.base_off;

    // Fast path: if all accesses are on the same mapped page, use direct pointer
    // This avoids per-access null checks in the loop (critical for LDM performance)
    const u8* base_page = local_page_ptrs[current >> FASTINTERP_PAGE_BITS];
    bool same_page = inst->lsm.count && (current & FASTINTERP_PAGE_MASK) + inst->lsm.count * 4 <= FASTINTERP_PAGE_MASK + 1;

    // Restrict bulk copies to direct RAM on one guest page. The extra host
    // range check also preserves scalar ordering in aliased test mappings.
    bool contiguous_ram = false;
    if (inst->lsm.contiguous_first != 0xFF && base_page && same_page) {
        contiguous_ram = ContiguousTransferRangesDisjoint(
            reinterpret_cast<std::uintptr_t>(base_page), current & FASTINTERP_PAGE_MASK,
            inst->lsm.count * 4, reinterpret_cast<std::uintptr_t>(state_.regs.data()));
    }
    if (contiguous_ram) {
        if (Common::FrameTiming::counters.enabled) {
            ++Common::FrameTiming::counters.contiguous_transfer_loads;
            Common::FrameTiming::counters.contiguous_transfer_bytes += inst->lsm.count * 4;
        }
        const auto* source = base_page + (current & FASTINTERP_PAGE_MASK);
        auto* destination = &state_.regs[inst->lsm.contiguous_first];
        if (inst->lsm.count == 4) std::memcpy(destination, source, 16);
        else std::memcpy(destination, source, 32);
    } else if (ram_burst_ && inst->lsm.count <= 4 && base_page != nullptr && same_page) {
        const u8* source = base_page + (current & FASTINTERP_PAGE_MASK);
        u32 remaining = reg_list;
        while (remaining) {
            const u32 reg = std::countr_zero(remaining);
            std::memcpy(&state_.regs[reg], source, 4);
            source += 4;
            remaining &= remaining - 1;
        }
    } else if (__builtin_expect(base_page != nullptr && same_page, 1)) {
        // Fast path: direct pointer access without per-iteration checks
        for (int i = 0; i < 16; i++) {
            if (reg_list & (1 << i)) {
                u32 loaded_val;
                std::memcpy(&loaded_val, &base_page[current & FASTINTERP_PAGE_MASK], 4);
                state_.regs[i] = loaded_val;
                current += 4;
            }
        }
    } else {
        // Slow path: per-access checks (page boundary crossing or unmapped)
        for (int i = 0; i < 16; i++) {
            if (reg_list & (1 << i)) {
                state_.regs[i] = MemRead32(current);
                current += 4;
            }
        }
    }

    if (writeback) {
        state_.regs[inst->lsm.rn] = addr + inst->lsm.wb_delta;
    }

    executed += inst->ticks;

    // If PC was loaded, it's a branch - don't advance PC, end block
    if (reg_list & (1 << 15)) {
        // PC was loaded from memory - handle mode switch if needed
        u32 new_pc = state_.regs[15];
        state_.thumb_mode = new_pc & 1;
        SET_PC(new_pc & ~1u);
        goto chain;
    }

    SET_PC(GET_PC() + inst->inst_size);
    DISPATCH();
}

handle_Stm: {
    CHECK_COND(Stm);
    u32 addr = state_.regs[inst->lsm.rn];
    u16 reg_list = inst->lsm.reg_list;
    bool writeback = (inst->lsm.flags & 0x01) != 0;  // W bit (bit 0)

    // count/base_off/wb_delta precomputed at decode (fold increment/before)
    u32 current = addr + inst->lsm.base_off;

    // Fast path: if all accesses are on the same mapped page, use direct pointer
    u8* base_page = local_page_ptrs[current >> FASTINTERP_PAGE_BITS];
    bool same_page = inst->lsm.count && (current & FASTINTERP_PAGE_MASK) + inst->lsm.count * 4 <= FASTINTERP_PAGE_MASK + 1;

    // Restrict bulk copies to direct RAM on one guest page. The extra host
    // range check also preserves scalar ordering in aliased test mappings.
    bool contiguous_ram = false;
    if (inst->lsm.contiguous_first != 0xFF && base_page && same_page) {
        contiguous_ram = ContiguousTransferRangesDisjoint(
            reinterpret_cast<std::uintptr_t>(base_page), current & FASTINTERP_PAGE_MASK,
            inst->lsm.count * 4, reinterpret_cast<std::uintptr_t>(state_.regs.data()));
    }
    if (contiguous_ram) {
        if (Common::FrameTiming::counters.enabled) {
            ++Common::FrameTiming::counters.contiguous_transfer_stores;
            Common::FrameTiming::counters.contiguous_transfer_bytes += inst->lsm.count * 4;
        }
        const auto* source = &state_.regs[inst->lsm.contiguous_first];
        auto* destination = base_page + (current & FASTINTERP_PAGE_MASK);
        if (inst->lsm.count == 4) std::memcpy(destination, source, 16);
        else std::memcpy(destination, source, 32);
    } else if (ram_burst_ && inst->lsm.count <= 4 && base_page != nullptr && same_page) {
        u8* destination = base_page + (current & FASTINTERP_PAGE_MASK);
        u32 remaining = reg_list;
        while (remaining) {
            const u32 reg = std::countr_zero(remaining);
            std::memcpy(destination, &state_.regs[reg], 4);
            destination += 4;
            remaining &= remaining - 1;
        }
    } else if (__builtin_expect(base_page != nullptr && same_page, 1)) {
        // Fast path: direct pointer access without per-iteration checks
        for (int i = 0; i < 16; i++) {
            if (reg_list & (1 << i)) {
                u32 val = state_.regs[i];
                std::memcpy(&base_page[current & FASTINTERP_PAGE_MASK], &val, 4);
                current += 4;
            }
        }
    } else {
        // Slow path: per-access checks (page boundary crossing or unmapped)
        for (int i = 0; i < 16; i++) {
            if (reg_list & (1 << i)) {
                MemWrite32(current, state_.regs[i]);
                current += 4;
            }
        }
    }

    if (writeback) {
        state_.regs[inst->lsm.rn] = addr + inst->lsm.wb_delta;
    }

    ADVANCE_AND_DISPATCH();
}
