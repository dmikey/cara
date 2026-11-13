// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <bit>
#include <algorithm>
#include "common/frame_timing.h"
#include <cmath>
#include <cstdlib>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
#include <cstring>

#include "common/assert.h"
#include "common/atomic_ops.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/arm/fastinterp/arm_tick_counts.h"
#include "core/arm/fastinterp/fastinterp.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/thread.h"
#include "video_core/video_core.h"
#include "core/memory.h"

class FastInterpThreadContext final : public ARM_Interface::ThreadContext {
public:
    FastInterpThreadContext() {
        Reset();
    }
    ~FastInterpThreadContext() override = default;

    void Reset() override {
        cpu_registers = {};
        cpsr = 0;
        fpu_registers = {};
        fpscr = 0;
        fpexc = 0;
    }

    u32 GetCpuRegister(std::size_t index) const override {
        return cpu_registers[index];
    }
    void SetCpuRegister(std::size_t index, u32 value) override {
        cpu_registers[index] = value;
    }
    u32 GetCpsr() const override {
        return cpsr;
    }
    void SetCpsr(u32 value) override {
        cpsr = value;
    }
    u32 GetFpuRegister(std::size_t index) const override {
        return fpu_registers[index];
    }
    void SetFpuRegister(std::size_t index, u32 value) override {
        fpu_registers[index] = value;
    }
    u32 GetFpscr() const override {
        return fpscr;
    }
    void SetFpscr(u32 value) override {
        fpscr = value;
    }
    u32 GetFpexc() const override {
        return fpexc;
    }
    void SetFpexc(u32 value) override {
        fpexc = value;
    }

private:
    friend class Core::FastInterp::ARM_FastInterp;

    std::array<u32, 16> cpu_registers;
    u32 cpsr;
    std::array<u32, 64> fpu_registers;
    u32 fpscr;
    u32 fpexc;
};


namespace Core::FastInterp {

// MMJ's scalar memory API assumes an access fits within one host page.
// Keep the common path direct; split uncommon unaligned page crossings explicitly.
template <typename T>
static T ReadCoherent(Memory::MemorySystem& memory, u32 addr) {
    if ((addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - (sizeof(T) - 1)) {
        if constexpr (sizeof(T) == 2) return memory.Read16(addr);
        else return memory.Read32(addr);
    }
    T value = 0;
    for (u32 i = 0; i < sizeof(T); ++i) value |= static_cast<T>(memory.Read8(addr + i)) << (8 * i);
    return value;
}
template <typename T>
static void WriteCoherent(Memory::MemorySystem& memory, u32 addr, T value) {
    if ((addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - (sizeof(T) - 1)) {
        if constexpr (sizeof(T) == 2) memory.Write16(addr, value);
        else memory.Write32(addr, value);
        return;
    }
    for (u32 i = 0; i < sizeof(T); ++i) memory.Write8(addr + i, static_cast<u8>(value >> (8 * i)));
}

// Never-null page table. When no page table is set, page_pointers points at
// this zero-initialized array (8MB of BSS nullptrs, zero-fill-on-demand, touched
// only if the guest runs without a page table), so the MEM_* fast paths and the
// unmapped-PC check never need a null test on the array itself.
static u8* empty_page_table[1 << (32 - FASTINTERP_PAGE_BITS)];

// Included inside the namespace: the macros reference LazyFlags and CondPassed.
#ifdef __aarch64__
#include "core/arm/fastinterp/fastinterp_arm64.inc"
#endif

ARM_FastInterp::ARM_FastInterp(Core::System& system, Memory::MemorySystem& memory, u32 id,
                               std::shared_ptr<Core::Timing::Timer> timer)
    : ARM_Interface(id, timer), system_(system), guest_pc_cpu_(GuestPCCPU::FromEnvironment()), memory_(memory),
      svc_context_(std::make_unique<Kernel::SVCContext>(system)) {
    if (guest_pc_cpu_.Enabled()) guest_pc_cpu_.Calibrate();
    LOG_INFO(Core_ARM11, "FastInterp ARM interpreter created for core {}", id);
    state_.regs.fill(0);
    state_.vfp_regs.fill(0);
    state_.cpsr_other = 0x10; // User mode
    state_.fpscr = 0;
    state_.fpexc = 0;
    state_.thumb_mode = false;
    state_.page_pointers = empty_page_table; // Never null
    const char* bkpt = std::getenv("CARA_FASTINTERP_DECODE_BKPT");
    decode_breakpoints_ = bkpt && std::strcmp(bkpt, "1") == 0;
    const char* al_dispatch = std::getenv("CARA_FASTINTERP_AL_DISPATCH");
    al_dispatch_ = al_dispatch && std::strcmp(al_dispatch, "1") == 0;
    const char* secondary_link = std::getenv("CARA_FASTINTERP_SECONDARY_LINK");
    secondary_link_ = secondary_link && std::strcmp(secondary_link, "1") == 0;
    const char* burst = std::getenv("CARA_FASTINTERP_RAM_BURST");
    ram_burst_ = burst && std::strcmp(burst, "1") == 0;
    const char* contiguous = std::getenv("CARA_FASTINTERP_CONTIGUOUS_TRANSFERS");
    contiguous_transfers_ = contiguous && std::strcmp(contiguous, "1") == 0;
    const char* precise_end = std::getenv("CARA_FASTINTERP_PRECISE_BLOCK_END");
    precise_block_end_ = !precise_end || std::strcmp(precise_end, "0") != 0;
    const char* chain_budget = std::getenv("CARA_FASTINTERP_CHAIN_BUDGET");
    chain_budget_ = chain_budget && std::strcmp(chain_budget, "1") == 0;
    const char* cache_ways = std::getenv("CARA_FASTINTERP_CACHE_WAYS");
    cache_.SetEightWay(cache_ways && std::strcmp(cache_ways, "8") == 0);
    const char* cache_profile = std::getenv("CARA_FASTINTERP_CACHE_PROFILE");
    cache_.SetProfiling(cache_profile && std::strcmp(cache_profile, "1") == 0);
    decode_cost_.SetEnabled(cache_.ProfilingEnabled());
}

ARM_FastInterp::~ARM_FastInterp() = default;

// Log each unique unhandled encoding once: a bad decode in a loop would
// otherwise flood the log on every block entry.
void ARM_FastInterp::LogUnhandledInstruction(u32 pc) {
    static std::array<u32, 64> seen{};
    static u32 seen_count = 0;
    const u32 raw = state_.thumb_mode ? ReadMemory16(pc) : ReadMemory32(pc);
    const u32 key = (raw & 0x0FF000F0) | (state_.thumb_mode ? 0x80000000u : 0);
    for (u32 i = 0; i < seen_count; i++) {
        if (seen[i] == key) {
            return;
        }
    }
    if (seen_count >= seen.size()) {
        return;
    }
    seen[seen_count++] = key;
    LOG_ERROR(Core_ARM11, "Unhandled instruction at PC {:08X}: raw={:08X}, thumb={}, cpsr={:08X}",
              pc, raw, state_.thumb_mode, GetCPSR());
}

[[gnu::noinline]] static void LogBreakpointInstruction(u32 immediate) {
    LOG_DEBUG(Core_ARM11, "Breakpoint instruction hit. Immediate: {:#010X}", immediate);
}

bool ARM_FastInterp::AluPairsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_FASTINTERP_ALU_PAIRS");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

void ARM_FastInterp::WriteBlockProfile(std::ostream& output) const {
    // The existing hotness counters have no instrumentation overhead. Their
    // census is incomplete after eviction and counts fused-loop dispatches, not
    // elapsed time or individual guest instructions. Never infer idleness from
    // this ranking alone.
    std::vector<const BasicBlock*> blocks;
    cache_.VisitResidentBlocks([&](const BasicBlock& block) {
        if (block.exec_count) blocks.push_back(&block);
    });
    std::sort(blocks.begin(), blocks.end(), [](const auto* a, const auto* b) {
        return a->exec_count != b->exec_count ? a->exec_count > b->exec_count
                                             : a->start_pc < b->start_pc;
    });
    u64 total = 0;
    for (const auto* block : blocks) total += block->exec_count;
    const char* alu_flag = std::getenv("CARA_FASTINTERP_ALU_OPERANDS");
    const bool alu_enabled = alu_flag && std::strcmp(alu_flag, "1") == 0;
    const bool pairs_enabled = AluPairsEnabled();
    std::array<u64, 8> pair_weighted{};
    std::array<u64, 4> alu_weighted{};
    for (const auto* block : blocks)
        for (unsigned slot = 0; slot < block->inst_count; ++slot) {
            const auto opcode = block->instructions[slot].opcode;
            if (opcode >= Opcode::AddImm && opcode <= Opcode::MovLslImm)
                alu_weighted[unsigned(opcode) - unsigned(Opcode::AddImm)] += block->exec_count;
            if (opcode >= Opcode::AddPairImmImm && opcode <= Opcode::MovPairLslLsl) {
                const unsigned pair = unsigned(opcode) - unsigned(Opcode::AddPairImmImm);
                pair_weighted[pair] += block->exec_count;
                alu_weighted[(pair / 4) * 2 + (pair % 4) / 2] += block->exec_count;
                alu_weighted[(pair / 4) * 2 + pair % 2] += block->exec_count;
                ++slot; // counts above include both logical original operations
            }
        }
    u64 unrolled_blocks = 0, unrolled_dispatches = 0;
    for (const auto* block : blocks)
        for (unsigned slot = 0; slot < block->inst_count; ++slot) {
            const auto opcode = block->instructions[slot].opcode;
            if (opcode == Opcode::ArmUnrolledSubsLoop ||
                opcode == Opcode::ThumbUnrolledSubsLoop || opcode == Opcode::ThumbUnrolledCmpLoop) {
                ++unrolled_blocks;
                unrolled_dispatches += block->exec_count;
            }
        }
    const auto& cache_profile = cache_.GetProfile();
    output << "{\"guest_pc_cpu\":";
    guest_pc_cpu_.Write(output);
    output << ",\"simd_tags_enabled\":" << (cache_.SimdTagsEnabled() ? "true" : "false")
           << ",\"simd_tags_supported\":" << (BlockCache::SimdTagsSupported() ? "true" : "false")
           << ",\"tag_comparison_semantics\":\"logical_scalar_equivalent_prefix_probes\"";
    output << ",\"cache_chain_recency\":" << (cache_.ChainRecencyEnabled() ? "true" : "false")
           << ",\"cache_replacement_profile\":";
    cache_.WriteReplacementProfile(output);
    output << ",\"decode_cost_profile\":";
    decode_cost_.Write(output);
    output << ",\"cache_secondary_probes\":" << cache_profile.secondary_probes
           << ",\"cache_secondary_hits\":" << cache_profile.secondary_hits
           << ",\"cache_lookup_calls\":" << cache_profile.lookup_calls
           << ",\"cache_lookup_hits\":" << cache_profile.lookup_hits
           << ",\"cache_lookup_tag_comparisons\":" << cache_profile.lookup_tag_comparisons
           << ",\"cache_lookup_or_allocate_calls\":" << cache_profile.lookup_or_allocate_calls
           << ",\"cache_lookup_or_allocate_hits\":" << cache_profile.lookup_or_allocate_hits
           << ",\"cache_lookup_or_allocate_tag_comparisons\":" << cache_profile.lookup_or_allocate_tag_comparisons;
    output << ",\"chain_budget_enabled\":" << (chain_budget_ ? "true" : "false")
           << ",\"precise_block_end_enabled\":" << (precise_block_end_ ? "true" : "false")
           << ",\"al_dispatch_enabled\":" << (al_dispatch_ ? "true" : "false")
           << ",\"secondary_link_enabled\":" << (secondary_link_ ? "true" : "false")
           << ",\"cache_ways\":" << cache_.Ways()
           << ",\"cache_sets\":" << cache_.Sets() << ",\"cache_entries\":" << CACHE_SIZE
           << ",\"cache_object_bytes\":" << sizeof(cache_)
           << ",\"hot_blocks_reoptimized\":" << fusion_stats_.hot_blocks_reoptimized
           << ",\"unrolled_resident_blocks\":" << unrolled_blocks
           << ",\"unrolled_resident_entry_weighted_dispatches\":" << unrolled_dispatches
           << ",\"blocks_decoded\":" << fusion_stats_.blocks_decoded
           << ",\"cache_profile_enabled\":" << (cache_.ProfilingEnabled() ? "true" : "false")
           << ",\"cache_allocation_misses\":" << cache_profile.allocation_misses
           << ",\"cache_empty_allocations\":" << cache_profile.empty_allocations
           << ",\"cache_live_replacements\":" << cache_profile.live_replacements
           << ",\"cache_clears\":" << cache_profile.clears
           << ",\"cache_invalidation_calls\":" << cache_profile.invalidation_calls
           << ",\"cache_invalidated_entries\":" << cache_profile.invalidated_entries
           << ",\"cache_mode_redecodes\":" << cache_mode_redecodes_
           << ",\"cache_page_table_changes\":" << cache_page_table_changes_
           << ",\"alu_operands_enabled\":" << (alu_enabled ? "true" : "false")
           << ",\"alu_pairs_enabled\":" << (pairs_enabled ? "true" : "false")
           << ",\"alu_pair_resident_entry_weighted_dispatches\":{\"AddPairImmImm\":" << pair_weighted[0]
           << ",\"AddPairImmLsl\":" << pair_weighted[1] << ",\"AddPairLslImm\":" << pair_weighted[2]
           << ",\"AddPairLslLsl\":" << pair_weighted[3] << ",\"MovPairImmImm\":" << pair_weighted[4]
           << ",\"MovPairImmLsl\":" << pair_weighted[5] << ",\"MovPairLslImm\":" << pair_weighted[6]
           << ",\"MovPairLslLsl\":" << pair_weighted[7] << "}"
           << ",\"alu_resident_entry_weighted_slots\":{\"AddImm\":" << alu_weighted[0]
           << ",\"AddLslImm\":" << alu_weighted[1] << ",\"MovImm\":" << alu_weighted[2]
           << ",\"MovLslImm\":" << alu_weighted[3] << "},\"core\":" << GetID() << ",\"resident_entries\":" << blocks.size()
           << ",\"resident_executions\":" << total << ",\"blocks\":[";
    for (size_t i = 0; i < std::min<size_t>(blocks.size(), 128); ++i) {
        const auto& block = *blocks[i];
        output << (i ? "," : "") << "{\"pc\":" << block.start_pc
               << ",\"end_pc\":" << block.end_pc << ",\"executions\":" << block.exec_count
               << ",\"thumb\":" << (block.IsThumb() ? "true" : "false") << ",\"opcodes\":[";
        for (unsigned j = 0; j < block.inst_count; ++j) {
            const auto opcode = block.instructions[j].opcode;
            output << (j ? "," : "") << unsigned(opcode);
            if (opcode == Opcode::Vmul4S || opcode == Opcode::Vmul4Gather ||
                opcode == Opcode::Vmla4S || opcode == Opcode::Vmla4Gather)
                j += 3; // Reserved constituent slots are skipped by the packed handler.
            else if (opcode == Opcode::AddCmpImmBcc || opcode == Opcode::VcmpVmrsS)
                ++j; // Retained constituent is skipped by the fused handler.
        }
        output << "],\"source_bytes\":[";
        // Read code only from ordinary mapped RAM. Avoid MemorySystem reads:
        // diagnostics must not trigger MMIO or invalidate a cached GPU surface.
        const u64 end = std::min<u64>(block.end_pc, u64(block.start_pc) + 256);
        for (u64 address = block.start_pc; address < end; ++address) {
            const auto* page = state_.page_pointers[address >> FASTINTERP_PAGE_BITS];
            if (!page) break;
            output << (address != block.start_pc ? "," : "")
                   << unsigned(page[address & FASTINTERP_PAGE_MASK]);
        }
        output << "]}";
    }
    output << "]}";
}

void ARM_FastInterp::Run() {
    Common::FrameTiming::Scope timing_scope(Common::FrameTiming::Phase::GuestCPU);
    halted_ = false;
    if (decode_cost_.Enabled()) decode_cost_.ObserveRunFrame(VideoCore::GetCurrentFrame());
    if (guest_pc_cpu_.Enabled()) {
        profile_thread_id_ = ProfileThreadId();
        guest_pc_cpu_.BeginRun(VideoCore::GetCurrentFrame(), profile_thread_id_);
    }
    SCOPE_EXIT({ if (guest_pc_cpu_.Enabled()) guest_pc_cpu_.EndRun(); });

    // Run until the timer slice is exhausted, flushing ticks to the timer per block so the
    // downcount stays live. This mirrors Dynarmic (which flushes incrementally via its
    // AddTicks callback and stops on the live GetTicksRemaining) rather than DynCom's batched
    // "capture the budget once, flush at the end" model. The live downcount lets events that
    // are force-scheduled mid-slice (ForceExceptionCheck) stop execution promptly instead of
    // being overshot, keeping multi-threaded scheduling aligned with Dynarmic.
    while (!halted_ && timer->GetDowncount() > 0) {
        u32 pc = state_.PC();
        if (guest_pc_cpu_.Active()) guest_pc_cpu_.PrepareBlock(pc, profile_thread_id_);

        // PC in unmapped memory: consume the slice so other threads still run
        if (!state_.page_pointers[pc >> 12]) {
            static u32 unmapped_pc_count = 0;
            if (unmapped_pc_count++ < 5) {
                LOG_ERROR(Core_ARM11, "PC {:08X} in unmapped memory (thumb={}, lr={:08X})", pc,
                          state_.thumb_mode, state_.regs[14]);
            }
            timer->AddTicks(static_cast<u64>(std::max<s64>(timer->GetDowncount(), 0)));
            break;
        }

        bool cache_hit;
        BasicBlock* block = cache_.LookupOrAllocate(pc, cache_hit);
        const bool allocation_miss = !cache_hit;
        if (cache_hit && block->IsThumb() != state_.thumb_mode) {
            if (cache_.ProfilingEnabled()) ++cache_mode_redecodes_;
            cache_hit = false; // Cached block is for wrong mode, re-decode
        }
        if (!cache_hit) {
            if (decode_cost_.Enabled()) {
                // Sample only allocation misses; mode-only redecodes retain a
                // matching cache tag and are counted separately without clocks.
                const auto sample = decode_cost_.BeforeDecode(allocation_miss);
                DecodeBlockInto(block, pc);
                decode_cost_.AfterDecode(sample);
            } else {
                DecodeBlockInto(block, pc);
            }
        }

        u64 executed = ExecuteBlock(block);

        if (executed == 0 && !halted_ && state_.PC() == pc &&
            block->instructions[0].opcode == Opcode::Invalid) {
            // The block truly failed to execute: it decoded to an invalid
            // instruction, which exits without adding ticks.
            // Every other executed==0 return is the SVC handlers' deliberate
            // tick flush - and an SVC may context-switch to a thread whose PC
            // equals this block's start (threads sharing an IPC wrapper), so
            // the PC check alone is not sufficient: without the opcode guard
            // the recovery below would skip the new thread's first instruction.
            LogUnhandledInstruction(pc);
            // Advance PC past the invalid instruction to avoid infinite loop
            state_.SetPC(state_.PC() + block->instructions[0].inst_size);
            executed = 1;
        }

        // Flush this block's ticks to the timer immediately (Dynarmic-style incremental
        // accounting), keeping the downcount live for the loop condition above. SVC blocks
        // flush inside their handler and return executed==0, so they are not double-counted.
        if (executed > 0) {
            if (Common::FrameTiming::counters.enabled) {
                auto& counters = Common::FrameTiming::counters;
                counters.guest_ticks += executed;
                counters.guest_core_ticks[GetID()] += executed;
                counters.last_guest_pc[GetID()] = state_.PC();
            }
            timer->AddTicks(executed);
        }
    }
}

void ARM_FastInterp::Step() {
    u32 pc = state_.PC();

    bool was_hit;
    BasicBlock* block = cache_.LookupOrAllocate(pc, was_hit);
    block->inst_count = 0;
    block->SetThumb(state_.thumb_mode);
    DecodedInst& decoded = block->instructions[0];
    std::memset(&decoded, 0, sizeof(decoded));
    u32 raw;
    u32 size = state_.thumb_mode ? 2 : 4;
    if (state_.thumb_mode) {
        const u16 first = ReadMemory16(pc);
        if ((first & 0xF800) >= 0xE800) {
            raw = (static_cast<u32>(first) << 16) | ReadMemory16(pc + 2);
            size = 4;
            DecodeThumb32(raw, pc, decoded);
        } else {
            raw = first;
            DecodeThumb16(first, pc, decoded);
        }
    } else {
        raw = ReadMemory32(pc);
        DecodeARM(raw, pc, decoded);
    }
    u16 slot = 0;
    const u32 pipeline_pc = pc + (state_.thumb_mode ? 4 : 8);
    if (ReadsPcAsSource(decoded)) {
        FoldPcSources(decoded, pipeline_pc);
        if (ReadsPcAsSource(decoded)) {
            block->instructions[1] = decoded;
            std::memset(&decoded, 0, sizeof(decoded));
            decoded.opcode = Opcode::PcSetup;
            decoded.cond = 0xE;
            decoded.imm32 = pipeline_pc;
            slot = 1;
        }
    }
    block->instructions[slot].ticks = static_cast<u8>(Core::TicksForInstruction(state_.thumb_mode, raw));
    DecodedInst& term = block->instructions[slot + 1];
    std::memset(&term, 0, sizeof(term));
    term.opcode = Opcode::EndBlock;
    term.cond = 0xE;
    block->inst_count = slot + 1;
    block->end_pc = pc + size;
    for (u16 i = 0; i <= block->inst_count; ++i) PrepareDispatch(block->instructions[i]);
    stepping_ = true;

    u64 executed = ExecuteBlock(block);
    timer->AddTicks(executed > 0 ? executed : 1);
    stepping_ = false;
    // A single-step block must not replace a full block on the next Run().
    cache_.InvalidateRange(pc, size);
}

// ============================================================================
// Computed-Goto Dispatch Loop
// ============================================================================
// Direct threading via GCC/Clang computed goto: each indirect jump gets its
// own branch predictor entry, unlike a switch's single dispatch point.
//
// Everything below (macros, lambdas, and the #included handlers) must live in
// this one function: computed-goto labels have function scope. Its codegen is
// fragile - an out-of-line call in a hot handler forces a frame that costs
// more than the call itself (measured more than once) - so keep slow paths in
// noinline helpers and benchmark any structural change here.

u64 ARM_FastInterp::ExecuteBlock(BasicBlock* block) {
// Must match Opcode order (the static_assert below checks the count)
#define HANDLER(name) &&handle_##name
    static const void* dispatch_table[] = {
        HANDLER(Invalid),
        HANDLER(Nop),
        HANDLER(EndBlock),

        // Data Processing - ALU (no flags)
        HANDLER(And),
        HANDLER(Eor),
        HANDLER(Sub),
        HANDLER(Rsb),
        HANDLER(Add),
        HANDLER(Adc),
        HANDLER(Sbc),
        HANDLER(Rsc),
        HANDLER(Tst),
        HANDLER(Teq),
        HANDLER(Cmp),
        HANDLER(Cmn),
        HANDLER(Orr),
        HANDLER(Mov),
        HANDLER(Bic),
        HANDLER(Mvn),

        // Data Processing - ALU (with flags)
        HANDLER(AndS),
        HANDLER(EorS),
        HANDLER(SubS),
        HANDLER(RsbS),
        HANDLER(AddS),
        HANDLER(AdcS),
        HANDLER(SbcS),
        HANDLER(RscS),
        HANDLER(OrrS),
        HANDLER(MovS),
        HANDLER(BicS),
        HANDLER(MvnS),

        // Multiply
        HANDLER(Mul),
        HANDLER(MulS),
        HANDLER(Mla),
        HANDLER(MlaS),
        HANDLER(Umull),
        HANDLER(UmullS),
        HANDLER(Umlal),
        HANDLER(UmlalS),
        HANDLER(Smull),
        HANDLER(SmullS),
        HANDLER(Smlal),
        HANDLER(SmlalS),

        // Load/Store
        HANDLER(Ldr),
        HANDLER(LdrB),
        HANDLER(LdrH),
        HANDLER(LdrSB),
        HANDLER(LdrSH),
        HANDLER(Str),
        HANDLER(StrB),
        HANDLER(StrH),

        // Load/Store Multiple
        HANDLER(Ldm),
        HANDLER(Stm),

        // Branch
        HANDLER(B),
        HANDLER(Bl),
        HANDLER(Bx),
        HANDLER(Blx),

        // Miscellaneous
        HANDLER(Clz),
        HANDLER(Swi),
        HANDLER(Mrs),
        HANDLER(Msr),

        // Extension
        HANDLER(Sxtb),
        HANDLER(Sxth),
        HANDLER(Uxtb),
        HANDLER(Uxth),

        // Byte reversal
        HANDLER(Rev),
        HANDLER(Rev16),
        HANDLER(Revsh),

        // Saturating arithmetic
        HANDLER(Qadd),
        HANDLER(Qsub),
        HANDLER(Qdadd),
        HANDLER(Qdsub),
        HANDLER(Ssat),
        HANDLER(Usat),

        // Parallel add/sub
        HANDLER(Sadd16),
        HANDLER(Ssub16),
        HANDLER(Sadd8),
        HANDLER(Ssub8),
        HANDLER(Uadd16),
        HANDLER(Usub16),
        HANDLER(Uadd8),
        HANDLER(Usub8),

        // Unsigned saturating parallel
        HANDLER(Uqadd16),
        HANDLER(Uqsub16),
        HANDLER(Uqadd8),
        HANDLER(Uqsub8),

        // Signed saturating parallel
        HANDLER(Qadd16),
        HANDLER(Qsub16),
        HANDLER(Qadd8),
        HANDLER(Qsub8),

        // Signed halving parallel
        HANDLER(Shadd16),
        HANDLER(Shsub16),
        HANDLER(Shadd8),
        HANDLER(Shsub8),

        // Unsigned halving parallel
        HANDLER(Uhadd16),
        HANDLER(Uhsub16),
        HANDLER(Uhadd8),
        HANDLER(Uhsub8),

        // Extend with Add
        HANDLER(Sxtab),
        HANDLER(Sxtab16),
        HANDLER(Sxtah),
        HANDLER(Uxtab),
        HANDLER(Uxtab16),
        HANDLER(Uxtah),

        // Bit field
        HANDLER(Bfc),
        HANDLER(Bfi),
        HANDLER(Sbfx),
        HANDLER(Ubfx),

        // Bit reversal
        HANDLER(Rbit),

        // Move top halfword
        HANDLER(Movt),

        // SIMD/Packed
        HANDLER(Pkhbt),
        HANDLER(Pkhtb),
        HANDLER(Sel),
        HANDLER(Usad8),
        HANDLER(Usada8),

        // VFP Register Transfer
        HANDLER(VmovArmToS),
        HANDLER(VmovSToArm),
        HANDLER(VmovArmToD),
        HANDLER(VmovDToArm),
        HANDLER(Vmrs),
        HANDLER(Vmsr),

        // VFP Data Processing (single precision)
        HANDLER(VaddS),
        HANDLER(VsubS),
        HANDLER(VmulS),
        HANDLER(VdivS),
        HANDLER(VmlaS),
        HANDLER(VmlsS),
        HANDLER(VnmlaS),
        HANDLER(VnmlsS),
        HANDLER(VnmulS),
        HANDLER(VabsS),
        HANDLER(VnegS),
        HANDLER(VsqrtS),
        HANDLER(VmovS),
        HANDLER(VmovImmS),

        // VFP Data Processing (double precision)
        HANDLER(VaddD),
        HANDLER(VsubD),
        HANDLER(VmulD),
        HANDLER(VdivD),
        HANDLER(VmlaD),
        HANDLER(VmlsD),
        HANDLER(VnmlaD),
        HANDLER(VnmlsD),
        HANDLER(VnmulD),
        HANDLER(VabsD),
        HANDLER(VnegD),
        HANDLER(VsqrtD),
        HANDLER(VmovD),
        HANDLER(VmovImmD),

        // VFP Comparison
        HANDLER(VcmpS),
        HANDLER(VcmpD),
        HANDLER(VcmpzS),
        HANDLER(VcmpzD),

        // VFP Conversion
        HANDLER(VcvtSToInt),
        HANDLER(VcvtIntToS),
        HANDLER(VcvtDToS),
        HANDLER(VcvtSToD),
        HANDLER(VcvtDToInt),
        HANDLER(VcvtIntToD),

        // VFP Load/Store
        HANDLER(VldrS),
        HANDLER(VstrS),
        HANDLER(VldrD),
        HANDLER(VstrD),

        // VFP Load/Store Multiple
        HANDLER(VldmS),
        HANDLER(VstmS),
        HANDLER(VldmD),
        HANDLER(VstmD),

        // Native Thumb16 handlers
        HANDLER(ThumbLslImm),
        HANDLER(ThumbLsrImm),
        HANDLER(ThumbAsrImm),
        HANDLER(ThumbAddReg3),
        HANDLER(ThumbSubReg3),
        HANDLER(ThumbAddImm3),
        HANDLER(ThumbSubImm3),
        HANDLER(ThumbMovImm8),
        HANDLER(ThumbCmpImm8),
        HANDLER(ThumbAddImm8),
        HANDLER(ThumbSubImm8),
        HANDLER(ThumbAnd),
        HANDLER(ThumbEor),
        HANDLER(ThumbLslReg),
        HANDLER(ThumbLsrReg),
        HANDLER(ThumbAsrReg),
        HANDLER(ThumbAdc),
        HANDLER(ThumbSbc),
        HANDLER(ThumbRor),
        HANDLER(ThumbTst),
        HANDLER(ThumbNeg),
        HANDLER(ThumbCmpReg),
        HANDLER(ThumbCmn),
        HANDLER(ThumbOrr),
        HANDLER(ThumbMul),
        HANDLER(ThumbBic),
        HANDLER(ThumbMvn),
        HANDLER(ThumbAddHi),
        HANDLER(ThumbCmpHi),
        HANDLER(ThumbMovHi),
        HANDLER(ThumbBx),
        HANDLER(ThumbBlxReg),
        HANDLER(ThumbLdrReg),
        HANDLER(ThumbStrReg),
        HANDLER(ThumbLdrImm5),
        HANDLER(ThumbStrImm5),
        HANDLER(ThumbLdrbReg),
        HANDLER(ThumbStrbReg),
        HANDLER(ThumbLdrbImm5),
        HANDLER(ThumbStrbImm5),
        HANDLER(ThumbLdrhReg),
        HANDLER(ThumbStrhReg),
        HANDLER(ThumbLdrhImm5),
        HANDLER(ThumbStrhImm5),
        HANDLER(ThumbLdrsb),
        HANDLER(ThumbLdrsh),
        HANDLER(ThumbLdrSp),
        HANDLER(ThumbStrSp),
        HANDLER(ThumbLdrPc),
        HANDLER(ThumbAddPcImm),
        HANDLER(ThumbAddSpImm),
        HANDLER(ThumbAddSpImm7),
        HANDLER(ThumbSubSpImm7),
        HANDLER(ThumbPush),
        HANDLER(ThumbPop),
        HANDLER(ThumbLdmia),
        HANDLER(ThumbStmia),
        HANDLER(ThumbBCond),
        HANDLER(ThumbB),
        HANDLER(ThumbSwi),
        HANDLER(ThumbSxth),
        HANDLER(ThumbSxtb),
        HANDLER(ThumbUxth),
        HANDLER(ThumbUxtb),
        HANDLER(ThumbRev),
        HANDLER(ThumbRev16),
        HANDLER(ThumbRevsh),

        // Thumb32 handlers
        HANDLER(ThumbBl),
        HANDLER(ThumbBlxImm),

        // Fused instruction handlers
        HANDLER(FusedSubsBcc),
        HANDLER(FusedSubsRegBcc),
        HANDLER(FusedCmpBcc),
        HANDLER(FusedCmpRegBcc),
        HANDLER(FusedTstBcc),
        HANDLER(FusedTstRegBcc),
        HANDLER(FusedTeqBcc),
        HANDLER(FusedTeqRegBcc),
        HANDLER(ThumbFusedSubsBcc),
        HANDLER(ThumbFusedCmpBcc),
        HANDLER(ThumbFusedCmpRegBcc),
        HANDLER(ThumbFusedTstBcc),
        HANDLER(FusedAddCmpBcc),
        // Unrolled loop handlers (hot block optimization)
        HANDLER(ThumbUnrolledSubsLoop),
        HANDLER(ArmUnrolledSubsLoop),
        HANDLER(ThumbUnrolledCmpLoop),
        // Coprocessor (CP15)
        HANDLER(Mrc),
        HANDLER(Mcr),
        // Exclusive load/store
        HANDLER(Ldrex),
        HANDLER(Strex),
        HANDLER(Ldrexb),
        HANDLER(Strexb),
        HANDLER(Ldrexh),
        HANDLER(Strexh),
        HANDLER(Ldrexd),
        HANDLER(Strexd),
        HANDLER(Clrex),
        // Doubleword load/store
        HANDLER(Ldrd),
        HANDLER(Strd),
        // Halfword multiplies
        HANDLER(Smulxy),
        HANDLER(Smlaxy),
        HANDLER(Smulwy),
        HANDLER(Smlawy),
        HANDLER(Smlalxy),
        // Decode-time specialized opcodes
        HANDLER(LdrImm),
        HANDLER(StrImm),
        HANDLER(LdrBImm),
        HANDLER(StrBImm),
        HANDLER(LdrHImm),
        HANDLER(StrHImm),
        HANDLER(BlxImm),
        // PC-source elimination
        HANDLER(LdrLit),
        HANDLER(PcSetup),
        HANDLER(Bkpt),
        HANDLER(Vmul4S),
        HANDLER(Vmul4Gather),
        HANDLER(Vmla4S),
        HANDLER(Vmla4Gather),
        HANDLER(AddCmpImmBcc),
        HANDLER(VcmpVmrsS),
        HANDLER(AddImm),
        HANDLER(AddLslImm),
        HANDLER(MovImm),
        HANDLER(MovLslImm),
        HANDLER(AddPairImmImm),
        HANDLER(AddPairImmLsl),
        HANDLER(AddPairLslImm),
        HANDLER(AddPairLslLsl),
        HANDLER(MovPairImmImm),
        HANDLER(MovPairImmLsl),
        HANDLER(MovPairLslImm),
        HANDLER(MovPairLslLsl),


        // AL entries share each checked handler body after its predicate gate.
        &&handle_Invalid,
        &&handle_Nop,
        &&handle_EndBlock,
        &&handle_And_al,
        &&handle_Eor_al,
        &&handle_Sub_al,
        &&handle_Rsb_al,
        &&handle_Add_al,
        &&handle_Adc_al,
        &&handle_Sbc_al,
        &&handle_Rsc_al,
        &&handle_Tst_al,
        &&handle_Teq_al,
        &&handle_Cmp_al,
        &&handle_Cmn_al,
        &&handle_Orr_al,
        &&handle_Mov_al,
        &&handle_Bic_al,
        &&handle_Mvn_al,
        &&handle_AndS_al,
        &&handle_EorS_al,
        &&handle_SubS_al,
        &&handle_RsbS_al,
        &&handle_AddS_al,
        &&handle_AdcS_al,
        &&handle_SbcS_al,
        &&handle_RscS_al,
        &&handle_OrrS_al,
        &&handle_MovS_al,
        &&handle_BicS_al,
        &&handle_MvnS_al,
        &&handle_Mul_al,
        &&handle_MulS_al,
        &&handle_Mla_al,
        &&handle_MlaS_al,
        &&handle_Umull_al,
        &&handle_UmullS_al,
        &&handle_Umlal_al,
        &&handle_UmlalS_al,
        &&handle_Smull_al,
        &&handle_SmullS_al,
        &&handle_Smlal_al,
        &&handle_SmlalS_al,
        &&handle_Ldr_al,
        &&handle_LdrB_al,
        &&handle_LdrH_al,
        &&handle_LdrSB_al,
        &&handle_LdrSH_al,
        &&handle_Str_al,
        &&handle_StrB_al,
        &&handle_StrH_al,
        &&handle_Ldm_al,
        &&handle_Stm_al,
        &&handle_B_al,
        &&handle_Bl_al,
        &&handle_Bx_al,
        &&handle_Blx_al,
        &&handle_Clz_al,
        &&handle_Swi_al,
        &&handle_Mrs_al,
        &&handle_Msr_al,
        &&handle_Sxtb_al,
        &&handle_Sxth_al,
        &&handle_Uxtb_al,
        &&handle_Uxth_al,
        &&handle_Rev_al,
        &&handle_Rev16_al,
        &&handle_Revsh_al,
        &&handle_Qadd_al,
        &&handle_Qsub_al,
        &&handle_Qdadd_al,
        &&handle_Qdsub_al,
        &&handle_Ssat_al,
        &&handle_Usat_al,
        &&handle_Sadd16_al,
        &&handle_Ssub16_al,
        &&handle_Sadd8_al,
        &&handle_Ssub8_al,
        &&handle_Uadd16_al,
        &&handle_Usub16_al,
        &&handle_Uadd8_al,
        &&handle_Usub8_al,
        &&handle_Uqadd16_al,
        &&handle_Uqsub16_al,
        &&handle_Uqadd8_al,
        &&handle_Uqsub8_al,
        &&handle_Qadd16_al,
        &&handle_Qsub16_al,
        &&handle_Qadd8_al,
        &&handle_Qsub8_al,
        &&handle_Shadd16_al,
        &&handle_Shsub16_al,
        &&handle_Shadd8_al,
        &&handle_Shsub8_al,
        &&handle_Uhadd16_al,
        &&handle_Uhsub16_al,
        &&handle_Uhadd8_al,
        &&handle_Uhsub8_al,
        &&handle_Sxtab_al,
        &&handle_Sxtab16_al,
        &&handle_Sxtah_al,
        &&handle_Uxtab_al,
        &&handle_Uxtab16_al,
        &&handle_Uxtah_al,
        &&handle_Bfc_al,
        &&handle_Bfi_al,
        &&handle_Sbfx_al,
        &&handle_Ubfx_al,
        &&handle_Rbit_al,
        &&handle_Movt_al,
        &&handle_Pkhbt_al,
        &&handle_Pkhtb_al,
        &&handle_Sel_al,
        &&handle_Usad8_al,
        &&handle_Usada8_al,
        &&handle_VmovArmToS_al,
        &&handle_VmovSToArm_al,
        &&handle_VmovArmToD_al,
        &&handle_VmovDToArm_al,
        &&handle_Vmrs_al,
        &&handle_Vmsr_al,
        &&handle_VaddS_al,
        &&handle_VsubS_al,
        &&handle_VmulS_al,
        &&handle_VdivS_al,
        &&handle_VmlaS_al,
        &&handle_VmlsS_al,
        &&handle_VnmlaS_al,
        &&handle_VnmlsS_al,
        &&handle_VnmulS_al,
        &&handle_VabsS_al,
        &&handle_VnegS_al,
        &&handle_VsqrtS_al,
        &&handle_VmovS_al,
        &&handle_VmovImmS_al,
        &&handle_VaddD_al,
        &&handle_VsubD_al,
        &&handle_VmulD_al,
        &&handle_VdivD_al,
        &&handle_VmlaD_al,
        &&handle_VmlsD_al,
        &&handle_VnmlaD_al,
        &&handle_VnmlsD_al,
        &&handle_VnmulD_al,
        &&handle_VabsD_al,
        &&handle_VnegD_al,
        &&handle_VsqrtD_al,
        &&handle_VmovD_al,
        &&handle_VmovImmD_al,
        &&handle_VcmpS_al,
        &&handle_VcmpD_al,
        &&handle_VcmpzS_al,
        &&handle_VcmpzD_al,
        &&handle_VcvtSToInt_al,
        &&handle_VcvtIntToS_al,
        &&handle_VcvtDToS_al,
        &&handle_VcvtSToD_al,
        &&handle_VcvtDToInt_al,
        &&handle_VcvtIntToD_al,
        &&handle_VldrS_al,
        &&handle_VstrS_al,
        &&handle_VldrD_al,
        &&handle_VstrD_al,
        &&handle_VldmS_al,
        &&handle_VstmS_al,
        &&handle_VldmD_al,
        &&handle_VstmD_al,
        &&handle_ThumbLslImm,
        &&handle_ThumbLsrImm,
        &&handle_ThumbAsrImm,
        &&handle_ThumbAddReg3,
        &&handle_ThumbSubReg3,
        &&handle_ThumbAddImm3,
        &&handle_ThumbSubImm3,
        &&handle_ThumbMovImm8,
        &&handle_ThumbCmpImm8,
        &&handle_ThumbAddImm8,
        &&handle_ThumbSubImm8,
        &&handle_ThumbAnd,
        &&handle_ThumbEor,
        &&handle_ThumbLslReg,
        &&handle_ThumbLsrReg,
        &&handle_ThumbAsrReg,
        &&handle_ThumbAdc,
        &&handle_ThumbSbc,
        &&handle_ThumbRor,
        &&handle_ThumbTst,
        &&handle_ThumbNeg,
        &&handle_ThumbCmpReg,
        &&handle_ThumbCmn,
        &&handle_ThumbOrr,
        &&handle_ThumbMul,
        &&handle_ThumbBic,
        &&handle_ThumbMvn,
        &&handle_ThumbAddHi,
        &&handle_ThumbCmpHi,
        &&handle_ThumbMovHi,
        &&handle_ThumbBx,
        &&handle_ThumbBlxReg,
        &&handle_ThumbLdrReg,
        &&handle_ThumbStrReg,
        &&handle_ThumbLdrImm5,
        &&handle_ThumbStrImm5,
        &&handle_ThumbLdrbReg,
        &&handle_ThumbStrbReg,
        &&handle_ThumbLdrbImm5,
        &&handle_ThumbStrbImm5,
        &&handle_ThumbLdrhReg,
        &&handle_ThumbStrhReg,
        &&handle_ThumbLdrhImm5,
        &&handle_ThumbStrhImm5,
        &&handle_ThumbLdrsb,
        &&handle_ThumbLdrsh,
        &&handle_ThumbLdrSp,
        &&handle_ThumbStrSp,
        &&handle_ThumbLdrPc,
        &&handle_ThumbAddPcImm,
        &&handle_ThumbAddSpImm,
        &&handle_ThumbAddSpImm7,
        &&handle_ThumbSubSpImm7,
        &&handle_ThumbPush,
        &&handle_ThumbPop,
        &&handle_ThumbLdmia,
        &&handle_ThumbStmia,
        &&handle_ThumbBCond,
        &&handle_ThumbB,
        &&handle_ThumbSwi,
        &&handle_ThumbSxth,
        &&handle_ThumbSxtb,
        &&handle_ThumbUxth,
        &&handle_ThumbUxtb,
        &&handle_ThumbRev,
        &&handle_ThumbRev16,
        &&handle_ThumbRevsh,
        &&handle_ThumbBl,
        &&handle_ThumbBlxImm,
        &&handle_FusedSubsBcc,
        &&handle_FusedSubsRegBcc,
        &&handle_FusedCmpBcc,
        &&handle_FusedCmpRegBcc,
        &&handle_FusedTstBcc,
        &&handle_FusedTstRegBcc,
        &&handle_FusedTeqBcc,
        &&handle_FusedTeqRegBcc,
        &&handle_ThumbFusedSubsBcc,
        &&handle_ThumbFusedCmpBcc,
        &&handle_ThumbFusedCmpRegBcc,
        &&handle_ThumbFusedTstBcc,
        &&handle_FusedAddCmpBcc,
        &&handle_ThumbUnrolledSubsLoop,
        &&handle_ArmUnrolledSubsLoop,
        &&handle_ThumbUnrolledCmpLoop,
        &&handle_Mrc_al,
        &&handle_Mcr_al,
        &&handle_Ldrex,
        &&handle_Strex,
        &&handle_Ldrexb,
        &&handle_Strexb,
        &&handle_Ldrexh,
        &&handle_Strexh,
        &&handle_Ldrexd,
        &&handle_Strexd,
        &&handle_Clrex,
        &&handle_Ldrd_al,
        &&handle_Strd_al,
        &&handle_Smulxy_al,
        &&handle_Smlaxy_al,
        &&handle_Smulwy_al,
        &&handle_Smlawy_al,
        &&handle_Smlalxy_al,
        &&handle_LdrImm_al,
        &&handle_StrImm_al,
        &&handle_LdrBImm_al,
        &&handle_StrBImm_al,
        &&handle_LdrHImm_al,
        &&handle_StrHImm_al,
        &&handle_BlxImm_al,
        &&handle_LdrLit_al,
        &&handle_PcSetup,
        &&handle_Bkpt_al,
        &&handle_Vmul4S,
        &&handle_Vmul4Gather,
        &&handle_Vmla4S,
        &&handle_Vmla4Gather,
        &&handle_AddCmpImmBcc,
        &&handle_VcmpVmrsS,
        &&handle_AddImm_al,
        &&handle_AddLslImm_al,
        &&handle_MovImm_al,
        &&handle_MovLslImm_al,
        &&handle_AddPairImmImm,
        &&handle_AddPairImmLsl,
        &&handle_AddPairLslImm,
        &&handle_AddPairLslLsl,
        &&handle_MovPairImmImm,
        &&handle_MovPairImmLsl,
        &&handle_MovPairLslImm,
        &&handle_MovPairLslLsl,
    };
#undef HANDLER

    static_assert(sizeof(dispatch_table) / sizeof(dispatch_table[0]) ==
                      2 * static_cast<size_t>(Opcode::OpcodeCount),
                  "Dispatch table size mismatch");

    const DecodedInst* inst = block->instructions.data();
    const DecodedInst* end = inst + block->inst_count;
    u64 executed = 0;

    // Block chaining: limit chains to ensure timers/interrupts are checked
    constexpr u32 MAX_CHAINS = 16;
    u32 chain_count = 0;

    // PC lives in this local (register-allocated); state_.regs[15] is not kept
    // current inside a block. That's safe because decode guarantees no handler
    // reads regs[15] as a source operand (such reads are folded to constants at
    // decode time, or preceded by a PcSetup that writes regs[15] first).
    // local_pc syncs back to regs[15] at chain/done (SYNC_PC) and before SVC.
    u32 local_pc = state_.regs[15];

    // A hot singleton represents repeated executions of its original block.
    // Its batch must obey the same budget boundary as cold fused dispatches.
    auto UnrollLimit = [this, &executed](u8 ticks) -> int {
        if (!chain_budget_) return 16;
        DEBUG_ASSERT(ticks != 0);
        const s64 budget = timer->GetDowncount();
        const u64 remaining = budget > 0 && static_cast<u64>(budget) > executed
            ? static_cast<u64>(budget) - executed : 0;
        return remaining ? static_cast<int>(std::min<u64>(16, (remaining - 1) / ticks + 1)) : 1;
    };

#if defined(__aarch64__) && defined(CARA_FASTINTERP_PIN_PAGE_TABLE) && CARA_FASTINTERP_PIN_PAGE_TABLE
// Private compiler experiment: keep the page-table base in a callee-saved
// register across dispatch. No guards, memory ordering or slow paths change.
#define KEEP_PAGE_TABLE_REGISTER() asm volatile("" : : "r"(local_page_ptrs))
#else
#define KEEP_PAGE_TABLE_REGISTER() ((void)0)
#endif

// Macros for PC access through local variable (synced at chain/done)
#define GET_PC() local_pc
#define SET_PC(val) (local_pc = (val))
#define SYNC_PC() (state_.regs[15] = local_pc)

// No bounds check: every block ends with an EndBlock terminator.
#define DISPATCH()                                                                                 \
    do {                                                                                           \
        KEEP_PAGE_TABLE_REGISTER();                                                               \
        ++inst;                                                                                    \
        goto* dispatch_table[inst->dispatch_index];                                                 \
    } while (0)

#define CHECK_COND_GATE()                                                                          \
    do {                                                                                           \
        if (inst->cond != 0xE && !CheckCondition(inst->cond)) {                                    \
            executed += inst->ticks;                                                               \
            SET_PC(GET_PC() + inst->inst_size);                                                    \
            DISPATCH();                                                                            \
        }                                                                                          \
    } while (0)

// AL labels must precede all handler-local initialization and side effects.
// Helpers that initialize locals before their gate use CHECK_COND_GATE instead.
#define CHECK_COND(name) CHECK_COND_GATE(); handle_##name##_al:

#define ADVANCE_AND_DISPATCH()                                                                     \
    do {                                                                                           \
        executed += inst->ticks;                                                                   \
        SET_PC(GET_PC() + inst->inst_size);                                                        \
        DISPATCH();                                                                                \
    } while (0)

    // Helper to compute shifted operand 2 (with carry computation for flag-setting ops)
    auto GetOp2 = [this, &inst](bool& carry) -> u32 {
        if (inst->dp.rm == 0xFF) {
            // Immediate: flags bit 0 = carry-out is imm bit 31,
            // bit 1 = complemented (ORN stores the inverted immediate)
            if (inst->dp.flags & 1) {
                carry = ((inst->dp.imm >> 31) ^ (inst->dp.flags >> 1)) & 1;
            }
            return inst->dp.imm;
        }
        u32 rm_val = state_.regs[inst->dp.rm];
        if (inst->dp.rs != 0xFF) {
            // Register-specified shift amount
            u8 shift_amt = state_.regs[inst->dp.rs] & 0xFF;
            return ShiftReg(rm_val, inst->dp.shift_type, shift_amt, carry);
        } else {
            // Immediate shift amount
            return ShiftImm(rm_val, inst->dp.shift_type, inst->dp.shift_imm, carry);
        }
    };

    // Fast helper for non-flag-setting operations (skips carry computation)
    auto GetOp2Fast = [this, &inst]() -> u32 {
        if (inst->dp.rm == 0xFF) {
            return inst->dp.imm;
        }
        u32 rm_val = state_.regs[inst->dp.rm];
        // Fast path: no shift (LSL #0)
        if (inst->dp.rs == 0xFF && inst->dp.shift_imm == 0 &&
            inst->dp.shift_type == ShiftType::LSL) {
            return rm_val;
        }
        if (inst->dp.rs != 0xFF) {
            u8 shift_amt = state_.regs[inst->dp.rs] & 0xFF;
            if (shift_amt == 0)
                return rm_val;
            return ShiftNoCarry(rm_val, inst->dp.shift_type, shift_amt);
        } else {
            return ShiftNoCarry(rm_val, inst->dp.shift_type, inst->dp.shift_imm);
        }
    };

    // ========================================================================
    // Memory Access Macros
    // ========================================================================
    // Macros rather than lambdas: capturing 'this' costs a closure in tight
    // loops like LDM/STM.

#if defined(__aarch64__) && defined(CARA_FASTINTERP_PIN_PAGE_TABLE) && CARA_FASTINTERP_PIN_PAGE_TABLE
    register u8** local_page_ptrs asm("x28") = state_.page_pointers;
#else
    u8** const local_page_ptrs = state_.page_pointers;
#endif

// Fast-path read macros with direct page table access
#define MEM_READ8(addr, result)                                                                    \
    do {                                                                                           \
        const u32 _addr = (addr);                                                                  \
        const u8* _page = local_page_ptrs[_addr >> FASTINTERP_PAGE_BITS];                          \
        if (__builtin_expect(_page != nullptr, 1)) {                                               \
            (result) = _page[_addr & FASTINTERP_PAGE_MASK];                                        \
        } else {                                                                                   \
            (result) = memory_.Read8(_addr);                                                       \
        }                                                                                          \
    } while (0)

#define MEM_READ16(addr, result)                                                                   \
    do {                                                                                           \
        const u32 _addr = (addr);                                                                  \
        const u8* _page = local_page_ptrs[_addr >> FASTINTERP_PAGE_BITS];                          \
        if (__builtin_expect(_page != nullptr && (_addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 1, 1)) {                                               \
            std::memcpy(&(result), &_page[_addr & FASTINTERP_PAGE_MASK], 2);                       \
        } else {                                                                                   \
            (result) = ReadCoherent<u16>(memory_, _addr);                                                      \
        }                                                                                          \
    } while (0)

#define MEM_READ32(addr, result)                                                                   \
    do {                                                                                           \
        const u32 _addr = (addr);                                                                  \
        const u8* _page = local_page_ptrs[_addr >> FASTINTERP_PAGE_BITS];                          \
        if (__builtin_expect(_page != nullptr && (_addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 3, 1)) {                                               \
            std::memcpy(&(result), &_page[_addr & FASTINTERP_PAGE_MASK], 4);                       \
        } else {                                                                                   \
            (result) = ReadCoherent<u32>(memory_, _addr);                                                      \
        }                                                                                          \
    } while (0)

// Fast-path write macros with direct page table access
#define MEM_WRITE8(addr, value)                                                                    \
    do {                                                                                           \
        const u32 _addr = (addr);                                                                  \
        const u8 _val = (value);                                                                   \
        u8* _page = local_page_ptrs[_addr >> FASTINTERP_PAGE_BITS];                                \
        if (__builtin_expect(_page != nullptr, 1)) {                                               \
            _page[_addr & FASTINTERP_PAGE_MASK] = _val;                                            \
        } else {                                                                                   \
            memory_.Write8(_addr, _val);                                                           \
        }                                                                                          \
    } while (0)

#define MEM_WRITE16(addr, value)                                                                   \
    do {                                                                                           \
        const u32 _addr = (addr);                                                                  \
        const u16 _val = (value);                                                                  \
        u8* _page = local_page_ptrs[_addr >> FASTINTERP_PAGE_BITS];                                \
        if (__builtin_expect(_page != nullptr && (_addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 1, 1)) {                                               \
            std::memcpy(&_page[_addr & FASTINTERP_PAGE_MASK], &_val, 2);                           \
        } else {                                                                                   \
            WriteCoherent<u16>(memory_, _addr, _val);                                                          \
        }                                                                                          \
    } while (0)

#define MEM_WRITE32(addr, value)                                                                   \
    do {                                                                                           \
        const u32 _addr = (addr);                                                                  \
        const u32 _val = (value);                                                                  \
        u8* _page = local_page_ptrs[_addr >> FASTINTERP_PAGE_BITS];                                \
        if (__builtin_expect(_page != nullptr && (_addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 3, 1)) {                                               \
            std::memcpy(&_page[_addr & FASTINTERP_PAGE_MASK], &_val, 4);                           \
        } else {                                                                                   \
            WriteCoherent<u32>(memory_, _addr, _val);                                                          \
        }                                                                                          \
    } while (0)

    // Expression-form counterparts of the statement-form MEM_* macros, used
    // inline by the load/store, VFP, and Thumb handlers.
    auto MemRead8 = [this, local_page_ptrs](u32 addr) -> u8 {
        const u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        if (__builtin_expect(page != nullptr, 1)) {
            return page[addr & FASTINTERP_PAGE_MASK];
        }
        return memory_.Read8(addr);
    };

    auto MemRead16 = [this, local_page_ptrs](u32 addr) -> u16 {
        const u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        if (__builtin_expect(page != nullptr && (addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 1, 1)) {
            u16 value;
            std::memcpy(&value, &page[addr & FASTINTERP_PAGE_MASK], 2);
            return value;
        }
        return ReadCoherent<u16>(memory_, addr);
    };

    auto MemRead32 = [this, local_page_ptrs](u32 addr) -> u32 {
        const u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        if (__builtin_expect(page != nullptr && (addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 3, 1)) {
            u32 value;
            std::memcpy(&value, &page[addr & FASTINTERP_PAGE_MASK], 4);
            return value;
        }
        return ReadCoherent<u32>(memory_, addr);
    };

    auto MemWrite8 = [this, local_page_ptrs](u32 addr, u8 value) {
        u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        if (__builtin_expect(page != nullptr, 1)) {
            page[addr & FASTINTERP_PAGE_MASK] = value;
            return;
        }
        memory_.Write8(addr, value);
    };

    auto MemWrite16 = [this, local_page_ptrs](u32 addr, u16 value) {
        u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        if (__builtin_expect(page != nullptr && (addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 1, 1)) {
            std::memcpy(&page[addr & FASTINTERP_PAGE_MASK], &value, 2);
            return;
        }
        WriteCoherent<u16>(memory_, addr, value);
    };

    auto MemWrite32 = [this, local_page_ptrs](u32 addr, u32 value) {
        u8* page = local_page_ptrs[addr >> FASTINTERP_PAGE_BITS];
        if (__builtin_expect(page != nullptr && (addr & FASTINTERP_PAGE_MASK) <= FASTINTERP_PAGE_MASK - 3, 1)) {
            std::memcpy(&page[addr & FASTINTERP_PAGE_MASK], &value, 4);
            return;
        }
        WriteCoherent<u32>(memory_, addr, value);
    };

    if (guest_pc_cpu_.ObservesBlocks()) guest_pc_cpu_.Block(block->start_pc, block->end_pc, profile_thread_id_);

    // Entry guard: poisoned blocks (inst_count = 0 from InvalidateRange/Clear)
    // must not execute their stale instructions.
    if (inst >= end)
        goto done;
    goto* dispatch_table[inst->dispatch_index];

    // ========================================================================
    // Instruction Handlers
    // ========================================================================
    // Each handlers/*.inc is #included here and becomes part of ExecuteBlock().
    // The handler environment: `inst`, `executed`, GET_PC()/SET_PC(), the MEM_*
    // macros and MemRead*/MemWrite* lambdas, GetOp2/GetOp2Fast, and the `chain`/
    // `done` exit labels. Every handler adds its ticks to `executed` and leaves
    // via a DISPATCH macro or `goto chain`/`goto done`; no handler may read
    // regs[15] as a source (see PC-source elimination, fastinterp_decode.cpp).

    // clang-format off: arm_alu.inc defines ALU_DISPATCH for arm_alu_s.inc,
    // so these are not sortable
#include "handlers/special.inc"
#include "handlers/arm_alu.inc"
#include "handlers/arm_alu_s.inc"
#include "handlers/arm_multiply.inc"
#include "handlers/arm_loadstore.inc"
#include "handlers/arm_branch.inc"
#include "handlers/arm_misc.inc"
#include "handlers/arm_vfp.inc"
#include "handlers/thumb16.inc"
#include "handlers/thumb32.inc"
#include "handlers/fused.inc"
    // clang-format on

chain:
    SYNC_PC();

    // Default-off accuracy experiment: a warm chain must stop at the same
    // decoded-block boundary as Run() after a cache miss. GetDowncount returns
    // remaining guest cycles (including configured clock scaling); negative
    // budgets must not become huge unsigned values. SVC already flushes and
    // exits via done. This check does not flush pending ticks per block, so
    // MMIO inside a block still sees the existing batched timer visibility.
    if (chain_budget_ &&
        (halted_ || executed >= static_cast<u64>(std::max<s64>(timer->GetDowncount(), 0)))) {
        goto done;
    }

    // Try to chain to the next block instead of returning to Run()
    if (!stepping_ && chain_count < MAX_CHAINS) {
        u32 next_pc = local_pc;

        // Self-loop fast path: branch back to the same block (tight loops)
        if (next_pc == block->start_pc) {
            chain_count++;
            block->exec_count++;
            inst = block->instructions.data();
            end = inst + block->inst_count;
            if (inst < end) {
                decode_cost_.ChainSelf();
                if (guest_pc_cpu_.ObservesBlocks()) guest_pc_cpu_.Block(block->start_pc, block->end_pc, profile_thread_id_);
                goto* dispatch_table[inst->dispatch_index];
            }
        }

        // Fast path: use cached chain target if it matches
        BasicBlock* next = block->chain_target;
        if (next && next->start_pc == next_pc && next->IsThumb() == state_.thumb_mode) {
            chain_count++;
            block->exec_count++;
            block = next;
            inst = block->instructions.data();
            end = inst + block->inst_count;
            if (inst < end) {
                decode_cost_.ChainPrimary();
                if (guest_pc_cpu_.ObservesBlocks()) guest_pc_cpu_.Block(block->start_pc, block->end_pc, profile_thread_id_);
                goto* dispatch_table[inst->dispatch_index];
            }
        }

        // Slow path: look up next block
        // Only cache if we don't already have a chain_target (don't thrash on alternating branches)
        decode_cost_.ChainLookup();
        u16 next_slot = INVALID_BLOCK_SLOT;
        if (secondary_link_) {
            next_slot = block->secondary_slot;
            next = cache_.LookupSecondary(next_pc, next_slot);
            if (!next) next = cache_.Lookup(next_pc, &next_slot);
        } else {
            next = cache_.Lookup(next_pc);
        }
        if (next && next->IsThumb() == state_.thumb_mode) {
            if (!block->chain_target) {
                block->chain_target = next; // Cache for next time
            } else if (secondary_link_) {
                block->secondary_slot = next_slot;
            }
            chain_count++;
            block->exec_count++;
            block = next;
            inst = block->instructions.data();
            end = inst + block->inst_count;
            if (inst < end) {
                decode_cost_.ChainFallback();
                if (guest_pc_cpu_.ObservesBlocks()) guest_pc_cpu_.Block(block->start_pc, block->end_pc, profile_thread_id_);
                goto* dispatch_table[inst->dispatch_index];
            }
        }
    }

done:
    SYNC_PC();
    block->exec_count++;
    // Track and reoptimize hot block when threshold is first reached. The chain
    // paths increment exec_count without this check, so a self-chaining block
    // strides past the exact threshold - compare with >= (IsHot() prevents
    // repeat triggers).
    if (block->exec_count >= HOT_BLOCK_THRESHOLD && !block->IsHot()) {
        block->SetHot();
        fusion_stats_.hot_blocks_detected++;
        if (!disable_reoptimization_) {
            ReoptimizeHotBlock(block);
        }
    }

#undef GET_PC
#undef SET_PC
#undef SYNC_PC
#undef DISPATCH
#undef KEEP_PAGE_TABLE_REGISTER
#undef CHECK_COND
#undef CHECK_COND_GATE
#undef ADVANCE_AND_DISPATCH
#undef MEM_READ8
#undef MEM_READ16
#undef MEM_READ32
#undef MEM_WRITE8
#undef MEM_WRITE16
#undef MEM_WRITE32

    return executed;
}

// ============================================================================
// Condition Check
// ============================================================================

bool ARM_FastInterp::CheckCondition(u8 cond) {
    state_.lazy_flags.Materialize();
    return CondPassed(cond, state_.lazy_flags.nzcv);
}

// ============================================================================
// Barrel Shifter
// ============================================================================

// For immediate shifts, amount==0 has special meanings:
// LSL #0 = no shift (carry unchanged), LSR/ASR #0 = #32, ROR #0 = RRX
u32 ARM_FastInterp::ShiftImm(u32 value, ShiftType type, u8 amount, bool& carry_out) {
    switch (type) {
    case ShiftType::LSL:
        if (amount == 0) {
            carry_out = state_.lazy_flags.GetC();
            return value;
        }
        if (amount >= 32) {
            carry_out = (amount == 32) ? (value & 1) : false;
            return 0;
        }
        carry_out = (value >> (32 - amount)) & 1;
        return value << amount;

    case ShiftType::LSR:
        // LSR #0 encodes LSR #32
        if (amount == 0) {
            amount = 32;
        }
        if (amount >= 32) {
            carry_out = (amount == 32) ? ((value >> 31) & 1) : false;
            return 0;
        }
        carry_out = (value >> (amount - 1)) & 1;
        return value >> amount;

    case ShiftType::ASR:
        // ASR #0 encodes ASR #32
        if (amount == 0) {
            amount = 32;
        }
        if (amount >= 32) {
            carry_out = (value >> 31) & 1;
            return static_cast<u32>(static_cast<s32>(value) >> 31);
        }
        carry_out = (value >> (amount - 1)) & 1;
        return static_cast<u32>(static_cast<s32>(value) >> amount);

    case ShiftType::ROR:
        if (amount == 0) {
            // RRX: rotate right extended (through carry)
            carry_out = value & 1;
            return (state_.lazy_flags.GetC() ? 0x80000000 : 0) | (value >> 1);
        }
        amount &= 31;
        if (amount == 0) {
            carry_out = (value >> 31) & 1;
            return value;
        }
        carry_out = (value >> (amount - 1)) & 1;
        return (value >> amount) | (value << (32 - amount));

    default:
        carry_out = false;
        return value;
    }
}

u32 ARM_FastInterp::ShiftReg(u32 value, ShiftType type, u8 amount, bool& carry_out) {
    // Register-specified shift amount (bottom byte only; amount is already u8)
    if (amount == 0) {
        carry_out = state_.lazy_flags.GetC();
        return value;
    }
    return ShiftImm(value, type, amount, carry_out);
}

// Fast shift without carry computation (for non-flag-setting operations)
u32 ARM_FastInterp::ShiftNoCarry(u32 value, ShiftType type, u8 amount) {
    switch (type) {
    case ShiftType::LSL:
        if (amount == 0)
            return value;
        if (amount >= 32)
            return 0;
        return value << amount;

    case ShiftType::LSR:
        if (amount == 0)
            amount = 32; // LSR #0 encodes LSR #32
        if (amount >= 32)
            return 0;
        return value >> amount;

    case ShiftType::ASR:
        if (amount == 0)
            amount = 32; // ASR #0 encodes ASR #32
        if (amount >= 32)
            return static_cast<u32>(static_cast<s32>(value) >> 31);
        return static_cast<u32>(static_cast<s32>(value) >> amount);

    case ShiftType::ROR:
        if (amount == 0) {
            // RRX - need carry for this, fall back to slow path
            bool carry;
            return ShiftImm(value, type, amount, carry);
        }
        amount &= 31;
        if (amount == 0)
            return value;
        return (value >> amount) | (value << (32 - amount));

    default:
        return value;
    }
}

// ============================================================================
// Memory Access
// ============================================================================
// Plain forwards to the memory system, for decode-time access (ExecuteBlock
// uses the MEM_* fast paths instead).

u16 ARM_FastInterp::ReadMemory16(u32 addr) {
    return memory_.Read16(addr);
}

u32 ARM_FastInterp::ReadMemory32(u32 addr) {
    return memory_.Read32(addr);
}

// ============================================================================
// Cache, Page Table & Register Accessors
// ============================================================================

void ARM_FastInterp::ClearInstructionCache() {
    cache_.Clear();
}

void ARM_FastInterp::InvalidateCacheRange(u32 start_address, std::size_t length) {
    cache_.InvalidateRange(start_address, static_cast<u32>(length));
}

void ARM_FastInterp::ClearExclusiveState() {
    exclusive_.valid = false;
}

void ARM_FastInterp::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    // The kernel calls this on every context switch, almost always with the
    // same page table. Only reset when the address space actually changes;
    // flushing the block cache every switch would re-decode all hot code.
    if (page_table_ == page_table) {
        return;
    }
    if (cache_.ProfilingEnabled()) ++cache_page_table_changes_;
    page_table_ = page_table;
    if (page_table) {
        state_.page_pointers = page_table->GetPointerArray().data();
    } else {
        state_.page_pointers = empty_page_table; // Never null
    }
    ClearInstructionCache();
}

std::shared_ptr<Memory::PageTable> ARM_FastInterp::GetPageTable() const {
    return page_table_;
}

void ARM_FastInterp::SetPC(u32 addr) {
    state_.thumb_mode = (addr & 1) != 0;
    state_.SetPC(addr & ~1u);
}

u32 ARM_FastInterp::GetPC() const {
    return state_.PC();
}

u32 ARM_FastInterp::GetReg(int index) const {
    return state_.regs[index];
}

void ARM_FastInterp::SetReg(int index, u32 value) {
    state_.regs[index] = value;
}

u32 ARM_FastInterp::GetVFPReg(int index) const {
    return state_.vfp_regs[index];
}

void ARM_FastInterp::SetVFPReg(int index, u32 value) {
    state_.vfp_regs[index] = value;
}

u32 ARM_FastInterp::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return state_.fpscr;
    case VFP_FPEXC:
        return state_.fpexc;
    default:
        return 0;
    }
}

void ARM_FastInterp::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        state_.fpscr = value;
        break;
    case VFP_FPEXC:
        state_.fpexc = value;
        break;
    default:
        break;
    }
}

u32 ARM_FastInterp::GetCPSR() const {
    // Need to materialize lazy flags for const access
    LazyFlags& flags = const_cast<LazyFlags&>(state_.lazy_flags);
    u32 cpsr = flags.GetPacked() | (state_.cpsr_other & 0x0FFFFFFF);
    // Merge T from thumb_mode, as in InterpreterState::GetCPSR
    cpsr = (cpsr & ~(1u << 5)) | (static_cast<u32>(state_.thumb_mode) << 5);
    return cpsr;
}

void ARM_FastInterp::SetCPSR(u32 cpsr) {
    state_.SetCPSR(cpsr);
    state_.thumb_mode = (cpsr >> 5) & 1;
}

u32 ARM_FastInterp::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return state_.cp15_thread_uprw;
    case CP15_THREAD_URO:
        return state_.cp15_thread_uro;
    default:
        return 0;
    }
}

void ARM_FastInterp::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        state_.cp15_thread_uprw = value;
        break;
    case CP15_THREAD_URO:
        state_.cp15_thread_uro = value;
        break;
    default:
        break;
    }
}

std::unique_ptr<ARM_Interface::ThreadContext> ARM_FastInterp::NewContext() const {
    return std::make_unique<FastInterpThreadContext>();
}

void ARM_FastInterp::SaveContext(const std::unique_ptr<ThreadContext>& arg) {
    auto& ctx = static_cast<FastInterpThreadContext&>(*arg);
    for (int i = 0; i < 16; i++) {
        ctx.cpu_registers[i] = state_.regs[i];
    }
    ctx.cpsr = GetCPSR();
    for (int i = 0; i < 64; i++) {
        ctx.fpu_registers[i] = state_.vfp_regs[i];
    }
    ctx.fpscr = state_.fpscr;
    ctx.fpexc = state_.fpexc;
}

void ARM_FastInterp::LoadContext(const std::unique_ptr<ThreadContext>& arg) {
    const auto& ctx = static_cast<const FastInterpThreadContext&>(*arg);
    if (guest_pc_cpu_.ObservesBlocks()) {
        profile_thread_id_ = ProfileThreadId();
        guest_pc_cpu_.SwitchThread(profile_thread_id_);
    }
    ClearExclusiveState();
    for (int i = 0; i < 16; i++) {
        state_.regs[i] = ctx.cpu_registers[i];
    }
    SetCPSR(ctx.cpsr);
    for (int i = 0; i < 64; i++) {
        state_.vfp_regs[i] = ctx.fpu_registers[i];
    }
    state_.fpscr = ctx.fpscr;
    state_.fpexc = ctx.fpexc;
}

std::uint32_t ARM_FastInterp::ProfileThreadId() const {
    // Standalone instruction fixtures do not initialize a kernel process.
    if (!system_.IsPoweredOn()) return 0;
    const auto* thread = system_.Kernel().GetThreadManager(GetID()).GetCurrentThread();
    return thread ? thread->GetThreadId() : 0;
}

void ARM_FastInterp::PrepareReschedule() {
    halted_ = true;
}

} // namespace Core::FastInterp
