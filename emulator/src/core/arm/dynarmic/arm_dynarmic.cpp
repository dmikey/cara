// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <cstdlib>
#include <ostream>
#include <stdexcept>
#ifdef ANDROID
#include <ctime>
#include <fstream>
#include <vector>
#include "common/file_util.h"
#endif
#include "core/arm/fastinterp/arm_tick_counts.h"
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/context.h>
#include "common/assert.h"
#include "common/atomic_ops.h"
#include "common/frame_timing.h"
#include <dynarmic/interface/exclusive_monitor.h>
#include "common/microprofile.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/gdbstub/gdbstub.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include "core/settings.h"

static Dynarmic::ExclusiveMonitor& GlobalExclusiveMonitor() {
    static Dynarmic::ExclusiveMonitor monitor(4);
    return monitor;
}

class DynarmicThreadContext final : public ARM_Interface::ThreadContext {
public:
    DynarmicThreadContext() {
        Reset();
    }
    ~DynarmicThreadContext() override = default;

    void Reset() override {
        ctx.Regs() = {};
        ctx.SetCpsr(0);
        ctx.ExtRegs() = {};
        ctx.SetFpscr(0);
        fpexc = 0;
    }

    u32 GetCpuRegister(std::size_t index) const override {
        return ctx.Regs()[index];
    }
    void SetCpuRegister(std::size_t index, u32 value) override {
        ctx.Regs()[index] = value;
    }
    u32 GetCpsr() const override {
        return ctx.Cpsr();
    }
    void SetCpsr(u32 value) override {
        ctx.SetCpsr(value);
    }
    u32 GetFpuRegister(std::size_t index) const override {
        return ctx.ExtRegs()[index];
    }
    void SetFpuRegister(std::size_t index, u32 value) override {
        ctx.ExtRegs()[index] = value;
    }
    u32 GetFpscr() const override {
        return ctx.Fpscr();
    }
    void SetFpscr(u32 value) override {
        ctx.SetFpscr(value);
    }
    u32 GetFpexc() const override {
        return fpexc;
    }
    void SetFpexc(u32 value) override {
        fpexc = value;
    }

private:
    friend class ARM_Dynarmic;

    Dynarmic::A32::Context ctx;
    u32 fpexc;
};

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(ARM_Dynarmic& parent)
        : parent(parent), svc_context(parent.system), memory(parent.memory) {}
    ~DynarmicUserCallbacks() override {
#ifdef ANDROID
        if (block_emissions.empty()) return;

        const auto path = FileUtil::GetUserPath(FileUtil::UserPath::LogDir) +
                          "jit-blocks-core-" + std::to_string(parent.GetID()) + ".csv";
        std::ofstream out;
        FileUtil::OpenFStream(out, path, std::ios::out | std::ios::trunc);
        if (!out) {
            LOG_WARNING(Core_ARM11, "Could not write Dynarmic block map for core {}", parent.GetID());
            return;
        }
        out << "# dropped=" << dropped_block_emissions << '\n'
            << "monotonic_ns,address_space,host_start,host_size,guest_pc,thumb\n";
        for (const auto& block : block_emissions) {
            out << block.monotonic_ns << ',' << block.address_space << ',' << block.host_start
                << ',' << block.host_size << ',' << block.guest_pc << ',' << block.thumb << '\n';
        }
#endif
    }

#ifdef ANDROID
    static void RecordBlockEmission(void* context, VAddr pc, bool thumb,
                                    std::uintptr_t address_space, std::uintptr_t host_start,
                                    std::size_t host_size) {
        auto& recorder = *static_cast<DynarmicUserCallbacks*>(context);
        timespec timestamp{};
        if (recorder.block_emissions.size() >= MaxBlockEmissions ||
            clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
            ++recorder.dropped_block_emissions;
            return;
        }
        const u64 time = static_cast<u64>(timestamp.tv_sec) * 1000000000ULL +
                         static_cast<u64>(timestamp.tv_nsec);
        recorder.block_emissions.push_back({time, address_space, host_start, host_size, pc, thumb});
    }
#endif

    std::uint8_t MemoryRead8(VAddr vaddr) override {
        return memory.Read8(vaddr);
    }
    std::uint16_t MemoryRead16(VAddr vaddr) override {
        return memory.Read16(vaddr);
    }
    std::uint32_t MemoryRead32(VAddr vaddr) override {
        return memory.Read32(vaddr);
    }
    std::uint64_t MemoryRead64(VAddr vaddr) override {
        return memory.Read64(vaddr);
    }

    void MemoryWrite8(VAddr vaddr, std::uint8_t value) override {
        memory.Write8(vaddr, value);
    }
    void MemoryWrite16(VAddr vaddr, std::uint16_t value) override {
        memory.Write16(vaddr, value);
    }
    void MemoryWrite32(VAddr vaddr, std::uint32_t value) override {
        memory.Write32(vaddr, value);
    }
    void MemoryWrite64(VAddr vaddr, std::uint64_t value) override {
        memory.Write64(vaddr, value);
    }

    template<typename T>
    bool WriteExclusive(VAddr address, T value, T expected) {
        if (address % alignof(T)) return false;
        Memory::RasterizerFlushVirtualRegion(address, sizeof(T), Memory::FlushMode::FlushAndInvalidate);
        auto* ptr = memory.GetPointer(address);
        return ptr && Common::AtomicCompareAndSwap(reinterpret_cast<volatile T*>(ptr), value, expected);
    }
    bool MemoryWriteExclusive8(VAddr a, u8 v, u8 e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive16(VAddr a, u16 v, u16 e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive32(VAddr a, u32 v, u32 e) override { return WriteExclusive(a, v, e); }
    bool MemoryWriteExclusive64(VAddr a, u64 v, u64 e) override { return WriteExclusive(a, v, e); }

    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        // Should never happen.
        UNREACHABLE_MSG("InterpeterFallback reached with pc = 0x{:08x}, code unavailable, num = {}",
                        pc, num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
#ifdef ANDROID
        static const bool trace = [] {
            const char* value = std::getenv("CARA_JIT_STARTUP_TRACE");
            return value && std::strcmp(value, "1") == 0;
        }();
        if (trace && startup_svc_count < 128) {
            ++startup_svc_count;
            LOG_INFO(Core_ARM11, "Dynarmic startup SVC core={} svc={:02X} pc={:08X} r0={:08X} r1={:08X}",
                     parent.GetID(), swi, parent.GetPC(), parent.GetReg(0), parent.GetReg(1));
        }
#endif
#ifdef CARA_MAC_CPU_JIT
        ++parent.diagnostic_svc_calls;
#endif
        svc_context.CallSVC(swi);
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::UndefinedInstruction:
        case Dynarmic::A32::Exception::UnpredictableInstruction:
            break;
        case Dynarmic::A32::Exception::Breakpoint:
            if (GDBStub::IsConnected()) {
                parent.jit->HaltExecution();
                parent.SetPC(pc);
                parent.ServeBreak();
                return;
            }
            break;
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
        case Dynarmic::A32::Exception::PreloadInstruction:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
            return;
        }
        ASSERT_MSG(false, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X})",
                   static_cast<std::size_t>(exception), pc, MemoryRead32(pc));
    }

    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb,instruction);
    }
    void AddTicks(std::uint64_t ticks) override {
        ticks = std::max(ticks, static_cast<std::uint64_t>(Settings::values.core_ticks_hack));
        parent.GetTimer().AddTicks(ticks);
#ifdef CARA_MAC_CPU_JIT
        parent.diagnostic_ticks_charged += ticks;
#endif
    }
    std::uint64_t GetTicksRemaining() override {
        s64 ticks = parent.GetTimer().GetDowncount();
        return static_cast<u64>(ticks <= 0 ? 0 : ticks);
    }

#ifdef ANDROID
    struct BlockEmission {
        u64 monotonic_ns;
        std::uintptr_t address_space;
        std::uintptr_t host_start;
        std::size_t host_size;
        VAddr guest_pc;
        bool thumb;
    };
    static constexpr std::size_t MaxBlockEmissions = 65536;
    std::vector<BlockEmission> block_emissions;
    u64 dropped_block_emissions = 0;
    unsigned startup_svc_count = 0;
#endif
    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
};

ARM_Dynarmic::ARM_Dynarmic(Core::System* system, u32 id, std::shared_ptr<Core::Timing::Timer> timer)
    : ARM_Interface(id, timer), system(*system), memory(system->Memory()),
      cb(std::make_unique<DynarmicUserCallbacks>(*this)) { GlobalExclusiveMonitor().ClearProcessor(id); }

ARM_Dynarmic::~ARM_Dynarmic() = default;

MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::Run() {
    ASSERT(memory.GetCurrentPageTable() == current_page_table);
    MICROPROFILE_SCOPE(ARM_Jit);
    // Inclusive of HLE callbacks and renderer work entered by guest SVCs.
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::GuestCPU);
    if(!jit)throw std::runtime_error("Dynarmic Run requires a bound guest page table");
    jit->Run();
#ifdef CARA_MAC_CPU_JIT
    ++diagnostic_run_calls;
#endif
}

void ARM_Dynarmic::Step() {
    if(!jit)throw std::runtime_error("Dynarmic Step requires a bound guest page table");
    jit->Step();
#ifdef CARA_MAC_CPU_JIT
    ++diagnostic_step_calls;
#endif

    if (GDBStub::IsConnected()) {
        ServeBreak();
    }
}

void ARM_Dynarmic::SetPC(u32 pc) {
    register_state.Regs(jit)[15] = pc;
}

u32 ARM_Dynarmic::GetPC() const {
    return register_state.Regs(jit)[15];
}

u32 ARM_Dynarmic::GetReg(int index) const {
    return register_state.Regs(jit)[index];
}

void ARM_Dynarmic::SetReg(int index, u32 value) {
    register_state.Regs(jit)[index] = value;
}

u32 ARM_Dynarmic::GetVFPReg(int index) const {
    return register_state.ExtRegs(jit)[index];
}

void ARM_Dynarmic::SetVFPReg(int index, u32 value) {
    register_state.ExtRegs(jit)[index] = value;
}

u32 ARM_Dynarmic::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return register_state.Fpscr(jit);
    case VFP_FPEXC:
        return fpexc;
    }
    UNREACHABLE_MSG("Unknown VFP system register: {}", static_cast<size_t>(reg));
}

void ARM_Dynarmic::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        register_state.SetFpscr(jit,value);
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    }
    UNREACHABLE_MSG("Unknown VFP system register: {}", static_cast<size_t>(reg));
}

u32 ARM_Dynarmic::GetCPSR() const {
    return register_state.Cpsr(jit);
}

void ARM_Dynarmic::SetCPSR(u32 cpsr) {
    register_state.SetCpsr(jit,cpsr);
}

u32 ARM_Dynarmic::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return cp15_state.cp15_thread_uprw;
    case CP15_THREAD_URO:
        return cp15_state.cp15_thread_uro;
    }
    UNREACHABLE_MSG("Unknown CP15 register: {}", static_cast<size_t>(reg));
}

void ARM_Dynarmic::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        cp15_state.cp15_thread_uprw = value;
        return;
    case CP15_THREAD_URO:
        cp15_state.cp15_thread_uro = value;
        return;
    }
    UNREACHABLE_MSG("Unknown CP15 register: {}", static_cast<size_t>(reg));
}

std::unique_ptr<ARM_Interface::ThreadContext> ARM_Dynarmic::NewContext() const {
    return std::make_unique<DynarmicThreadContext>();
}

void ARM_Dynarmic::SaveContext(const std::unique_ptr<ThreadContext>& arg) {
    DynarmicThreadContext* ctx = dynamic_cast<DynarmicThreadContext*>(arg.get());
    ASSERT(ctx);

    register_state.Capture(jit,ctx->ctx);
    ctx->fpexc = fpexc;
}

void ARM_Dynarmic::LoadContext(const std::unique_ptr<ThreadContext>& arg) {
    const DynarmicThreadContext* ctx = dynamic_cast<DynarmicThreadContext*>(arg.get());
    ASSERT(ctx);

    register_state.Restore(jit,ctx->ctx);
    if(jit)jit->ClearExclusiveState();
    GlobalExclusiveMonitor().ClearProcessor(GetID());
    fpexc = ctx->fpexc;
}

void ARM_Dynarmic::PrepareReschedule() {
    if (jit && jit->IsExecuting()) {
        jit->HaltExecution();
    }
}

void ARM_Dynarmic::ClearInstructionCache() {
    for (const auto& j : jits) {
        j.second->ClearCache();
    }
}

void ARM_Dynarmic::InvalidateCacheRange(u32 start_address, std::size_t length) {
    // Conservative across every retained address-space translation cache.
    // No guest register/tick/IRQ state changes; also safe for an unbound core.
    for(const auto& item:jits)item.second->InvalidateCacheRange(start_address,length);
    LOG_DEBUG(Core_ARM11, "arm jit invalidate cache range");
}

std::shared_ptr<Memory::PageTable> ARM_Dynarmic::GetPageTable() const {
    return current_page_table;
}

void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    if(current_page_table==page_table)return;
    Dynarmic::A32::Context ctx{};
    register_state.Capture(jit,ctx);
    current_page_table=page_table;
    if(!page_table) {
        jit=nullptr;
        register_state.Restore(nullptr,ctx);
        return;
    }
    auto iter=jits.find(page_table);
    if(iter==jits.end()) {
        auto next=MakeJit();jit=next.get();jits.emplace(page_table,std::move(next));
    } else jit=iter->second.get();
    register_state.Restore(jit,ctx);
}

void ARM_Dynarmic::ServeBreak() {
    Kernel::Thread* thread = system.Kernel().GetCurrentThreadManager().GetCurrentThread();
    SaveContext(thread->context);
    GDBStub::Break();
    GDBStub::SendTrap(thread, 5);
}

std::unique_ptr<Dynarmic::A32::Jit> ARM_Dynarmic::MakeJit() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = cb.get();
    config.processor_id = GetID();
    config.global_monitor = &GlobalExclusiveMonitor();
    config.arch_version = Dynarmic::A32::ArchVersion::v6K;
    config.page_table = &current_page_table->GetPointerArray();
#ifdef ANDROID
    if (const char* value = std::getenv("CARA_DYNARMIC_BLOCK_MAP");
        value && std::strcmp(value, "1") == 0) {
        config.block_emitted = &DynarmicUserCallbacks::RecordBlockEmission;
        config.block_emission_context = cb.get();
    }
    if (const char* value = std::getenv("CARA_JIT_CALLBACK_MEMORY"); value && std::strcmp(value, "1") == 0) {
        config.page_table = nullptr;
        LOG_INFO(Core_ARM11, "Dynarmic diagnostic: callback memory enabled");
    }
    if (const char* value = std::getenv("CARA_JIT_NO_OPTIMIZATIONS"); value && std::strcmp(value, "1") == 0) {
        config.optimizations = Dynarmic::no_optimizations;
        LOG_INFO(Core_ARM11, "Dynarmic diagnostic: optimizations disabled");
    }
#endif
    config.coprocessors[15] = std::make_shared<DynarmicCP15>(cp15_state);
    config.define_unpredictable_behaviour = true;
    return std::make_unique<Dynarmic::A32::Jit>(config);
}

void ARM_Dynarmic::PurgeState() {
    ClearInstructionCache();
}

#ifdef CARA_MAC_CPU_JIT
void ARM_Dynarmic::WriteDiagnosticJSON(std::ostream& out) const {
    out<<"{\"core\":"<<GetID()<<",\"backend\":\"dynarmic\",\"cpu_jit_compiled\":true"
       <<",\"page_table_bound\":"<<(current_page_table?"true":"false")
       <<",\"jit_ready\":"<<(jit?"true":"false")
       <<",\"run_calls_completed\":"<<diagnostic_run_calls
       <<",\"step_calls_completed\":"<<diagnostic_step_calls
       <<",\"svc_calls\":"<<diagnostic_svc_calls
       <<",\"ticks_charged\":"<<diagnostic_ticks_charged<<'}';
}
#endif
