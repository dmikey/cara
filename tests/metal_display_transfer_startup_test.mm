// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>

#include "video_core/renderer_metal/pica_metal_transfers.h"

namespace Cara::MetalTransfer {
std::optional<LinearColorTransferPlan> PlanNativeLinearColorTransfer(DisplayTransferRegisters regs);
}

int main() {
    using namespace Cara::MetalTransfer;
    constexpr DisplayTransferRegisters crop{
        0x18070800, 0x18300000, 512, 400, 480, 400, 0x01001004, false};
    constexpr DisplayTransferRegisters scale{
        0x18070800, 0x18561400, 512, 400, 512, 400, 0x01000000, false};
    constexpr DisplayTransferRegisters retile{
        0x18561400, 0x184e1400, 256, 400, 256, 400, 0x00001002, false};

    constexpr auto cropped = PlanScaledCropRgba8(crop);
    static_assert(cropped && cropped->source.Size() == 819200 &&
                  cropped->destination.Size() == 288000 && cropped->destination_bpp == 3);
    constexpr auto tiled = PlanLinearToTiledRgb8(retile);
    static_assert(tiled && tiled->source.Size() == 409600 && tiled->destination.Size() == 307200);

    const auto scaled = PlanNativeLinearColorTransfer(scale);
    assert(scaled && scaled->destination_bpp == 4 && scaled->source_tiled && !scaled->flip);
    assert(scaled->source.begin == cropped->source.begin && scaled->source.end == cropped->source.end);
    assert(scaled->destination.begin == tiled->source.begin && scaled->destination.end == tiled->source.end);

    auto test = scale;
    test.flags |= 1u;
    const auto flipped = PlanNativeLinearColorTransfer(test);
    assert(flipped && flipped->flip && flipped->destination.Size() == scaled->destination.Size());
    test.flags |= 1u << 5;
    assert(!PlanNativeLinearColorTransfer(test));

    test = retile;
    test.flags |= 1u;
    assert(PlanLinearToTiledRgb8(test));
    test.destination_address = test.source_address + 8u;
    assert(!PlanLinearToTiledRgb8(test));
    test = retile;
    test.address_override_enabled = true;
    assert(!PlanLinearToTiledRgb8(test));
    test = retile;
    test.destination_address = 0xfffffff8;
    assert(!PlanLinearToTiledRgb8(test));
}
