// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_pc_sampler.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <thread>
#include <vector>
#if defined(__APPLE__) && defined(__aarch64__)
#define CARA_HOST_PC_SUPPORTED 1
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/arm/thread_status.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

namespace Cara {
namespace {
// One capture per process. Never acquire this ownership while a target is suspended.
std::atomic_flag capture_owner = ATOMIC_FLAG_INIT;
// JSON escaping includes control characters; image paths need not be ASCII.
void String(std::ostream& out, const std::string& value) {
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    out << '"';
}
}
struct HostPcSampler::Impl {
    Options options;
    bool enabled, started = false, written = false, owns_capture = false;
    std::atomic<bool> stop{false};
    std::thread worker;
    struct Record {
        std::uint64_t before_ns = 0, after_ns = 0, suspend_ns = 0;
        std::uint64_t pc = 0, lr = 0, sp = 0, fp = 0;
        int run_state = -1, info_result = -1, suspend_result = -1, state_result = -1, resume_result = -1;
    };
    struct Image { std::string path, uuid; std::uint64_t address; std::int64_t slide; };
    std::vector<Record> records;
    std::vector<Image> images;
    std::size_t count = 0;
    int sampler_preflight_result = -1;
    std::uint64_t info_failures = 0, suspend_failures = 0, state_failures = 0, resume_failures = 0;
    std::uint64_t start_ns = 0, end_ns = 0, start_unix_ns = 0;
    std::uint64_t start_frame = 0, last_frame = 0;
    const char* reason = "not_started";
#ifdef CARA_HOST_PC_SUPPORTED
    // Resolve callable addresses before capture, avoiding first-call dyld lazy
    // symbol binding while the target may own a loader/runtime lock.
    decltype(&thread_suspend) suspend_target = nullptr;
    decltype(&thread_get_state) get_target_state = nullptr;
    decltype(&thread_resume) resume_target = nullptr;
    thread_t target = MACH_PORT_NULL;
    mach_timebase_info_data_t timebase{};
    std::uint64_t Now() const {
        const auto ticks = mach_absolute_time();
        return static_cast<std::uint64_t>((static_cast<__uint128_t>(ticks) * timebase.numer) / timebase.denom);
    }
    void Images() {
        const auto n = _dyld_image_count();
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto* header = _dyld_get_image_header(i);
            if (!header || header->magic != MH_MAGIC_64) continue;
            Image image{_dyld_get_image_name(i) ? _dyld_get_image_name(i) : "", "", reinterpret_cast<std::uint64_t>(header), _dyld_get_image_vmaddr_slide(i)};
            const auto* command = reinterpret_cast<const load_command*>(reinterpret_cast<const mach_header_64*>(header) + 1);
            for (std::uint32_t j = 0; j < header->ncmds; ++j) {
                if (command->cmd == LC_UUID) {
                    const auto* uuid = reinterpret_cast<const uuid_command*>(command);
                    const char* hex = "0123456789ABCDEF";
                    for (unsigned k = 0; k < 16; ++k) {
                        if (k == 4 || k == 6 || k == 8 || k == 10) image.uuid += '-';
                        image.uuid += hex[uuid->uuid[k] >> 4]; image.uuid += hex[uuid->uuid[k] & 15];
                    }
                }
                command = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(command) + command->cmdsize);
            }
            images.push_back(std::move(image));
        }
    }
    void Capture() {
        // All memory is allocated and touched, dyld metadata captured, and timebase
        // initialized BEFORE the first suspension. No stack memory is dereferenced.
        start_unix_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        start_ns = Now();
        // Warm Mach state RPC / sampler-thread MIG reply-port TLS before the
        // target is stopped. Constructor prebinding alone does not warm TLS on
        // this newly created thread. Failure aborts capture without suspension.
        const thread_t sampler_self = mach_thread_self();
        arm_thread_state64_t warm_state{};
        mach_msg_type_number_t warm_count = ARM_THREAD_STATE64_COUNT;
        sampler_preflight_result = get_target_state(sampler_self, ARM_THREAD_STATE64,
            reinterpret_cast<thread_state_t>(&warm_state), &warm_count);
        mach_port_deallocate(mach_task_self(), sampler_self);
        if (sampler_preflight_result != KERN_SUCCESS) {
            reason = "sampler_preflight_failure"; end_ns = Now(); return;
        }
        std::uint32_t random = 0x6d2b79f5;
        while (!stop.load(std::memory_order_relaxed)) {
            if (count == records.size()) { reason = "capacity"; break; }
            if (Now() - start_ns >= static_cast<std::uint64_t>(options.max_seconds * 1e9)) { reason = "time_limit"; break; }
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            std::this_thread::sleep_for(std::chrono::microseconds(8000 + random % 4001));
            if (stop.load(std::memory_order_relaxed)) break;
            if (Now() - start_ns >= static_cast<std::uint64_t>(options.max_seconds * 1e9)) { reason = "time_limit"; break; }
            Record record;
            thread_basic_info_data_t info{};
            mach_msg_type_number_t info_count = THREAD_BASIC_INFO_COUNT;
            record.info_result = thread_info(target, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &info_count);
            if (record.info_result == KERN_SUCCESS) record.run_state = info.run_state;
            // Timestamp, suspension, state read, timestamp, and own balanced resume
            // only. Absolutely no allocation/logging/ObjC/dyld/runtime locks here.
            arm_thread_state64_t state{};
            mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
            record.before_ns = Now();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            record.suspend_result = suspend_target(target);
            if (record.suspend_result == KERN_SUCCESS) {
                const auto suspended_at = Now();
                record.state_result = get_target_state(target, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &state_count);
                record.resume_result = resume_target(target); // Every successful suspend owns exactly one resume.
                record.after_ns = Now();
                record.suspend_ns = record.after_ns - suspended_at;
            } else record.after_ns = Now();
#pragma clang diagnostic pop
            // Accessors handle PAC; publish and counters only AFTER resume.
            if (record.state_result == KERN_SUCCESS) {
                record.pc = arm_thread_state64_get_pc(state); record.lr = arm_thread_state64_get_lr(state);
                record.sp = arm_thread_state64_get_sp(state); record.fp = arm_thread_state64_get_fp(state);
            }
            info_failures += record.info_result != KERN_SUCCESS;
            suspend_failures += record.suspend_result != KERN_SUCCESS;
            state_failures += record.suspend_result == KERN_SUCCESS && record.state_result != KERN_SUCCESS;
            resume_failures += record.suspend_result == KERN_SUCCESS && record.resume_result != KERN_SUCCESS;
            records[count++] = record;
            if (record.resume_result != KERN_SUCCESS && record.suspend_result == KERN_SUCCESS) { reason = "resume_failure"; break; }
        }
        end_ns = Now();
    }
#endif
    Impl(bool active, Options opts) : options(opts), enabled(active && CompiledAvailable()) {
        options.capacity = std::clamp<std::size_t>(options.capacity, 1, 6000);
        if (!std::isfinite(options.max_seconds)) options.max_seconds = 60;
        options.max_seconds = std::clamp(options.max_seconds, .01, 60.0);
#ifdef CARA_HOST_PC_SUPPORTED
        if (enabled) {
            suspend_target = reinterpret_cast<decltype(suspend_target)>(dlsym(RTLD_DEFAULT, "thread_suspend"));
            get_target_state = reinterpret_cast<decltype(get_target_state)>(dlsym(RTLD_DEFAULT, "thread_get_state"));
            resume_target = reinterpret_cast<decltype(resume_target)>(dlsym(RTLD_DEFAULT, "thread_resume"));
            if (!suspend_target || !get_target_state || !resume_target) {
                enabled = false; reason = "api_resolution_failure"; return;
            }
            mach_timebase_info(&timebase);
            records.resize(options.capacity);
            // Volatile stores ensure pre-touch isn't optimized away.
            auto* bytes = reinterpret_cast<volatile unsigned char*>(records.data());
            for (std::size_t i = 0; i < records.size() * sizeof(Record); ++i) bytes[i] = 0;
            target = mach_thread_self(); // Held send right; only our current thread.
        }
#endif
    }
    void Stop() {
        stop.store(true, std::memory_order_relaxed);
        if (worker.joinable()) worker.join();
        if (owns_capture) { capture_owner.clear(std::memory_order_release); owns_capture = false; }
    }
    ~Impl() {
        Stop();
#ifdef CARA_HOST_PC_SUPPORTED
        if (target != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), target);
#endif
    }
};
bool HostPcSampler::CompiledAvailable() {
#ifdef CARA_HOST_PC_SUPPORTED
    return true;
#else
    return false;
#endif
}
HostPcSampler::HostPcSampler(bool enabled) : HostPcSampler(enabled, Options{}) {}
HostPcSampler::HostPcSampler(bool enabled, Options options) : impl(std::make_unique<Impl>(enabled, options)) {}
HostPcSampler::~HostPcSampler() = default;
void HostPcSampler::OnFrame(std::uint64_t frame) {
    auto& p = *impl;
    p.last_frame = frame;
    if (!p.enabled) return;
    if (!p.started && frame >= p.options.start_frame && frame < p.options.end_frame) {
#ifdef CARA_HOST_PC_SUPPORTED
        if (p.stop.load(std::memory_order_relaxed)) return;
        if (capture_owner.test_and_set(std::memory_order_acquire)) {
            p.enabled = false; p.reason = "capture_busy"; return;
        }
        p.owns_capture = true;
        p.Images(); p.started = true; p.start_frame = frame; p.reason = "stopped";
        p.worker = std::thread([&p] { p.Capture(); });
#endif
    }
    if (p.started && frame >= p.options.end_frame) p.Stop();
}
bool HostPcSampler::StopAndWrite(const std::string& path, const std::string& fingerprint) {
    auto& p = *impl;
    p.Stop();
    if (p.written) return true;
    std::ofstream out(path);
    if (!out) return false;
    out << "{\"kind\":\"wall_time_pc_residency_not_cpu_percent\",\"compiled_available\":" << (CompiledAvailable() ? "true" : "false")
        << ",\"enabled\":" << (p.enabled ? "true" : "false") << ",\"started\":" << (p.started ? "true" : "false")
        << ",\"source_fingerprint\":"; String(out, fingerprint);
    out << ",\"clock\":\"mach_absolute_time_nanoseconds\",\"unix_time_at_write\":" << std::time(nullptr)
        << ",\"start_frame\":" << p.start_frame << ",\"last_observed_frame\":" << p.last_frame
        << ",\"requested_start_frame\":" << p.options.start_frame << ",\"requested_end_frame\":" << p.options.end_frame
        << ",\"capacity\":" << p.options.capacity << ",\"max_seconds\":" << p.options.max_seconds
        << ",\"nominal_hz\":100,\"jitter_interval_us\":[8000,12000],\"dropped_records\":0,\"start_ns\":" << p.start_ns << ",\"end_ns\":" << p.end_ns
        << ",\"start_unix_ns\":" << p.start_unix_ns
        << ",\"stop_reason\":"; String(out, p.reason);
    out << ",\"sampler_preflight_result\":" << p.sampler_preflight_result
        << ",\"info_failures\":" << p.info_failures << ",\"suspend_failures\":" << p.suspend_failures
        << ",\"state_failures\":" << p.state_failures << ",\"resume_failures\":" << p.resume_failures << ",\"images\":[";
    bool comma = false;
    for (const auto& image : p.images) {
        if (comma) out << ','; comma = true;
        out << "{\"path\":"; String(out, image.path); out << ",\"uuid\":"; String(out, image.uuid);
        out << ",\"runtime_header\":" << image.address << ",\"slide\":" << image.slide << '}';
    }
    out << "],\"samples\":[";
    for (std::size_t i = 0; i < p.count; ++i) {
        if (i) out << ',';
        const auto& r = p.records[i];
        out << "{\"before_ns\":" << r.before_ns << ",\"after_ns\":" << r.after_ns << ",\"suspend_ns\":" << r.suspend_ns
            << ",\"pc\":" << r.pc << ",\"lr\":" << r.lr << ",\"sp\":" << r.sp << ",\"fp\":" << r.fp
            << ",\"natural_run_state\":" << r.run_state << ",\"info_result\":" << r.info_result
            << ",\"suspend_result\":" << r.suspend_result << ",\"state_result\":" << r.state_result << ",\"resume_result\":" << r.resume_result << '}';
    }
    out << "]}\n"; out.flush();
    p.written = bool(out); return p.written;
}
}
