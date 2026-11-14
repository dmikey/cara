// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>
#include "cara/guest_gpu_trace_buffer.h"
namespace Cara::GuestGPUTrace {
inline Buffer<32768> buffer;
inline std::atomic<bool> active{false};
inline std::atomic<std::uint64_t> foreign_events{0};
inline std::atomic<std::uint64_t> owner_generation{0};
inline thread_local std::uint64_t local_generation = 0;
inline bool IsOwner() { return local_generation != 0 && local_generation == owner_generation.load(std::memory_order_relaxed); }
inline bool armed = false, complete = false, exported = false, export_attempted = false;
inline bool export_failed = false, export_error_reported = false;
inline std::uint64_t frame = 0, start_frame = 0, submission = 0, vblanks_seen = 0;
inline std::string output_path;
inline bool draws_enabled = false;
inline std::uint64_t duration_vblanks = 16, vs_generation = 0, gs_generation = 0;
// Arm AFTER state restoration. Diagnostic state is never guest-save-state data.
inline void Arm(const std::string& default_path) {
    active.store(false, std::memory_order_relaxed);
    local_generation = 0;
    const auto generation = owner_generation.fetch_add(1, std::memory_order_relaxed) + 1;
    armed = complete = exported = export_attempted = export_failed = export_error_reported = false;
    draws_enabled = false; duration_vblanks = 16; vs_generation = gs_generation = 0;
    const char* enable = std::getenv("CARA_GPU_TRACE");
    if (!enable || std::string(enable) != "1") return;
    const char* path = std::getenv("CARA_GPU_TRACE_PATH");
    output_path = path && *path ? path : default_path;
    start_frame = 0;
    if (const char* start = std::getenv("CARA_GPU_TRACE_START_FRAME")) {
        char* end = nullptr;
        auto value = std::strtoull(start, &end, 10);
        if (end != start && !*end && *start != '-') start_frame = value;
    }
    if (const char* draws = std::getenv("CARA_GPU_TRACE_DRAWS")) draws_enabled = std::string(draws) == "1";
    if (const char* duration = std::getenv("CARA_GPU_TRACE_DURATION_VBLANKS")) {
        char* end = nullptr;
        auto value = std::strtoull(duration, &end, 10);
        if (end != duration && !*end && value >= 1 && value <= 16) duration_vblanks = value;
    }
    buffer.size = 0; buffer.truncated = false;
    frame = submission = vblanks_seen = 0;
    foreign_events.store(0, std::memory_order_relaxed);
    local_generation = generation; armed = true;
}
inline bool Enabled() {
    if (!active.load(std::memory_order_relaxed)) return false;
    if (IsOwner()) return true;
    foreign_events.fetch_add(1, std::memory_order_relaxed);
    return false;
}
inline bool DrawsEnabled() { return Enabled() && draws_enabled; }
inline void ShaderWordChanged(bool geometry) {
    if (DrawsEnabled()) ++(geometry ? gs_generation : vs_generation);
}
// Reserve the whole adjacent draw triplet or stop without a partial draw record.
inline bool ReserveDrawRecords() {
    if (!DrawsEnabled()) return false;
    if (buffer.records.size() - buffer.size < 3) {
        buffer.truncated = true; active = false; complete = true; return false;
    }
    return true;
}
template<class Timing>
inline void Emit(Timing& timing, Kind kind, std::uint32_t thread = 0,
                 std::uint32_t object = 0, std::uint64_t a = 0, std::uint64_t b = 0,
                 std::uint64_t c = 0, std::uint64_t d = 0) {
    if (!Enabled()) return;
    if (!buffer.Push({0, frame, submission, timing.GetTimer(0)->GetTicks(), timing.GetTicks(),
                      timing.GetGlobalTicks(), static_cast<std::uint32_t>(kind), thread, object,
                      timing.GetCurrentTimerId(), a, b, c, d})) { active = false; complete = true; }
}
template<class Timing>
inline void VBlank(Timing& timing) {
    if (!IsOwner()) { (void)Enabled(); return; }
    if (!armed || complete) return;
    frame = vblanks_seen++;
    if (frame >= start_frame && frame - start_frame >= duration_vblanks) {
        active = false; complete = true; return;
    }
    active = frame >= start_frame;
    Emit(timing, Kind::VBlank);
}
// Only call outside GPU/kernel hooks. One export attempt per capture.
inline bool Flush(bool force = false) {
    if (!IsOwner() || !armed || export_attempted || (!complete && !force)) return false;
    export_attempted = true;
    active = false;
    std::ofstream out(output_path);
    if (!out) { export_failed = true; return false; }
    out << "# foreign_events_dropped=" << foreign_events.load(std::memory_order_relaxed) << '\n';
    WriteCSV(out, buffer, start_frame, complete, duration_vblanks, draws_enabled);
    out.flush(); exported = static_cast<bool>(out); export_failed = !exported; return exported;
}
inline bool TakeExportError() {
    if (!IsOwner() || !export_failed || export_error_reported) return false;
    export_error_reported = true; return true;
}
}
