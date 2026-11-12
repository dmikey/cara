#pragma once

#include <array>
#include "common/common_types.h"
#include "video_core/regs_pipeline.h"

namespace Pica {

namespace DebugUtils {
class MemoryAccessTracker;
}

namespace Shader {
struct AttributeBuffer;
}

class VertexLoader {
public:
    VertexLoader() = default;
    explicit VertexLoader(const PipelineRegs& regs) {
        Setup(regs);
    }

    void Setup(const PipelineRegs& regs);
    void LoadVertex(u32 base_address, int index, int vertex, Shader::AttributeBuffer& input,
                    DebugUtils::MemoryAccessTracker& memory_accesses);

    int GetNumTotalAttributes() const {
        return num_total_attributes;
    }

    struct AttributeLayout { u32 offset, stride, elements; PipelineRegs::VertexAttributeFormat format; bool is_default; };
    AttributeLayout GetAttributeLayout(unsigned index) const {
        return {vertex_attribute_sources[index], vertex_attribute_strides[index],
                index < unsigned(num_total_attributes) ? vertex_attribute_elements[index] : 0,
                vertex_attribute_formats[index],
                index < unsigned(num_total_attributes) && vertex_attribute_is_default[index]};
    }
private:
    std::array<u32, 16> vertex_attribute_sources;
    std::array<u32, 16> vertex_attribute_strides{};
    std::array<PipelineRegs::VertexAttributeFormat, 16> vertex_attribute_formats{};
    std::array<u32, 16> vertex_attribute_elements{};
    std::array<bool, 16> vertex_attribute_is_default;
    int num_total_attributes = 0;
    bool is_setup = false;
};

} // namespace Pica
