// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_packed_inputs.h"

#include <algorithm>
#include <bitset>
#include <cstring>
#include <limits>
#include "video_core/pica_state.h"
#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
#include "video_core/renderer_opengl/gl_vertex_source_map.h"
#include "video_core/renderer_opengl/gl_vertex_upload_ranges.h"
#include "video_core/vertex_loader.h"

namespace Cara {
namespace {

constexpr std::size_t MaxVertexBytes = 8 * 1024 * 1024;

std::vector<u32> ExpandTriangles(const Pica::State& state, bool is_indexed,
                                 const MetalGuestAuthority::GuestReader& read) {
    const auto& pipe = state.regs.pipeline;
    const auto topology = state.primitive_assembler.GetTopology();
    using Topology = Pica::PipelineRegs::TriangleTopology;
    const bool expanded = topology == Topology::Strip || topology == Topology::Fan;
    const u32 count = pipe.num_vertices;
    const u32 limit = OpenGL::IOS_MAX_EXPANDED_VERTICES;
    if (!state.primitive_assembler.IsEmpty() || count < 3 ||
        count > (expanded ? limit / 3 : limit))
        return {};

    std::vector<u8> indices;
    const unsigned index_width = pipe.index_array.format ? 2 : 1;
    if (is_indexed) {
        const u64 address = u64{pipe.vertex_attributes.GetPhysicalBaseAddress()} +
                            pipe.index_array.offset;
        indices.resize(std::size_t{count} * index_width);
        if (address + indices.size() > std::numeric_limits<PAddr>::max() ||
            !read(static_cast<PAddr>(address), indices))
            return {};
    } else if (u64{pipe.vertex_offset} + count - 1 > std::numeric_limits<u32>::max()) {
        return {};
    }

    Pica::PrimitiveAssembler<u32> assembler(topology);
    std::vector<u32> triangles;
    triangles.reserve(expanded ? (count - 2) * 3 : count);
    const Pica::PrimitiveAssembler<u32>::TriangleHandler append =
        [&](u32 a, u32 b, u32 c) { triangles.insert(triangles.end(), {a, b, c}); };
    for (u32 i = 0; i < count; ++i) {
        u32 index = pipe.vertex_offset + i;
        if (is_indexed) {
            index = indices[i * index_width];
            if (index_width == 2)
                index |= u32{indices[i * index_width + 1]} << 8;
        }
        assembler.SubmitVertex(index, append);
    }
    return triangles;
}

} // namespace

std::optional<PicaMetalPackedInputs> CapturePicaMetalPackedInputs(
    const Pica::State& state, bool is_indexed, u16 selected_inputs,
    const MetalGuestAuthority::GuestReader& read) {
    const auto& regs = state.regs;
    if (!read || regs.pipeline.use_gs != Pica::PipelineRegs::UseGS::No ||
        regs.framebuffer.IsShadowRendering())
        return std::nullopt;
    const auto triangles = ExpandTriangles(state, is_indexed, read);
    if (triangles.empty())
        return std::nullopt;
    const auto bounds = std::minmax_element(triangles.begin(), triangles.end());
    const u32 first = *bounds.first;
    const u32 span = *bounds.second - first;
    if (span >= 65536)
        return std::nullopt;

    PicaMetalPackedInputs result;
    result.vertex_count = static_cast<u32>(triangles.size());
    const auto row = OpenGL::IOS_VERTEX_INPUT_WIDTH;
    result.indices.resize(((triangles.size() + row - 1) / row) * row);
    std::bitset<65536> seen;
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const auto index = static_cast<u16>(triangles[i] - first);
        result.indices[i] = index;
        seen.set(index);
    }

    const OpenGL::IOSVertexInputLayout layout(regs.vs, selected_inputs);
    Pica::VertexLoader loader(regs.pipeline);
    const u64 base = regs.pipeline.vertex_attributes.GetPhysicalBaseAddress();
    OpenGL::BoundedVertexUploadRanges<16> ranges;
    std::array<u64, 16> addresses{};
    std::array<bool, 16> loaded{};
    std::array<std::array<float, 4>, 16> constants{};
    for (unsigned reg = 0; reg < layout.offsets.size(); ++reg) {
        const auto offset = layout.offsets[reg];
        if (offset < 0)
            continue;
        const auto attr = layout.source_attributes[offset];
        const auto desc = loader.GetAttributeLayout(attr);
        if (!desc.elements) {
            if (desc.is_default) {
                const auto& value = state.input_default_attributes.attr[attr];
                constants[reg] = {value.x.ToFloat32(), value.y.ToFloat32(), value.z.ToFloat32(),
                                  value.w.ToFloat32()};
            }
            result.layout[reg] = {static_cast<s32>(reg * sizeof(constants[reg])), 0, 3, 4};
            continue;
        }
        const auto format = static_cast<unsigned>(desc.format);
        if (desc.elements > 4 || format > 3)
            return std::nullopt;
        const unsigned bytes = format == 3 ? 4 : format == 2 ? 2 : 1;
        const auto start = base + desc.offset + u64{first} * desc.stride;
        const auto end = start + u64{span} * desc.stride + bytes * desc.elements;
        if (end > std::numeric_limits<PAddr>::max() || end - start > MaxVertexBytes ||
            !ranges.Add(start, end))
            return std::nullopt;

        addresses[reg] = start;
        loaded[reg] = true;
        result.layout[reg] = {0, static_cast<s32>(desc.stride), static_cast<s32>(format),
                              static_cast<s32>(desc.elements)};
    }
    ranges.SortAndMerge();
    std::size_t buffer_bytes = sizeof(constants);
    for (auto& range : ranges) {
        range.destination = (buffer_bytes + 3) & ~std::size_t{3};
        buffer_bytes = range.destination + (range.end - range.begin);
    }
    if (buffer_bytes > MaxVertexBytes ||
        buffer_bytes > sizeof(constants) + seen.count() * layout.stride * sizeof(constants[0]))
        return std::nullopt;
    const auto row_bytes = row * sizeof(constants[0]);
    result.vertices.resize(((buffer_bytes + row_bytes - 1) / row_bytes) * row_bytes);
    std::memcpy(result.vertices.data(), constants.data(), sizeof(constants));
    for (const auto& range : ranges) {
        if (!read(static_cast<PAddr>(range.begin),
                  {result.vertices.data() + range.destination, range.end - range.begin}))
            return std::nullopt;
        for (unsigned reg = 0; reg < loaded.size(); ++reg) {
            if (loaded[reg] && addresses[reg] >= range.begin && addresses[reg] < range.end)
                result.layout[reg][0] =
                    static_cast<s32>(range.destination + addresses[reg] - range.begin);
        }
    }
    return result;
}

std::optional<PicaMetalPackedInputs> CapturePicaMetalDecodedInputs(
    const Pica::State& state, bool is_indexed, u16 selected_inputs,
    const MetalGuestAuthority::GuestReader& read) {
    const auto triangles = ExpandTriangles(state, is_indexed, read);
    if (triangles.empty())
        return std::nullopt;
    const auto& r = state.regs;
    const auto& pipe = r.pipeline;
    const OpenGL::IOSVertexInputLayout layout(r.vs, selected_inputs);
    const Pica::VertexLoader loader(pipe);
    const u64 base = pipe.vertex_attributes.GetPhysicalBaseAddress();

    PicaMetalPackedInputs result;
    result.vertex_count = static_cast<u32>(triangles.size());
    constexpr auto row = OpenGL::IOS_VERTEX_INPUT_WIDTH;
    result.indices.resize(((triangles.size() + row - 1) / row) * row);
    using Float4 = std::array<float, 4>;
    std::vector<Float4> values;
    OpenGL::IOSVertexSourceMap records;
    std::array<Pica::VertexLoader::AttributeLayout, 16> attrs{};
    for (unsigned i = 0; i < layout.stride && layout.mask; ++i)
        attrs[i] = loader.GetAttributeLayout(layout.source_attributes[i]);

    for (std::size_t v = 0; v < triangles.size(); ++v) {
        const auto index = triangles[v];
        const auto key = is_indexed ? index : index - pipe.vertex_offset;
        if (key >= 65536)
            return std::nullopt;
        const auto record = records.Resolve(static_cast<u16>(key));
        result.indices[v] = record.index;
        if (!record.inserted)
            continue;
        for (unsigned i = 0; i < layout.stride; ++i) {
            Float4 value{};
            const auto& a = attrs[i];
            if (a.elements) {
                const unsigned format = static_cast<unsigned>(a.format);
                if (a.elements > 4 || format > 3)
                    return std::nullopt;
                const unsigned size = format == 3 ? 4 : format == 2 ? 2 : 1;
                const u64 address = base + a.offset + u64(index) * a.stride;
                const auto bytes = a.elements * size;
                std::array<u8, 16> raw{};
                if (address + bytes > std::numeric_limits<PAddr>::max() ||
                    !read(static_cast<PAddr>(address), {raw.data(), bytes}))
                    return std::nullopt;
                value[3] = 1.f;
                for (unsigned c = 0; c < a.elements; ++c) {
                    const auto* src = raw.data() + c * size;
                    if (format == 3) {
                        std::memcpy(&value[c], src, size);
                    } else if (format == 2) {
                        s16 integer;
                        std::memcpy(&integer, src, size);
                        value[c] = integer;
                    } else if (format == 0) {
                        s8 integer;
                        std::memcpy(&integer, src, size);
                        value[c] = integer;
                    } else {
                        value[c] = *src;
                    }
                    value[c] = Pica::float24::FromFloat32(value[c]).ToFloat32();
                }
            } else if (a.is_default) {
                const auto& v = state.input_default_attributes.attr[layout.source_attributes[i]];
                value = {v.x.ToFloat32(), v.y.ToFloat32(), v.z.ToFloat32(), v.w.ToFloat32()};
            }
            values.push_back(value);
        }
    }
    const auto bytes = values.size() * sizeof(Float4);
    constexpr auto row_bytes = row * sizeof(Float4);
    result.vertices.resize(((bytes + row_bytes - 1) / row_bytes) * row_bytes);
    std::memcpy(result.vertices.data(), values.data(), bytes);
    return result;
}

} // namespace Cara
