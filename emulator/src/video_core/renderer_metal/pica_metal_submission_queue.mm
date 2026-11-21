// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_submission_queue.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/pica_metal_draw_stage_profile.hh"

#include <stdexcept>
#include <utility>

namespace Cara {
namespace {

// The native Gate uses one active submission lane per worker. If profiling
// switches to a different lane, old entries report unknown instead of a stale label.
struct QueueProfile {
    const PicaMetalSubmissionQueue* owner = nullptr;
    std::array<NativeMetalCost::QueueWork, 8> producer{};
};
thread_local QueueProfile queue_profile;

} // namespace

PicaMetalSubmissionQueue::PicaMetalSubmissionQueue(id<MTLCommandQueue> queue_,
                                                   std::function<void()> reject_writes_)
    : owner(std::this_thread::get_id()), queue(queue_), reject_writes(std::move(reject_writes_)) {
    if (!queue || !reject_writes)
        throw std::invalid_argument("Metal submissions need a queue and an owning memory fault handler");
    if (NativeMetalCost::Enabled())
        queue_profile = {.owner = this};
}

void PicaMetalSubmissionQueue::RejectWrites() {
    faulted = true;
    reject_writes();
}

void PicaMetalSubmissionQueue::Reserve() {
    CheckThread();
    if (Full()) {
        NativeMetalCost::Scope scope(NativeMetalCost::QueueReserveFull);
        Retire(true);
    }
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

void PicaMetalSubmissionQueue::Retire(bool wait_for_oldest) {
    bool wait = wait_for_oldest;
    while (count != 0) {
        auto& ticket = in_flight[first];
        std::uint64_t queue_wait_ns = 0;
        auto state = ticket.Poll();
        if (wait && state == PicaMetalCompletion::Pending) {
            using namespace NativeMetalCost;
            const bool profile = Enabled();
            const auto start = profile ? Scope::Clock::now() : Scope::Clock::time_point{};
            state = ticket.WaitUntilGuestRead();
            if (profile) {
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Scope::Clock::now() - start).count();
                const auto producer = queue_profile.owner == this ? queue_profile.producer[first]
                                                                 : QueueWork::Other;
                queue_wait_ns = ns;
                RecordNs(ReserveWait, ns);
                RecordNs(WaitSite(queue_request), ns);
                RecordNs(WaitOldest(producer), ns);
            }
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
            MetalDrawStageProfile::Retire(ticket.command, ticket.count, queue_wait_ns, valid, ns);
        }
        wait = false;
        ticket = {};
        first = (first + 1) % in_flight.size();
        --count;
    }
}

PicaMetalDrawTicket& PicaMetalSubmissionQueue::Track(const PicaMetalDrawTicket& ticket) {
    CheckThread();
    if (!ticket.command || ticket.command.commandQueue != queue ||
        ticket.Poll() == PicaMetalCompletion::Failed)
        Fail("Metal submission has no valid committed command on the owner queue");
    if (Full()) {
        NativeMetalCost::Scope scope(NativeMetalCost::QueueTrackFull);
        Retire(true);
    }

    const auto slot = (first + count) % in_flight.size();
    if (NativeMetalCost::Enabled()) {
        static_assert(std::tuple_size_v<decltype(in_flight)> ==
                      std::tuple_size_v<decltype(queue_profile.producer)>);
        if (queue_profile.owner != this)
            queue_profile = {.owner = this};
        queue_profile.producer[slot] = NativeMetalCost::queue_request;
    }
    auto& submitted = in_flight[slot];
    submitted = ticket;
    ++count;
    return submitted;
}

bool PicaMetalSubmissionQueue::IsTrackedProducer(id<MTLCommandBuffer> command) const {
    CheckThread();
    if (!command || command.commandQueue != queue)
        return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (in_flight[(first + i) % in_flight.size()].command == command)
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
    auto predecessors = std::make_shared<MetalProducerPredecessors>();
    static_assert(std::tuple_size_v<decltype(in_flight)> ==
                  std::tuple_size_v<decltype(predecessors->commands)>);
    predecessors->count = count;
    for (std::size_t i = 0; i < count; ++i)
        predecessors->commands[i] = in_flight[(first + i) % in_flight.size()].command;
    return predecessors;
}

void PicaMetalSubmissionQueue::Drain() {
    CheckThread();
    NativeMetalCost::ScopeQueueWork work(NativeMetalCost::QueueWork::Drain);
    NativeMetalCost::Scope scope(NativeMetalCost::QueueDrain);
    while (count != 0)
        Retire(true);
}

} // namespace Cara
