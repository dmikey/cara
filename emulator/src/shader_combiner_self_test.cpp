// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdexcept>
#include "common/scope_exit.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_vars.h"

namespace {
using Stage = Pica::TexturingRegs::TevStageConfig;

OpenGL::TevStageConfigRaw Raw(const Stage& stage) {
    return {stage.sources_raw, stage.modifiers_raw, stage.ops_raw, stage.scales_raw};
}

void Require(bool passed, const char* message) {
    if (!passed)
        throw std::runtime_error(message);
}
} // namespace

void CheckCombinerBufferElision() {
    const bool old_gles = OpenGL::GLES;
    const bool old_shadow = OpenGL::AllowShadow;
    OpenGL::GLES = true;
    OpenGL::AllowShadow = false;
    SCOPE_EXIT({ OpenGL::GLES = old_gles; OpenGL::AllowShadow = old_shadow; });

    OpenGL::PicaFSConfig no_writes{};
    auto& state = no_writes.state;
    state.alpha_test_func = Pica::FramebufferRegs::CompareFunc::Always;
    state.logic_op = Pica::FramebufferRegs::LogicOp::Copy;
    Stage passthrough{};
    passthrough.color_source1.Assign(Stage::Source::Previous);
    passthrough.alpha_source1.Assign(Stage::Source::Previous);
    for (std::size_t i = 1; i < state.tev_stages.size(); ++i)
        state.tev_stages[i] = Raw(passthrough);

    auto writes = no_writes;
    // Update both RGB and alpha after stage 0.
    writes.state.combiner_buffer_input = 0x11;
    const auto generate = [](const OpenGL::PicaFSConfig& config) {
        return OpenGL::GenerateFragmentShader(config, false);
    };
    const auto legacy = [](const OpenGL::PicaFSConfig& config) {
        return OpenGL::GenerateFragmentShader(config, false, {true, false});
    };
    Require(legacy(no_writes) != legacy(writes), "GLES fixture must cover buffer writes");
    Require(generate(no_writes) == generate(writes), "GLES must deduplicate dead buffer writes");
    Require(generate(writes).find("vec4 combiner_buffer") == std::string::npos,
            "GLES must elide the dead combiner declaration");

    auto ignored_operand = writes;
    Stage modulate{};
    modulate.color_op.Assign(Stage::Operation::Modulate);
    modulate.color_source3.Assign(Stage::Source::PreviousBuffer);
    ignored_operand.state.tev_stages[0] = Raw(modulate);
    Require(generate(ignored_operand).find("vec4 combiner_buffer") == std::string::npos,
            "An unused TEV operand must not keep the combiner buffer alive");

    // The buffer update becomes readable two stages later.
    for (bool alpha : {false, true}) {
        auto reads = writes;
        auto reader = passthrough;
        if (alpha) {
            reader.alpha_source1.Assign(Stage::Source::PreviousBuffer);
            reader.alpha_modifier1.Assign(Stage::AlphaModifier::OneMinusSourceAlpha);
        } else {
            reader.color_source1.Assign(Stage::Source::PreviousBuffer);
            reader.color_modifier1.Assign(Stage::ColorModifier::OneMinusSourceColor);
        }
        reads.state.tev_stages[2] = Raw(reader);
        Require(generate(reads) == legacy(reads),
                alpha ? "GLES must preserve the live alpha buffer shader"
                      : "GLES must preserve the live RGB buffer shader");
        auto reads_without_writes = reads;
        reads_without_writes.state.combiner_buffer_input = 0;
        Require(generate(reads) != generate(reads_without_writes),
                "GLES must preserve the update mask for a live combiner buffer");
    }

    OpenGL::GLES = false;
    Require(generate(writes) == OpenGL::GenerateFragmentShader(writes, false, {false, false}),
            "Desktop GL must preserve its generated source");
}

void CheckMaterialBorderConfig() {
    using Texture = Pica::TexturingRegs::TextureConfig;
    Pica::Regs regs{};
    regs.texturing.main_config.texture0_enable.Assign(1);
    auto& t = regs.texturing.texture0;
    t.type.Assign(Texture::Texture2D);
    t.wrap_s.Assign(Texture::ClampToBorder);
    t.wrap_t.Assign(Texture::ClampToBorder);
    t.min_filter.Assign(Texture::Linear);
    t.mag_filter.Assign(Texture::Linear);
    t.lod.max_level.Assign(1);
    t.lod.bias.Assign(-180);
    t.border_color.raw = 0x00f8a4fe;
    const OpenGL::PicaFragmentShaderTarget target{
        .gles = true, .allow_shadow = false, .emit_explicit_material_lod_bias = true,
        .emulate_material_border = true};
    const auto config = [&] { return OpenGL::PicaFSConfig::BuildFromRegs(regs, false, target); };
    const auto state = config().state;
    const auto& border = state.material_borders[0];
    Require(border.axes == 3 && border.rgba == t.border_color.raw &&
            border.min_level == 0 && border.max_level == 1 &&
            border.min_linear && border.mag_linear && !border.mip_linear &&
            state.material_lod_bias[0] == -180, "Preserve colored mip border and signed LOD");
    Require(!OpenGL::PicaFSConfig::BuildFromRegs(regs, false, {true, false})
                 .state.material_borders[0].axes, "GL retains hardware border sampling");
    t.wrap_s.Assign(Texture::Repeat);
    Require(config().state.material_borders[0].axes == 2, "Only T clamps to border");
    t.border_color.raw = 0xff000000;
    Require(!config().state.material_borders[0].axes, "Metal retains fixed opaque black sampler");
    t.border_color.raw = 0x00f8a4fe;
    regs.texturing.main_config.texture0_enable.Assign(0);
    Require(!config().state.material_borders[0].axes, "Ignore stale disabled sampler registers");
}
