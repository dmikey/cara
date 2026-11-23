// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Cara::RepeatInstances {
// Synchronous PICA command processor and rasterizer share one thread. An instance
// is issued only after the submitted input and sampled/output ranges are known.
struct Range {
    std::uint64_t start{}, end{};
    bool Valid() const { return start < end && end <= (1ULL << 32); }
    bool Intersects(Range other) const { return start < other.end && other.start < end; }
};
struct Pair {
    Range commands;
    std::array<Range, 18> inputs{};
    std::size_t count{};
    bool allowed{}, issued{}, depth_idempotent{}, elided{};
    void Read(Range r) {
        if (!r.Valid() || count == inputs.size()) allowed = false;
        else inputs[count++] = r;
    }
    bool Independent(Range output) const {
        if (!output.Valid() || output.Intersects(commands)) return false;
        for (std::size_t i=0; i<count; ++i) if (output.Intersects(inputs[i])) return false;
        return true;
    }
};
inline thread_local Pair* pending = nullptr;
inline thread_local const std::uint32_t* skip = nullptr;
inline thread_local Range list_range;
inline bool ElideDepthDuplicates() {
    static const bool on = std::getenv("CARA_ELIDE_DEPTH_DUPLICATES") &&
                           !std::strcmp(std::getenv("CARA_ELIDE_DEPTH_DUPLICATES"), "1");
    return on;
}
inline bool Enabled() {
    static const bool on = std::getenv("CARA_REPEAT_INSTANCES") &&
                           !std::strcmp(std::getenv("CARA_REPEAT_INSTANCES"), "1");
    return on || ElideDepthDuplicates();
}
} // namespace Cara::RepeatInstances
