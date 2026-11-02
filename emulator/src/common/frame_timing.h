// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace Common::FrameTiming {

// Opt-in diagnostics on the emulation thread. No clocks are read during normal play.
struct Counters {
    bool enabled = false;
    double shader_seconds = 0;
    double guest_cpu_seconds = 0;
    double draw_seconds = 0;
    double vertex_prepare_seconds = 0;
    double vertex_upload_seconds = 0;
    double draw_submit_seconds = 0;
    std::uint64_t guest_ticks = 0;
    double readback_seconds = 0;
    double present_seconds = 0;
    std::uint64_t shader_calls = 0;
    std::uint64_t gpu_vertex_draws = 0;
    std::uint64_t gpu_vertices = 0;
    std::uint64_t cpu_vertex_draws = 0;
};
inline thread_local Counters counters;

enum class Phase { Shader, Readback, Present, GuestCPU, Draw, VertexPrepare, VertexUpload, DrawSubmit };
class Scope {
public:
    explicit Scope(Phase phase_) : phase(phase_), active(counters.enabled) {
        if (active) started = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (!active) return;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (phase == Phase::Shader) {
            counters.shader_seconds += seconds;
            ++counters.shader_calls;
        } else if (phase == Phase::Readback) {
            counters.readback_seconds += seconds;
        } else if (phase == Phase::GuestCPU) {
            counters.guest_cpu_seconds += seconds;
        } else if (phase == Phase::Draw) {
            counters.draw_seconds += seconds;
        } else if (phase == Phase::VertexPrepare) {
            counters.vertex_prepare_seconds += seconds;
        } else if (phase == Phase::VertexUpload) {
            counters.vertex_upload_seconds += seconds;
        } else if (phase == Phase::DrawSubmit) {
            counters.draw_submit_seconds += seconds;
        } else {
            counters.present_seconds += seconds;
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Phase phase;
    bool active;
    std::chrono::steady_clock::time_point started;
};
} // namespace Common::FrameTiming
