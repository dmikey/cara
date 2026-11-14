// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace OpenGL::ValidationCopyProfile {
// Independent of the common timing gate. Frame numbers are original VideoCore frames.
constexpr bool InWindow(std::uint32_t frame, std::uint32_t start, std::uint32_t end) {
    return frame >= start && frame < end;
}
template <std::size_t Capacity>
struct SlotBudget {
    std::size_t used = 0;
    bool Take() {
        if (used == Capacity) return false;
        ++used;
        return true;
    }
};
struct Elapsed {
    bool valid = false;
    std::uint64_t ns = 0;
};
constexpr Elapsed Delta(bool before_ok, std::uint64_t before, bool after_ok,
                        std::uint64_t after) {
    return {before_ok && after_ok && after >= before,
            before_ok && after_ok && after >= before ? after - before : 0};
}
struct Totals {
    std::uint64_t calls = 0, guest_bytes = 0, wall_ns = 0, thread_cpu_ns = 0;
    std::uint64_t wall_samples = 0, cpu_samples = 0, wall_failures = 0, cpu_failures = 0;
    std::uint64_t saturations = 0;
    void AddInteger(std::uint64_t& target, std::uint64_t value) {
        if (value > std::numeric_limits<std::uint64_t>::max() - target) {
            target = std::numeric_limits<std::uint64_t>::max();
            if (saturations != std::numeric_limits<std::uint64_t>::max()) ++saturations;
        } else {
            target += value;
        }
    }
    void Add(std::uint64_t bytes, Elapsed wall, Elapsed cpu) {
        AddInteger(calls, 1);
        AddInteger(guest_bytes, bytes);
        AddInteger(wall.valid ? wall_samples : wall_failures, 1);
        AddInteger(cpu.valid ? cpu_samples : cpu_failures, 1);
        if (wall.valid) AddInteger(wall_ns, wall.ns);
        if (cpu.valid) AddInteger(thread_cpu_ns, cpu.ns);
    }
};
} // namespace OpenGL::ValidationCopyProfile
