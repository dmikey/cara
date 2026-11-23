// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>

#include "video_core/renderer_metal/pica_metal_cube_plan.h"
#include "video_core/renderer_metal/pica_metal_transfers.h"

namespace Cara::MetalTransfer {
std::optional<LinearColorTransferPlan> PlanNativeLinearColorTransfer(DisplayTransferRegisters regs);
}

namespace {
using namespace Cara::MetalTransfer;

constexpr bool CubePlanMatchesGuestLayout() {
    constexpr std::uint32_t face_bytes = 21760;
    constexpr auto first = std::uint32_t{0x20000000};
    CubeRegisters regs{{}, 64, 64, 256, 0, 3, 0};
    for (std::size_t i = 0; i < regs.addresses.size(); ++i)
        regs.addresses[i] = first + i * face_bytes;
    constexpr GuestInterval color{0x18100000, 0x18104000};
    constexpr GuestInterval depth{0x18200000, 0x18204000};
    const auto cube = PlanCube(regs, color, depth);
    if (!cube || cube->levels != 4)
        return false;
    for (std::size_t i = 0; i < cube->faces.size(); ++i) {
        const auto& face = cube->faces[i];
        if (face.bytes.begin != regs.addresses[i] || face.bytes.Size() != face_bytes ||
            face.mips[0].Size() != 16384 || face.mips[1].Size() != 4096 ||
            face.mips[2].Size() != 1024 || face.mips[3].Size() != 256)
            return false;
    }
    if (PlanCube(regs, {first, first + 8}, depth) ||
        PlanCube(regs, color, depth, GuestInterval{first + 8, first + 16}))
        return false;
    regs.max_level = 4;
    if (PlanCube(regs, color, depth))
        return false;
    regs.max_level = 3;
    regs.addresses[5] = 0xfffffff8;
    return !PlanCube(regs, color, depth);
}

static_assert(CubePlanMatchesGuestLayout());
} // namespace

int main() {
    using namespace Cara::MetalTransfer;
    constexpr DisplayTransferRegisters crop{
        0x18070800, 0x18300000, 512, 400, 480, 400, 0x01001004, false};
    constexpr DisplayTransferRegisters scale{
        0x18070800, 0x18561400, 512, 400, 512, 400, 0x01000000, false};
    constexpr DisplayTransferRegisters retile{
        0x18561400, 0x184e1400, 256, 400, 256, 400, 0x00001002, false};

    constexpr auto cropped = PlanScaledColorCrop(crop);
    static_assert(cropped && cropped->source.Size() == 819200 &&
                  cropped->destination.Size() == 288000 && cropped->source_bpp == 4 &&
                  cropped->destination_bpp == 3);
    constexpr auto packed = PlanScaledColorCrop(
        {0x1829bfc0, 0x21689c80, 512, 400, 480, 400, 0x01002204, false});
    static_assert(packed && packed->source.Size() == 409600 &&
                  packed->destination.Size() == 192000 && packed->output_width == 240 &&
                  packed->output_height == 400 && packed->source_bpp == 2 &&
                  packed->destination_bpp == 2);
    constexpr auto packed_xy = PlanScaledColorCrop(
        {0x1829bfc0, 0x1806ac00, 512, 400, 512, 400, 0x02002220, false});
    static_assert(packed_xy && packed_xy->scale_xy_tiled && packed_xy->source.Size() == 409600 &&
                  packed_xy->destination.Size() == 102400 && packed_xy->output_width == 256 &&
                  packed_xy->output_height == 200);
    constexpr auto rgba_xy = PlanScaledColorCrop(
        {0x18537fc0, 0x1802ab80, 512, 400, 512, 400, 0x02000020, false});
    static_assert(rgba_xy && rgba_xy->scale_xy_tiled && rgba_xy->source.Size() == 819200 &&
                  rgba_xy->destination.Size() == 204800 && rgba_xy->source_bpp == 4 &&
                  rgba_xy->destination_bpp == 4);
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
