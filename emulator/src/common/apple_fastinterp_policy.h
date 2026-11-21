// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 Cara contributors.
#pragma once
#if defined(__APPLE__)
#include <cstdlib>
#include "common/apple_hardware.h"

namespace Common {
// Precompiled FastInterp paths measured on Apple Silicon. Preserve explicit
// overrides and the guest timing policy.
inline void ApplyAppleFastInterpDefaults(std::uint64_t memory_bytes = ApplePhysicalMemoryBytes()) {
    struct Option {
        const char* name;
        const char* value;
    };
    constexpr Option options[]{
        {"CARA_FASTINTERP_ALU_OPERANDS", "1"},
        {"CARA_FASTINTERP_TICK_BUCKETS", "1"},
        {"CARA_FASTINTERP_CHAIN_BUDGET", "1"},
        {"CARA_FASTINTERP_CACHE_WAYS", "8"},
        {"CARA_FASTINTERP_RAM_BURST", "1"},
        {"CARA_FASTINTERP_AL_DISPATCH", "1"},
        {"CARA_FASTINTERP_ALU_PAIRS", "1"},
    };
    for (const auto& option : options)
        setenv(option.name, option.value, 0);
    if (memory_bytes >= 6ULL * 1024 * 1024 * 1024)
        setenv("CARA_FASTINTERP_CACHE_ENTRIES", "32768", 0);
}
} // namespace Common
#endif
