// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Cara::LutEffectiveChange {
enum class Kind : std::size_t { Lighting, Fog, ProcTex, Count };
struct Decision {
    bool changed;
    bool notify;
    bool valid_target;
};
// Compare the actual indexed target, never the backing PICA data-port register.
// The raw payload is always stored, including bits unused by float conversion.
inline Decision StoreRaw(std::uint32_t& raw, std::uint32_t value, bool enabled, bool observe) {
    const bool changed = (enabled || observe) && raw != value;
    raw = value;
    return {changed, !enabled || changed, true};
}
constexpr Decision InvalidTarget() { return {false, true, false}; }
struct Counts { std::uint64_t writes{}, changed{}, notifications{}, invalid_targets{}; };
using Snapshot = std::array<Counts, static_cast<std::size_t>(Kind::Count)>;
struct AtomicCounts {
    std::atomic<std::uint64_t> writes{}, changed{}, notifications{}, invalid_targets{};
};
inline std::array<AtomicCounts, static_cast<std::size_t>(Kind::Count)> counters{};
// Called only under the separate profile switch. Relaxed atomics allow safe
// native diagnostic snapshots; field reads are not a transactional boundary.
inline void Record(Kind kind, Decision decision) {
    auto& c = counters[static_cast<std::size_t>(kind)];
    c.writes.fetch_add(1, std::memory_order_relaxed);
    if (decision.changed) c.changed.fetch_add(1, std::memory_order_relaxed);
    if (decision.notify) c.notifications.fetch_add(1, std::memory_order_relaxed);
    if (!decision.valid_target) c.invalid_targets.fetch_add(1, std::memory_order_relaxed);
}
inline Snapshot ReadCounters() {
    Snapshot result{};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& c = counters[i];
        result[i] = {c.writes.load(std::memory_order_relaxed),
            c.changed.load(std::memory_order_relaxed),
            c.notifications.load(std::memory_order_relaxed),
            c.invalid_targets.load(std::memory_order_relaxed)};
    }
    return result;
}
} // namespace Cara::LutEffectiveChange
