// SPDX-License-Identifier: GPL-2.0-or-later
// The standalone executable has stderr and no Android application logging backend.
#include <cstdio>
#include "common/logging/log.h"

void Log::FmtLogMessageImpl(Log::Class, Log::Level, const char* file, unsigned int line,
                            const char* function, const char* format, const fmt::format_args& args) {
    std::fprintf(stderr, "%s:%u %s: %s\n", file, line, function, fmt::vformat(format, args).c_str());
}
