// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_guest_authority.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace Cara {

void MetalGuestAuthority::CheckLane() const {
    if (std::this_thread::get_id() != lane)
        throw std::logic_error("Metal guest authority accessed outside its rasterizer lane");
    if (faulted)
        throw std::runtime_error("Metal guest write authority cannot resume after a fault");
}

void MetalGuestAuthority::SplitAt(PAddr position) {
    const auto next = regions.upper_bound(position);
    if (next == regions.begin())
        return;
    const auto containing = std::prev(next);
    if (position <= containing->first || position >= containing->second.end)
        return;
    regions.emplace_hint(next, position, containing->second);
    containing->second.end = position;
}

void MetalGuestAuthority::Replace(PAddr begin, PAddr end, const MetalGuestWriterRef& writer) {
    if (!materialized.empty())
        materialized.subtract(MaterializedIntervals::interval_type::right_open(begin, end));
    // Known-exact replacement already owns a map node for these bounds.
    // Fall through if replacing it must change the canonical coalescing.
    static const bool reuse_exact_write = [] {
        const auto* value = std::getenv("CARA_METAL_REUSE_EXACT_WRITE_NODE");
        return value && std::string_view(value) == "1";
    }();
    if (reuse_exact_write && writer) {
        const auto exact = regions.find(begin);
        if (exact != regions.end() && exact->second.end == end) {
            const auto left = exact == regions.begin() ? regions.end() : std::prev(exact);
            const auto right = std::next(exact);
            const bool join_left = left != regions.end() && left->second.end == begin &&
                                   left->second.writer == writer;
            const bool join_right = right != regions.end() && right->first == end &&
                                    right->second.writer == writer;
            if (!join_left && !join_right) {
                exact->second.writer = writer;
                NativeMetalCost::Count(NativeMetalCost::ExactWriteNodeReuse, 1);
                return;
            }
        }
    }
    SplitAt(begin);
    SplitAt(end);
    const auto next = regions.erase(regions.lower_bound(begin), regions.lower_bound(end));
    if (!writer)
        return;

    // Join only identical leases, never merely matching addresses or formats.
    const auto previous = next == regions.begin() ? regions.end() : std::prev(next);
    const bool join_left = previous != regions.end() && previous->second.end == begin &&
                           previous->second.writer == writer;
    const bool join_right = next != regions.end() && next->first == end &&
                            next->second.writer == writer;
    const PAddr last = join_right ? next->second.end : end;
    if (join_left) {
        previous->second.end = last;
    } else {
        regions.emplace_hint(next, begin, Region{last, writer});
    }
    if (join_right)
        regions.erase(next);
}

void MetalGuestAuthority::Publish(std::span<const MetalGuestWrite> writes) {
    CheckLane();
    // A committed producer cannot be replayed after a failed publication.
    faulted = true;
    for (const auto& write : writes) {
        if (write.begin >= write.end || !write.writer ||
            !write.writer->Covers(write.begin, write.end) ||
            write.writer->Status() == MetalGuestWriteStatus::Failed)
            throw std::runtime_error("Invalid committed Metal guest write");
    }
    for (const auto& write : writes)
        Replace(write.begin, write.end, write.writer);
    faulted = false;
}

void MetalGuestAuthority::Invalidate(PAddr begin, PAddr end) {
    CheckLane();
    if (begin > end)
        throw std::invalid_argument("Invalid guest overwrite interval");
    if (begin == end)
        return;
    faulted = true;
    Replace(begin, end, {});
    faulted = false;
}

std::vector<MetalGuestWrite> MetalGuestAuthority::NativeSegments(PAddr begin, PAddr end) const {
    CheckLane();
    if (begin > end)
        throw std::invalid_argument("Invalid guest read interval");
    std::vector<MetalGuestWrite> result;
    if (begin == end)
        return result;

    auto it = regions.lower_bound(begin);
    if (it != regions.begin() && std::prev(it)->second.end > begin)
        --it;
    for (; it != regions.end() && it->first < end; ++it) {
        const auto first = std::max(begin, it->first);
        const auto last = std::min(end, it->second.end);
        const auto& writer = it->second.writer;
        if (!writer->Covers(first, last) || writer->Status() == MetalGuestWriteStatus::Failed)
            throw std::runtime_error("Guest read depends on an invalid Metal write");
        result.push_back({first, last, writer});
    }
    return result;
}

std::vector<MetalGuestWrite> MetalGuestAuthority::UnmaterializedSegments(
    const MetalGuestWrite& piece) const {
    CheckLane();
    if (materialized.empty())
        return {piece};
    MaterializedIntervals missing;
    const auto range = MaterializedIntervals::interval_type::right_open(piece.begin, piece.end);
    missing.add(range);
    for (auto it = materialized.lower_bound(range);
         it != materialized.end() && boost::icl::lower(*it) < piece.end; ++it) {
        missing.subtract(*it);
    }
    std::vector<MetalGuestWrite> result;
    for (const auto& interval : missing)
        result.push_back({boost::icl::first(interval), boost::icl::last(interval) + 1,
                          piece.writer});
    return result;
}

void MetalGuestAuthority::MarkMaterialized(PAddr begin, PAddr end) {
    CheckLane();
    materialized.add(MaterializedIntervals::interval_type::right_open(begin, end));
}

MetalGuestWrite MetalGuestAuthority::OwningExtent(const MetalGuestWrite& piece) const {
    CheckLane();
    const auto after = regions.upper_bound(piece.begin);
    if (after != regions.begin()) {
        const auto owner = std::prev(after);
        if (owner->second.writer == piece.writer && piece.end <= owner->second.end)
            return {owner->first, owner->second.end, piece.writer};
    }
    return piece;
}

bool MetalGuestAuthority::ReadIntoScratch(PAddr begin, std::span<u8> output,
                                         const GuestReader& read_guest) {
    CheckLane();
    if (!read_guest || output.size() > std::numeric_limits<PAddr>::max() - begin)
        return false;
    const auto end = begin + static_cast<PAddr>(output.size());
    const auto bytes = [&](PAddr first, PAddr last) {
        return output.subspan(first - begin, last - first);
    };
    auto cursor = begin;
    for (const auto& piece : NativeSegments(begin, end)) {
        if (cursor < piece.begin && !read_guest(cursor, bytes(cursor, piece.begin)))
            return false;
        if (!piece.writer->ExportGuestBytes(piece.begin, piece.end, bytes(piece.begin, piece.end))) {
            RejectSubmittedWrites();
            return false;
        }
        cursor = piece.end;
    }
    return cursor == end || read_guest(cursor, bytes(cursor, end));
}

} // namespace Cara
