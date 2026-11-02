// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <iostream>
#include "core/frontend/emu_window.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/pica_state.h"
#include "video_core/swrasterizer/framebuffer.h"
#include "video_core/swrasterizer/rasterizer.h"
#include "video_core/swrasterizer/texturing.h"
#include "video_core/video_core.h"

class TestWindow final : public Frontend::EmuWindow {
public:
    TestWindow() { UpdateFramebufferLayout(400, 480); }
    void PollEvents() override {}
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    void SwapBuffers() override {}
};

static void Square(const Common::Vec4<float>& color) {
    using namespace Pica;
    const auto Vertex = [&](float x, float y) {
        Shader::OutputVertex output{};
        output.pos.w = float24::FromFloat32(1);
        for (unsigned c = 0; c < 4; ++c) output.color[c] = float24::FromFloat32(color[c]);
        Rasterizer::Vertex vertex(output);
        vertex.screenpos = {float24::FromFloat32(x), float24::FromFloat32(y), float24::FromFloat32(0)};
        return vertex;
    };
    auto a = Vertex(0, 0), b = Vertex(64, 0), c = Vertex(0, 64), d = Vertex(64, 64);
    Rasterizer::ProcessTriangle(a, b, c);
    Rasterizer::ProcessTriangle(b, d, c);
}

static bool Check(const char* name, const Common::Vec4<u8>& expected) {
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 64; ++x) {
            const auto actual = Pica::Rasterizer::GetPixel(x, y);
            for (unsigned c = 0; c < 4; ++c) {
                if (actual[c] != expected[c]) {
                    std::cerr << name << " mismatch at " << x << ',' << y << " channel " << c
                              << ": " << unsigned(actual[c]) << " != " << unsigned(expected[c]) << '\n';
                    return false;
                }
            }
        }
    }
    std::cout << name << ": 4096 pixels passed\n";
    return true;
}

int main() {
    Memory::MemorySystem memory;
    TestWindow window;
    Settings::values.resolution_factor = 1;
    if (VideoCore::Init(window, memory) != VideoCore::ResultStatus::Success) return 1;
    auto& regs = Pica::g_state.regs;
    auto& fb = regs.framebuffer.framebuffer;
    fb.color_buffer_address.Assign(Memory::VRAM_PADDR / 8);
    fb.color_format.Assign(Pica::FramebufferRegs::ColorFormat::RGBA8);
    fb.width.Assign(64); fb.height.Assign(63); fb.allow_color_write.Assign(1);
    regs.lighting.disable.Assign(1);
    auto& om = regs.framebuffer.output_merger;
    om.red_enable.Assign(1); om.green_enable.Assign(1); om.blue_enable.Assign(1); om.alpha_enable.Assign(1);
    om.logic_op.Assign(Pica::FramebufferRegs::LogicOp::Copy);
    using Tev = Pica::TexturingRegs::TevStageConfig;
    const std::array<Tev*, 6> stages{&regs.texturing.tev_stage0,&regs.texturing.tev_stage1,
        &regs.texturing.tev_stage2,&regs.texturing.tev_stage3,&regs.texturing.tev_stage4,&regs.texturing.tev_stage5};
    for (unsigned i = 0; i < stages.size(); ++i) {
        stages[i]->color_source1.Assign(i == 0 ? Tev::Source::PrimaryColor : Tev::Source::Previous);
        stages[i]->alpha_source1.Assign(i == 0 ? Tev::Source::PrimaryColor : Tev::Source::Previous);
    }
    Square({1,0.5f,0,1});
    if (!Check("opaque copy and triangle edge coverage", {255,128,0,255})) return 2;

    om.alphablend_enable.Assign(1);
    using Factor = Pica::FramebufferRegs::BlendFactor;
    om.alpha_blending.factor_source_rgb.Assign(Factor::SourceAlpha);
    om.alpha_blending.factor_dest_rgb.Assign(Factor::OneMinusSourceAlpha);
    om.alpha_blending.factor_source_a.Assign(Factor::One);
    om.alpha_blending.factor_dest_a.Assign(Factor::Zero);
    const Common::Vec4<u8> src{0,255,0,128}, dest{255,128,0,255};
    auto expected = Pica::Rasterizer::EvaluateBlendEquation(src,{128,128,128,255},dest,{127,127,127,0},Pica::FramebufferRegs::BlendEquation::Add);
    Square({0,1,0,0.5f});
    if (!Check("straight alpha blend", expected)) return 3;

    om.alphablend_enable.Assign(0); om.red_enable.Assign(0);
    Square({0,0,1,1});
    if (!Check("masked color copy", {expected.r(),0,255,255})) return 4;
    om.red_enable.Assign(1);
    stages[0]->color_source1.Assign(Tev::Source::Constant);
    stages[0]->alpha_source1.Assign(Tev::Source::Constant);
    stages[0]->const_r.Assign(20); stages[0]->const_g.Assign(40); stages[0]->const_b.Assign(200); stages[0]->const_a.Assign(100);
    stages[0]->color_scale.Assign(1); stages[0]->alpha_scale.Assign(1);
    Square({1,1,1,1});
    if (!Check("constant combiner scaling and saturation", {40,80,255,200})) return 5;
    VideoCore::Shutdown();
    return 0;
}
