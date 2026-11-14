// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in native attribution only. No guest timing, input or common profiler changes.
#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include "common/cpu_category_timing.h"

namespace NativeMac {
inline bool CategoryFlag(const char* name) {
    const auto* value = std::getenv(name);
    return value && std::string_view(value) == "1";
}
inline std::uint32_t ParseCategoryFrames(std::string_view value) {
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !result || result > 100000)
        throw std::runtime_error("Native category frame count must be 1..100000");
    return result;
}
inline std::uint32_t CategoryFrames(const char* name, std::uint32_t fallback) {
    const auto* value = std::getenv(name);
    return value ? ParseCategoryFrames(value) : fallback;
}
inline void ValidateCategoryConfiguration(bool diagnostics, bool snapshots, bool counters,
                                         bool categories, bool sampled_phase_cpu,
                                         bool command_profile, bool competing_controls) {
    if (!diagnostics || !snapshots || !counters || !categories || sampled_phase_cpu ||
        command_profile || competing_controls)
        throw std::runtime_error("Native category profile requires diagnostics, snapshots, timing "
            "counters and CPU categories ON; sampled phase CPU, PICA command profile and other "
            "benchmark/input/capture controls must be OFF");
}

enum class CategoryBoundary { None, Before, After };
class CategoryWindow {
public:
    CategoryWindow(std::uint32_t base, std::uint32_t warmup, std::uint32_t end)
        : base_(base), warmup_(warmup), end_(end), previous_(base) {
        if (!warmup || warmup >= end || end > 100000)
            throw std::runtime_error("Native category window requires 0 < warmup < end <= 100000");
    }
    CategoryBoundary Poll(std::uint32_t frame, std::uint64_t ticks, std::uint64_t global_ticks) {
        if (frame < previous_ || frame < base_)
            throw std::runtime_error("Native category frame counter moved backward");
        previous_ = frame;
        if (finished_) return CategoryBoundary::None;
        const auto relative = frame - base_;
        if (!before_frame_) {
            if (relative >= end_)
                throw std::runtime_error("Native category run missed the warmup boundary");
            if (relative < warmup_) return CategoryBoundary::None;
            before_frame_ = frame; before_ticks_ = ticks; before_global_ticks_ = global_ticks;
            return CategoryBoundary::Before;
        }
        if (relative < end_) return CategoryBoundary::None;
        if (frame <= *before_frame_ || ticks <= before_ticks_ || global_ticks <= before_global_ticks_)
            throw std::runtime_error("Native category interval has no positive frame/tick advance");
        finished_ = true;
        return CategoryBoundary::After;
    }
private:
    std::uint32_t base_, warmup_, end_, previous_;
    std::optional<std::uint32_t> before_frame_;
    std::uint64_t before_ticks_ = 0, before_global_ticks_ = 0;
    bool finished_ = false;
};

struct CategoryEndpoint {
    std::uint32_t frame = 0;
    std::uint64_t ticks = 0, global_ticks = 0, wall_ns = 0, thread_cpu_ns = 0;
    bool thread_cpu_valid = false;
    Common::CPUCategoryTiming::Counters categories{};
};
inline Common::CPUCategoryTiming::Counters CategoryDelta(const CategoryEndpoint& before,
                                                        const CategoryEndpoint& after) {
    if (after.frame <= before.frame || after.ticks <= before.ticks ||
        after.global_ticks <= before.global_ticks || after.wall_ns <= before.wall_ns ||
        (before.thread_cpu_valid && after.thread_cpu_valid &&
         after.thread_cpu_ns < before.thread_cpu_ns))
        throw std::runtime_error("Native category endpoints are not a positive monotonic interval");
    const auto& a = before.categories;
    const auto& b = after.categories;
    Common::CPUCategoryTiming::Counters delta;
    for (std::size_t i = 0; i < delta.cpu_ns.size(); ++i) {
        if (b.cpu_ns[i] < a.cpu_ns[i] || b.entries[i] < a.entries[i])
            throw std::runtime_error("Native category counters reset inside measured interval");
        delta.cpu_ns[i] = b.cpu_ns[i] - a.cpu_ns[i];
        delta.entries[i] = b.entries[i] - a.entries[i];
    }
    if (b.clock_failures < a.clock_failures || b.counter_overflows < a.counter_overflows)
        throw std::runtime_error("Native category error counters reset inside measured interval");
    delta.clock_failures = b.clock_failures - a.clock_failures;
    delta.counter_overflows = b.counter_overflows - a.counter_overflows;
    return delta;
}
inline void RequireInactiveCategoryTracker(const Common::CPUCategoryTiming::Tracker& tracker) {
    if (tracker.Active())
        throw std::runtime_error("Native category endpoint requires closed RunLoop scopes");
}

class CPUCategoryProfiler {
public:
    CPUCategoryProfiler(std::string directory, std::string fingerprint, std::uint32_t base)
        : directory_(std::move(directory)), fingerprint_(std::move(fingerprint)), base_(base),
          warmup_(CategoryFrames("CARA_NATIVE_CPU_CATEGORY_WARMUP", 120)),
          end_(CategoryFrames("CARA_NATIVE_CPU_CATEGORY_FRAMES", 720)),
          window_(base_, warmup_, end_) {
        if (fingerprint_.size() != 64 || !std::all_of(fingerprint_.begin(), fingerprint_.end(),
                [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
            throw std::runtime_error("Native category report requires the generated engine fingerprint");
        bool competing = CategoryFlag("CARA_NATIVE_BENCHMARK");
        for (const char* name : {"CARA_NATIVE_CIRCLE_SEGMENTS", "CARA_NATIVE_PRESS_A_FRAMES",
                                "CARA_NATIVE_PRESS_START_FRAMES", "CARA_NATIVE_CAPTURE_FRAMES"}) {
            const auto* value = std::getenv(name);
            competing |= value && *value;
        }
        ValidateCategoryConfiguration(CategoryFlag("CARA_NATIVE_DIAGNOSTICS"),
            CategoryFlag("CARA_NATIVE_PROFILE_SNAPSHOT"), Common::FrameTiming::counters.enabled,
            Common::CPUCategoryTiming::DiagnosticEnabled(), CategoryFlag("CARA_PHASE_CPU_TIMING"),
            CategoryFlag("CARA_PICA_COMMAND_PROFILE"), competing);
        RequireInactiveCategoryTracker(Common::CPUCategoryTiming::ThreadTracker());
        if (!Common::CPUCategoryTiming::Reset())
            throw std::runtime_error("Native category tracker could not reset outside RunLoop");
        // Before warmup: report this call-cost estimate, never subtract it from
        // measurements. It does not quantify all instrumentation overhead.
        std::uint64_t begin = 0, end = 0, value = 0;
        const bool first = Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr, begin);
        for (unsigned i = 0; i < 4096; ++i)
            calibration_failures_ += !Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr, value);
        const bool last = Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr, end);
        calibration_failures_ += !first;
        calibration_failures_ += !last;
        calibrated_ = !calibration_failures_ && end >= begin;
        if (calibrated_) ns_per_clock_read_ = double(end - begin) / 4097;
    }
    bool Observe(std::uint32_t frame, std::uint64_t ticks, std::uint64_t global_ticks) {
        RequireInactiveCategoryTracker(Common::CPUCategoryTiming::ThreadTracker());
        const auto boundary = window_.Poll(frame, ticks, global_ticks);
        if (boundary == CategoryBoundary::None) return false;
        auto endpoint = Capture(frame, ticks, global_ticks);
        if (boundary == CategoryBoundary::Before) {
            before_ = endpoint;
            return false;
        }
        if (!before_) throw std::runtime_error("Native category end has no baseline");
        const auto delta = CategoryDelta(*before_, endpoint);
        // All file I/O follows the final sample. Baseline I/O cannot contaminate
        // the interval. Endpoint snapshots do not mutate emulated state.
        WriteEndpoint("before", *before_);
        WriteEndpoint("after", endpoint);
        WriteSummary(*before_, endpoint, delta);
        return true;
    }
private:
    static CategoryEndpoint Capture(std::uint32_t frame, std::uint64_t ticks,
                                    std::uint64_t global_ticks) {
        RequireInactiveCategoryTracker(Common::CPUCategoryTiming::ThreadTracker());
        CategoryEndpoint result;
        result.frame = frame; result.ticks = ticks; result.global_ticks = global_ticks;
        result.categories = Common::CPUCategoryTiming::Snapshot();
        result.thread_cpu_valid = Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr,
                                                                               result.thread_cpu_ns);
        result.wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return result;
    }
    static void WriteCounters(std::ostream& out, const Common::CPUCategoryTiming::Counters& c) {
        out << "\"cpu_category_names\":[";
        for (std::size_t i = 0; i < c.cpu_ns.size(); ++i)
            out << (i ? "," : "") << '\"' << Common::CPUCategoryTiming::Names[i] << '\"';
        out << "],\"cpu_category_ns\":[";
        for (std::size_t i = 0; i < c.cpu_ns.size(); ++i) out << (i ? "," : "") << c.cpu_ns[i];
        out << "],\"cpu_category_entries\":[";
        for (std::size_t i = 0; i < c.entries.size(); ++i) out << (i ? "," : "") << c.entries[i];
        out << "],\"cpu_category_clock_failures\":" << c.clock_failures
            << ",\"cpu_category_counter_overflows\":" << c.counter_overflows;
    }
    std::ofstream Open(const char* suffix) const {
        std::ofstream out(directory_ + "native-cpu-category-" + suffix + ".json");
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << std::boolalpha << std::setprecision(17);
        return out;
    }
    void WriteEndpoint(const char* suffix, const CategoryEndpoint& e) const {
        auto out = Open(suffix);
        out << "{\"kind\":\"native_exclusive_thread_cpu_category_endpoint\",\"source_fingerprint\":\""
            << fingerprint_ << "\",\"frame\":" << e.frame << ",\"current_timer_ticks\":" << e.ticks
            << ",\"global_ticks\":" << e.global_ticks << ",\"steady_wall_ns\":" << e.wall_ns
            << ",\"thread_cpu_ns\":";
        if (e.thread_cpu_valid) out << e.thread_cpu_ns; else out << "null";
        out << ",\"tracker_active\":false,";
        WriteCounters(out, e.categories);
        out << "}\n";
        out.flush();
    }
    void WriteSummary(const CategoryEndpoint& a, const CategoryEndpoint& b,
                      const Common::CPUCategoryTiming::Counters& delta) const {
        auto out = Open("summary");
        out << "{\"kind\":\"native_exclusive_thread_cpu_category_interval\",\"source_fingerprint\":\""
            << fingerprint_ << "\",\"base_frame\":" << base_ << ",\"requested_warmup_refreshes\":"
            << warmup_ << ",\"requested_end_refresh\":" << end_
            << ",\"requested_measured_refreshes\":" << end_ - warmup_
            << ",\"before_frame\":" << a.frame << ",\"after_frame\":" << b.frame
            << ",\"actual_measured_refreshes\":" << b.frame - a.frame
            << ",\"current_timer_tick_delta\":" << b.ticks - a.ticks
            << ",\"global_tick_delta\":" << b.global_ticks - a.global_ticks
            << ",\"wall_ns\":" << b.wall_ns - a.wall_ns << ",\"thread_cpu_ns\":";
        if (a.thread_cpu_valid && b.thread_cpu_valid) out << b.thread_cpu_ns - a.thread_cpu_ns;
        else out << "null";
        out << ",\"category_clock_calibrated\":" << calibrated_
            << ",\"category_calibration_reads\":4098,\"category_calibration_failures\":"
            << calibration_failures_ << ",\"category_clock_ns_per_read\":";
        if (calibrated_) out << ns_per_clock_read_; else out << "null";
        out << ",\"common_phase_wall_clocks\":true,\"sampled_phase_cpu_clocks\":false"
            << ",\"pica_command_profile\":false,\"scripted_input\":false"
            << ",\"category_counters_valid\":" << (!delta.clock_failures && !b.categories.counter_overflows)
            << ",\"observer_note\":\"Instrumentation is enabled; category scopes add two thread-CPU "
               "clock reads per entry. Common phase wall clocks remain enabled. Calibration is a "
               "call-cost estimate, not total overhead, and is not subtracted. Scope totals exclude "
               "host work outside RunLoop and other threads; report IO follows the final sample.\",";
        WriteCounters(out, delta);
        out << "}\n";
        out.flush();
    }
    std::string directory_, fingerprint_;
    std::uint32_t base_, warmup_, end_;
    CategoryWindow window_;
    std::optional<CategoryEndpoint> before_;
    bool calibrated_ = false;
    unsigned calibration_failures_ = 0;
    double ns_per_clock_read_ = 0;
};
} // namespace NativeMac
