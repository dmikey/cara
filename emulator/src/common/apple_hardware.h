// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 Cara contributors.
#pragma once
#if defined(__APPLE__)
#include <cstddef>
#include <cstdint>
#include <sys/sysctl.h>

namespace Common {
// Physical memory is fixed for the lifetime of an Apple device process. Share
// one query across the CPU and renderer startup policies; zero is conservative.
inline std::uint64_t ApplePhysicalMemoryBytes() {
    static const std::uint64_t memory = [] {
        std::uint64_t bytes = 0;
        std::size_t size = sizeof(bytes);
        return sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
    }();
    return memory;
}
} // namespace Common
#endif
