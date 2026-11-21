// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <span>
#include "video_core/renderer_metal/pica_metal_guest_authority.h"
#include "video_core/renderer_metal/pica_transfer_plan.h"

namespace Cara {

// The input comes from NativeSegments over this footprint. The caller checks Metal
// backing, queue membership, completion status, full buffer range and alignment.
inline bool HasOneStripedSourceOwner(const MetalTransfer::Rows& rows,
                                     std::span<const MetalGuestWrite> segments) {
    if (rows.count < 2 || segments.empty() || !segments.front().writer)
        return false;
    const auto& writer = segments.front().writer;
    auto owner = segments.begin();
    for (u32 i = 0; i < rows.count; ++i) {
        const auto stripe = rows.Stripe(i);
        auto cursor = stripe.begin;
        while (cursor < stripe.end) {
            while (owner != segments.end() && owner->end <= cursor)
                ++owner;
            if (owner == segments.end() || owner->begin > cursor || owner->writer != writer)
                return false;
            cursor = std::min(stripe.end, owner->end);
        }
    }
    return true;
}

} // namespace Cara
