// SPDX-License-Identifier: GPL-2.0-or-later
#include "batched_vertex_source.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace Cara {
namespace {
struct Parameter {
    std::string_view annotation;
    std::string_view type;
    std::string_view argument;
    std::string_view member;
};

constexpr std::array Parameters{
    Parameter{"[[buffer(0)]]", "constant CaraPackedLayout&", "*job.layout",
              "constant CaraPackedLayout* layout [[id(0)]];"},
    Parameter{"[[buffer(1)]]", "constant vs_config&", "*job.uniforms",
              "constant vs_config* uniforms [[id(1)]];"},
    Parameter{"[[buffer(2)]]", "constant shader_data&", "*job.common",
              "constant shader_data* common [[id(2)]];"},
    Parameter{"[[texture(0)]]", "texture2d<uint>", "job.vertices",
              "texture2d<uint> vertices [[id(3)]];"},
    Parameter{"[[texture(1)]]", "texture2d<uint>", "job.indices",
              "texture2d<uint> indices [[id(4)]];"},
    Parameter{"[[sampler(0)]]", "sampler", "job.vertex_sampler",
              "sampler vertex_sampler [[id(5)]];"},
    Parameter{"[[sampler(1)]]", "sampler", "job.index_sampler",
              "sampler index_sampler [[id(6)]];"},
    Parameter{"[[vertex_id]]", "uint", "local_vertex", ""},
};

std::string_view Trim(std::string_view value) {
    constexpr std::string_view whitespace = " \t\n\r";
    const auto first = value.find_first_not_of(whitespace);
    if (first == value.npos)
        return {};
    return value.substr(first, value.find_last_not_of(whitespace) - first + 1);
}

bool Identifier(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9'))
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    });
}
} // namespace

std::optional<std::string> MakeUniquePackedVertexSource(std::string_view vertex) {
    constexpr std::string_view input =
        "void cara_load_vertex(int vertex_id) {\n"
        "int input_vertex_id = int(texelFetch(cara_vertex_indices, ivec2(vertex_id % 128, vertex_id / 128), 0).r);";
    constexpr std::string_view layout = "ivec4 info = cara_vertex_layout[reg];";
    constexpr std::string_view main =
        "void main() {\nvec4 first_quat=vec4(0.0);\n"
        "if (gl_VertexID % 3 != 0) { cara_run_vertex(gl_VertexID - gl_VertexID % 3); first_quat = ";
    constexpr std::string_view actual = "; }\ncara_run_vertex(gl_VertexID);\nnormquat = ";
    constexpr std::string_view correction =
        ";\nif (gl_VertexID % 3 != 0 && dot(first_quat, normquat) < 0.0) normquat = -normquat;\n";
    const auto only = [&](std::string_view token) {
        const auto pos = vertex.find(token);
        return pos != vertex.npos && vertex.find(token, pos + token.size()) == vertex.npos
                   ? pos : vertex.npos;
    };
    const auto input_pos = only(input);
    const auto layout_pos = only(layout);
    const auto main_pos = only(main);
    const auto actual_pos = only(actual);
    const auto correction_pos = only(correction);
    if (layout_pos == vertex.npos || input_pos == vertex.npos || main_pos == vertex.npos ||
        actual_pos == vertex.npos || correction_pos == vertex.npos ||
        layout_pos >= input_pos || input_pos >= main_pos ||
        main_pos + main.size() >= actual_pos || actual_pos + actual.size() >= correction_pos)
        return std::nullopt;

    const auto reference = vertex.substr(main_pos + main.size(), actual_pos - main_pos - main.size());
    const auto quaternion = vertex.substr(actual_pos + actual.size(),
                                         correction_pos - actual_pos - actual.size());
    if (reference != quaternion || !quaternion.starts_with("vec4(") ||
        !quaternion.ends_with(')') || quaternion.find_first_of(";{}\n") != vertex.npos)
        return std::nullopt;

    std::string result(vertex);
    result.replace(main_pos, correction_pos + correction.size() - main_pos,
                   "void main() {\ncara_run_vertex(gl_VertexID);\nnormquat = " +
                       std::string(quaternion) + ";\n");
    result.replace(input_pos, input.size(),
                   "void cara_load_vertex(int vertex_id) {\nint input_vertex_id = vertex_id;");
    result.insert(layout_pos + layout.size(), "\n    info.w &= 7;");
    return result;
}

std::optional<std::string> MakeBatchedVertexSource(std::string_view vertex,
                                                   bool gather_unique_vertices) {
    constexpr std::string_view entry = "vertex cara_pica_vert_out cara_pica_vert(";
    const auto begin = vertex.find(entry);
    if (begin == vertex.npos || vertex.find(entry, begin + entry.size()) != vertex.npos ||
        vertex.find("struct cara_pica_vert_out\n{") == vertex.npos ||
        vertex.find("cara_compute_") != vertex.npos)
        return std::nullopt;
    const auto end = vertex.find(")\n{", begin + entry.size());
    if (end == vertex.npos)
        return std::nullopt;

    auto signature = vertex.substr(begin + entry.size(), end - begin - entry.size());
    std::vector<std::string_view> arguments;
    std::vector<std::string_view> declarations;
    std::vector<std::string_view> members;
    std::array<bool, Parameters.size()> seen{};
    while (!signature.empty()) {
        const auto comma = signature.find(',');
        const auto parameter = Trim(signature.substr(0, comma));
        const auto rule = std::find_if(Parameters.begin(), Parameters.end(),
                                       [&](const Parameter& item) {
                                           return parameter.ends_with(item.annotation);
                                       });
        if (rule == Parameters.end() || !parameter.starts_with(rule->type))
            return std::nullopt;
        const auto declaration = Trim(parameter.substr(0, parameter.size() - rule->annotation.size()));
        if (!Identifier(Trim(declaration.substr(rule->type.size()))) ||
            std::exchange(seen[static_cast<std::size_t>(rule - Parameters.begin())], true))
            return std::nullopt;
        declarations.push_back(declaration);
        arguments.push_back(rule->argument);
        if (!rule->member.empty())
            members.push_back(rule->member);
        if (comma == signature.npos)
            break;
        signature.remove_prefix(comma + 1);
    }
    if (!seen.back())
        return std::nullopt;
    if (gather_unique_vertices &&
        (seen[4] || seen[6] || vertex.find("float4 normquat [[user(locn5)]];") == vertex.npos))
        return std::nullopt;

    std::string code(vertex.substr(0, begin));
    code += "static inline cara_pica_vert_out cara_compute_vertex(";
    for (std::size_t i = 0; i < declarations.size(); ++i) {
        if (i)
            code += ", ";
        code += declarations[i];
    }
    code += vertex.substr(end);
    code += "\nstruct CaraComputeJob {\n";
    for (const auto member : members) {
        code += "    ";
        code += member;
        code += '\n';
    }
    code += R"(
};
struct alignas(16) CaraComputeOutput {
    cara_pica_vert_out value;
};
static_assert(sizeof(CaraComputeOutput) <= 128, "PICA vertex output storage bound");
struct CaraComputeRange {
    uint first_group;
    uint vertex_count;
    uint output_base;
};

kernel void cara_compute_batch(
    constant CaraComputeJob* jobs [[buffer(0)]],
    device const CaraComputeRange* ranges [[buffer(1)]],
    device uchar* outputs [[buffer(2)]],
    constant uint& job_count [[buffer(3)]],
    uint group [[threadgroup_position_in_grid]],
    uint lane [[thread_index_in_threadgroup]],
    uint width [[threads_per_threadgroup]]) {
    if (job_count == 0) return;
    uint first = 0;
    uint last = job_count;
    while (first + 1 < last) {
        uint middle = first + (last - first) / 2;
        if (ranges[middle].first_group <= group) first = middle;
        else last = middle;
    }
    const CaraComputeRange range = ranges[first];
    uint local_vertex = (group - range.first_group) * width + lane;
    if (local_vertex >= range.vertex_count) return;
    constant CaraComputeJob& job = jobs[first];
    auto destination = reinterpret_cast<device CaraComputeOutput*>(
        outputs + ulong(range.output_base + local_vertex) * 128);
    destination->value = cara_compute_vertex()";
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i)
            code += ", ";
        code += arguments[i];
    }
    code += R"();
}
)";
    if (gather_unique_vertices) {
        if (code.find("struct CaraPackedLayout\n{") == code.npos)
            code += "\nstruct CaraPackedLayout { int4 cara_vertex_layout[16]; };\n";
        // Mirror the raw-index decoder in pica_metal_runtime_shader.cpp.
        // The compute output is indexed by the compact, rebased source ID.
        code += R"(
static inline uint cara_compute_source_slot(uint slot, uint packed,
                                             texture2d<uint, access::read> indices) {
    uint header = (packed >> 3u) & 15u;
    uint topology = header & 3u;
    uint width = header >> 2u;
    if (topology == 1u || topology == 2u) {
        uint triangle = slot / 3u;
        uint corner = slot % 3u;
        if (topology == 1u)
            slot = triangle + (corner == 2u ? 2u : corner ^ (triangle & 1u));
        else
            slot = corner == 0u ? 0u : triangle + corner;
    }
    if (width == 0u)
        return slot;
    uint word = 2u + (width == 1u ? slot / 2u : slot);
    uint index = indices.read(uint2(word & 255u, word >> 8u)).r;
    if (width == 1u)
        index = (index >> ((slot & 1u) * 8u)) & 255u;
    return index - (packed >> 7u);
}

vertex cara_pica_vert_out cara_compute_raster(
    device const uchar* outputs [[buffer(3)]],
    constant CaraPackedLayout& layout [[buffer(0)]],
    texture2d<uint, access::read> indices [[texture(1)]],
    uint local_vertex [[vertex_id]]) {
    uint packed = uint(layout.cara_vertex_layout[0].w);
    uint source = cara_compute_source_slot(local_vertex, packed, indices);
    auto result = reinterpret_cast<device const CaraComputeOutput*>(
        outputs + ulong(source) * 128)->value;
    uint corner = local_vertex % 3u;
    if (corner != 0u) {
        uint first = cara_compute_source_slot(local_vertex - corner, packed, indices);
        float4 reference = reinterpret_cast<device const CaraComputeOutput*>(
            outputs + ulong(first) * 128)->value.normquat;
        if (dot(reference, result.normquat) < 0.0f)
            result.normquat = -result.normquat;
    }
    return result;
}
)";
        return code;
    }
    code += R"(
vertex cara_pica_vert_out cara_compute_raster(
    device const uchar* outputs [[buffer(3)]],
    uint local_vertex [[vertex_id]]) {
    return reinterpret_cast<device const CaraComputeOutput*>(
        outputs + ulong(local_vertex) * 128)->value;
}
)";
    return code;
}
} // namespace Cara
