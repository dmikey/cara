// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include "core/core_timing.h"

namespace Cara {
// All dependent completions use one event heap. KernelSystem::Advance does not
// select Timing::current_timer, so callback time must come from this owner too.
inline s64 GPUCompletionNow(Core::Timing& timing) {
    return timing.GetTimer(0)->GetTicks();
}
inline s64 GPUSubmissionNow(Core::Timing& timing) {
    return std::max(timing.GetTicks(), GPUCompletionNow(timing));
}
inline void ScheduleGPUCompletion(Core::Timing& timing, s64 deadline,
                                  const Core::TimingEventType* event, u64 token) {
    const auto now = GPUCompletionNow(timing);
    if (deadline < 0 || now < 0) throw std::runtime_error("Invalid GPU completion timeline");
    timing.ScheduleEvent(deadline > now ? deadline - now : 0, event, token, 0);
}
} // namespace Cara
