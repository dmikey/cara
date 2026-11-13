// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace Cara {
// Pure scheduling policy. Supply monotonic host seconds; the caller sleeps until
// the returned absolute deadline. This never changes the emulated CPU clock.
class FramePacer {
public:
    static constexpr unsigned DefaultSpeedPercent = 100;
    static constexpr unsigned MinSpeedPercent = 50;
    static constexpr unsigned MaxSpeedPercent = 200;
    static constexpr double VBlankHz = 59.833997376556916;

    explicit FramePacer(unsigned speed_percent = DefaultSpeedPercent)
        : speed_percent_(ClampSpeedPercent(speed_percent)) {}

    static constexpr unsigned ClampSpeedPercent(unsigned percent) {
        return std::clamp(percent, MinSpeedPercent, MaxSpeedPercent);
    }
    unsigned SpeedPercent() const { return speed_percent_; }
    double SecondsPerVBlank() const {
        return (1.0 / VBlankHz) * (100.0 / speed_percent_);
    }

    // Call before running again after pause, checkpoint load, or discontinuity.
    void Reset(double now_seconds) {
        ValidateTime(now_seconds);
        deadline_seconds_ = last_now_seconds_ = now_seconds;
        initialized_ = true;
    }

    void SetSpeedPercent(unsigned percent, double now_seconds) {
        ValidateTime(now_seconds);
        percent = ClampSpeedPercent(percent);
        if (percent != speed_percent_) {
            speed_percent_ = percent;
            Reset(now_seconds);
        }
    }

    // Pass the TOTAL number of vblanks advanced since the preceding call, not
    // one per frontend iteration. Late work discards debt instead of allowing
    // subsequent fast iterations to burst through overdue guest frames.
    double Advance(std::uint64_t advanced_vblanks, double now_seconds) {
        ValidateTime(now_seconds);
        if (!initialized_ || now_seconds < last_now_seconds_) {
            Reset(now_seconds);
        }
        // Floating multiplication avoids integer overflow even at UINT64_MAX.
        // Accumulate in long double where the host provides extra precision.
        const long double candidate = static_cast<long double>(deadline_seconds_) +
            static_cast<long double>(advanced_vblanks) * SecondsPerVBlank();
        if (candidate > std::numeric_limits<double>::max()) {
            // A nonrepresentable host deadline cannot safely be slept until.
            // Discard the invalid timeline rather than returning Inf/overflow.
            Reset(now_seconds);
            return deadline_seconds_;
        }
        deadline_seconds_ = std::max(static_cast<double>(candidate), now_seconds);
        last_now_seconds_ = now_seconds;
        return deadline_seconds_;
    }

private:
    static void ValidateTime(double now_seconds) {
        if (!std::isfinite(now_seconds)) {
            throw std::invalid_argument("FramePacer requires finite monotonic host seconds");
        }
    }
    unsigned speed_percent_;
    bool initialized_ = false;
    double deadline_seconds_ = 0;
    double last_now_seconds_ = 0;
};
} // namespace Cara
