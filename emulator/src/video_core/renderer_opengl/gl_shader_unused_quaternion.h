// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <string_view>
#include <regex>

namespace OpenGL::UnusedQuaternion {
enum class Device { None, GCloud, RGSlide };
struct Policy {
    bool target = false;
    bool enabled = false;
    Device device = Device::None;
};
inline constexpr std::string_view GCloudDriver =
    "OpenGL ES 3.2 V@0502.0 (GIT@46d1463393, I43876eb25d, 1638292465) (Date:11/30/21)";
inline constexpr std::string_view SlideDriver =
    "OpenGL ES 3.2 v1.r40p0-01eac0.b0251c048237dcd59e6be15fba11a31a";
constexpr bool ResolveRequest(bool performance, bool preference) { return performance && preference; }
constexpr bool RequestedByDefault(std::string_view model) { return model == "GR0006"; }
constexpr Policy Resolve(bool requested, bool monolithic, std::string_view model,
                         std::string_view product, std::string_view vendor,
                         std::string_view renderer, std::string_view version,
                         std::string_view language) {
    if (!monolithic || language != "OpenGL ES GLSL ES 3.20") return {};
    const auto device = model == "GR0006" && vendor == "Qualcomm" &&
        renderer == "Adreno (TM) 618" && version == GCloudDriver ? Device::GCloud :
        model == "RG Slide" && product == "ums9620_2h10_native" && vendor == "ARM" &&
        renderer == "Mali-G57" && version == SlideDriver ? Device::RGSlide : Device::None;
    return {device != Device::None, device != Device::None && requested, device};
}
constexpr std::string_view CacheTarget(const Policy& policy) {
    return policy.device == Device::GCloud ? "gcloud-adreno618" :
           policy.device == Device::RGSlide ? "rgslide-malig57" : "unsupported";
}
inline bool NoRead(std::string_view source) {
    // Closed generator form: one exact varying declaration, no other mention,
    // including comments/macros/alternate identifiers. Unknown forms reject.
    constexpr std::string_view declaration = "in vec4 normquat;";
    const auto at = source.find("normquat");
    const auto decl = source.find(declaration);
    return decl != source.npos && at == decl + 8 &&
        source.find("normquat", at + 8) == source.npos &&
        source.find("##") == source.npos;
}
inline bool VertexDelta(std::string_view original, std::string_view specialized) {
    const auto main = original.rfind("void main() {\n");
    const auto other_main = specialized.rfind("void main() {\n");
    const auto run = original.rfind("void cara_run_vertex(int vertex_id) {\n", main);
    if (main == original.npos || other_main != main || run == original.npos ||
        original.substr(0, main) != specialized.substr(0, main)) return false;
    const std::string prefix(original.substr(0, main));
    const std::string body(original.substr(run, main-run));
    if (body.find("conditional_code = bvec2(false);") == body.npos ||
        body.find("address_registers = ivec3(0);") == body.npos ||
        body.find("cara_load_vertex(vertex_id);") == body.npos ||
        body.find("exec_shader();") == body.npos) return false;
    if (prefix.find("imageStore") != prefix.npos || prefix.find("atomic") != prefix.npos ||
        prefix.find("memoryBarrier") != prefix.npos || prefix.find("buffer ") != prefix.npos)
        return false;
    const auto load = prefix.rfind("void cara_load_vertex(int vertex_id) {\n");
    if (load == prefix.npos || load >= run) return false;
    const auto loader = prefix.substr(load, run-load);
    const std::regex inputs(R"(vec4\s+(vs_in_reg([0-9]+))\s*;)");
    unsigned input_count = 0;
    for (std::sregex_iterator it(prefix.begin(), prefix.end(), inputs), end; it != end; ++it) {
        ++input_count;
        const auto name = (*it)[1].str();
        const auto index = (*it)[2].str();
        if (loader.find(name + " = cara_decode_vertex(input_vertex_id, " + index + ");") == loader.npos &&
            loader.find(name + " = vec4(0.0);") == loader.npos) return false;
    }
    if (!input_count) return false;
    unsigned reset_count = 0;
    const std::regex regs(R"(vec4\s+(reg_tmp[0-9]+|vs_out_attr[0-9]+)\s*(?:=[^;]*)?;)");
    for (std::sregex_iterator it(prefix.begin(), prefix.end(), regs), end; it != end; ++it) {
        ++reset_count;
        const auto name = (*it)[1].str();
        if (body.find(name + " = vec4(0.0, 0.0, 0.0, 1.0);") == body.npos) return false;
    }
    if (reset_count < 17) return false;
    std::string expected(original);
    const auto erase_once = [&](std::string_view line) {
        const auto at = expected.find(line, main);
        if (at == expected.npos || expected.find(line, at + line.size()) != expected.npos)
            return false;
        expected.erase(at, line.size()); return true;
    };
    if (!erase_once("vec4 first_quat=vec4(0.0);\n")) return false;
    const auto ref = expected.find("if (gl_VertexID % 3 != 0) { cara_run_vertex(", main);
    if (ref == expected.npos) return false;
    const auto ref_end = expected.find("\n", ref);
    if (ref_end == expected.npos) return false;
    const auto ref_line = expected.substr(ref, ref_end-ref+1);
    if (ref_line.compare(0, std::string("if (gl_VertexID % 3 != 0) { cara_run_vertex(gl_VertexID - gl_VertexID % 3); first_quat = ").size(), "if (gl_VertexID % 3 != 0) { cara_run_vertex(gl_VertexID - gl_VertexID % 3); first_quat = ") != 0 ||
        ref_line.size() < 4 || ref_line.compare(ref_line.size()-4, 4, "; }\n") != 0 || !erase_once(ref_line)) return false;
    const auto quat = expected.find("\nnormquat = ", main);
    if (quat == expected.npos) return false;
    const auto quat_end = expected.find("\n", quat+1);
    if (quat_end == expected.npos || expected[quat_end-1] != ';') return false;
    const auto quat_line = expected.substr(quat+1, quat_end-quat);
    if (!erase_once(quat_line)) return false;
    if (!erase_once("if (gl_VertexID % 3 != 0 && dot(first_quat, normquat) < 0.0) normquat = -normquat;\n")) return false;
    return expected == specialized;
}
} // namespace OpenGL::UnusedQuaternion
