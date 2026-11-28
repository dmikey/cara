// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace Cara::MetalTransfer {
template<class Source> struct CopyRowRun {
    Source source{};
    std::uint64_t from{}, to{}, width{}, source_stride{}, destination_stride{}, count{};
};
template<class Source> struct CopyRowRuns {
    std::array<CopyRowRun<Source>, 2> runs{};
    std::size_t count{};
};

// A private staging destination is disjoint from all sources. Only strictly
// increasing, nonoverlapping destination spans may be regrouped by source.
// This proves the exact word mapping of the existing cara_pica_copy_rows
// kernel; source status/ownership, allocation and queue proofs stay outside.
template<class Source, class Copy>
std::optional<CopyRowRuns<Source>> PlanTwoSourceCopyRows(std::span<const Copy> copies) {
    if (copies.size() < 4)
        return std::nullopt;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    constexpr auto max32 = std::numeric_limits<std::uint32_t>::max();
    CopyRowRuns<Source> result;
    std::array<std::uint64_t, 2> last_from{}, last_to{};
    std::uint64_t previous_end = 0;
    bool first = true;
    for (const auto& copy : copies) {
        if (!copy.size || ((copy.from | copy.to | copy.size) & 3u) ||
            copy.size > max32 || copy.from > max - copy.size ||
            copy.to > max - copy.size || (!first && copy.to < previous_end))
            return std::nullopt;
        first = false;
        previous_end = copy.to + copy.size;
        std::size_t k = 0;
        while (k < result.count && result.runs[k].source != copy.source)
            ++k;
        if (k == result.count) {
            if (k == result.runs.size())
                return std::nullopt;
            ++result.count;
            result.runs[k].source = copy.source;
            result.runs[k].from = copy.from;
            result.runs[k].to = copy.to;
            result.runs[k].width = copy.size;
        }
        auto& run = result.runs[k];
        if (run.width != copy.size || run.count == max32)
            return std::nullopt;
        if (run.count) {
            if (copy.from < last_from[k] || copy.to < last_to[k])
                return std::nullopt;
            const auto source_step = copy.from - last_from[k];
            const auto destination_step = copy.to - last_to[k];
            if (source_step < run.width || destination_step < run.width ||
                source_step > max32 || destination_step > max32)
                return std::nullopt;
            if (run.count == 1) {
                run.source_stride = source_step;
                run.destination_stride = destination_step;
            } else if (source_step != run.source_stride ||
                       destination_step != run.destination_stride) {
                return std::nullopt;
            }
        }
        ++run.count;
        last_from[k] = copy.from;
        last_to[k] = copy.to;
    }
    for (std::size_t k = 0; k < result.count; ++k) {
        const auto& run = result.runs[k];
        if (run.count < 2 || run.count * (run.width / 4) > max32 ||
            last_from[k] - run.from + run.width > max32 ||
            last_to[k] - run.to + run.width > max32)
            return std::nullopt;
    }
    return result;
}
} // namespace Cara::MetalTransfer
