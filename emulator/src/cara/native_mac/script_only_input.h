// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <SDL_events.h>

namespace Cara::NativeScriptOnlyInput {
inline bool ExactOne(const char* value) { return value && std::strcmp(value, "1") == 0; }
inline bool Flags(const char* requested, const char* diagnostics, const char* benchmark) {
    return ExactOne(requested) && ExactOne(diagnostics) && ExactOne(benchmark);
}
inline bool Enabled() {
    // Startup-only configuration; window setup and profile snapshots share this value.
    static const bool enabled = Flags(std::getenv("CARA_NATIVE_SCRIPT_ONLY_INPUT"),
        std::getenv("CARA_NATIVE_DIAGNOSTICS"), std::getenv("CARA_NATIVE_BENCHMARK"));
    return enabled;
}
inline bool PhysicalEvent(std::uint32_t type) {
    switch (type) {
    case SDL_KEYDOWN: case SDL_KEYUP:
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
    case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP:
        return true;
    default: return false;
    }
}
// Only the test script owns touch across a focus transition in this mode.
// Close/quit still release touch and stop in the original event handlers.
inline bool ReleaseTouchOnFocusLost(bool enabled) { return !enabled; }
inline std::uint64_t ignored_physical_event_count = 0;
inline bool Ignore(bool enabled, std::uint32_t type, std::uint64_t& count) {
    if (!enabled || !PhysicalEvent(type)) return false;
    if (count != std::numeric_limits<std::uint64_t>::max()) ++count;
    return true;
}
inline bool Ignore(std::uint32_t type) {
    return Ignore(Enabled(), type, ignored_physical_event_count);
}
} // namespace Cara::NativeScriptOnlyInput
