// SPDX-License-Identifier: GPL-2.0-or-later
// ROM-free controls/delta tests for the actual native reporting header.
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include "cara/native_mac/cpu_category_profile.h"

namespace {
unsigned checks = 0;
void Require(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> void Reject(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, message);
}
bool Clock(void*, std::uint64_t& now) noexcept { now = 100; return true; }
NativeMac::CategoryEndpoint Before() {
    NativeMac::CategoryEndpoint a;
    a.frame = 130; a.ticks = 1000; a.global_ticks = 1100;
    a.wall_ns = 2000; a.thread_cpu_ns = 3000; a.thread_cpu_valid = true;
    for (std::size_t i = 0; i < a.categories.cpu_ns.size(); ++i) {
        a.categories.cpu_ns[i] = 10 + i;
        a.categories.entries[i] = 20 + i;
    }
    a.categories.clock_failures = 2; a.categories.counter_overflows = 3;
    return a;
}
NativeMac::CategoryEndpoint After() {
    auto b = Before();
    b.frame += 601; b.ticks += 17; b.global_ticks += 18;
    b.wall_ns += 100; b.thread_cpu_ns += 90;
    for (std::size_t i = 0; i < b.categories.cpu_ns.size(); ++i) {
        b.categories.cpu_ns[i] += 30 + i;
        b.categories.entries[i] += 40 + i;
    }
    b.categories.clock_failures += 4; b.categories.counter_overflows += 5;
    return b;
}
}
int main() {
    try {
        using namespace NativeMac;
        for (const auto text : {"1", "120", "720", "100000"})
            Require(ParseCategoryFrames(text) == std::stoul(text), "valid frame count changed");
        for (const auto text : {"", "0", "100001", "-1", "+1", "1x", " 1", "1 ",
                                "4294967296", "18446744073709551616"})
            Reject([&] { ParseCategoryFrames(text); }, "invalid frame count accepted");
        // Only the exact required gate combination is accepted. No profiler or
        // clocks are created here: normal-mode construction is tested by review.
        for (unsigned bits = 0; bits < 128; ++bits) {
            const bool accepted = bits == 15;
            bool actual = true;
            try {
                ValidateCategoryConfiguration(bits&1, bits&2, bits&4, bits&8,
                                              bits&16, bits&32, bits&64);
            } catch (const std::runtime_error&) { actual = false; }
            Require(actual == accepted, "category gate combination mismatch");
        }
        for (const auto bounds : {std::pair<unsigned,unsigned>{0,720}, {120,120},
                                   {720,120}, {120,100001}})
            Reject([&] { CategoryWindow(10, bounds.first, bounds.second); }, "invalid window accepted");
        CategoryWindow w(10,120,720);
        Require(w.Poll(10,1,2)==CategoryBoundary::None, "base produced boundary");
        Require(w.Poll(129,2,3)==CategoryBoundary::None, "early baseline");
        Require(w.Poll(130,1000,1100)==CategoryBoundary::Before, "exact baseline missed");
        Require(w.Poll(130,1000,1100)==CategoryBoundary::None, "duplicate frame recaptured baseline");
        Require(w.Poll(729,1010,1110)==CategoryBoundary::None, "early final boundary");
        Require(w.Poll(731,1017,1118)==CategoryBoundary::After, "overshoot final boundary missed");
        Require(w.Poll(732,1018,1119)==CategoryBoundary::None, "second final boundary");
        CategoryWindow overshoot(10,120,720);
        Require(overshoot.Poll(132,5,6)==CategoryBoundary::Before, "overshoot baseline missed");
        Require(overshoot.Poll(734,8,9)==CategoryBoundary::After, "overshoot end missed");
        Reject([] { CategoryWindow x(10,120,720); x.Poll(730,1,2); }, "missed baseline accepted");
        Reject([] { CategoryWindow x(10,120,720); x.Poll(9,1,2); }, "frame before base accepted");
        Reject([] { CategoryWindow x(10,120,720); x.Poll(129,1,2); x.Poll(128,2,3); }, "frame reset accepted");
        // RunLoopSingleCore advances the local CPU timer but can leave the
        // global scheduler timer at zero. This is an observer, not a clock fix.
        CategoryWindow single_core(10,120,720);
        Require(single_core.Poll(130,10,0)==CategoryBoundary::Before, "single-core baseline missed");
        Require(single_core.Poll(730,11,0)==CategoryBoundary::After, "flat global zero rejected");
        CategoryWindow flat_global(10,120,720);
        Require(flat_global.Poll(130,10,20)==CategoryBoundary::Before, "flat global baseline missed");
        Require(flat_global.Poll(730,11,20)==CategoryBoundary::After, "flat nonzero global rejected");
        for (unsigned which=0; which<3; ++which)
            Reject([&] { CategoryWindow x(10,120,720); x.Poll(130,10,20);
                x.Poll(730,which==0?10:which==1?9:11,which==2?19:20); },
                "nonpositive local or backward global interval accepted");
        const auto a=Before(), b=After();
        const auto delta=CategoryDelta(a,b);
        Require(b.frame-a.frame==601, "actual refresh delta normalized to requested count");
        for (std::size_t i=0; i<delta.cpu_ns.size(); ++i) {
            Require(delta.cpu_ns[i]==30+i, "category nanosecond delta wrong");
            Require(delta.entries[i]==40+i, "category entry delta wrong");
            auto reset=b; reset.categories.cpu_ns[i]=a.categories.cpu_ns[i]-1;
            Reject([&] { CategoryDelta(a,reset); }, "category time reset accepted");
            reset=b; reset.categories.entries[i]=a.categories.entries[i]-1;
            Reject([&] { CategoryDelta(a,reset); }, "category entry reset accepted");
        }
        Require(delta.clock_failures==4 && delta.counter_overflows==5, "error deltas hidden");
        auto single_a=a, single_b=b;
        single_a.global_ticks=0; single_b.global_ticks=0;
        Require(CategoryDelta(single_a,single_b).cpu_ns==delta.cpu_ns, "single-core zero global delta rejected");
        single_b.global_ticks=single_a.global_ticks=100;
        Require(CategoryDelta(single_a,single_b).entries==delta.entries, "flat nonzero global delta rejected");
        for (unsigned which=0; which<9; ++which) {
            auto bad=b;
            switch(which) {
            case 0: bad.frame=a.frame; break;
            case 1: bad.ticks=a.ticks; break;
            case 2: bad.global_ticks=a.global_ticks-1; break;
            case 3: bad.wall_ns=a.wall_ns; break;
            case 4: bad.thread_cpu_ns=a.thread_cpu_ns-1; break;
            case 5: bad.categories.clock_failures=a.categories.clock_failures-1; break;
            case 6: bad.categories.counter_overflows=a.categories.counter_overflows-1; break;
            case 7: bad.frame=a.frame-1; break;
            case 8: bad.ticks=a.ticks-1; break;
            }
            Reject([&] { CategoryDelta(a,bad); }, "invalid endpoint accepted");
        }
        auto unavailable=b; unavailable.thread_cpu_valid=false; unavailable.thread_cpu_ns=0;
        Require(CategoryDelta(a,unavailable).cpu_ns==delta.cpu_ns, "unavailable total CPU hid categories");
        auto stationary=b; stationary.thread_cpu_ns=a.thread_cpu_ns;
        Require(CategoryDelta(a,stationary).entries==delta.entries, "zero total CPU delta rejected");
        auto large_a=a, large_b=b;
        large_a.categories.cpu_ns[0]=std::numeric_limits<std::uint64_t>::max()-7;
        large_b.categories.cpu_ns[0]=std::numeric_limits<std::uint64_t>::max();
        Require(CategoryDelta(large_a,large_b).cpu_ns[0]==7, "large unsigned delta wrapped");
        Common::CPUCategoryTiming::Tracker tracker(Clock);
        RequireInactiveCategoryTracker(tracker);
        const auto old=tracker.Enter(Common::CPUCategoryTiming::Category::Others);
        Reject([&] { RequireInactiveCategoryTracker(tracker); }, "active tracker endpoint accepted");
        tracker.Exit(old); RequireInactiveCategoryTracker(tracker);
        std::cout << "Native CPU category controls: " << checks << " checks passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
