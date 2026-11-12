// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Common::CPUCategoryTiming {
// CPUExecution includes emulated RAM/MMIO accesses and backend/cache/decode work.
// It is not a measurement of pure dispatch. HLE and GPUCommand subtract nested
// work from their parents; GPUCommand includes host renderer calls it invokes.
enum class Category : std::size_t { Others, CPUExecution, HLE, GPUCommand, DSPHLETick, Count, Inactive = Count };
inline constexpr std::size_t CategoryCount = static_cast<std::size_t>(Category::Count);
inline constexpr std::array<const char*, CategoryCount> Names = {
    "others", "cpu_execution_including_memory", "hle_svc", "gpu_command", "dsp_hle_tick"};

struct Counters {
    std::array<std::uint64_t, CategoryCount> cpu_ns{};
    std::array<std::uint64_t, CategoryCount> entries{};
    std::uint64_t clock_failures = 0; // Failed or backward samples, never subtracted unsigned.
    std::uint64_t counter_overflows = 0;
};

// Inject a clock in ROM-free tests. No allocation, wall clock, sleeping or guest
// state is involved. Enter/Exit must be properly nested on one host thread.
class Tracker {
public:
    using Clock = bool (*)(void*, std::uint64_t&) noexcept;
    explicit Tracker(Clock clock, void* context = nullptr) noexcept : clock_(clock), context_(context) {}

    Category Enter(Category category) noexcept {
        const Category previous = current_;
        if (category == Category::Inactive) return previous;
        Transition(category);
        Add(counters_.entries[static_cast<std::size_t>(category)], 1);
        return previous;
    }
    void Exit(Category previous) noexcept { Transition(previous); }
    const Counters& Snapshot() const noexcept { return counters_; }
    bool Reset() noexcept {
        if (current_ != Category::Inactive) return false;
        counters_ = {};
        sample_valid_ = false;
        return true;
    }
    bool Active() const noexcept { return current_ != Category::Inactive; }

private:
    void Add(std::uint64_t& counter, std::uint64_t delta) noexcept {
        if (delta > std::numeric_limits<std::uint64_t>::max() - counter) {
            counter = std::numeric_limits<std::uint64_t>::max();
            if (counters_.counter_overflows != std::numeric_limits<std::uint64_t>::max())
                ++counters_.counter_overflows;
        } else counter += delta;
    }
    void Failure() noexcept {
        if (counters_.clock_failures != std::numeric_limits<std::uint64_t>::max())
            ++counters_.clock_failures;
    }
    void Transition(Category next) noexcept {
        std::uint64_t now = 0;
        const bool ok = clock_ && clock_(context_, now);
        if (!ok) {
            Failure();
        } else if (current_ != Category::Inactive && sample_valid_) {
            if (now >= last_ns_) Add(counters_.cpu_ns[static_cast<std::size_t>(current_)], now - last_ns_);
            else Failure();
        }
        current_ = next;
        // Inactive gaps must never be charged to the next root or child scope.
        sample_valid_ = ok && next != Category::Inactive;
        last_ns_ = now;
    }
    Clock clock_;
    void* context_;
    Counters counters_{};
    Category current_ = Category::Inactive;
    std::uint64_t last_ns_ = 0;
    bool sample_valid_ = false;
};
} // namespace Common::CPUCategoryTiming
