// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "cara/frame_pacer.h"

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Near(double actual, double expected, double tolerance, const char* message) {
    Require(std::abs(actual - expected) <= tolerance, message);
}
}

int main() {
    try {
        Cara::FramePacer pacer;
        Require(pacer.SpeedPercent() == 100, "Default is not 100 percent");
        const double period = (1.0 / 59.833997376556916) * (100.0 / 100.0);
        pacer.Reset(10);
        Near(pacer.Advance(3, 10.002), 10 + 3 * period, 1e-12,
             "Multiple vblanks counted as one frontend iteration");
        Near(pacer.Advance(0, 10.003), 10 + 3 * period, 1e-12,
             "Zero advanced frames introduced a new budget");
        // A slow frame cannot leave debt that fast following frames catch up.
        Near(pacer.Advance(1, 11), 11, 1e-12, "Slow frame did not discard debt");
        Near(pacer.Advance(1, 11.001), 11 + period, 1e-12,
             "Frame after slow work burst through old debt");
        pacer.Reset(3600); // Pause/load: old future deadline must also disappear.
        Near(pacer.Advance(2, 3600.001), 3600 + 2 * period, 1e-10,
             "Reset retained old timeline");
        const unsigned changed_speed=pacer.SpeedPercent()==100?200:100;
        const double changed_period=(1.0 / Cara::FramePacer::VBlankHz)*(100.0/changed_speed);
        pacer.SetSpeedPercent(changed_speed, 3601);
        Near(pacer.Advance(1, 3601.001), 3601 + changed_period,
             1e-10, "Live speed change retained previous deadline");
        const double before = pacer.Advance(1, 3601.002);
        pacer.SetSpeedPercent(changed_speed, 3601.003);
        Near(pacer.Advance(0, 3601.004), before, 1e-10,
             "Unchanged speed unexpectedly reset schedule");
        Near(pacer.Advance(1, 3), 3 + changed_period,
             1e-12, "Backward clock retained stale future deadline");
        Require(Cara::FramePacer(0).SpeedPercent() == 50, "Lower clamp failed");
        Require(Cara::FramePacer(999).SpeedPercent() == 200, "Upper clamp failed");
        for (unsigned percent : {100u, 105u, 200u}) {
            Cara::FramePacer run(percent);
            double now = 0;
            run.Reset(now);
            std::uint64_t total = 0;
            for (unsigned iteration = 0; iteration < 200000; ++iteration) {
                const std::uint64_t frames = iteration % 7 + 1;
                total += frames;
                // Fake work takes a quarter of this iteration's frame budget.
                // Advancing the fake clock to the deadline models sleeping.
                now = run.Advance(frames, now + frames * run.SecondsPerVBlank() * .25);
            }
            const double expected = static_cast<double>(total) /
                59.833997376556916 * (100.0 / percent);
            Near(now, expected, 2e-7, "Long-run speed budget drifted");
        }
        pacer.Reset(0);
        Require(std::isfinite(pacer.Advance(std::numeric_limits<std::uint64_t>::max(), 0)),
                "Large frame count overflowed");
        pacer.Reset(std::numeric_limits<double>::max());
        Require(std::isfinite(pacer.Advance(std::numeric_limits<std::uint64_t>::max(),
                                           std::numeric_limits<double>::max())),
                "Large absolute time overflowed");
        for (double invalid : {std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
            bool rejected = false;
            try { pacer.Advance(1, invalid); }
            catch (const std::invalid_argument&) { rejected = true; }
            Require(rejected, "Nonfinite clock accepted");
        }
        std::cout << "Frame pacing: full vblank counts, debt discard, reset, speed changes, "
                     "bounds, backward/invalid time and long-run budgets passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
