// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_guest_authority.h"
#include "common/cpu_category_timing.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
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
    {
        Common::CPUCategoryTiming::Scope cpu(Common::CPUCategoryTiming::Category::PublicationValidate);
        for (const auto& write : writes) {
            if (write.begin >= write.end || !write.writer ||
                !write.writer->Covers(write.begin, write.end) ||
                write.writer->Status() == MetalGuestWriteStatus::Failed)
                throw std::runtime_error("Invalid committed Metal guest write");
        }
    }
    {
        Common::CPUCategoryTiming::Scope cpu(Common::CPUCategoryTiming::Category::PublicationUpdate);
        static const bool reuse_exact = [] {
            const char* value = std::getenv("CARA_METAL_REUSE_OWNERSHIP_PUBLICATIONS");
            return value && std::string_view(value) == "1";
        }();
        if (reuse_exact && !writes.empty()) {
            auto position = regions.end();
            for (const auto& write : writes) {
                if (position != regions.end() && position->second.end <= write.begin)
                    ++position; // One untouched interval may separate the stripes.
                if (position == regions.end() || position->first != write.begin)
                    position = regions.lower_bound(write.begin);
                if (position != regions.end() && position->first == write.begin &&
                    position->second.end == write.end) {
                    const auto next = std::next(position);
                    const auto previous = position == regions.begin() ? regions.end()
                                                                       : std::prev(position);
                    const bool joins_left = previous != regions.end() &&
                        previous->second.end == write.begin && previous->second.writer == write.writer;
                    const bool joins_right = next != regions.end() &&
                        next->first == write.end && next->second.writer == write.writer;
                    if (!joins_left && !joins_right) {
                        // Reuse the existing node, including its key and exact byte bounds.
                        if (!materialized.empty())
                            materialized.subtract(
                                MaterializedIntervals::interval_type::right_open(write.begin, write.end));
                        position->second.writer = write.writer;
                        position = next;
                        continue;
                    }
                }
                Replace(write.begin, write.end, write.writer);
                // Replace can erase or join either neighbour. Invalidate the cursor.
                position = regions.end();
            }
        } else {
            for (const auto& write : writes)
                Replace(write.begin, write.end, write.writer);
        }
    }
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

    static const bool reuse_scan_status = [] {
        const auto* value = std::getenv("CARA_METAL_REUSE_SCAN_STATUS");
        return value && std::string_view(value) == "1";
    }();
    // Only this immutable lane-local map snapshot may reuse observations.
    // Covers still validates every clipped interval. Recheck both remembered
    // producers before returning, including those that were Pending initially:
    // a failed command must never be hidden by a cached Pending observation.
    std::array<const MetalGuestWriter*, 2> checked{};
    std::size_t checked_count = 0;
    auto it = regions.lower_bound(begin);
    if (it != regions.begin() && std::prev(it)->second.end > begin)
        --it;
    for (; it != regions.end() && it->first < end; ++it) {
        const auto first = std::max(begin, it->first);
        const auto last = std::min(end, it->second.end);
        const auto& writer = it->second.writer;
        const bool known = reuse_scan_status &&
            ((checked_count > 0 && checked[0] == writer.get()) ||
             (checked_count > 1 && checked[1] == writer.get()));
        if (!writer->Covers(first, last) ||
            (!known && writer->Status() == MetalGuestWriteStatus::Failed))
            throw std::runtime_error("Guest read depends on an invalid Metal write");
        if (reuse_scan_status && !known && checked_count < checked.size())
            checked[checked_count++] = writer.get();
        if (known) {
            static thread_local bool reported = false;
            if (!reported) {
                const auto* diagnostics = std::getenv("CARA_NATIVE_DIAGNOSTICS");
                if (diagnostics && std::string_view(diagnostics) == "1")
                    std::fputs("CARA_METAL_SCAN_STATUS_REUSE active=1\n", stderr);
                reported = true;
            }
        }
        result.push_back({first, last, writer});
    }
    for (std::size_t i = 0; i < checked_count; ++i) {
        if (checked[i]->Status() == MetalGuestWriteStatus::Failed)
            throw std::runtime_error("Guest read depends on an invalid Metal write");
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
