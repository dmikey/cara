// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include "common/common_types.h"
#include "video_core/regs_shader.h"

namespace OpenGL {
// A full 16-register layout at the policy limit occupies 16 MiB and 8,192 rows.
// Each draw is also checked against the active device texture-size limit.
constexpr unsigned IOS_VERTEX_INPUT_WIDTH = 128;
constexpr unsigned IOS_MAX_EXPANDED_VERTICES = 65536;
struct IOSVertexInputLayout {
    u16 mask = 0;
    unsigned stride = 0;
    std::array<int, 16> offsets{};
    std::array<unsigned, 16> source_attributes{};
    explicit IOSVertexInputLayout(u16 mask_) : mask(mask_) {
        offsets.fill(-1);
        for (unsigned reg = 0; reg < offsets.size(); ++reg)
            if (mask & (1u << reg)) {
                source_attributes[stride] = reg;
                offsets[reg] = static_cast<int>(stride++);
            }
        if (!stride) stride = 1;
    }
    explicit IOSVertexInputLayout(const Pica::ShaderRegs& regs)
        : IOSVertexInputLayout(MaskFromRegs(regs)) {
        // Match UnitState::LoadInput: the last attribute mapped to a register wins.
        for (unsigned attr = 0; attr <= regs.max_input_attribute_index; ++attr)
            source_attributes[offsets[regs.GetRegisterForAttribute(attr)]] = attr;
    }
    template <typename AttributeBuffer, typename PackedVertex>
    void PackAttributes(const AttributeBuffer& input, PackedVertex& packed) const {
        if (!mask) { packed[0] = {}; return; }
        for (unsigned offset = 0; offset < stride; ++offset) {
            const auto& value = input.attr[source_attributes[offset]];
            packed[offset] = {value.x.ToFloat32(), value.y.ToFloat32(),
                              value.z.ToFloat32(), value.w.ToFloat32()};
        }
    }
    static u16 MaskFromRegs(const Pica::ShaderRegs& regs) {
        u16 result = 0;
        for (unsigned attr = 0; attr <= regs.max_input_attribute_index; ++attr)
            result |= u16(1u << regs.GetRegisterForAttribute(attr));
        return result;
    }
};
} // namespace OpenGL
