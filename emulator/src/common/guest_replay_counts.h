// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace Common::GuestReplayCounts {
enum Event : std::size_t { Draw, CommandList, Transfer, Copy, Fill, SoftwareVertex, SkipDraw, Count };
inline std::array<std::atomic<std::uint64_t>, Count> totals{};
inline bool Enabled() {
#if defined(CARA_MAC_CPU_JIT)
    static const bool enabled = [] {
        const auto* a = std::getenv("CARA_NATIVE_REPLAY_LEDGER");
        const auto* b = std::getenv("CARA_NATIVE_DIAGNOSTICS");
        return a && b && std::strcmp(a, "1") == 0 && std::strcmp(b, "1") == 0;
    }();
    return enabled;
#else
    return false;
#endif
}
inline void Observe(Event event) {
    if (Enabled())
        totals[event].fetch_add(1, std::memory_order_relaxed);
}
inline std::array<std::uint64_t, Count> Read() {
    std::array<std::uint64_t, Count> result{};
    for (std::size_t i = 0; i < Count; ++i)
        result[i] = totals[i].load(std::memory_order_relaxed);
    return result;
}
}
