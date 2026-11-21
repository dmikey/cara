// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <thread>
#include "video_core/renderer_metal/pica_metal_queue_policy.h"
#include <string>
#include "video_core/renderer_metal/pica_metal_draw_group.h"
#include "video_core/renderer_metal/pica_metal_surface_writer.h"

namespace Cara {

// An OS memory warning is forwarded to the renderer owner at the next window.
void SignalNativeMetalMemoryPressure();
// Bounded, diagnostic-only command timing coverage.
void ReportNativeMetalCommandTimeline(const std::string& path);

// Tracks committed Metal work independently of the rasterizer's memory owner.
class PicaMetalSubmissionQueue {
public:
    static constexpr std::size_t Capacity = MetalQueuePolicy::MinSlots;
    static constexpr std::size_t MaxCapacity = MetalProducerMaxPredecessors;
    static_assert(MaxCapacity == MetalQueuePolicy::MaxSlots);
    static_assert((MaxCapacity & (MaxCapacity - 1)) == 0);
    PicaMetalSubmissionQueue(id<MTLCommandQueue> queue, std::function<void()> reject_writes);
    void CheckThread() const;
    [[noreturn]] void Fail(const char* reason);
    void RejectWrites();
    void Retire(bool wait_for_oldest, bool capacity_wait = false);
    bool Full() const { return count >= Limit(); }
    std::size_t Limit() const noexcept { return policy.Slots(); }
    void Reserve();
    void EndRefresh(std::uint64_t upload_wait_ns);
    PicaMetalDrawTicket& Track(const PicaMetalDrawTicket& ticket);
    bool IsTrackedProducer(id<MTLCommandBuffer> command) const;
    std::shared_ptr<const MetalProducerPredecessors> Predecessors() const;
    void Drain();
    // Failure-only owner-thread drain. Reject all writes and never resume.
    void Abort() noexcept;

private:
    const std::thread::id owner;
    const id<MTLCommandQueue> queue;
    const std::function<void()> reject_writes;
    // Wait for the oldest ticket when full. No guest work is discarded.
    const bool adaptive;
    MetalQueuePolicy policy;
    using Clock = std::chrono::steady_clock;
    Clock::time_point previous_refresh{};
    std::array<std::uint64_t, MetalQueuePolicy::Window> refresh_ns{};
    std::size_t samples = 0;
    std::uint64_t reserve_wait_ns = 0, window_reserve_wait_ns = 0, upload_wait_total_ns = 0;
    std::uint64_t observed_memory_pressure = 0;
    std::array<PicaMetalDrawTicket, MaxCapacity> in_flight{};
    std::size_t first = 0;
    std::size_t count = 0;
    bool faulted = false;
};

} // namespace Cara
