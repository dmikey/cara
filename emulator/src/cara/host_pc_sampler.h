// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#ifndef CARA_INTERNAL_HOST_PC_SAMPLER
#error "Host PC sampler is internal diagnostic code; enable its default-off build option."
#endif
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Cara {
// Construct and destroy on the registered target thread. Never transfer ownership.
// Every record describes wall-time PC residency, NOT on-CPU attribution.
class HostPcSampler final {
public:
    struct Options {
        std::uint64_t start_frame = 300;
        std::uint64_t end_frame = 1200;
        std::size_t capacity = 6000;
        double max_seconds = 60;
    };
    static bool CompiledAvailable();
    explicit HostPcSampler(bool enabled);
    HostPcSampler(bool enabled, Options options);
    ~HostPcSampler();
    HostPcSampler(const HostPcSampler&) = delete;
    HostPcSampler& operator=(const HostPcSampler&) = delete;
    void OnFrame(std::uint64_t frame);
    // Joins before serializing; must run before the registered thread exits.
    bool StopAndWrite(const std::string& path, const std::string& source_fingerprint = "");
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
