// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "cara/guest_gpu_trace.h"
#include "core/core.h"
#include <algorithm>
#include <map>
#include <vector>
#include "common/archives.h"
#include "common/assert.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/thread.h"

SERIALIZE_EXPORT_IMPL(Kernel::Event)

namespace Kernel {

Event::Event(KernelSystem& kernel) : WaitObject(kernel) {}
Event::~Event() {}

std::shared_ptr<Event> KernelSystem::CreateEvent(ResetType reset_type, std::string name) {
    auto evt{std::make_shared<Event>(*this)};

    evt->signaled = false;
    evt->reset_type = reset_type;
    evt->name = std::move(name);

    return evt;
}

bool Event::ShouldWait(const Thread* thread) const {
    return !signaled;
}

void Event::Acquire(Thread* thread) {
    if (Cara::GuestGPUTrace::Enabled()) Cara::GuestGPUTrace::Emit(Core::System::GetInstance().CoreTiming(),
        Cara::GuestGPUTrace::Kind::EventAcquire, thread ? thread->GetThreadId() : 0, GetObjectId());
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");

    if (reset_type == ResetType::OneShot)
        signaled = false;
}

void Event::Signal() {
    if (Cara::GuestGPUTrace::Enabled()) Cara::GuestGPUTrace::Emit(Core::System::GetInstance().CoreTiming(),
        Cara::GuestGPUTrace::Kind::EventSignal, 0, GetObjectId());
    signaled = true;
    WakeupAllWaitingThreads();
}

void Event::Clear() {
    if (Cara::GuestGPUTrace::Enabled()) Cara::GuestGPUTrace::Emit(Core::System::GetInstance().CoreTiming(),
        Cara::GuestGPUTrace::Kind::EventClear, 0, GetObjectId());
    signaled = false;
}

void Event::WakeupAllWaitingThreads() {
    WaitObject::WakeupAllWaitingThreads();

    if (reset_type == ResetType::Pulse)
        signaled = false;
}

} // namespace Kernel
