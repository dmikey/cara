// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging/log.h"

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace OpenGL::ShaderCensus {

using Clock = std::chrono::steady_clock;

inline bool Enabled() {
    const char* value = std::getenv("CARA_GL_SHADER_CENSUS");
    return value && std::strcmp(value, "1") == 0;
}

// Captured as host wall time, including any blocking driver status query.
template <typename... Args>
void Record(const char* format, Args&&... args) {
    const auto message = fmt::format(format, std::forward<Args>(args)...);
#if defined(__ANDROID__)
    __android_log_write(ANDROID_LOG_INFO, "CaraShader", message.c_str());
#else
    LOG_INFO(Render_OpenGL, "CaraShader {}", message);
#endif
}

inline auto ElapsedUs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

class FirstDraw {
public:
    FirstDraw(bool enabled, std::unordered_set<u32>& seen, u32 program) : program(program) {
        if (enabled && program && seen.insert(program).second)
            start = Clock::now();
    }

    ~FirstDraw() {
        if (start != Clock::time_point{})
            Record("first_draw program={} us={}", program, ElapsedUs(start));
    }

private:
    const u32 program;
    Clock::time_point start{};
};

} // namespace OpenGL::ShaderCensus
