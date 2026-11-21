// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace Pica::Shader::Detail {

// Zero means unavailable; MAX is a permanent exhausted sentinel. Never wrap.
inline std::uint64_t AllocateIdentity(std::atomic<std::uint64_t>& next) {
    auto value = next.load(std::memory_order_relaxed);
    while (value && value != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) {
            return value;
        }
    }
    return 0;
}

} // namespace Pica::Shader::Detail
