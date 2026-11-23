// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 Cara contributors.
#pragma once
#if defined(__APPLE__)
#include <pthread/qos.h>

namespace Common {
struct AppleEmulationQoS {
    qos_class_t initial;
    qos_class_t requested;
    qos_class_t effective;
    int result;
};

// Active emulation is user-requested latency-sensitive work. Leave P/E-core
// placement to the OS and preserve an already-interactive main thread. Call on
// the owning emulation thread, not on import/cache/background workers.
// https://developer.apple.com/documentation/apple-silicon/tuning-your-code-s-performance-for-apple-silicon
inline AppleEmulationQoS ApplyAppleEmulationQoS(bool interactive_diagnostic = false) {
    const auto initial=qos_class_self();
    const auto requested=(interactive_diagnostic || initial==QOS_CLASS_USER_INTERACTIVE)
        ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_USER_INITIATED;
    const int result=pthread_set_qos_class_self_np(requested,0);
    return {initial,requested,qos_class_self(),result};
}
} // namespace Common
#endif
