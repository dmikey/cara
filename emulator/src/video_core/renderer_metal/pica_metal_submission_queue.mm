// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_submission_queue.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/pica_metal_draw_stage_profile.hh"
#include "video_core/video_core.h"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mach/mach_time.h>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "common/logging/log.h"

namespace Cara {
namespace {

std::atomic<std::uint64_t> memory_pressure_generation{0};

bool AdaptiveSubmissionWindow() {
    const auto* option = std::getenv("CARA_METAL_ADAPTIVE_QUEUE");
    return !std::getenv("CARA_METAL_SUBMISSION_WINDOW") && option &&
           std::string_view(option) == "1";
}

bool ResourcePressure(id<MTLDevice> device) {
    if (NSProcessInfo.processInfo.thermalState >= NSProcessInfoThermalStateSerious)
        return true;
    if (@available(iOS 16.0, *)) {
        const auto budget = device.recommendedMaxWorkingSetSize;
        return budget != 0 && device.currentAllocatedSize >= budget - budget / 5;
    }
    return false;
}

std::size_t SubmissionWindow() {
    const auto* option = std::getenv("CARA_METAL_SUBMISSION_WINDOW");
    if (!option) {
        const auto* wide = std::getenv("CARA_METAL_QUEUE_START_WIDE");
        return wide && std::string_view(wide) == "1" ? MetalQueuePolicy::MaxSlots
                                                     : MetalQueuePolicy::MinSlots;
    }
    if (std::string_view(option) == "8")
        return PicaMetalSubmissionQueue::Capacity;
    if (std::string_view(option) == "16")
        return 16;
    if (std::string_view(option) == "32")
        return PicaMetalSubmissionQueue::MaxCapacity;
    throw std::invalid_argument("CARA_METAL_SUBMISSION_WINDOW must be 8, 16 or 32");
}

// The native Gate uses one active submission lane per worker. If profiling
// switches to a different lane, old entries report unknown instead of a stale label.
struct QueueProfile {
    struct Timing {
        std::uint64_t submit_refresh, retire_refresh;
        NativeMetalCost::QueueWork producer;
        double start, end;
    };
    static constexpr std::size_t MaxTimings = 262144;
    const PicaMetalSubmissionQueue* owner = nullptr;
    std::array<NativeMetalCost::QueueWork, PicaMetalSubmissionQueue::MaxCapacity> producer{};
    std::array<std::uint64_t, PicaMetalSubmissionQueue::MaxCapacity> submit_refresh{};
    std::vector<Timing> timings;
    std::uint64_t missing = 0, dropped = 0;
    std::size_t window = PicaMetalSubmissionQueue::Capacity;
};
thread_local QueueProfile queue_profile;

bool QueueWaitTimelineEnabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_QUEUE_WAIT_TIMELINE");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

struct QueueWait {
    std::uint64_t refresh;
    std::uint64_t elapsed_ns;
    std::size_t oldest_draw_count;
    double gpu_elapsed_ms;
    double after_gpu_ms;
};
constexpr std::size_t MaxQueueWaits = 65536;
thread_local std::vector<QueueWait> queue_waits;
thread_local std::size_t queue_waits_dropped;
struct QueuePolicyTiming {
    std::uint64_t refresh;
    std::size_t slots;
    std::size_t next_slots;
    MetalQueueSample sample;
    const char* reason;
};
thread_local std::vector<QueuePolicyTiming> policy_timings;

bool QueuePolicyReportEnabled() {
    const auto* option = std::getenv("CARA_METAL_QUEUE_POLICY_REPORT");
    return option && std::string_view(option) == "1";
}

double MachSeconds(std::uint64_t ticks) {
    static const double seconds_per_tick = [] {
        mach_timebase_info_data_t base{};
        mach_timebase_info(&base);
        return base.denom ? 1e-9 * base.numer / base.denom : 0.0;
    }();
    return ticks * seconds_per_tick;
}

void RecordQueueWait(const PicaMetalDrawTicket& ticket, std::uint64_t start, std::uint64_t end) {
    if (queue_waits.size() == MaxQueueWaits) {
        ++queue_waits_dropped;
        return;
    }
    const double gpu_start = ticket.command.GPUStartTime;
    const double gpu_end = ticket.command.GPUEndTime;
    const double wait_start = MachSeconds(start);
    const double wait_end = MachSeconds(end);
    const bool valid_gpu = gpu_start > 0 && gpu_end >= gpu_start;
    const double gpu_ms = valid_gpu ? (gpu_end - gpu_start) * 1000 : -1;
    // Metal reports host mach time in seconds. Count only the part of the
    // host wait after the GPU endpoint, excluding any time before this wait.
    const double after_gpu_ms = valid_gpu && gpu_end <= wait_end
                                    ? (wait_end - std::max(wait_start, gpu_end)) * 1000 : -1;
    const auto ns = static_cast<std::uint64_t>(MachSeconds(end - start) * 1e9);
    queue_waits.push_back({VideoCore::GetCurrentFrame(), ns, ticket.count, gpu_ms, after_gpu_ms});
}

} // namespace

void SignalNativeMetalMemoryPressure() {
    memory_pressure_generation.fetch_add(1, std::memory_order_relaxed);
}

void ReportNativeMetalCommandTimeline(const std::string& path) {
    if (QueuePolicyReportEnabled()) {
        std::ofstream out(path + "metal-queue-policy.csv");
        out << "refresh,slots,next_slots,p99_ns,queue_wait_ns,upload_wait_ns,pressure,reason\n";
        for (const auto& entry : policy_timings) {
            const auto& sample = entry.sample;
            out << entry.refresh << ',' << entry.slots << ',' << entry.next_slots << ','
                << sample.p99_ns << ',' << sample.queue_wait_ns << ',' << sample.upload_wait_ns
                << ',' << sample.resource_pressure << ',' << entry.reason << '\n';
        }
        if (!out)
            throw std::runtime_error("Could not retain the Metal queue policy report");
    }
    if (QueueWaitTimelineEnabled()) {
        std::ofstream waits(path + "metal-queue-waits.csv");
        waits << "refresh_before_vblank,elapsed_ns,oldest_draw_count,"
                 "oldest_command_gpu_ms,wait_after_gpu_ms\n"
              << std::setprecision(17);
        for (const auto& wait : queue_waits)
            waits << wait.refresh << ',' << wait.elapsed_ns << ',' << wait.oldest_draw_count
                  << ',' << wait.gpu_elapsed_ms << ',' << wait.after_gpu_ms << '\n';
        std::ofstream info(path + "metal-queue-waits-info.json");
        info << "{\"dropped\":" << queue_waits_dropped << "}\n";
        if (!waits || !info)
            throw std::runtime_error("Could not retain diagnostic Metal queue waits");
    }
    if (!NativeMetalCost::Enabled())
        return;
    std::ofstream out(path + "metal-command-timeline.csv");
    out << "submit_refresh,retire_refresh,producer,start_seconds,end_seconds\n" << std::setprecision(17);
    for (const auto& timing : queue_profile.timings)
        out << timing.submit_refresh << ',' << timing.retire_refresh << ','
            << static_cast<unsigned>(timing.producer) << ',' << timing.start << ',' << timing.end << '\n';
    std::ofstream info(path + "metal-command-timeline-info.json");
    info << "{\"retained\":" << queue_profile.timings.size()
         << ",\"missing\":" << queue_profile.missing << ",\"dropped\":" << queue_profile.dropped
         << ",\"submission_window\":" << queue_profile.window
         << ",\"capacity\":" << QueueProfile::MaxTimings
         << ",\"scope\":\"Retired tracked command GPU start/end coverage; overlapping command intervals are not exclusive GPU utilization.\"}\n";
    if (!out || !info)
        throw std::runtime_error("Could not retain diagnostic Metal command timeline");
}

PicaMetalSubmissionQueue::PicaMetalSubmissionQueue(id<MTLCommandQueue> queue_,
                                                   std::function<void()> reject_writes_)
    : owner(std::this_thread::get_id()), queue(queue_), reject_writes(std::move(reject_writes_)),
      adaptive(AdaptiveSubmissionWindow()), policy(SubmissionWindow()),
      observed_memory_pressure(memory_pressure_generation.load(std::memory_order_relaxed)) {
    if (!queue || !reject_writes)
        throw std::invalid_argument("Metal submissions need a queue and an owning memory fault handler");
    LOG_INFO(Render_OpenGL, "Native Metal submission window={} adaptive={}", Limit(), adaptive);
    if (QueuePolicyReportEnabled())
        policy_timings.clear();
    if (QueueWaitTimelineEnabled()) {
        queue_waits.clear();
        queue_waits.reserve(MaxQueueWaits);
        queue_waits_dropped = 0;
    }
    if (NativeMetalCost::Enabled())
        queue_profile = {.owner = this, .window = Limit()};
}

void PicaMetalSubmissionQueue::RejectWrites() {
    faulted = true;
    reject_writes();
}

void PicaMetalSubmissionQueue::Reserve() {
    CheckThread();
    if (Full()) {
        NativeMetalCost::Scope scope(NativeMetalCost::QueueReserveFull);
        while (Full())
            Retire(true, true);
    }
}

void PicaMetalSubmissionQueue::EndRefresh(std::uint64_t upload_wait_ns) {
    if (!adaptive)
        return;
    CheckThread();
    const auto now = Clock::now();
    const auto previous = std::exchange(previous_refresh, now);
    const auto queue_wait = std::exchange(reserve_wait_ns, 0);
    if (previous == Clock::time_point{})
        return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous).count();
    // Backgrounding and debugger pauses should not influence the next trial.
    if (elapsed <= 0 || elapsed > 500'000'000) {
        samples = 0;
        upload_wait_total_ns = 0;
        window_reserve_wait_ns = 0;
        return;
    }
    refresh_ns[samples++] = static_cast<std::uint64_t>(elapsed);
    window_reserve_wait_ns += queue_wait;
    upload_wait_total_ns += upload_wait_ns;
    if (samples != refresh_ns.size())
        return;

    auto p99 = refresh_ns.begin() + (99 * refresh_ns.size() + 99) / 100 - 1;
    std::nth_element(refresh_ns.begin(), p99, refresh_ns.end());
    const auto generation = memory_pressure_generation.load(std::memory_order_relaxed);
    const bool memory_warning = std::exchange(observed_memory_pressure, generation) != generation;
    const MetalQueueSample sample{*p99, window_reserve_wait_ns / samples,
                                 upload_wait_total_ns / samples,
                                 memory_warning || ResourcePressure(queue.device)};
    const auto old = Limit();
    const char* reason = policy.Update(sample);
    if (reason) {
        LOG_INFO(Render_OpenGL, "Native Metal adaptive slots={} -> {} refresh={} reason={}",
                 old, Limit(), VideoCore::GetCurrentFrame(), reason);
    }
    if (QueuePolicyReportEnabled() && policy_timings.size() < 16384)
        policy_timings.push_back({VideoCore::GetCurrentFrame(), old, Limit(), sample,
                                  reason ? reason : "hold"});
    samples = 0;
    window_reserve_wait_ns = 0;
    upload_wait_total_ns = 0;
}

void PicaMetalSubmissionQueue::CheckThread() const {
    if (std::this_thread::get_id() != owner)
        throw std::logic_error("Metal rasterizer used outside its owning thread");
    if (faulted)
        throw std::runtime_error("Metal rasterizer cannot resume after a submission fault");
}

void PicaMetalSubmissionQueue::Fail(const char* reason) {
    RejectWrites();
    throw std::runtime_error(reason);
}

void PicaMetalSubmissionQueue::Retire(bool wait_for_oldest, bool capacity_wait) {
    bool wait = wait_for_oldest;
    while (count != 0) {
        auto& ticket = in_flight[first];
        std::uint64_t queue_wait_ns = 0;
        auto state = ticket.Poll();
        if (wait && state == PicaMetalCompletion::Pending) {
            using namespace NativeMetalCost;
            const bool profile = Enabled();
            const bool trace_wait = QueueWaitTimelineEnabled();
            const auto start = (profile || adaptive) ? Clock::now() : Clock::time_point{};
            const auto mach_start = trace_wait ? mach_absolute_time() : 0;
            state = ticket.WaitUntilGuestRead();
            const auto mach_end = trace_wait ? mach_absolute_time() : 0;
            if (profile || adaptive)
                queue_wait_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - start).count();
            if (adaptive && capacity_wait)
                reserve_wait_ns += queue_wait_ns;
            if (profile) {
                const auto ns = queue_wait_ns;
                const auto producer = queue_profile.owner == this
                                          ? queue_profile.producer[first] : QueueWork::Other;
                RecordNs(ReserveWait, ns);
                RecordNs(WaitSite(queue_request), ns);
                RecordNs(WaitOldest(producer), ns);
            }
            if (trace_wait)
                RecordQueueWait(ticket, mach_start, mach_end);
        }
        if (state == PicaMetalCompletion::Failed)
            Fail("Committed Metal draw failed");
        if (state == PicaMetalCompletion::Pending)
            break;
        if (NativeMetalCost::Enabled()) {
            const double start = ticket.command.GPUStartTime, end = ticket.command.GPUEndTime;
            const bool valid = start > 0 && end >= start && end - start < 60;
            const auto ns = valid ? static_cast<std::uint64_t>((end - start) * 1e9) : 0;
            if (valid)
                NativeMetalCost::RecordNs(NativeMetalCost::GpuCompleted, ns);
            else
                NativeMetalCost::Count(NativeMetalCost::GpuTimeMissing, 1);
            if (queue_profile.owner == this) {
                if (!valid)
                    ++queue_profile.missing;
                else if (queue_profile.timings.size() == QueueProfile::MaxTimings)
                    ++queue_profile.dropped;
                else
                    queue_profile.timings.push_back({queue_profile.submit_refresh[first],
                        VideoCore::GetCurrentFrame(), queue_profile.producer[first], start, end});
            }
            MetalDrawStageProfile::Retire(ticket.command, ticket.count, queue_wait_ns, valid, ns);
        }
        wait = false;
        ticket = {};
        first = (first + 1) & (MaxCapacity - 1);
        --count;
    }
}

PicaMetalDrawTicket& PicaMetalSubmissionQueue::Track(const PicaMetalDrawTicket& ticket) {
    try {
        CheckThread();
        if (!ticket.command || ticket.command.commandQueue != queue ||
            ticket.Poll() == PicaMetalCompletion::Failed)
            Fail("Metal submission has no valid committed command on the owner queue");
        if (Full()) {
            NativeMetalCost::Scope scope(NativeMetalCost::QueueTrackFull);
            while (Full())
                Retire(true, true);
        }

        const auto slot = (first + count) & (MaxCapacity - 1);
        if (NativeMetalCost::Enabled()) {
            static_assert(std::tuple_size_v<decltype(in_flight)> ==
                          std::tuple_size_v<decltype(queue_profile.producer)>);
            if (queue_profile.owner != this)
                queue_profile = {.owner = this, .window = Limit()};
            queue_profile.producer[slot] = NativeMetalCost::queue_request;
            queue_profile.submit_refresh[slot] = VideoCore::GetCurrentFrame();
        }
        auto& submitted = in_flight[slot];
        submitted = ticket;
        ++count;
        return submitted;
    } catch (...) {
        // Callers commit before handing over the ticket. A failed older
        // validation can throw before this command acquires a queue slot.
        // Finish that committed command before exposing the terminal failure;
        // Abort owns the earlier slots, but cannot see this unslotted command.
        if (ticket.command.status >= MTLCommandBufferStatusCommitted &&
            ticket.command.status < MTLCommandBufferStatusCompleted)
            [ticket.command waitUntilCompleted];
        throw;
    }
}

bool PicaMetalSubmissionQueue::IsTrackedProducer(id<MTLCommandBuffer> command) const {
    CheckThread();
    if (!command || command.commandQueue != queue)
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (in_flight[(first + i) & (MaxCapacity - 1)].command == command)
            return true;
    }
    return false;
}

std::shared_ptr<const MetalProducerPredecessors>
PicaMetalSubmissionQueue::Predecessors() const {
    CheckThread();
    // A dependency snapshot may describe every occupied slot. Callers that
    // submit new GPU commands must reserve their slot before encoding the lease.
    if (count == 0)
        return {};
    MetalProducerPredecessors::Commands commands{};
    MetalProducerPredecessors::Validations validations{};
    static_assert(std::tuple_size_v<decltype(in_flight)> ==
                  std::tuple_size_v<decltype(commands)>);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& ticket = in_flight[(first + i) & (MaxCapacity - 1)];
        commands[i] = ticket.command;
        validations[i] = ticket.validation;
    }
    return std::make_shared<MetalProducerPredecessors>(std::move(commands),
                                                       std::move(validations), count);
}

void PicaMetalSubmissionQueue::Drain() {
    CheckThread();
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Drain);
    NativeMetalCost::Scope scope(NativeMetalCost::QueueDrain);
    while (count != 0)
        Retire(true);
}

void PicaMetalSubmissionQueue::Abort() noexcept {
    if (std::this_thread::get_id() != owner)
        std::terminate();
    faulted = true;
    try { reject_writes(); } catch (...) {
        // The queue remains permanently faulted even if an external rejection
        // callback fails during stack unwinding. Destruction must not throw.
    }
    // Unlike normal retirement, failure cleanup must not throw on a failed
    // command or publish a completion as usable guest data.
    while (count) {
        auto& ticket = in_flight[first];
        if (ticket.command.status >= MTLCommandBufferStatusCommitted &&
            ticket.command.status < MTLCommandBufferStatusCompleted)
            [ticket.command waitUntilCompleted];
        MetalDrawStageProfile::Cancel(ticket.command);
        ticket = {};
        first = (first + 1) & (MaxCapacity - 1);
        --count;
    }
}

} // namespace Cara
