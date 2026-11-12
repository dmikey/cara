// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include <stdexcept>
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
}

/// Shutdown the kernel
KernelSystem::~KernelSystem() {
    // Break process-handle/thread-owner cycles while all kernel managers live.
    // Repeated state loads tear down an entire kernel each time.
    for (auto& manager : thread_managers) {
        if (manager) for (const auto& thread : manager->GetThreadList()) if (thread) thread->Stop();
    }
    for (auto& process : process_list) if (process) process->handle_table.Clear();
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

    // If we don't have a currently active thread then don't execute instructions,
    // instead advance to the next event and try to yield to the next thread
    if (thread_managers[new_cpu_id]->GetCurrentThread() == nullptr) {
        LOG_TRACE(Core_ARM11, "Core {} idling", new_cpu_id);
        current_cpu->GetTimer().Idle();
        thread_managers[new_cpu_id]->PrepareReschedule();
    } else {
        current_cpu->Run();
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
