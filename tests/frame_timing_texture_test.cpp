// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/frame_timing.h"
#include <cassert>
#include <cstdio>

int main(int argc, char**) {
    using namespace Common::FrameTiming;
    const bool expect_profile = argc > 1;
    const auto acquire = static_cast<std::size_t>(TextureAcquisitionPhase::Acquire);
    const auto select = static_cast<std::size_t>(TextureAcquisitionPhase::Selection);
    {
        TextureAcquisitionContext context;
        TextureAcquisitionScope scope(TextureAcquisitionPhase::Acquire);
    }
    assert(counters.texture_acquisition_samples[acquire].calls == 0);
    counters.enabled = true;
    { TextureAcquisitionScope outside(TextureAcquisitionPhase::Selection); }
    assert(counters.texture_acquisition_samples[select].calls == 0);
    for (unsigned i = 0; i < 4096; ++i) {
        TextureAcquisitionContext context;
        TextureAcquisitionScope outer(TextureAcquisitionPhase::Acquire);
        TextureAcquisitionContext nested;
        TextureAcquisitionScope inner(TextureAcquisitionPhase::Selection);
        volatile unsigned value = 0;
        for (unsigned j = 0; j < 1000; ++j) value += j;
    }
    assert(texture_acquisition_depth == 0);
    for (const auto index : {acquire, select}) {
        const auto& value = counters.texture_acquisition_samples[index];
        assert(value.calls == (expect_profile ? 4096 : 0));
        assert(value.timing.clock_failures == 0);
        if (expect_profile) {
            assert(value.timing.samples > 32 && value.timing.samples < 96);
            assert(value.timing.wall_seconds > 0 && value.timing.cpu_seconds > 0);
        } else assert(value.timing.samples == 0);
        std::printf("calls=%llu samples=%llu failures=%llu\n", value.calls,
                    value.timing.samples, value.timing.clock_failures);
    }
}
