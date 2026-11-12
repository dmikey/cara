// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include "video_core/renderer_opengl/gl_color_readback.h"
#include <array>
#include <iostream>
#include <unordered_map>
#include <memory>
#include "video_core/renderer_opengl/gl_vertex_source_map.h"
#include "video_core/renderer_opengl/gl_vertex_index_cache.h"
#include "core/frontend/emu_window.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/pica_state.h"
#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
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

static bool CheckVertexSourceMap() {
    auto map = std::make_unique<OpenGL::IOSVertexSourceMap>();
    std::unordered_map<u16,u16> reference;
    u32 seed=0x87654321;
    for (unsigned draw=0;draw<32;++draw) {
        map->BeginDraw(); reference.clear();
        for (unsigned i=0;i<8192;++i) {
            seed^=seed<<13; seed^=seed>>17; seed^=seed<<5;
            const u16 source=draw%2 ? u16(seed%257) : u16(seed);
            const auto [it,inserted]=reference.emplace(source,u16(reference.size()));
            const auto actual=map->Resolve(source);
            if (actual.index!=it->second || actual.inserted!=inserted) return false;
        }
        if (map->Size()!=reference.size()) return false;
    }
    map->BeginDraw();
    for (u32 source=0;source<65536;++source) {
        const auto record=map->Resolve(u16(source));
        if (!record.inserted || record.index!=source) return false;
    }
    if (map->Size()!=65536) return false;
    map->BeginDraw(); map->Resolve(1); map->Resolve(2); map->Resolve(42);
    // Return to this exact stamp value after wrapping; dormant entries must retire.
    for (unsigned draw=0;draw<65535;++draw) map->BeginDraw();
    map->BeginDraw();
    const auto fresh=map->Resolve(42);
    if (!fresh.inserted || fresh.index!=0 || map->Size()!=1) return false;
    std::cout << "Source vertex reuse, draw resets, full u16 domain and epoch wrap passed\n";
    return true;
}

static bool CheckPackedVertexInputs() {
    u32 seed=0x1479ABCD;
    const auto random=[&]() { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; };
    for (unsigned trial=0;trial<8192;++trial) {
        Pica::ShaderRegs regs{};
        regs.max_input_attribute_index.Assign(trial%16);
        regs.input_attribute_to_register_map_low=random();
        regs.input_attribute_to_register_map_high=random();
        Pica::Shader::AttributeBuffer attributes{};
        for (unsigned attr=0;attr<16;++attr)
            for (unsigned c=0;c<4;++c)
                attributes.attr[attr][c]=Pica::float24::FromFloat32(float(s32(random()))/65536.0f);
        Pica::Shader::UnitState reference;
        reference.LoadInput(regs,attributes);
        const OpenGL::IOSVertexInputLayout layout(regs);
        std::array<Common::Vec4<float>,16> packed{};
        layout.PackAttributes(attributes,packed);
        for (unsigned reg=0;reg<16;++reg) {
            if (layout.offsets[reg]<0) continue;
            for (unsigned c=0;c<4;++c) {
                if (packed[layout.offsets[reg]][c]!=reference.registers.input[reg][c].ToFloat32()) {
                    std::cerr << "Packed vertex differs from interpreter register mapping\n";
                    return false;
                }
            }
        }
    }
    std::cout << "8192 sparse, permuted and duplicate vertex mappings match interpreter inputs\n";
    return true;
}

static bool CheckVertexIndexCache() {
    using Cache = OpenGL::IOSVertexIndexCache;
    Cache cache(2 * 1024 * 1024, 4);
    std::array<u8,6> source{3,1,3,7,1,7};
    const Cache::Key key{0x20000000,6,0,0,1};
    std::vector<u32> triangles{3,1,3,7,1,7};
    const auto expected=triangles;
    const auto* data=cache.Store(key,source.data(),source.size(),triangles);
    if (!data || data->triangles!=expected || data->first!=1 || data->last!=7 || data->unique!=3) return false;
    for (size_t i=0;i<expected.size();++i)
        if (u32(data->packed[i])+data->first!=expected[i]) return false;
    for (size_t i=expected.size();i<data->packed.size();++i) if (data->packed[i]) return false;
    if (cache.Find(key,source.data(),source.size())!=data) return false;
    // A write to the same physical address must invalidate before reuse.
    source[2]=9;
    if (cache.Find(key,source.data(),source.size()) || cache.Bytes()!=0) return false;
    triangles={3,1,9,7,1,7};
    if (!cache.Store(key,source.data(),source.size(),triangles)) return false;
    auto other=key; ++other.topology;
    if (cache.Find(other,source.data(),source.size())) return false;
    other=key; ++other.offset;
    if (cache.Find(other,source.data(),source.size())) return false;
    other=key; ++other.format;
    if (cache.Find(other,source.data(),source.size())) return false;
    if (cache.Find(key,source.data(),source.size()-1)) return false;
    // All 16-bit indices, including the largest, round-trip without aliasing.
    triangles.resize(65536);
    for (u32 i=0;i<65536;++i) triangles[i]=65535-i;
    data=cache.Store(key,nullptr,0,triangles);
    if (!data || data->unique!=65536 || data->packed.front()!=65535 || data->packed.back()!=0) return false;
    triangles={0,65536,3};
    data=cache.Store(key,nullptr,0,triangles);
    if (!data || data->packable || !data->packed.empty()) return false;
    // Account actual vector capacity, and preserve the caller on rejection.
    Cache tiny(8,4); triangles={1,2,3}; const auto original=triangles;
    if (tiny.Store(key,nullptr,0,triangles) || triangles!=original || tiny.Bytes()!=0) return false;
    Cache bounded(800,2);
    for (u32 i=0;i<30;++i) {
        auto current=key; current.address+=i*16; triangles={1,2,3};
        if (!bounded.Store(current,nullptr,0,triangles) || bounded.Bytes()>800 || bounded.Size()>2) return false;
    }
    if (bounded.Find(key,nullptr,0)) return false;
    auto last=key; last.address+=29*16;
    if (!bounded.Find(last,nullptr,0)) return false;
    std::cout << "Index cache: byte invalidation, metadata keys, full u16 range, bounds and eviction passed\n";
    return true;
}

static bool CheckPackedColorReadback() {
    const auto normalized=[](u32 value,u32 max) { return u8((value*255+max/2)/max); };
    for (u32 packed=0;packed<=0xffff;++packed) {
        const std::array<u8,4> rgb565{normalized((packed>>11)&31,31),normalized((packed>>5)&63,63),normalized(packed&31,31),255};
        const std::array<u8,4> rgb5a1{normalized((packed>>11)&31,31),normalized((packed>>6)&31,31),normalized((packed>>1)&31,31),normalized(packed&1,1)};
        const std::array<u8,4> rgba4{normalized((packed>>12)&15,15),normalized((packed>>8)&15,15),normalized((packed>>4)&15,15),normalized(packed&15,15)};
        if (OpenGL::ColorReadback::PackRGB565(rgb565.data())!=packed ||
            OpenGL::ColorReadback::PackRGB5A1(rgb5a1.data())!=packed ||
            OpenGL::ColorReadback::PackRGBA4(rgba4.data())!=packed) return false;
    }
    std::cout << "196608 normalized packed-color round trips passed\n";
    return true;
}

int main() {
    if (!CheckPackedColorReadback()) return 1;
    if (!CheckVertexIndexCache()) { std::cerr << "Vertex index cache regression\n"; return 1; }
    if (!CheckVertexSourceMap()) { std::cerr << "Source vertex map regression\n"; return 1; }
    if (!CheckPackedVertexInputs()) return 1;
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
