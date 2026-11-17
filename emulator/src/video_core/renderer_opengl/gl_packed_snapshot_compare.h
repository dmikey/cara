// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace OpenGL::PackedSnapshotCompare {
// Standalone prototype: no production caller. Sources must be validated readable
// spans, held alive and stable for this synchronous call. No pointer identity,
// hash, page flag or generation can establish equality.
inline constexpr std::size_t MaxSegments = 17; // constants + 16 merged ranges
inline constexpr std::size_t MaxSnapshotBytes = 8 * 1024 * 1024;
struct Segment {
    const std::uint8_t* source = nullptr;
    std::size_t available = 0;
    std::size_t destination = 0;
    std::size_t size = 0;
};
enum class Padding { Unknown, Zero };
enum class Decision { Invalid, Different, Equal };
struct Result {
    Decision decision = Decision::Invalid;
    // Requested comparison lengths, not bytes actually examined by memcmp.
    std::size_t payload_bytes_requested = 0;
    std::size_t padding_bytes_requested = 0;
};
inline bool AddressFits(const void* pointer, std::size_t size) {
    return pointer && size <= std::numeric_limits<std::uintptr_t>::max() -
                                  reinterpret_cast<std::uintptr_t>(pointer);
}
inline Result Compare(const Segment* segments, std::size_t count,
                      std::size_t snapshot_size, Padding padding,
                      const std::uint8_t* shadow, std::size_t shadow_size) {
    Result result;
    if (!segments || !count || count > MaxSegments || !snapshot_size ||
        snapshot_size > MaxSnapshotBytes || shadow_size != snapshot_size ||
        !AddressFits(shadow, shadow_size) || padding != Padding::Zero) return result;
    std::array<Segment, MaxSegments> ordered{};
    // Validate the entire description before touching either byte payload.
    for (std::size_t i = 0; i < count; ++i) {
        const auto& s = segments[i];
        if (!s.size || s.size > s.available || !AddressFits(s.source, s.size) ||
            s.destination > snapshot_size || s.size > snapshot_size - s.destination)
            return result;
        ordered[i] = s;
    }
    std::sort(ordered.begin(), ordered.begin() + count,
              [](const Segment& a, const Segment& b) { return a.destination < b.destination; });
    std::size_t end = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (ordered[i].destination < end) return result;
        end = ordered[i].destination + ordered[i].size;
    }
    result.decision = Decision::Different;
    const auto zero = [&](std::size_t begin, std::size_t finish) {
        result.padding_bytes_requested += finish - begin;
        for (auto i = begin; i < finish; ++i) if (shadow[i] != 0) return false;
        return true;
    };
    end = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& s = ordered[i];
        if (!zero(end, s.destination)) return result;
        result.payload_bytes_requested += s.size;
        if (std::memcmp(shadow + s.destination, s.source, s.size)) return result;
        end = s.destination + s.size;
    }
    if (zero(end, snapshot_size)) result.decision = Decision::Equal;
    return result;
}
} // namespace OpenGL::PackedSnapshotCompare
