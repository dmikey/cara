// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <cstring>
#include <thread>
#include "common/cpu_category_timing.h"

using namespace Common::CPUCategoryTiming;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::uint64_t Value(const Counters& counts, Category category) { return counts.cpu_ns[static_cast<std::size_t>(category)]; }
std::uint64_t Sum(const Counters& counts) { std::uint64_t sum = 0; for (auto value : counts.cpu_ns) sum += value; return sum; }
struct Clock {
    std::uint64_t now = 0;
    bool valid = true;
    unsigned reads = 0;
    static bool Read(void* opaque, std::uint64_t& value) noexcept {
        auto& clock = *static_cast<Clock*>(opaque); ++clock.reads; value = clock.now; return clock.valid;
    }
};
}
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::strcmp(argv[1], "--disabled") == 0) {
            Require(!DiagnosticEnabled(), "Disabled test requires CARA_CPU_CATEGORY_TIMING=0");
            Common::FrameTiming::counters.enabled = true;
            { Scope root(Category::Others); Scope cpu(Category::CPUExecution); }
            Require(Snapshot().entries[0] == 0 && Sum(Snapshot()) == 0 &&
                    Snapshot().clock_failures == 0, "Disabled diagnostic changed counters");
            std::cout << "Diagnostic env-disabled scopes are inert\n";
            return 0;
        }
        if (argc > 1 && std::strcmp(argv[1], "--wrapper") == 0) {
            Require(DiagnosticEnabled(), "Wrapper test requires CARA_CPU_CATEGORY_TIMING=1");
            Common::FrameTiming::counters.enabled = true;
            Require(Reset(), "Wrapper reset failed");
            { Scope orphan(Category::GPUCommand); }
            Require(Sum(Snapshot()) == 0 && Snapshot().entries[3] == 0,
                    "Child outside RunLoop root charged CPU");
            {
                Scope root(Category::Others);
                Scope cpu(Category::CPUExecution);
                bool isolated = false;
                std::thread worker([&] {
                    Common::FrameTiming::counters.enabled = true;
                    { Scope orphan(Category::GPUCommand); }
                    isolated = Snapshot().entries[3] == 0 && Sum(Snapshot()) == 0;
                });
                worker.join();
                Require(isolated, "Worker without root charged counters");
            }
            const auto before = Snapshot();
#if defined(CLOCK_THREAD_CPUTIME_ID)
            Require(before.clock_failures == 0 && Sum(before) > 0, "Production thread CPU clock failed");
#endif
            { Scope orphan(Category::HLE); }
            Require(Snapshot().entries == before.entries, "Child after root charged counters");
            Require(before.entries[0] == 1 && before.entries[1] == 1,
                    "Owned root/CPU scope not recorded");
            Require(Reset(), "Wrapper reset while inactive failed");
            Common::FrameTiming::counters.enabled = false;
            { Scope root(Category::Others); Scope cpu(Category::CPUExecution); }
            Require(Snapshot().entries[0] == 0, "Disabled FrameTiming charged categories");
            std::cout << "Production TLS owner/root gating and disabled path passed\n";
            return 0;
        }

        Clock clock;
        Tracker tracker(Clock::Read, &clock);
        clock.now = 1000;
        auto root = tracker.Enter(Category::Others);
        Require(!tracker.Reset(), "Reset accepted with active parent");
        clock.now = 1010; auto cpu = tracker.Enter(Category::CPUExecution);
        clock.now = 1030; auto hle = tracker.Enter(Category::HLE);
        clock.now = 1035; auto gpu = tracker.Enter(Category::GPUCommand);
        clock.now = 1047; tracker.Exit(gpu);
        clock.now = 1050; tracker.Exit(hle);
        clock.now = 1060; tracker.Exit(cpu);
        clock.now = 1063; auto dsp = tracker.Enter(Category::DSPHLETick);
        clock.now = 1070; tracker.Exit(dsp);
        clock.now = 1080; tracker.Exit(root);
        const auto a = tracker.Snapshot();
        Require(Value(a, Category::Others) == 23 && Value(a, Category::CPUExecution) == 30 &&
                Value(a, Category::HLE) == 8 && Value(a, Category::GPUCommand) == 12 &&
                Value(a, Category::DSPHLETick) == 7, "Nested priority/restoration wrong");
        Require(Sum(a) == 80 && a.clock_failures == 0, "Nested categories double counted");
        // Nothing between independent roots is charged, even if CPU time advances.
        clock.now = 50000; auto isolated = tracker.Enter(Category::GPUCommand);
        clock.now = 50009; tracker.Exit(isolated);
        Require(Sum(tracker.Snapshot()) == 89, "Inactive gap charged to next scope");
        Require(tracker.Reset(), "Inactive reset failed");
        clock.now = 100; root = tracker.Enter(Category::Others);
        clock.now = 110; cpu = tracker.Enter(Category::CPUExecution);
        clock.valid = false; hle = tracker.Enter(Category::HLE);
        clock.valid = true; clock.now = 130; tracker.Exit(hle);
        clock.now = 140; tracker.Exit(cpu);
        clock.now = 150; tracker.Exit(root);
        const auto failed = tracker.Snapshot();
        Require(Sum(failed) == 30 && failed.clock_failures == 1,
                "Clock failure charged unknown interval or underflowed");
        Require(tracker.Reset(), "Second reset failed");
        clock.now = 200; root = tracker.Enter(Category::Others);
        clock.now = 190; cpu = tracker.Enter(Category::CPUExecution);
        clock.now = 195; tracker.Exit(cpu);
        clock.now = 205; tracker.Exit(root);
        Require(Sum(tracker.Snapshot()) == 15 && tracker.Snapshot().clock_failures == 1,
                "Backward clock underflowed");
        Require(tracker.Reset(), "Third reset failed");
        clock.valid = false; root = tracker.Enter(Category::Others);
        clock.valid = true; clock.now = 1000; tracker.Exit(root);
        Require(Sum(tracker.Snapshot()) == 0 && tracker.Snapshot().clock_failures == 1,
                "Initial failed clock treated as time zero");
        tracker.Reset();
        clock.now = 0; root = tracker.Enter(Category::Others);
        clock.now = std::numeric_limits<std::uint64_t>::max(); tracker.Exit(root);
        clock.now = 0; root = tracker.Enter(Category::Others);
        clock.now = 1; tracker.Exit(root);
        Require(Value(tracker.Snapshot(), Category::Others) == std::numeric_limits<std::uint64_t>::max() &&
                tracker.Snapshot().counter_overflows == 1, "Counter overflow wrapped");
        // Same-category recursion still partitions time and restores its parent.
        tracker.Reset(); clock.now = 0; root = tracker.Enter(Category::CPUExecution);
        clock.now = 4; cpu = tracker.Enter(Category::CPUExecution);
        clock.now = 8; tracker.Exit(cpu); clock.now = 10; tracker.Exit(root);
        Require(Sum(tracker.Snapshot()) == 10 && tracker.Snapshot().entries[1] == 2,
                "Same-category recursion double counted");
        std::cout << "Exclusive CPU categories: nested restoration, idle gaps, initial inactivity, "
                     "failed/backward clocks, overflow and recursion passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
