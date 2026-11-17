#include "cara/guest_gpu_trace.h"
// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/cpu_category_timing.h"
#include "common/archives.h"
#include <stdexcept>
#include "core/core_timing.h"
#include "core/settings.h"
#include "common/serialization/atomic.h"
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/config_mem.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/timer.h"

namespace Kernel {

/// Initialize the kernel
KernelSystem::KernelSystem(Memory::MemorySystem& memory, Core::Timing& timing, u32 system_mode,
                           u8 n3ds_mode)
    : memory(memory), timing(timing) {
    for (auto& region : memory_regions) region = std::make_shared<MemoryRegionInfo>();
    MemoryInit(system_mode, n3ds_mode);

    resource_limits = std::make_unique<ResourceLimitList>(*this);
    for (u32 i = 0; i < thread_managers.size(); ++i) {
        thread_managers[i] = std::make_unique<ThreadManager>(*this, i);
    }
    timer_manager = std::make_unique<TimerManager>(timing);
    ipc_recorder = std::make_unique<IPCDebugger::Recorder>();

    next_thread_id = 1;
    if (Old3dsSyscoreRequested()) {
        old3ds_syscore_event = timing.RegisterEvent("CaraOld3dsSyscoreBoundary",
            [this](u64, int) {
                // Phase is derived from the saved absolute epoch, never callback wall time.
                old3ds_syscore.Refresh(this->timing.GetTimer(1)->GetTicks());
                ScheduleOld3dsSyscoreBoundary();
            });
    }
}

/// Shutdown the kernel
KernelSystem::~KernelSystem() {
    // Break process-handle/thread-owner cycles while all kernel managers live.
    // Repeated state loads tear down an entire kernel each time.
    for (auto& manager : thread_managers) {
        if (manager) for (const auto& thread : manager->GetThreadList()) if (thread) thread->Stop();
    }
    for (auto& process : process_list) if (process) process->handle_table.Clear();
    if (old3ds_syscore_event) {
        timing.GetTimer(1)->MoveEvents();
        timing.RemoveEvent(old3ds_syscore_event);
    }
    ResetThreadIDs();
};

ResourceLimitList& KernelSystem::ResourceLimit() {
    return *resource_limits;
}

const ResourceLimitList& KernelSystem::ResourceLimit() const {
    return *resource_limits;
}

u32 KernelSystem::GenerateObjectID() {
    return next_object_id++;
}

void KernelSystem::Initialize(const std::shared_ptr<Process>& process, ARM_Interface* cpu) {
    current_cpu = cpu;
    current_process = process;
    memory.SetCurrentPageTable(process->vm_manager.page_table);
    for (int i = 0; i < thread_managers.size(); ++i) {
        stored_processes[i] = process;
        thread_managers[i]->cpu->SetPageTable(process->vm_manager.page_table);
    }
}

std::shared_ptr<Process> KernelSystem::GetCurrentProcess() const {
    return current_process;
}

void KernelSystem::SetCurrentProcess(const std::shared_ptr<Process>& process) {
    current_process = process;
    memory.SetCurrentPageTable(process->vm_manager.page_table);
    stored_processes[current_cpu->GetID()] = process;
    current_cpu->SetPageTable(process->vm_manager.page_table);
}

void KernelSystem::SetCurrentProcessForCPU(const std::shared_ptr<Process>& process, u32 core_id) {
    if (current_cpu->GetID() == core_id) {
        SetCurrentProcess(process);
    } else {
        stored_processes[core_id] = process;
        thread_managers[core_id]->cpu->SetPageTable(process->vm_manager.page_table);
    }
}

void KernelSystem::Advance(ARM_Interface* cpu, s64 max_slice_length) {
    ARM_Interface* previous_cpu = current_cpu;
    current_cpu = cpu;
    cpu->GetTimer().Advance(max_slice_length);
    current_cpu = previous_cpu;
}

void KernelSystem::Run(ARM_Interface* cpu) {
    const u32 new_cpu_id = cpu->GetID();
    const u32 old_cpu_id = current_cpu->GetID();
    current_cpu = cpu;

    // try switch process context
    auto& new_process = stored_processes[new_cpu_id];
    if (new_process != current_process) {
        stored_processes[old_cpu_id] = current_process;
        SetCurrentProcess(new_process);
    }

    timing.SetCurrentTimer(new_cpu_id);

    // Eligibility is separate from Ready/Wait state. Device events and the timer continue;
    // a real waiter is never awakened merely because its application window opens.
    const bool syscore_run = Old3dsSyscoreEnabled() && new_cpu_id == 1;
    const s64 syscore_before = syscore_run ? cpu->GetTimer().GetTicks() : 0;
    if (syscore_run) {
        ++syscore_run_calls;
        if (current_process && current_process->resource_limit !=
            resource_limits->GetForCategory(ResourceLimitCategory::APPLICATION))
            throw std::runtime_error("Old3DS syscore diagnostic supports HLE application execution only");
        old3ds_syscore.Refresh(cpu->GetTimer().GetTicks());
        if (!old3ds_syscore.AppEligible()) {
            Cara::GuestGPUTrace::ExecutionContext(timing,0,new_cpu_id);
            cpu->GetTimer().Idle();
            ++syscore_idle_calls;
            syscore_idled_ticks += cpu->GetTimer().GetTicks() - syscore_before;
            return;
        }
    }

    // If we don't have a currently active thread then don't execute instructions,
    // instead advance to the next event and try to yield to the next thread
    const auto* executing_thread=thread_managers[new_cpu_id]->GetCurrentThread();
    Cara::GuestGPUTrace::ExecutionContext(timing,executing_thread ? executing_thread->GetThreadId() : 0,new_cpu_id);
    if (thread_managers[new_cpu_id]->GetCurrentThread() == nullptr) {
        LOG_TRACE(Core_ARM11, "Core {} idling", new_cpu_id);
        current_cpu->GetTimer().Idle();
        thread_managers[new_cpu_id]->PrepareReschedule();
        if (syscore_run) ++syscore_idle_calls;
    } else {
        Common::CPUCategoryTiming::Scope cpu_category_scope(Common::CPUCategoryTiming::Category::CPUExecution);
        if (syscore_run) ++syscore_execute_calls;
        current_cpu->Run();
    }
    if (syscore_run) {
        const auto elapsed = cpu->GetTimer().GetTicks() - syscore_before;
        if (thread_managers[new_cpu_id]->GetCurrentThread() == nullptr)
            syscore_idled_ticks += elapsed;
        else syscore_executed_ticks += elapsed;
    }
}

ThreadManager& KernelSystem::GetThreadManager(u32 core_id) {
    return *thread_managers[core_id];
}

const ThreadManager& KernelSystem::GetThreadManager(u32 core_id) const {
    return *thread_managers[core_id];
}

ThreadManager& KernelSystem::GetCurrentThreadManager() {
    return *thread_managers[current_cpu->GetID()];
}

const ThreadManager& KernelSystem::GetCurrentThreadManager() const {
    return *thread_managers[current_cpu->GetID()];
}

TimerManager& KernelSystem::GetTimerManager() {
    return *timer_manager;
}

const TimerManager& KernelSystem::GetTimerManager() const {
    return *timer_manager;
}

SharedPage::Handler& KernelSystem::GetSharedPageHandler() {
    return *shared_page_handler;
}

const SharedPage::Handler& KernelSystem::GetSharedPageHandler() const {
    return *shared_page_handler;
}

IPCDebugger::Recorder& KernelSystem::GetIPCRecorder() {
    return *ipc_recorder;
}

const IPCDebugger::Recorder& KernelSystem::GetIPCRecorder() const {
    return *ipc_recorder;
}

void KernelSystem::AddNamedPort(std::string name, std::shared_ptr<ClientPort> port) {
    named_ports.emplace(std::move(name), std::move(port));
}

void KernelSystem::PrepareReschedule() {
    current_cpu->PrepareReschedule();
    for (auto& manager : thread_managers) {
        manager->PrepareReschedule();
    }
}

/// Reschedule the core emulation
void KernelSystem::RescheduleMultiCores() {
    for (auto& manager : thread_managers) {
        manager->Reschedule();
    }
}

void KernelSystem::RescheduleSingleCore() {
    thread_managers[0]->Reschedule();
}

u32 KernelSystem::NewThreadId() {
    return next_thread_id++;
}

void KernelSystem::ResetThreadIDs() {
    next_thread_id = 0;
}

template <class Archive>
void KernelSystem::serialize(Archive& ar, const unsigned int file_version) {
    u32 running_id = current_cpu ? current_cpu->GetID() : 0;
    ar& running_id;
    // System's mode marker rejects cross-mode loads before reinitialization.
    if (Old3dsSyscoreRequested()) {
        old3ds_syscore.Serialize(ar);
        ar& syscore_run_calls; ar& syscore_execute_calls; ar& syscore_idle_calls;
        ar& syscore_executed_ticks; ar& syscore_idled_ticks;
        if (Archive::is_loading::value) old3ds_syscore.Validate();
    }
    if (running_id >= thread_managers.size()) throw std::runtime_error("Invalid saved CPU index");
    if (Archive::is_loading::value) current_cpu = thread_managers[running_id]->cpu;
    ar& memory_regions;
    ar& named_ports;
    // current_cpu set externally
    // NB: subsystem references and prepare_reschedule_callback are constant
    ar&* resource_limits.get();
    u32 saved_object_id = next_object_id;
    ar& saved_object_id;
    ar&* timer_manager.get();
    u32 saved_process_id = next_process_id;
    ar& saved_process_id;
    ar& process_list;
    ar& current_process;
    // NB: core count checked in 'core'
    for (auto& thread_manager : thread_managers) {
        ar&* thread_manager.get();
    }
    ar& config_mem_handler;
    ar& shared_page_handler;
    ar& stored_processes;
    ar& next_thread_id;
    if (Archive::is_loading::value) { next_object_id = saved_object_id; next_process_id = saved_process_id; }
    // Deliberately don't include debugger info to allow debugging through loads
}

bool KernelSystem::Old3dsSyscoreRequested() const {
    return !Settings::values.is_new_3ds && Old3dsSyscore::Requested();
}

void KernelSystem::ConfigureOld3dsSyscore(u16 descriptor) {
    if (!Old3dsSyscoreRequested()) return;
    old3ds_syscore.Configure(descriptor);
    auto application = resource_limits->GetForCategory(ResourceLimitCategory::APPLICATION);
    if (old3ds_syscore.Supported()) {
        application->max_cpu_time = old3ds_syscore.MaxPercent();
        application->current_cpu_time = 0;
    }
    ScheduleOld3dsSyscoreBoundary();
    if (!old3ds_syscore.Supported()) {
        LOG_WARNING(Kernel, "Old3DS syscore diagnostic: unsupported descriptor {:04x}; legacy core0 fallback", descriptor);
    }
}

bool KernelSystem::SetOld3dsSyscoreLimit(u32 percent) {
    if (!Old3dsSyscoreRequested()) return false;
    if (!old3ds_syscore.SetLimit(percent, timing.GetTicks())) return false;
    resource_limits->GetForCategory(ResourceLimitCategory::APPLICATION)->current_cpu_time = percent;
    ScheduleOld3dsSyscoreBoundary();
    if (current_cpu && current_cpu->GetID() == 1) {
        // A successful update closes APP now, even when the new allowance is zero.
        current_cpu->PrepareReschedule();
        timing.GetTimer(1)->ForceExceptionCheck(0);
    }
    return true;
}

void KernelSystem::ScheduleOld3dsSyscoreBoundary() {
    // Cross-core ScheduleEvent otherwise uses the MPSC queue, which UnscheduleEvent does
    // not remove. Move it first, then queue directly under the emulation-thread owner.
    auto timer = timing.GetTimer(1);
    timer->MoveEvents();
    timing.UnscheduleEvent(old3ds_syscore_event, 0);
    if (!old3ds_syscore.pending) return;
    const auto prior = timing.GetCurrentTimerId();
    timing.SetCurrentTimer(1);
    timing.ScheduleEvent(old3ds_syscore.deadline - timer->GetTicks(), old3ds_syscore_event, 0, 1);
    timing.SetCurrentTimer(prior);
}

void KernelSystem::WriteOld3dsSyscoreJSON(std::ostream& out) const {
    out << "{\"requested\":" << (Old3dsSyscoreRequested() ? "true" : "false")
        << ",\"active\":" << (Old3dsSyscoreEnabled() ? "true" : "false")
        << ",\"supported\":" << (old3ds_syscore.Supported() ? "true" : "false")
        << ",\"descriptor\":" << old3ds_syscore.descriptor
        << ",\"max_percent\":" << old3ds_syscore.MaxPercent()
        << ",\"current_percent\":" << old3ds_syscore.percent
        << ",\"phase\":\"" << (!old3ds_syscore.pending ? "disabled" : old3ds_syscore.app_phase ? "APP" : "SYS")
        << "\",\"epoch\":" << old3ds_syscore.epoch
        << ",\"deadline\":" << old3ds_syscore.deadline
        << ",\"pending\":" << (old3ds_syscore.pending ? "true" : "false")
        << ",\"core1_run_calls\":" << syscore_run_calls
        << ",\"core1_execute_calls\":" << syscore_execute_calls
        << ",\"core1_idle_calls\":" << syscore_idle_calls
        << ",\"core1_executed_ticks\":" << syscore_executed_ticks
        << ",\"core1_idled_ticks\":" << syscore_idled_ticks << "}";
}

SERIALIZE_IMPL(KernelSystem)

} // namespace Kernel

namespace Kernel {
void KernelSystem::RestoreExecutionContext() {
    if (current_process) memory.SetCurrentPageTable(current_process->vm_manager.page_table);
    for (std::size_t i = 0; i < thread_managers.size(); ++i) {
        auto process = stored_processes[i] ? stored_processes[i] : current_process;
        if (process) thread_managers[i]->cpu->SetPageTable(process->vm_manager.page_table);
    }
}
}
