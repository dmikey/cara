// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <thread>
#include <string>
#include "video_core/renderer_metal/pica_metal_draw_group.h"
#include "video_core/renderer_metal/pica_metal_surface_writer.h"

namespace Cara {

// Bounded, diagnostic-only command timing coverage; not a GPU utilization counter.
void ReportNativeMetalCommandTimeline(const std::string& path);

// Tracks committed Metal work independently of the rasterizer's memory owner.
class PicaMetalSubmissionQueue {
public:
    static constexpr std::size_t Capacity = 8;
    static constexpr std::size_t MaxCapacity = MetalProducerMaxPredecessors;
    PicaMetalSubmissionQueue(id<MTLCommandQueue> queue, std::function<void()> reject_writes);
    void CheckThread() const;
    [[noreturn]] void Fail(const char* reason);
    void RejectWrites();
    void Retire(bool wait_for_oldest);
    bool Full() const { return count == limit; }
    std::size_t Limit() const noexcept { return limit; }
    void Reserve();
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
    const std::size_t limit;
    std::array<PicaMetalDrawTicket, MaxCapacity> in_flight{};
    std::size_t first = 0;
    std::size_t count = 0;
    bool faulted = false;
};

} // namespace Cara
