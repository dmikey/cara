// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <map>
#include <thread>
#include <vector>
#include <boost/icl/interval_set.hpp>
#include "video_core/renderer_metal/pica_metal_guest_writer.h"

namespace Cara {

struct MetalGuestWrite {
    PAddr begin;
    PAddr end;
    MetalGuestWriterRef writer;
};

// Access on the rasterizer lane, after the caller publishes any pending draws.
// Missing intervals are guest RAM. A lease certifies exact bytes, not that
// an entire texture with a matching address is safe to sample.
class MetalGuestAuthority {
public:
    using GuestReader = std::function<bool(PAddr, std::span<u8>)>;

    MetalGuestAuthority() = default;
    MetalGuestAuthority(const MetalGuestAuthority&) = delete;
    MetalGuestAuthority& operator=(const MetalGuestAuthority&) = delete;

    void Publish(std::span<const MetalGuestWrite> writes);
    // Invalidate only the bytes covered by a successful guest overwrite.
    void Invalidate(PAddr begin, PAddr end);
    std::vector<MetalGuestWrite> NativeSegments(PAddr begin, PAddr end) const;
    // These bytes have already been copied to guest RAM from the current
    // native producer. The producer's lease is still needed for GPU readers.
    std::vector<MetalGuestWrite> UnmaterializedSegments(const MetalGuestWrite& piece) const;
    void MarkMaterialized(PAddr begin, PAddr end);
    // Bounds of the current contiguous producer, including bytes adjacent to
    // a clipped CPU read. A wider export may use only this producer.
    MetalGuestWrite OwningExtent(const MetalGuestWrite& piece) const;

    // The caller owns private scratch and must not expose a failed result to
    // guest RAM. Guest gaps come from the supplied physical-memory reader.
    bool ReadIntoScratch(PAddr begin, std::span<u8> output, const GuestReader& read_guest);
    void RejectSubmittedWrites() noexcept { faulted = true; }

private:
    struct Region {
        PAddr end;
        MetalGuestWriterRef writer;
    };

    void CheckLane() const;
    void SplitAt(PAddr position);
    void Replace(PAddr begin, PAddr end, const MetalGuestWriterRef& writer);
    using MaterializedIntervals = boost::icl::interval_set<PAddr>;
    std::map<PAddr, Region> regions;
    MaterializedIntervals materialized;
    const std::thread::id lane = std::this_thread::get_id();
    bool faulted = false;
};

} // namespace Cara
