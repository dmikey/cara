// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "cara/frame_pacer.h"

namespace Cara {
// Host submission spacing only. Never drops guest draws, events or presentations.
class PresentationGuard {
public:
    void Reset(double now) {
        Validate(now);
        last_observed_ = now;
        observed_ = true;
        submitted_ = false;
    }
    double ReadyAt(double now, bool enabled, unsigned speed_percent) {
        Observe(now);
        enabled_ = enabled;
        period_ = FramePacer(speed_percent).SecondsPerVBlank();
        if (!enabled_) submitted_ = false;
        return submitted_ ? std::max(now, NextStart()) : now;
    }
    // Call immediately BEFORE presentRenderbuffer, using a fresh host timestamp.
    // Recheck ReadyAt after sleep: an early wake must not start a submission.
    void MarkSubmitted(double actual_start) {
        Observe(actual_start);
        if (!enabled_) return;
        if (submitted_ && actual_start < NextStart())
            throw std::logic_error("PresentationGuard submission precedes deadline");
        last_start_ = actual_start;
        submitted_ = true;
    }
private:
    static void Validate(double now) {
        if (!std::isfinite(now))
            throw std::invalid_argument("PresentationGuard requires finite host seconds");
    }
    void Observe(double now) {
        Validate(now);
        if (!observed_ || now < last_observed_) Reset(now);
        last_observed_ = now;
    }
    double NextStart() const {
        const double next = last_start_ + period_;
        if (!std::isfinite(next) || next <= last_start_)
            throw std::overflow_error("PresentationGuard deadline is not representable");
        return next;
    }
    bool enabled_ = false, observed_ = false, submitted_ = false;
    double last_observed_ = 0, last_start_ = 0;
    double period_ = FramePacer{}.SecondsPerVBlank();
};
} // namespace Cara
