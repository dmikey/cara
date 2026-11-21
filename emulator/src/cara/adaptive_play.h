// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <chrono>
#include <utility>

#include "cara/adaptive_clock.h"

namespace Cara {

// Worker-owned policy. Display callbacks supply a host timestamp and never
// modify guest callbacks, clocks or renderer commands.
class AdaptivePlay {
public:
    void Configure(bool adaptive_cpu, bool frame_skip) {
        cpu_.SetEnabled(adaptive_cpu);
        frame_skip_ = frame_skip;
        ResetSamples();
    }

    void ResetPresentation() {
        seen_callback_ = false;
        callback_ = 0;
        same_callback_ = 0;
        present_work_ = 0;
    }

    void ResetSamples() {
        cpu_.ResetSamples();
        ResetPresentation();
    }

    bool AdmitPresentation(double callback, double now) {
        if (!frame_skip_)
            return true;
        if (!std::isfinite(callback) || !std::isfinite(now) || callback <= 0 || now < callback) {
            ResetPresentation();
            return true;
        }
        if (!seen_callback_ || callback != callback_) {
            seen_callback_ = true;
            callback_ = callback;
            same_callback_ = 0;
            return true;
        }
        // Leave normal 60 Hz delivery alone. Repeated opportunities on a
        // callback older than two expected display periods indicate that the
        // host has stopped keeping up. Deliver again on a fresh callback.
        same_callback_ = std::min(same_callback_ + 1, 2u);
        return same_callback_ < 2 || now - callback <= 2.0 / 60.0;
    }

    void SetFrameSkipEnabled(bool enabled) {
        frame_skip_ = enabled;
        ResetSamples();
    }

    bool FrameSkipEnabled() const { return frame_skip_; }
    bool CpuEnabled() const { return cpu_.Enabled(); }
    unsigned CpuPercent() const { return cpu_.Percent(); }

    bool MeasurePresentation() const { return frame_skip_ && CpuEnabled(); }

    void AddPresentationWork(double seconds) {
        if (MeasurePresentation() && std::isfinite(seconds) && seconds > 0)
            present_work_ += seconds;
    }

    void ObserveFrames(double work, double elapsed, unsigned advanced) {
        const double presentation = std::exchange(present_work_, 0.0);
        if (!CpuEnabled() || !advanced || !std::isfinite(work) || work < 0)
            return;
        if (advanced > 4) {
            ResetSamples();
            return;
        }
        // Host delivery already has its own lever. Only the remaining work
        // should drive guest CPU adaptation while that option is enabled.
        const double guest_work = std::max(0.0, work - presentation);
        for (unsigned i = 0; i < advanced; ++i)
            cpu_.ObserveFrame(guest_work / advanced, elapsed / advanced);
    }

private:
    AdaptiveClock cpu_;
    bool frame_skip_ = false;
    bool seen_callback_ = false;
    double callback_ = 0;
    unsigned same_callback_ = 0;
    double present_work_ = 0;
};

// Active only while the iOS emulation worker owns this controller. Only
// final host composition scopes use it; native scanout stays in guest work.
inline thread_local AdaptivePlay* active_adaptive_play = nullptr;

class ScopedAdaptivePlay {
public:
    explicit ScopedAdaptivePlay(AdaptivePlay& play)
        : previous_(std::exchange(active_adaptive_play, &play)) {}
    ~ScopedAdaptivePlay() { active_adaptive_play = previous_; }
    ScopedAdaptivePlay(const ScopedAdaptivePlay&) = delete;
    ScopedAdaptivePlay& operator=(const ScopedAdaptivePlay&) = delete;

private:
    AdaptivePlay* previous_;
};

class AdaptivePresentationWork {
public:
    AdaptivePresentationWork()
        : play_(active_adaptive_play && active_adaptive_play->MeasurePresentation()
                    ? active_adaptive_play : nullptr),
          started_(play_ ? Clock::now() : Clock::time_point{}) {}
    ~AdaptivePresentationWork() { Stop(); }
    AdaptivePresentationWork(const AdaptivePresentationWork&) = delete;
    AdaptivePresentationWork& operator=(const AdaptivePresentationWork&) = delete;

    void Stop() {
        if (auto* play = std::exchange(play_, nullptr)) {
            play->AddPresentationWork(std::chrono::duration<double>(Clock::now() - started_).count());
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    AdaptivePlay* play_;
    Clock::time_point started_;
};

} // namespace Cara
