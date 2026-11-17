// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>
#include <ctime>
#include <utility>
#include "cara/guest_wait_origins.h"
#include "cara/guest_callback_markers.h"
#include "cara/guest_record_snapshots.h"
#include "cara/command_origin_trace.h"
#include "cara/guest_gpu_trace_buffer.h"
#include "cara/guest_gpu_trace_join.h"
namespace Cara::GuestGPUTrace {
inline Buffer<32768> buffer;
inline CallbackMarkers callback_markers;
inline std::uint32_t execution_thread=0,execution_core=0;
inline bool have_execution_context=false;
inline std::uint64_t callback_clock_failures = 0;
inline std::uint64_t CallbackCPUStamp() {
    timespec value{};
#if defined(CLOCK_THREAD_CPUTIME_ID)
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) { ++callback_clock_failures; return 0; }
    return static_cast<std::uint64_t>(value.tv_sec)*1000000000ULL + value.tv_nsec;
#else
    (void)value; ++callback_clock_failures; return 0;
#endif
}
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
inline bool validation_copy_join_enabled = false;
inline bool record_snapshots_enabled = false;
inline bool wait_origins_enabled = false;
inline std::uint32_t wait_origin_error=0;
inline WaitOrigins::Tracker<256> wait_origin_tracker;
struct WaitArbContext {std::uint64_t sequence=WaitOrigins::UnknownSequence;std::uint32_t thread=0;bool waited=false;};
inline thread_local WaitArbContext wait_arb_context;
inline std::uint64_t duration_vblanks = 16, vs_generation = 0, gs_generation = 0;
// Arm AFTER state restoration. Diagnostic state is never guest-save-state data.
inline void Arm(const std::string& default_path) {
    active.store(false, std::memory_order_relaxed);
    local_generation = 0;
    const auto generation = owner_generation.fetch_add(1, std::memory_order_relaxed) + 1;
    armed = complete = exported = export_attempted = export_failed = export_error_reported = false;
    callback_markers.Load(nullptr); callback_clock_failures = 0;
    execution_thread=execution_core=0;have_execution_context=false;
    validation_copy_join_enabled = false;
    command_origins_enabled = false;
    record_snapshots_enabled = false;
    wait_origins_enabled=false;wait_origin_error=0;wait_arb_context={};
    draws_enabled = false; duration_vblanks = 16; vs_generation = gs_generation = 0;
    const char* enable = std::getenv("CARA_GPU_TRACE");
    if (!enable || std::string(enable) != "1") return;
    callback_markers.Load(std::getenv("CARA_GUEST_CALLBACK_MAP"));
    if(const char* waits=std::getenv("CARA_GPU_TRACE_WAIT_ORIGINS"))
        wait_origins_enabled=std::string(waits)=="1";
    if(wait_origins_enabled)wait_origin_tracker.Reset();
    if(const char* snapshots=std::getenv("CARA_GPU_TRACE_RECORD_SNAPSHOTS"))
        record_snapshots_enabled=std::string(snapshots)=="1";
    const char* join = std::getenv("CARA_SURFACE_VALIDATION_COPY_JOIN");
    const char* copy = std::getenv("CARA_SURFACE_VALIDATION_COPY_TRACE");
    validation_copy_join_enabled = join && copy && std::string(join)=="1" && std::string(copy)=="1";
    const char* path = std::getenv("CARA_GPU_TRACE_PATH");
    output_path = path && *path ? path : default_path;
    start_frame = 0;
    if (const char* start = std::getenv("CARA_GPU_TRACE_START_FRAME")) {
        char* end = nullptr;
        auto value = std::strtoull(start, &end, 10);
        if (end != start && !*end && *start != '-') start_frame = value;
    }
    if (const char* draws = std::getenv("CARA_GPU_TRACE_DRAWS")) draws_enabled = std::string(draws) == "1";
    if (const char* origins = std::getenv("CARA_GPU_TRACE_COMMAND_ORIGINS"))
        command_origins_enabled = draws_enabled && std::string(origins) == "1";
    if (command_origins_enabled) ResetCommandOrigins();
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
inline Join::Snapshot CurrentValidationCopyJoin() {
    if (!validation_copy_join_enabled) return Join::Unknown(Join::Reason::Disabled);
    return Join::state.Read(owner_generation.load(std::memory_order_relaxed),
        active.load(std::memory_order_relaxed), IsOwner(), !buffer.truncated);
}
inline void ShaderWordChanged(bool geometry) {
    if (DrawsEnabled()) ++(geometry ? gs_generation : vs_generation);
}
// Reserve the entire draw triplet plus optional origin pair before emitting any record.
inline bool ReserveDrawRecords() {
    if (!DrawsEnabled()) return false;
    if (buffer.records.size() - buffer.size < (command_origins_enabled ? 5u : 3u)) {
        buffer.truncated = true; active = false; complete = true; return false;
    }
    return true;
}
inline bool CommandOriginsTracking() {
    return IsOwner() && command_origins_enabled && DrawsEnabled();
}
inline void AppendCommandOrigin(std::size_t draw_anchor) {
    if (!command_origins_enabled || !Enabled() || draw_anchor >= buffer.size) return;
    const auto origin = CurrentCommandOrigin();
    auto record = buffer.records[draw_anchor];
    record.kind = static_cast<std::uint32_t>(Kind::DrawOriginAddress);
    record.a = origin.header_address; record.b = origin.parameter_address;
    record.c = origin.header_epoch; record.d = origin.parameter_epoch;
    if (!buffer.Push(record)) { active=false; complete=true; return; }
    record.kind = static_cast<std::uint32_t>(Kind::DrawOriginContext);
    record.a = origin.capture; record.b = origin.scope; record.c = origin.packet;
    record.d = (std::uint64_t(origin.reason) << 48) | (std::uint64_t(origin.valid) << 47) |
               (std::uint64_t(origin.slot) << 16) | origin.port;
    if (!buffer.Push(record)) { active=false; complete=true; }
}
template<class Timing>
inline void Emit(Timing& timing, Kind kind, std::uint32_t thread = 0,
                 std::uint32_t object = 0, std::uint64_t a = 0, std::uint64_t b = 0,
                 std::uint64_t c = 0, std::uint64_t d = 0) {
    if (!Enabled()) return;
    // Run records use the formerly unused d column only while callback tracing is configured.
    if (callback_markers.Count() && kind == Kind::Run) d = CallbackCPUStamp();
    if (!buffer.Push({0, frame, submission, timing.GetTimer(0)->GetTicks(), timing.GetTicks(),
                      timing.GetGlobalTicks(), static_cast<std::uint32_t>(kind), thread, object,
                      timing.GetCurrentTimerId(), a, b, c, d})) { active = false; complete = true; }
    if (active && callback_markers.Count() && (kind == Kind::GXBegin || kind == Kind::GXEnd)) {
        auto stamp=buffer.records[buffer.size-1];
        stamp.kind=static_cast<std::uint32_t>(Kind::HostCPUStamp);
        stamp.a=static_cast<std::uint32_t>(kind); stamp.b=stamp.c=0; stamp.d=CallbackCPUStamp();
        if (!buffer.Push(stamp)) { active=false; complete=true; }
    }
}
#include "cara/guest_wait_origin_trace.inc"
// Decoded block entries include direct/indirect calls and their return targets.
// Normal execution pays one disabled branch. Trace lookup is diagnostic-only.
inline bool CallbacksEnabled() { return callback_markers.Count() && active.load(std::memory_order_relaxed); }
// Actual host interpreter ownership is distinct from scheduling a guest thread.
// A switch between emulated cores can occur without a ThreadManager context switch.
template<class Timing>
inline void ExecutionContext(Timing& timing,std::uint32_t thread,std::uint32_t core) {
    if(!CallbacksEnabled() || !IsOwner())return;
    if(have_execution_context && execution_thread==thread && execution_core==core)return;
    Emit(timing,Kind::GuestExecutionContext,thread,core,execution_thread,execution_core,0,CallbackCPUStamp());
    execution_thread=thread;execution_core=core;have_execution_context=true;
}
#if defined(__GNUC__) || defined(__clang__)
#define CARA_RECORD_SNAPSHOT_COLD __attribute__((noinline,cold))
#elif defined(_MSC_VER)
#define CARA_RECORD_SNAPSHOT_COLD __declspec(noinline)
#else
#define CARA_RECORD_SNAPSHOT_COLD
#endif
// Keep Packet/Capture and companion emission out of ExecuteBlock's hot frame.
template<class Timing,class ThreadId,class SnapshotFn>
CARA_RECORD_SNAPSHOT_COLD void EmitRecordSnapshotCallback(
    Timing& timing,const CallbackMarkers::Marker* marker,std::uint32_t pc,std::uint32_t lr,
    std::uint32_t r0,std::uint32_t r1,std::uint32_t r2,std::uint32_t r3,
    ThreadId&& thread_id,SnapshotFn&& snapshot_fn) {
    // Lazy only after active-owner, marker and typed-PC checks. No clock,
    // RAM access or register snapshot is requested when disabled/unmatched.
    const auto snapshot=snapshot_fn(pc);
    const auto needed=2+snapshot.RecordCount();
    if(buffer.records.size()-buffer.size<needed) {
        buffer.truncated=true;active=false;complete=true;return;
    }
    const auto anchor=buffer.size;
    Emit(timing,Kind::GuestCallback,thread_id(),marker->phase,marker->id,pc,
         (std::uint64_t(lr)<<32)|r0,CallbackCPUStamp());
    auto args=buffer.records[anchor];args.kind=static_cast<std::uint32_t>(Kind::GuestCallbackArgs);
    args.a=marker->id;args.b=r1;args.c=r2;args.d=r3;buffer.Push(args);
    // Every companion inherits the original callback stamp. Whole-group
    // capacity was checked before emitting either callback-pair record.
    auto row=buffer.records[anchor];row.a=anchor;
    row.kind=static_cast<std::uint32_t>(Kind::RecordSnapshotRegisters);
    row.b=snapshot.r4;row.c=snapshot.r5;row.d=snapshot.r8;buffer.Push(row);
    for(std::size_t i=0;i<snapshot.group_count;++i) {
        const auto& g=snapshot.groups[i];
        row.kind=static_cast<std::uint32_t>(Kind::RecordSnapshotGroup);
        row.b=static_cast<std::uint32_t>(g.group);
        row.c=std::uint64_t(g.reason)|(std::uint64_t(g.known)<<32)|
            (std::uint64_t(g.requested)<<40)|(std::uint64_t(g.failed_index)<<48);
        row.d=g.base;buffer.Push(row);
    }
    for(std::size_t i=0;i<snapshot.word_count;++i) {
        const auto& w=snapshot.words[i];
        row.kind=static_cast<std::uint32_t>(Kind::RecordSnapshotWord);
        row.b=(std::uint64_t(w.group)<<32)|static_cast<std::uint32_t>(w.word.field);
        row.c=w.word.address;row.d=w.word.value;buffer.Push(row);
    }
}
#undef CARA_RECORD_SNAPSHOT_COLD
template<class Timing, class ThreadId, class SnapshotFn>
inline void CallbackBlock(Timing& timing, std::uint32_t pc, std::uint32_t lr,
                          std::uint32_t r0, std::uint32_t r1, std::uint32_t r2, std::uint32_t r3, ThreadId&& thread_id, SnapshotFn&& snapshot_fn) {
    if (!CallbacksEnabled() || !IsOwner()) return;
    const auto* marker=callback_markers.Find(pc,lr);
    if (!marker) return;
    if(record_snapshots_enabled && RecordSnapshots::TypedPC(pc)) {
        EmitRecordSnapshotCallback(timing,marker,pc,lr,r0,r1,r2,r3,thread_id,snapshot_fn);
        return;
    }
    Emit(timing, Kind::GuestCallback, thread_id(), marker->phase, marker->id, pc,
         (std::uint64_t(lr)<<32)|r0, CallbackCPUStamp());
    if (active) {
        auto args=buffer.records[buffer.size-1];
        args.kind=static_cast<std::uint32_t>(Kind::GuestCallbackArgs);
        args.a=marker->id; args.b=r1; args.c=r2; args.d=r3;
        if (!buffer.Push(args)) { active=false; complete=true; }
    }
}
// Called immediately after the existing GXBegin/GXEnd emission; no timing read.
inline void QueueStateAtLast(QueuePhase phase, std::uint32_t raw_header,
                             std::uint32_t slot, std::uint32_t raw_command_word) {
    if (!Enabled() || buffer.size == 0) return;
    const auto record = QueueStateRecord(buffer.records[buffer.size - 1], phase,
                                         raw_header, slot, raw_command_word);
    if (!buffer.Push(record)) { active = false; complete = true; }
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
    out << "# callback_markers=" << callback_markers.Count()
        << ",callback_clock_failures=" << callback_clock_failures
        << ",callback_map_error=" << callback_markers.Error() << '\n';
    if (validation_copy_join_enabled)
        out << "# validation_copy_join_generation="
            << owner_generation.load(std::memory_order_relaxed) << '\n';
    if (command_origins_enabled)
        out << "# command_origins_enabled=1,command_origin_failure="
            << static_cast<unsigned>(command_origin_tracker.Failure()) << '\n';
    WriteCSV(out, buffer, start_frame, complete, duration_vblanks, draws_enabled, command_origins_enabled, record_snapshots_enabled, wait_origins_enabled, wait_origin_error);
    out.flush(); exported = static_cast<bool>(out); export_failed = !exported; return exported;
}
inline bool TakeExportError() {
    if (!IsOwner() || !export_failed || export_error_reported) return false;
    export_error_reported = true; return true;
}
}
