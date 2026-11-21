// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include "core.h"
#include "core/hw/gpu.h"
#include "core/perf_stats.h"
#include "core/settings.h"

using namespace std::chrono_literals;
using DoubleSecs = std::chrono::duration<double, std::chrono::seconds::period>;
using std::chrono::duration_cast;
using std::chrono::microseconds;

namespace Core {

void PerfStats::BeginSystemFrame() {
    frame_pacing_sleep =
        frame_limiter.DoFrameLimiting(System::GetInstance().CoreTiming().GetGlobalTimeUs());
}

void PerfStats::EndSystemFrame() {
    auto frame_end = Clock::now();
    previous_frame_length = frame_end - previous_frame_end;
    previous_frame_end = frame_end;
    previous_frame_pacing_sleep = frame_pacing_sleep;
    system_frames += 1;
}

void PerfStats::EndGameFrame() {
    game_frames += 1;
}

PerfStats::Results PerfStats::GetAndResetStats(microseconds current_system_time_us) {
    const auto now = Clock::now();
    // Walltime elapsed since stats were reset
    const auto interval = duration_cast<DoubleSecs>(now - reset_point).count();
    const auto system_us_per_second = (current_system_time_us - reset_point_system_us) / interval;

    Results results{};
    results.system_fps = static_cast<double>(system_frames) / interval;
    results.game_fps = static_cast<double>(game_frames) / interval;
    results.emulation_speed = system_us_per_second.count() / 1'000'000.0;

    // Reset counters
    reset_point = now;
    reset_point_system_us = current_system_time_us;
    system_frames = 0;
    game_frames = 0;

    return results;
}

double PerfStats::GetLastFrameTimeScale() const {
    return duration_cast<DoubleSecs>(previous_frame_length).count() * GPU::SCREEN_REFRESH_RATE;
}

PerfStats::FrameTiming PerfStats::GetLastFrameTiming() const {
    return {std::max(Clock::duration::zero(), previous_frame_length - previous_frame_pacing_sleep),
            previous_frame_length};
}

FrameLimiter::Clock::duration FrameLimiter::DoFrameLimiting(microseconds current_system_time_us) {
    if (!Settings::values.use_frame_limit) {
        return Clock::duration::zero();
    }

    auto now = Clock::now();
    const double sleep_scale = Settings::values.frame_limit / 100.0;

    // Max lag caused by slow frames. Shouldn't be more than the length of a frame at the current
    // speed percent or it will clamp too much and prevent this from properly limiting to that
    // percent. High values means it'll take longer after a slow frame to recover and start limiting
    const auto max_lag_time_us = duration_cast<microseconds>(DoubleSecs(25ms / sleep_scale));
    frame_limiting_delta_err += duration_cast<microseconds>(
        DoubleSecs((current_system_time_us - previous_system_time_us) / sleep_scale));
    frame_limiting_delta_err -= duration_cast<microseconds>(now - previous_walltime);
    frame_limiting_delta_err =
        std::clamp(frame_limiting_delta_err, -max_lag_time_us, max_lag_time_us);

    auto pacing_sleep = Clock::duration::zero();
    if (frame_limiting_delta_err > microseconds::zero()) {
        std::this_thread::sleep_for(frame_limiting_delta_err);
        auto now_after_sleep = Clock::now();
        pacing_sleep = now_after_sleep - now;
        frame_limiting_delta_err -= duration_cast<microseconds>(pacing_sleep);
        now = now_after_sleep;
    }

    previous_system_time_us = current_system_time_us;
    previous_walltime = now;
    return pacing_sleep;
}

} // namespace Core
