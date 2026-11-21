// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_native_uniforms.h"

#include "video_core/pica_state.h"
#include "video_core/renderer_metal/pica_metal_lut.h"

namespace Cara {

PicaMetalNativeUniforms CapturePicaMetalUniforms(const Pica::State& state, bool disable_clip_coef) {
    const auto& regs = state.regs;
    const auto& r = regs.rasterizer;
    const auto& t = regs.texturing;
    const auto& sc = r.scissor_test;
    PicaMetalNativeUniforms out{};
    out.vertex.uniforms.SetFromRegs(regs.vs, state.vs);
    auto& common = out.common;
    common.alphatest_ref = regs.framebuffer.output_merger.alpha_test.ref;
    common.depth_scale = Pica::float24::FromRaw(r.viewport_depth_range).ToFloat32();
    common.depth_offset = Pica::float24::FromRaw(r.viewport_depth_near_plane).ToFloat32();
    common.scissor_x1 = sc.x1;
    common.scissor_y1 = sc.y1;
    common.scissor_x2 = u32(sc.x2) + 1;
    common.scissor_y2 = u32(sc.y2) + 1;
    common.fog_lut_offset = PicaMetalLutData::fog_offset;
    common.fog_color = {t.fog_color.r.Value() / 255.0f, t.fog_color.g.Value() / 255.0f,
                        t.fog_color.b.Value() / 255.0f};
    const auto tev = t.GetTevStages();
    for (std::size_t i = 0; i < tev.size(); ++i)
        common.const_color[i] = PicaToGL::ColorRGBA8(tev[i].const_color);
    common.tev_combiner_buffer_color = PicaToGL::ColorRGBA8(t.tev_combiner_buffer_color.raw);
    if (!disable_clip_coef) {
        const auto clip = r.GetClipCoef();
        common.clip_coef = {clip.x.ToFloat32(), clip.y.ToFloat32(), clip.z.ToFloat32(),
                            clip.w.ToFloat32()};
    }
    common.cara_viewport = {
        static_cast<float>(r.viewport_corner.x), static_cast<float>(r.viewport_corner.y),
        Pica::float24::FromRaw(r.viewport_size_x).ToFloat32(),
        Pica::float24::FromRaw(r.viewport_size_y).ToFloat32()};

    auto& lighting = out.light;
    const auto& l = regs.lighting;
    lighting.lighting_global_ambient = PicaToGL::LightColor(l.global_ambient);
    for (unsigned i = 0; i < Pica::LightingRegs::NumLightingSampler; ++i)
        lighting.lighting_lut_offset[i / 4][i % 4] = i * PicaMetalLutData::entries_per_sampler;
    const auto& scale = l.lut_scale;
    const float scales[] = {scale.GetScale(scale.d0), scale.GetScale(scale.d1),
                            scale.GetScale(scale.sp), scale.GetScale(scale.fr),
                            scale.GetScale(scale.rb), scale.GetScale(scale.rg),
                            scale.GetScale(scale.rr)};
    for (unsigned i = 0; i < std::size(scales); ++i)
        lighting.lighting_lut_scales[i] = scales[i];

    for (unsigned i = 0; i < std::size(lighting.light_src); ++i) {
        const auto& source = l.light[i];
        auto& dest = lighting.light_src[i];
        dest.specular_0 = PicaToGL::LightColor(source.specular_0);
        dest.specular_1 = PicaToGL::LightColor(source.specular_1);
        dest.diffuse = PicaToGL::LightColor(source.diffuse);
        dest.ambient = PicaToGL::LightColor(source.ambient);
        dest.position = {Pica::float16::FromRaw(source.x).ToFloat32(),
                         Pica::float16::FromRaw(source.y).ToFloat32(),
                         Pica::float16::FromRaw(source.z).ToFloat32()};
        dest.spot_direction = {source.spot_x / 2047.0f, source.spot_y / 2047.0f,
                               source.spot_z / 2047.0f};
        dest.dist_atten_bias = Pica::float20::FromRaw(source.dist_atten_bias).ToFloat32();
        dest.dist_atten_scale = Pica::float20::FromRaw(source.dist_atten_scale).ToFloat32();
    }
    // The caller rejects procedural and shadow rendering before using this layout.
    return out;
}

} // namespace Cara
