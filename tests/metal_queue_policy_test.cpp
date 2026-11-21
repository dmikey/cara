// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <string_view>
#include "video_core/renderer_metal/pica_metal_queue_policy.h"

int main() {
    using Cara::MetalQueuePolicy;
    using Cara::MetalQueueSample;
    const MetalQueueSample blocked{20'000'000, 1'000'000, 0, false};
    const MetalQueueSample fast{17'000'000, 0, 0, false};
    const MetalQueueSample paused_on_upload{19'000'000, 0, 1'000'000, false};
    const MetalQueueSample memory{17'000'000, 0, 0, true};
    MetalQueuePolicy policy(8);
    assert(policy.Update(fast) == nullptr);
    assert(std::string_view(policy.Update(blocked)) == "trial" && policy.Slots() == 16);
    assert(std::string_view(policy.Update(paused_on_upload)) == "rollback" && policy.Slots() == 8);
    for (int i = 0; i < 3; ++i)
        assert(policy.Update(blocked) == nullptr && policy.Slots() == 8);
    assert(std::string_view(policy.Update(blocked)) == "trial" && policy.Slots() == 16);
    assert(std::string_view(policy.Update(fast)) == "accepted" && policy.Slots() == 16);
    assert(std::string_view(policy.Update(blocked)) == "trial" && policy.Slots() == 32);
    assert(std::string_view(policy.Update(fast)) == "accepted" && policy.Slots() == 32);
    assert(policy.Update(blocked) == nullptr && policy.Slots() == 32);
    assert(std::string_view(policy.Update(memory)) == "resource-pressure" && policy.Slots() == 16);
    assert(std::string_view(policy.Update(memory)) == "resource-pressure" && policy.Slots() == 8);
    assert(policy.Update(memory) == nullptr && policy.Slots() == 8);
}
