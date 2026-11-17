// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include "cara/native_mac/gl_query_census.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <dlfcn.h>
#include <fstream>
#include <pthread.h>
#include <stdexcept>
#include <time.h>
#include "common/common_types.h"
#include "video_core/video_core.h"

namespace Cara::NativeGLQueryCensus {
namespace {
constexpr std::size_t Capacity = 1024;
enum class API : unsigned { GetIntegerv, CheckFramebufferStatus, IsEnabled, GetProgramiv, GetError };
constexpr const char* Names[]{"glGetIntegerv", "glCheckFramebufferStatus", "glIsEnabled", "glGetProgramiv", "glGetError"};
struct Entry {
    API api{};
    GLenum pname{}; // FBO target / IsEnabled cap; zero for GetError.
    std::uintptr_t caller{};
    std::uint64_t calls{}, thread_cpu_ns{}, timed_calls{}, clock_failures{};
    std::uint64_t first_frame{}, last_frame{};
};
struct State {
    std::array<Entry, Capacity> entries{};
    std::size_t size{};
    std::uint64_t overflow_calls{}, outside_window_calls{};
    std::atomic<std::uint64_t> foreign_thread_calls{};
    bool enabled{}, cpu{};
    pthread_t owner{};
    PFNGLGETINTEGERVPROC get_integer{};
    PFNGLCHECKFRAMEBUFFERSTATUSPROC check_framebuffer{};
    PFNGLISENABLEDPROC is_enabled{};
    PFNGLGETPROGRAMIVPROC get_program{};
    PFNGLGETERRORPROC get_error{};
} state;

bool ThreadCPU(std::uint64_t& value) {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) return false;
    value = std::uint64_t(t.tv_sec) * 1000000000ULL + std::uint64_t(t.tv_nsec);
    return true;
}
Entry* Record(API api, GLenum pname, void* caller) {
    if (!pthread_equal(pthread_self(), state.owner)) {
        state.foreign_thread_calls.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    const auto frame = VideoCore::GetCurrentFrame(); // Host counter only; no guest/timing reads.
    if (frame < 60 || frame >= 180) { ++state.outside_window_calls; return nullptr; }
    const auto pc = reinterpret_cast<std::uintptr_t>(caller);
    for (std::size_t i = 0; i < state.size; ++i) {
        auto& e = state.entries[i];
        if (e.api == api && e.pname == pname && e.caller == pc) {
            ++e.calls; e.last_frame = frame; return &e;
        }
    }
    if (state.size == Capacity) { ++state.overflow_calls; return nullptr; }
    auto& e = state.entries[state.size++];
    e.api = api; e.pname = pname; e.caller = pc; e.calls = 1;
    e.first_frame = e.last_frame = frame;
    return &e;
}
// Lookup/frame checks precede the first timestamp. Only the forwarded driver (or
// existing audit wrapper) lies between timestamps. Clock overhead is not removed.
struct Timer {
    Entry* entry;
    std::uint64_t begin{};
    bool started{};
    explicit Timer(Entry* e) : entry(e) {
        if (entry && state.cpu) started = ThreadCPU(begin);
    }
    ~Timer() {
        if (!entry || !state.cpu) return;
        std::uint64_t end{};
        if (started && ThreadCPU(end) && end >= begin) {
            entry->thread_cpu_ns += end - begin; ++entry->timed_calls;
        } else ++entry->clock_failures;
    }
};
#define CALLER __builtin_extract_return_addr(__builtin_return_address(0))
void APIENTRY GetInteger(GLenum pname, GLint* value) {
    Timer timer(Record(API::GetIntegerv, pname, CALLER)); state.get_integer(pname, value);
}
GLenum APIENTRY CheckFramebuffer(GLenum target) {
    Timer timer(Record(API::CheckFramebufferStatus, target, CALLER)); return state.check_framebuffer(target);
}
GLboolean APIENTRY IsEnabled(GLenum cap) {
    Timer timer(Record(API::IsEnabled, cap, CALLER)); return state.is_enabled(cap);
}
void APIENTRY GetProgram(GLuint program, GLenum pname, GLint* value) {
    Timer timer(Record(API::GetProgramiv, pname, CALLER)); state.get_program(program, pname, value);
}
GLenum APIENTRY GetError() {
    Timer timer(Record(API::GetError, 0, CALLER)); return state.get_error();
}
#undef CALLER
bool Connected() {
    return glad_glGetIntegerv == GetInteger && glad_glCheckFramebufferStatus == CheckFramebuffer &&
           glad_glIsEnabled == IsEnabled && glad_glGetProgramiv == GetProgram && glad_glGetError == GetError;
}
void Quote(std::ostream& out, const char* s) {
    out << '"';
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s ? s : ""); *p; ++p) {
        if (*p == '"' || *p == '\\') out << '\\' << char(*p);
        else if (*p < 32) out << '?'; else out << char(*p);
    }
    out << '"';
}
} // namespace

void Install(bool measure_thread_cpu) {
    if (state.enabled) throw std::runtime_error("GL query census already installed");
    if (!glad_glGetIntegerv || !glad_glCheckFramebufferStatus || !glad_glIsEnabled ||
        !glad_glGetProgramiv || !glad_glGetError)
        throw std::runtime_error("GL query census missing required entry point");
    state.entries.fill({}); state.size = 0; state.overflow_calls = state.outside_window_calls = 0;
    state.foreign_thread_calls.store(0); state.cpu = measure_thread_cpu; state.owner = pthread_self();
    state.get_integer = glad_glGetIntegerv; state.check_framebuffer = glad_glCheckFramebufferStatus;
    state.is_enabled = glad_glIsEnabled; state.get_program = glad_glGetProgramiv;
    state.get_error = glad_glGetError; // Preserves NativeAuditedGetError when audit is enabled.
    state.enabled = true;
    glad_glGetIntegerv = GetInteger; glad_glCheckFramebufferStatus = CheckFramebuffer;
    glad_glIsEnabled = IsEnabled; glad_glGetProgramiv = GetProgram; glad_glGetError = GetError;
}
void Uninstall() {
    if (!state.enabled) return;
    glad_glGetIntegerv = state.get_integer; glad_glCheckFramebufferStatus = state.check_framebuffer;
    glad_glIsEnabled = state.is_enabled; glad_glGetProgramiv = state.get_program; glad_glGetError = state.get_error;
    state.enabled = false;
}
bool AuditConnected(PFNGLGETERRORPROC expected) {
    return state.enabled ? glad_glGetError == GetError && state.get_error == expected : glad_glGetError == expected;
}
void WriteReport(const std::string& path, const std::string& source_fingerprint) {
    std::ofstream out(path);
    out << "{\"schema\":1,\"diagnostic_only\":true,\"source_fingerprint\":";
    Quote(out, source_fingerprint.c_str());
    out << ",\"window_frame_begin\":60,\"window_frame_end_exclusive\":180,\"capacity\":" << Capacity
        << ",\"enabled\":" << (state.enabled ? "true" : "false")
        << ",\"hooks_connected\":" << (Connected() ? "true" : "false")
        << ",\"thread_cpu_timing\":" << (state.cpu ? "true" : "false")
        << ",\"clock\":\"CLOCK_THREAD_CPUTIME_ID; two timestamp overhead included; no wall/GPU duration\""
        << ",\"overflow_calls\":" << state.overflow_calls << ",\"outside_window_calls\":" << state.outside_window_calls
        << ",\"foreign_thread_calls\":" << state.foreign_thread_calls.load() << ",\"records\":[";
    for (std::size_t i = 0; i < state.size; ++i) {
        const auto& e = state.entries[i]; Dl_info image{};
        dladdr(reinterpret_cast<void*>(e.caller), &image); // Export only, never per query.
        if (i) out << ',';
        out << "{\"function\":"; Quote(out, Names[unsigned(e.api)]);
        out << ",\"pname\":" << e.pname << ",\"caller_pc\":" << e.caller
            << ",\"image_base\":" << reinterpret_cast<std::uintptr_t>(image.dli_fbase) << ",\"image\":";
        Quote(out, image.dli_fname);
        out << ",\"calls\":" << e.calls << ",\"thread_cpu_ns\":" << e.thread_cpu_ns
            << ",\"timed_calls\":" << e.timed_calls << ",\"clock_failures\":" << e.clock_failures
            << ",\"first_frame\":" << e.first_frame << ",\"last_frame\":" << e.last_frame << '}';
    }
    out << "]}\n";
}
} // namespace Cara::NativeGLQueryCensus
