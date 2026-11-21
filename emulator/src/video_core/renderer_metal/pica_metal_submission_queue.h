// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <thread>
#include "video_core/renderer_metal/pica_metal_draw_group.h"
#include "video_core/renderer_metal/pica_metal_surface_writer.h"

namespace Cara {

// Tracks committed Metal work independently of the rasterizer's memory owner.
class PicaMetalSubmissionQueue {
public:
    PicaMetalSubmissionQueue(id<MTLCommandQueue> queue, std::function<void()> reject_writes);
    void CheckThread() const;
    [[noreturn]] void Fail(const char* reason);
    void RejectWrites();
    void Retire(bool wait_for_oldest);
    bool Full() const { return count == in_flight.size(); }
    void Reserve();
    PicaMetalDrawTicket& Track(const PicaMetalDrawTicket& ticket);
    bool IsTrackedProducer(id<MTLCommandBuffer> command) const;
    std::shared_ptr<const MetalProducerPredecessors> Predecessors() const;
    void Drain();

private:
    const std::thread::id owner;
    const id<MTLCommandQueue> queue;
    const std::function<void()> reject_writes;
    // Wait for the oldest ticket when full. No guest work is discarded.
    std::array<PicaMetalDrawTicket, 8> in_flight{};
    std::size_t first = 0;
    std::size_t count = 0;
    bool faulted = false;
};

} // namespace Cara
