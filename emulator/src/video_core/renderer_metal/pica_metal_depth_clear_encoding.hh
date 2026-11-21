// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include <cstdint>
#include <utility>

namespace Cara {

// Installed only while the native lane appends the first draw for this
// exact attachment. The renderer's other draw paths have no clear request.
struct PicaDepthClearEncoding {
    id<MTLCommandQueue> queue;
    id<MTLTexture> depth;
    std::uint32_t word;
    bool encoded = false;
};

inline thread_local PicaDepthClearEncoding* active_depth_clear = nullptr;

class ScopedPicaDepthClearEncoding {
public:
    explicit ScopedPicaDepthClearEncoding(PicaDepthClearEncoding* clear)
        : previous(std::exchange(active_depth_clear, clear)) {}
    ~ScopedPicaDepthClearEncoding() { active_depth_clear = previous; }

    ScopedPicaDepthClearEncoding(const ScopedPicaDepthClearEncoding&) = delete;
    ScopedPicaDepthClearEncoding& operator=(const ScopedPicaDepthClearEncoding&) = delete;

private:
    PicaDepthClearEncoding* previous;
};

} // namespace Cara
