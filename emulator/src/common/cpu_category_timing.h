// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdlib>
#include <cstring>
#include <ctime>
#include "common/cpu_category_tracker.h"
#include "common/frame_timing.h"

namespace Common::CPUCategoryTiming {
inline bool DiagnosticEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_CPU_CATEGORY_TIMING");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
inline bool ReadThreadCPUTime(void*, std::uint64_t& ns) noexcept {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0 || value.tv_sec < 0 ||
        value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) return false;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    const auto fraction = static_cast<std::uint64_t>(value.tv_nsec);
    if (seconds > maximum / 1000000000ULL ||
        (seconds == maximum / 1000000000ULL && fraction > maximum % 1000000000ULL)) return false;
    ns = seconds * 1000000000ULL + fraction;
    return true;
#else
    return false;
#endif
}
inline Tracker& ThreadTracker() noexcept {
    static thread_local Tracker tracker(ReadThreadCPUTime);
    return tracker;
}
// Take copies before/after a window only outside RunLoop (all scopes closed).
// Reset only between sessions, likewise outside scopes; false means still active.
inline Counters Snapshot() noexcept { return ThreadTracker().Snapshot(); }
inline bool Reset() noexcept { return ThreadTracker().Reset(); }

class Scope {
public:
    explicit Scope(Category category) noexcept {
        // No clocks, TLS tracker access or allocation in the disabled path.
        if (!FrameTiming::counters.enabled || !DiagnosticEnabled()) return;
        // Others is the RunLoop root. Child entrypoints can also run on audio
        // or setup threads; outside a live root they do not read the clock.
        Tracker& tracker = ThreadTracker();
        if (category != Category::Others && !tracker.Active()) return;
        tracker_ = &tracker;
        previous_ = tracker_->Enter(category);
    }
    ~Scope() noexcept { if (tracker_) tracker_->Exit(previous_); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Tracker* tracker_ = nullptr;
    Category previous_ = Category::Inactive;
};
} // namespace Common::CPUCategoryTiming
