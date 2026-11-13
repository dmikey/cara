// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <nihstro/inline_assembly.h>
#include "video_core/renderer_opengl/gl_shader_decompiler.h"
#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
#include "video_core/renderer_opengl/gl_vertex_input_usage.h"
#include "video_core/shader/shader.h"
using S = nihstro::SourceRegister;
using D = nihstro::DestRegister;
using O = nihstro::OpCode;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static Pica::Shader::ShaderSetup Make(std::initializer_list<nihstro::InlineAsm> instructions) {
    const auto binary = nihstro::InlineAsm::CompileToRawBinary(instructions);
    Pica::Shader::ShaderSetup result{};
    std::transform(binary.program.begin(), binary.program.end(), result.program_code.begin(),
                   [](auto word) { return word.hex; });
    std::transform(binary.swizzle_table.begin(), binary.swizzle_table.end(), result.swizzle_data.begin(),
                   [](auto word) { return word.hex; });
    return result;
}
static std::optional<u16> Read(const Pica::Shader::ShaderSetup& setup, u32 entry = 0) {
    const auto generated = OpenGL::ShaderDecompiler::DecompileProgram(setup.program_code,
        setup.swizzle_data, entry,
        [](u32 reg) { return "input" + std::to_string(reg); },
        [](u32 reg) { return "output" + std::to_string(reg); }, 0);
    return generated ? std::optional<u16>(generated->input_mask) : std::nullopt;
}
static void CheckAnalysis() {
    auto setup = Make({{O::Id::END}});
    Require(Read(setup) == 0, "constant shader must have zero input mask");
    setup = Make({{O::Id::MOV, D::MakeOutput(0), S::MakeInput(7)}, {O::Id::END},
                  {O::Id::MOV, D::MakeOutput(0), S::MakeInput(15)}, {O::Id::END}});
    Require((Read(setup).value() & (1u << 7)) != 0, "reachable input missing");
    Require((Read(setup).value() & (1u << 15)) == 0, "instructions past END are unreachable");
    Require((Read(setup, 2).value() & (1u << 15)) != 0, "entry point must change read set");
    // IFU covers both runtime-uniform outcomes; no current uniform value prunes either.
    setup = Make({{O::Id::NOP}, {O::Id::MOV, D::MakeOutput(0), S::MakeInput(2)},
                  {O::Id::NOP}, {O::Id::MOV, D::MakeOutput(0), S::MakeInput(9)},
                  {O::Id::NOP}, {O::Id::END}});
    nihstro::Instruction branch{};
    branch.opcode = O::Id::IFU;
    branch.flow_control.dest_offset = 3;
    branch.flow_control.num_instructions = 2;
    setup.program_code[0] = branch.hex;
    auto mask = Read(setup);
    Require(mask && (*mask & (1u << 2)) && (*mask & (1u << 9)), "both uniform branch inputs required");
    // CALLU reaches a separately emitted subroutine, irrespective of the bool value.
    setup = Make({{O::Id::NOP}, {O::Id::MOV, D::MakeOutput(0), S::MakeInput(1)},
                  {O::Id::END}, {O::Id::NOP}, {O::Id::NOP},
                  {O::Id::MOV, D::MakeOutput(1), S::MakeInput(11)}, {O::Id::NOP}});
    branch = {};
    branch.opcode = O::Id::CALLU;
    branch.flow_control.dest_offset = 5;
    branch.flow_control.num_instructions = 2;
    setup.program_code[0] = branch.hex;
    mask = Read(setup);
    Require(mask && (*mask & (1u << 1)) && (*mask & (1u << 11)), "subroutine input required");
    // Address registers affect uniforms, not the literal input register index.
    setup = Make({{O::Id::ADD, D::MakeOutput(0), S::MakeFloat(0), S::MakeInput(12)},
                  {O::Id::END}});
    nihstro::Instruction indirect{setup.program_code[0]};
    indirect.common.address_register_index = 1;
    setup.program_code[0] = indirect.hex;
    mask = Read(setup);
    Require(mask && (*mask & (1u << 12)), "indirect uniform must retain other operand input");
    setup = {};
    Require(!Read(setup), "unbounded program must conservatively fail analysis");
}
static void CheckCache() {
    OpenGL::IOSVertexInputUsage cache;
    OpenGL::IOSVertexInputReadKey key{};
    unsigned analyses = 0;
    const auto analyze = [&]() -> std::optional<u16> { ++analyses; return 0; };
    Require(cache.Get(key, analyze) == 0 && cache.Get(key, analyze) == 0 && analyses == 1,
            "zero mask must remain cached");
    for (unsigned field = 0; field < 6; ++field) {
        auto changed = key;
        if (field == 0) ++changed.program_hash;
        if (field == 1) ++changed.swizzle_hash;
        if (field == 2) ++changed.main_offset;
        if (field == 3) ++changed.num_outputs;
        if (field == 4) ++changed.sanitize_mul;
        if (field == 5) ++changed.output_map[7];
        Require(cache.Get(changed, analyze) == 0, "changed config missing");
    }
    Require(analyses == 7, "each relevant identity change must invalidate cached result");
    key.program_hash = 999;
    const auto fail = []() -> std::optional<u16> { return std::nullopt; };
    Require(cache.Get(key, fail) == 0xffff, "failed analysis must retain all configured inputs");
}
static void CheckLayouts() {
    unsigned seed = 0x1479abcd;
    const auto random = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    for (unsigned trial = 0; trial < 8192; ++trial) {
        Pica::ShaderRegs regs{};
        regs.max_input_attribute_index.Assign(trial % 16);
        regs.input_attribute_to_register_map_low = random();
        regs.input_attribute_to_register_map_high = random();
        const u16 needed = trial == 0 ? 0 : u16(random());
        Pica::Shader::AttributeBuffer attributes{};
        std::array<Common::Vec4<float>, 16> reference{}, packed{};
        for (unsigned attr = 0; attr <= regs.max_input_attribute_index; ++attr) {
            auto& value = attributes.attr[attr];
            value = {Pica::float24::FromFloat32(float(attr)), Pica::float24::FromFloat32(-float(attr)),
                     Pica::float24::FromFloat32(0.5f), Pica::float24::FromFloat32(1.0f)};
            reference[regs.GetRegisterForAttribute(attr)] = {value.x.ToFloat32(), value.y.ToFloat32(),
                                                           value.z.ToFloat32(), value.w.ToFloat32()};
        }
        const OpenGL::IOSVertexInputLayout layout(regs, needed);
        Require(layout.mask == (OpenGL::IOSVertexInputLayout::MaskFromRegs(regs) & needed), "configured/read intersection");
        layout.PackAttributes(attributes, packed);
        for (unsigned reg = 0; reg < 16; ++reg)
            if (layout.offsets[reg] >= 0)
                for (unsigned component = 0; component < 4; ++component)
                    Require(packed[layout.offsets[reg]][component] == reference[reg][component], "sparse/duplicate/default attribute mapping differs");
        if (!needed) {
            Require(layout.stride == 1, "zero-mask bounded payload");
            for (unsigned component = 0; component < 4; ++component)
                Require(packed[0][component] == 0, "zero-mask padding must be initialized");
        }
    }
    Pica::ShaderRegs regs{};
    regs.max_input_attribute_index.Assign(3);
    regs.input_attribute_to_register_map_low = 0x5152; // attr1 and3 -> reg5; last wins.
    const OpenGL::IOSVertexInputLayout layout(regs, 1u << 5);
    Require(layout.mask == (1u << 5) && layout.source_attributes[0] == 3, "last configured duplicate mapping must win");
}
int main() {
    CheckAnalysis(); CheckCache(); CheckLayouts();
    std::cout << "Used vertex inputs: constant/branch/subroutine/indirect-uniform analysis, failure fallback, config cache and 8192 filtered last-wins mappings passed\n";
}
