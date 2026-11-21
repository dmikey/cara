// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace Cara {

struct MetalQueueSample {
    std::uint64_t p99_ns;
    std::uint64_t queue_wait_ns;
    std::uint64_t upload_wait_ns;
    bool resource_pressure;
};

class MetalQueuePolicy {
public:
    static constexpr std::size_t MinSlots = 8;
    static constexpr std::size_t MaxSlots = 32;
    static constexpr std::size_t Window = 120;

    explicit MetalQueuePolicy(std::size_t initial) : slots(initial) {}

    std::size_t Slots() const { return slots; }

    // Wait values are per-refresh averages. Trial the next level and retain
    // it when queue pressure falls without worsening frame or upload waits.
    const char* Update(const MetalQueueSample sample) {
        if (sample.resource_pressure) {
            trial.reset();
            cooldown = RetryWindows;
            if (slots > MinSlots) {
                slots /= 2;
                return "resource-pressure";
            }
            return nullptr;
        }

        if (trial) {
            const auto baseline = *trial;
            trial.reset();
            if (sample.p99_ns > baseline.p99_ns + FrameTolerance ||
                sample.upload_wait_ns > baseline.upload_wait_ns + WaitTolerance ||
                (sample.p99_ns >= baseline.p99_ns &&
                 sample.queue_wait_ns + WaitTolerance / 2 > baseline.queue_wait_ns)) {
                slots /= 2;
                cooldown = RetryWindows;
                return "rollback";
            }
            return "accepted";
        }

        if (cooldown) {
            --cooldown;
            return nullptr;
        }
        if (slots < MaxSlots && sample.queue_wait_ns >= WaitTolerance &&
            sample.upload_wait_ns < sample.queue_wait_ns) {
            trial = sample;
            slots *= 2;
            return "trial";
        }
        return nullptr;
    }

private:
    static constexpr std::uint64_t WaitTolerance = 250'000;
    static constexpr std::uint64_t FrameTolerance = 500'000;
    static constexpr unsigned RetryWindows = 3;
    std::size_t slots;
    std::optional<MetalQueueSample> trial;
    unsigned cooldown = 0;
};

} // namespace Cara
