// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include "audio_core/hle/mixers.h"

int main() {
    using namespace AudioCore;
    using namespace AudioCore::HLE;
    static_assert(sizeof(DspStatus) == 32);
    static_assert(std::is_trivially_copyable_v<DspStatus>);
    DspConfiguration config{};
    IntermediateMixSamples read_samples{};
    IntermediateMixSamples write_samples{};
    const std::array<QuadFrame32, 3> input{};
    std::size_t status_bytes_checked = 0;
    for (unsigned instance = 0; instance < 2; ++instance) {
        Mixers mixers;
        for (unsigned tick = 0; tick < 64; ++tick) {
            if (tick == 32) {
                mixers.Reset();
            }
            // Match AudioTick's whole-struct assignment to shared DSP memory. Poison the
            // destination too: no previous destination contents may survive the return.
            DspStatus shared_status{};
            std::memset(&shared_status, 0x5a, sizeof(shared_status));
            shared_status = mixers.Tick(config, read_samples, write_samples, input);
            std::array<unsigned char, sizeof(DspStatus)> bytes{};
            std::memcpy(bytes.data(), &shared_status, bytes.size());
            for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
                // Zero is Cara's deterministic initialization policy for currently unused
                // bytes, not a claim about reserved-byte values on Nintendo hardware.
                if (bytes[offset] != 0) {
                    std::fprintf(stderr, "DSP status instance=%u tick=%u byte=%zu: "
                                         "expected 0, got 0x%02x\n",
                                 instance, tick, offset, static_cast<unsigned>(bytes[offset]));
                    return 1;
                }
                ++status_bytes_checked;
            }
            for (const auto& sample : mixers.GetOutput()) {
                if (sample[0] != 0 || sample[1] != 0) {
                    std::fprintf(stderr, "Zero mixer input produced nonzero output\n");
                    return 1;
                }
            }
        }
    }
    std::printf("Public Mixers::Tick: %zu defined status bytes checked across "
                "128 ticks, reset and reconstruction; zero input remains silent\n",
                status_bytes_checked);
}
