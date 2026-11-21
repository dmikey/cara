// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <optional>
#include <vector>
#include "video_core/renderer_metal/pica_metal_guest_authority.h"

namespace Pica {
struct State;
}

namespace Cara {

struct PicaMetalPackedInputs {
    std::vector<u8> vertices;
    std::vector<u16> indices;
    std::array<std::array<s32, 4>, 16> layout{};
    u32 vertex_count = 0;
    u32 unique_dense_vertices = 0;
};

// Use the same register mapping and triangle order as the packed iOS path.
// The owner resolves guest visibility when each physical interval is read.
std::optional<PicaMetalPackedInputs> CapturePicaMetalPackedInputs(
    const Pica::State& state, bool is_indexed, u16 selected_inputs,
    const MetalGuestAuthority::GuestReader& read);

// The decoded indexed ABI still executes the original PICA vertex shader on GPU.
std::optional<PicaMetalPackedInputs> CapturePicaMetalDecodedInputs(
    const Pica::State& state, bool is_indexed, u16 selected_inputs,
    const MetalGuestAuthority::GuestReader& read);

} // namespace Cara
