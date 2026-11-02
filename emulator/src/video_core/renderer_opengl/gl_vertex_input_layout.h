// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include "common/common_types.h"
#include "video_core/regs_shader.h"

namespace OpenGL {
// At the 16,384-vertex limit, even a full 16-register layout fits 2,048 rows.
constexpr unsigned IOS_VERTEX_INPUT_WIDTH = 128;
struct IOSVertexInputLayout {
    u16 mask = 0;
    unsigned stride = 0;
    std::array<int, 16> offsets{};
    explicit IOSVertexInputLayout(u16 mask_) : mask(mask_) {
        offsets.fill(-1);
        for (unsigned reg = 0; reg < offsets.size(); ++reg)
            if (mask & (1u << reg)) offsets[reg] = static_cast<int>(stride++);
        if (!stride) stride = 1;
    }
    explicit IOSVertexInputLayout(const Pica::ShaderRegs& regs)
        : IOSVertexInputLayout(MaskFromRegs(regs)) {}
    static u16 MaskFromRegs(const Pica::ShaderRegs& regs) {
        u16 result = 0;
        for (unsigned attr = 0; attr <= regs.max_input_attribute_index; ++attr)
            result |= u16(1u << regs.GetRegisterForAttribute(attr));
        return result;
    }
};
} // namespace OpenGL
