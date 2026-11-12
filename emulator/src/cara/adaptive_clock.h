// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>

namespace Cara {

// Host policy only: the frontend supplies work time without its pacing sleep.
// Peripheral clocks keep their native rate; this changes the guest CPU budget.
class AdaptiveClock {
public:
    static constexpr unsigned MinimumPercent = 50;
    static constexpr double RefreshRate = 59.833997376556916;

    void SetEnabled(bool value) {
        enabled = value;
        percent = 100;
        startup_seconds = 0;
        ResetSamples();
    }
    bool Enabled() const { return enabled; }
    unsigned Percent() const { return percent; }

    void ResetSamples() {
        elapsed_seconds = work_seconds = 0;
        frames = slow_windows = recovery_windows = 0;
    }

    unsigned ObserveFrame(double work, double elapsed) {
        if (!enabled || !std::isfinite(work) || !std::isfinite(elapsed) ||
            work < 0 || elapsed <= 0) return percent;
        if (startup_seconds < 2.0) {
            startup_seconds += elapsed;
            return percent;
        }
        elapsed_seconds += elapsed;
        work_seconds += work;
        ++frames;
        if (elapsed_seconds < 1.0 || frames < 12) return percent;

        const double utilization = work_seconds * RefreshRate / frames;
        elapsed_seconds = work_seconds = 0;
        frames = 0;
        if (utilization > 1.04) {
            recovery_windows = 0;
            if (++slow_windows >= 2) {
                const unsigned step = utilization > 1.2 ? 10 : 5;
                percent = std::max(MinimumPercent, percent - step);
                slow_windows = 1;
            }
        } else {
            slow_windows = 0;
            if (utilization < 0.85) {
                recovery_windows = std::min(3u, recovery_windows + 1);
                if (recovery_windows == 3) percent = std::min(100u, percent + 5);
            } else {
                recovery_windows = 0;
            }
        }
        return percent;
    }

private:
    bool enabled = false;
    unsigned percent = 100;
    unsigned frames = 0, slow_windows = 0, recovery_windows = 0;
    double startup_seconds = 0, elapsed_seconds = 0, work_seconds = 0;
};

} // namespace Cara
