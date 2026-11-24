// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Default-off, owner-thread-only diagnostics. No allocation or I/O in GPU hooks.
#include <array>
#include <atomic>
#ifdef _WIN32
#include <chrono>
#endif
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ostream>
#include <string>
#include <time.h>
namespace Cara::TextureCopyTrace {
enum class Reason : std::uint32_t { Unknown, InvalidSource, InvalidDestination, ZeroSize,
    InputWidth, InputRemainder, OutputWidth, OutputRemainder, SourceMiss,
    SourceMissSkipped, OutputLayout, DestinationMiss, DestinationTexture, BlitFailed,
    Accelerated, Count };
enum class Phase : std::uint32_t { Acceleration, SourceLookup, SourceValidate,
    DestinationAcquire, Blit, SourceCoherence, DestinationCoherence, CPUCopy,
    Download, ReadPixels, Repack, RequestTotal, Count };
inline constexpr const char* reason_names[] = {"unknown", "invalid_source", "invalid_destination",
    "zero_size", "input_width", "input_remainder", "output_width", "output_remainder",
    "source_miss", "source_miss_skipped", "output_layout", "destination_miss",
    "destination_texture", "blit_failed", "accelerated"};
inline constexpr const char* phase_names[] = {"acceleration", "source_lookup", "source_validate",
    "destination_acquire", "blit", "source_coherence", "destination_coherence", "cpu_copy",
    "download", "read_pixels", "repack", "request_total"};
struct Input { std::uint32_t source{}, destination{}, raw_size{}, aligned_size{},
    input_width{}, input_gap{}, output_width{}, output_gap{}, flags{}; };
struct Surface { bool valid{}; std::uint64_t address{}, size{}; std::uint32_t width{}, height{},
    stride{}, format{}, tiled{}, scale{}, type{}; };
struct Time { std::uint64_t wall{}, cpu{}, calls{}, clock_failures{}; };
struct Clocks { std::uint64_t wall{}, cpu{}; bool wall_valid{}, cpu_valid{}; };
inline Clocks ReadClocks() {
    Clocks out;
#ifdef _WIN32
    out.wall = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    out.wall_valid = true;
#else
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) == 0) {
        out.wall_valid = true; out.wall = std::uint64_t(value.tv_sec)*1000000000ULL+value.tv_nsec;
    }
#endif
#ifdef CLOCK_THREAD_CPUTIME_ID
    timespec cpu_value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_value) == 0) {
        out.cpu_valid = true;
        out.cpu = std::uint64_t(cpu_value.tv_sec)*1000000000ULL+cpu_value.tv_nsec;
    }
#endif
    return out;
}
inline Time Difference(Clocks a, Clocks b) {
    Time out; out.calls = 1;
    if (a.wall_valid && b.wall_valid && b.wall >= a.wall) out.wall = b.wall-a.wall;
    else ++out.clock_failures;
    if (a.cpu_valid && b.cpu_valid && b.cpu >= a.cpu) out.cpu = b.cpu-a.cpu;
    else ++out.clock_failures;
    return out;
}
inline void Add(Time& to, const Time& from) {
    to.wall += from.wall; to.cpu += from.cpu; to.calls += from.calls;
    to.clock_failures += from.clock_failures;
}
struct Download { Surface surface; std::uint32_t left{}, bottom{}, width{}, height{};
    std::uint64_t bytes{}; std::uint32_t parent_phase{}; };
struct DirtyFlush { Surface surface; std::uint64_t address{}, bytes{}; std::uint32_t parent_phase{}; };
struct Record {
    Input input; std::uint64_t frame{}, sequence{}; Reason reason{Reason::Unknown};
    Surface source_surface, destination_surface;
    std::uint64_t source_flush_bytes{}, destination_flush_bytes{}, copied_bytes{},
        download_count{}, download_bytes{}, dirty_flush_count{};
    std::uint32_t destination_flush_mode{};
    std::array<Time, static_cast<std::size_t>(Phase::Count)> phases{};
    std::array<Download, 4> downloads{};
    std::array<DirtyFlush, 8> dirty_flushes{};
};
struct Capture {
    std::array<Record, 256> records{}; Record overflow_record{};
    std::array<std::uint64_t, static_cast<std::size_t>(Reason::Count)> reasons{};
    std::array<Time, static_cast<std::size_t>(Phase::Count)> phases{};
    std::uint64_t size{}, requests{}, dropped_details{}, copied_bytes{}, download_count{}, download_bytes{};
};
inline Capture capture;
inline std::atomic<std::uint64_t> generation{0};
inline thread_local std::uint64_t owner_generation = 0;
inline thread_local Record* current = nullptr;
inline thread_local Phase current_phase = Phase::Acceleration;
inline bool armed = false, complete = false, export_attempted = false, export_failed = false, error_reported = false;
inline std::uint64_t frame = 0, next_frame = 0, start_frame = 0, duration = 4;
inline std::string output_path;
inline bool IsOwner() { return owner_generation && owner_generation == generation.load(std::memory_order_relaxed); }
inline bool Enabled() { return IsOwner() && armed && !complete && frame >= start_frame && next_frame != 0; }
inline void Configure(bool enabled, const std::string& path, std::uint64_t start, std::uint64_t count) {
    owner_generation = generation.fetch_add(1, std::memory_order_relaxed)+1;
    current = nullptr; armed = enabled; complete = export_attempted = export_failed = error_reported = false;
    frame = next_frame = 0; start_frame = start; duration = count >= 1 && count <= 16 ? count : 4;
    if (!enabled) { owner_generation = 0; return; }
    capture.size = capture.requests = capture.dropped_details = capture.copied_bytes = 0;
    capture.download_count = capture.download_bytes = 0;
    capture.reasons.fill(0); capture.phases.fill({}); output_path = path;
}
inline void Arm(const std::string& path) {
    const char* enable = std::getenv("CARA_TEXTURE_COPY_TRACE");
    std::uint64_t start = 1000;
    if (const char* value = std::getenv("CARA_TEXTURE_COPY_TRACE_START_FRAME")) {
        char* end = nullptr; auto n = std::strtoull(value, &end, 10);
        if (end != value && !*end && *value != '-') start = n;
    }
    Configure(enable && std::string(enable) == "1", path, start, 4);
}
inline void VBlank() {
    if (!IsOwner() || !armed || complete) return;
    frame = next_frame++;
    if (frame >= start_frame && frame-start_frame >= duration) complete = true;
}
class Request {
    Record* record{}; Clocks started{};
public:
    explicit Request(Input input) {
        if (!Enabled() || current) return;
        ++capture.requests;
        if (capture.size < capture.records.size()) record = &capture.records[capture.size++];
        else { ++capture.dropped_details; capture.overflow_record = {}; record = &capture.overflow_record; }
        *record = {};
        record->input = input; record->frame = frame; record->sequence = capture.requests;
        current = record; current_phase = Phase::Acceleration; started = ReadClocks();
    }
    Request(const Request&) = delete;
    ~Request() {
        if (!record) return;
        Add(record->phases[static_cast<std::size_t>(Phase::RequestTotal)], Difference(started, ReadClocks()));
        ++capture.reasons[static_cast<std::size_t>(record->reason)];
        capture.copied_bytes += record->copied_bytes; capture.download_count += record->download_count;
        capture.download_bytes += record->download_bytes;
        for (std::size_t i=0; i<record->phases.size(); ++i) Add(capture.phases[i], record->phases[i]);
        current = nullptr;
    }
};
class Scope {
    Record* record{}; Phase phase{}, previous{}; Clocks started{};
public:
    explicit Scope(Phase value) : record(current), phase(value), previous(current_phase) {
        if (record) { current_phase = value; started = ReadClocks(); }
    }
    Scope(const Scope&) = delete;
    ~Scope() {
        if (!record) return;
        Add(record->phases[static_cast<std::size_t>(phase)], Difference(started, ReadClocks()));
        current_phase = previous;
    }
};
inline void SetReason(Reason reason) { if (current) current->reason = reason; }
template<class T> inline Surface Snapshot(const T& surface) {
    return {true, surface.addr, surface.size, surface.width, surface.height, surface.stride,
        static_cast<std::uint32_t>(surface.pixel_format), static_cast<std::uint32_t>(surface.is_tiled),
        surface.res_scale, static_cast<std::uint32_t>(surface.type)};
}
template<class T> inline void Source(const T& surface) { if (current) current->source_surface = Snapshot(surface); }
template<class T> inline void Destination(const T& surface) { if (current) current->destination_surface = Snapshot(surface); }
template<class T> inline void DownloadSurface(const T& surface, std::uint32_t left, std::uint32_t bottom,
    std::uint32_t width, std::uint32_t height, std::uint64_t bytes) {
    if (!current) return;
    auto index = current->download_count++;
    current->download_bytes += bytes;
    if (index < current->downloads.size()) current->downloads[index] =
        {Snapshot(surface),left,bottom,width,height,bytes,static_cast<std::uint32_t>(current_phase)};
}
template<class T> inline void DirtySurface(const T& surface, std::uint64_t address, std::uint64_t bytes) {
    if (!current) return;
    auto index = current->dirty_flush_count++;
    if (index < current->dirty_flushes.size()) current->dirty_flushes[index] =
        {Snapshot(surface),address,bytes,static_cast<std::uint32_t>(current_phase)};
}
inline void WriteTime(std::ostream& out, const Time& time) {
    out << "{\"wall_ns\":" << time.wall << ",\"thread_cpu_ns\":" << time.cpu
        << ",\"calls\":" << time.calls << ",\"clock_failures\":" << time.clock_failures << '}';
}
inline void WriteSurface(std::ostream& out, const Surface& s) {
    out << "{\"valid\":" << s.valid << ",\"address\":" << s.address << ",\"size\":" << s.size
        << ",\"width\":" << s.width << ",\"height\":" << s.height << ",\"stride\":" << s.stride
        << ",\"format\":" << s.format << ",\"tiled\":" << s.tiled << ",\"scale\":" << s.scale
        << ",\"type\":" << s.type << '}';
}
inline void WritePhases(std::ostream& out, const std::array<Time,static_cast<std::size_t>(Phase::Count)>& phases) {
    out << '{';
    for (std::size_t i=0; i<phases.size(); ++i) { if(i) out << ','; out << '\"' << phase_names[i] << "\":"; WriteTime(out, phases[i]); }
    out << '}';
}
inline void Write(std::ostream& out) {
    out << "{\"schema\":1,\"complete\":" << complete << ",\"start_frame\":" << start_frame
        << ",\"duration_vblanks\":" << duration << ",\"requests\":" << capture.requests
        << ",\"dropped_request_details\":" << capture.dropped_details << ",\"copied_bytes\":" << capture.copied_bytes
        << ",\"download_count\":" << capture.download_count << ",\"download_bytes\":" << capture.download_bytes << ",\"reasons\":{";
    for (std::size_t i=0;i<capture.reasons.size();++i) { if(i) out << ','; out << '\"' << reason_names[i] << "\":" << capture.reasons[i]; }
    out << "},\"phases\":"; WritePhases(out,capture.phases); out << ",\"records\":[";
    for (std::size_t i=0;i<capture.size;++i) {
        if(i) out << ','; const auto& r=capture.records[i]; const auto& p=r.input;
        out << "{\"sequence\":" << r.sequence << ",\"frame\":" << r.frame << ",\"reason\":\""
            << reason_names[static_cast<std::size_t>(r.reason)] << "\",\"source\":" << p.source << ",\"destination\":" << p.destination
            << ",\"raw_size\":" << p.raw_size << ",\"aligned_size\":" << p.aligned_size << ",\"input_width\":" << p.input_width
            << ",\"input_gap\":" << p.input_gap << ",\"output_width\":" << p.output_width << ",\"output_gap\":" << p.output_gap
            << ",\"flags\":" << p.flags << ",\"source_flush_bytes\":" << r.source_flush_bytes
            << ",\"destination_flush_bytes\":" << r.destination_flush_bytes << ",\"destination_flush_mode\":" << r.destination_flush_mode
            << ",\"copied_bytes\":" << r.copied_bytes << ",\"source_surface\":";
        WriteSurface(out,r.source_surface); out << ",\"destination_surface\":"; WriteSurface(out,r.destination_surface);
        out << ",\"phases\":"; WritePhases(out,r.phases);
        out << ",\"download_count\":" << r.download_count << ",\"download_details_dropped\":"
            << (r.download_count>r.downloads.size()?r.download_count-r.downloads.size():0) << ",\"downloads\":[";
        for(std::size_t j=0;j<r.download_count&&j<r.downloads.size();++j) { if(j)out<<','; const auto& v=r.downloads[j];
            out << "{\"surface\":"; WriteSurface(out,v.surface); out << ",\"left\":"<<v.left<<",\"bottom\":"<<v.bottom
                <<",\"width\":"<<v.width<<",\"height\":"<<v.height<<",\"bytes\":"<<v.bytes<<",\"parent_phase\":"<<v.parent_phase<<'}'; }
        out << "],\"dirty_flush_count\":"<<r.dirty_flush_count<<",\"dirty_flush_details_dropped\":"
            <<(r.dirty_flush_count>r.dirty_flushes.size()?r.dirty_flush_count-r.dirty_flushes.size():0)<<",\"dirty_flushes\":[";
        for(std::size_t j=0;j<r.dirty_flush_count&&j<r.dirty_flushes.size();++j) { if(j)out<<','; const auto& v=r.dirty_flushes[j];
            out << "{\"surface\":"; WriteSurface(out,v.surface); out << ",\"address\":"<<v.address<<",\"bytes\":"<<v.bytes
                <<",\"parent_phase\":"<<v.parent_phase<<'}'; }
        out << "]}";
    }
    out << "]}\n";
}
// Call only after RunLoop or shutdown, never inside GPU/kernel hooks.
inline bool Flush(bool force=false) {
    if (!IsOwner() || !armed || export_attempted || (!complete && !force) || current) return false;
    export_attempted=true;
    std::ofstream out(output_path); if (!out) { export_failed=true; return false; }
    Write(out); out.flush(); export_failed=!out; return !export_failed;
}
inline bool TakeExportError() {
    if (!IsOwner() || !export_failed || error_reported) return false;
    error_reported=true; return true;
}
} // namespace Cara::TextureCopyTrace
