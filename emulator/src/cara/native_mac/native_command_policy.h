// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>
namespace NativeMac::DeveloperCommands {
inline bool One(const char* value) { return value && std::strcmp(value,"1")==0; }
inline bool Enabled(const char* request,const char* diagnostics) {
    return One(request) && One(diagnostics);
}
inline bool InvalidRequest(const char* request,const char* diagnostics) {
    return One(request) && !One(diagnostics);
}
inline bool EnabledFromEnvironment() {
    return Enabled(std::getenv("CARA_NATIVE_DEV_COMMANDS"),std::getenv("CARA_NATIVE_DIAGNOSTICS"));
}
} // namespace NativeMac::DeveloperCommands
