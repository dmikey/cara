// SPDX-License-Identifier: GPL-2.0-or-later
// Real PICA generator -> GLSL/SPIR-V/MSL -> GPU equivalence of optional source
// transformations. Canonicalize all varyings/clip outputs, never struct padding.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <nihstro/inline_assembly.h>
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"
#include "video_core/renderer_metal/batched_vertex_source.h"
#include "video_core/renderer_metal/pica_metal_runtime_shader.h"
#include "video_core/renderer_metal/pica_metal_upload.h"
#include "video_core/renderer_metal/pica_metal_source_pool.h"
#include "video_core/renderer_metal/pica_metal_pipelines.h"
#include "video_core/renderer_metal/pica_metal_raw_index_format.hh"
#include "video_core/renderer_metal/simd_vertex_source.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
using A=nihstro::InlineAsm; using O=nihstro::OpCode; using S=nihstro::SourceRegister;
using D=nihstro::DestRegister; using I=nihstro::Instruction;
void Require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
constexpr char fragment[] = R"(
in vec4 primary_color;
in vec2 texcoord0; in vec2 texcoord1; in vec2 texcoord2;
in float texcoord0_w; in vec4 normquat; in vec3 view;
layout(location=0) out vec4 color;
void main() {
    color=primary_color+normquat+vec4(texcoord0,texcoord1)+vec4(texcoord2,texcoord0_w,0.0)+vec4(view,0.0);
}
)";
Pica::Shader::ShaderSetup Program(unsigned kind) {
    const auto binary=A::CompileToRawBinary({
        {O::Id::MOV,D::MakeTemporary(0),S::MakeInput(1)}, {O::Id::NOP},
        {O::Id::ADD,D::MakeTemporary(0),S::MakeFloat(0),S::MakeTemporary(0)},
        {O::Id::ADD,D::MakeTemporary(0),S::MakeTemporary(0),S::MakeFloat(1)},
        {O::Id::NOP},
        {O::Id::ADD,D::MakeTemporary(0),S::MakeTemporary(0),S::MakeFloat(2)},
        {O::Id::MOV,D::MakeOutput(0),S::MakeInput(0)},
        {O::Id::MOV,D::MakeOutput(1),S::MakeTemporary(0)},
        {O::Id::MOV,D::MakeOutput(2),S::MakeInput(2)},
        {O::Id::MOV,D::MakeOutput(3),S::MakeInput(3)},
        {O::Id::MOV,D::MakeOutput(4),S::MakeInput(4)},
        {O::Id::MOV,D::MakeOutput(5),S::MakeInput(5)}, {O::Id::END}});
    Pica::Shader::ShaderSetup setup{};
    for (unsigned i=0;i<binary.program.size();++i) {
        setup.program_code[i]=binary.program[i].hex;
    }
    setup.MarkProgramCodeDirty();
    for (unsigned i=0;i<binary.swizzle_table.size();++i) {
        setup.swizzle_data[i]=binary.swizzle_table[i].hex;
    }
    setup.MarkSwizzleDataDirty();
    if (kind==1 || kind==2) {
        I flow{}; flow.opcode=kind==1 ? O::Id::LOOP : O::Id::IFU;
        flow.flow_control.dest_offset=kind==1 ? 3 : 4;
        flow.flow_control.num_instructions=kind==1 ? 0 : 2;
        flow.flow_control.bool_uniform_id=0;
        setup.program_code[1]=flow.hex;
        setup.MarkProgramCodeDirty();
    } else if (kind==3) {
        const auto address=A::CompileToRawBinary({{O::Id::MOVA,D::MakeTemporary(0),"xy",S::MakeInput(5)}});
        I move=address.program[0];
        // Add the instruction's descriptor instead of reinterpreting another mask.
        unsigned desc=binary.swizzle_table.size(); setup.swizzle_data[desc]=address.swizzle_table[0].hex;
        setup.MarkSwizzleDataDirty();
        move.common.operand_desc_id=desc; setup.program_code[1]=move.hex;
        setup.MarkProgramCodeDirty();
        I add{}; add.hex=setup.program_code[2]; add.common.address_register_index=A::A1;
        setup.program_code[2]=add.hex;
        setup.MarkProgramCodeDirty();
    }
    return setup;
}
}
namespace Cara { bool EncodeGroupedMetalDraw(id<MTLCommandBuffer>,PicaMetalUploadArena&,const TexturedMetalDraw&); bool CloseGroupedMetalDraw(id<MTLCommandBuffer>); }
int main() { @autoreleasepool { try {
 setenv("CARA_METAL_RAW_INDICES","1",1);setenv("CARA_METAL_RAW_INDEX_CONSTANTS","1",1);
 setenv("CARA_METAL_GROUPED_PASS","1",1);setenv("CARA_METAL_FRAME_COST","1",1);
 setenv("CARA_METAL_UNIQUE_VERTEX_COMPUTE","0",1);setenv("CARA_METAL_BATCH_VERTEX_COMPUTE","0",1);
 setenv("CARA_GPU_QUATERNION_SLICE","0",1);setenv("CARA_GPU_PACKED_FETCH_REUSE","0",1);
 auto device=MTLCreateSystemDefaultDevice();Require(device && [device supportsFamily:MTLGPUFamilyApple7],"Requires Apple7+ GPU");
 auto queue=[device newCommandQueue];Cara::PicaMetalPipelines pipelines(device);
 Pica::Regs regs{};regs.vs.output_mask.Assign(0x3f);OpenGL::PicaFixedGSConfig output(regs);
 for(unsigned i=0;i<16;++i)output.state.semantic_maps[i]={i/4,i%4};
 output.state.semantic_maps[16]={4,2};output.state.semantic_maps[18]={4,3};output.state.semantic_maps[19]={5,0};
 output.state.semantic_maps[20]={5,1};output.state.semantic_maps[22]={4,0};output.state.semantic_maps[23]={4,1};
 unsigned cases=0;
 for(unsigned kind=0;kind<4;++kind) {
  auto setup=Program(kind);OpenGL::PicaVSConfig config(regs,setup);
  std::array<std::string,3> sources;
  sources[0]=OpenGL::GenerateVertexShader(setup,config,false,&output,0x3f,2,true,false);sources[1]=sources[0];
  Require(Cara::TagSimdVertexSource(sources[1]),"SIMD source admission failed");
  sources[2]=sources[0];
  const auto init=sources[2].find("vec4 first_quat=vec4(0.0);");
  Require(init!=sources[2].npos,"Missing fallback fixture initializer");
  sources[2].replace(init,std::strlen("vec4 first_quat=vec4(0.0);"),"vec4 first_quat=vec4(0.0,0.0,0.0,0.0);");
  Require(!Cara::TagSimdVertexSource(sources[2]),"Unsupported source spelling was admitted");
  sources[2]+=Cara::SimdVertexTag; // Represents an unsupported/stale tagged request.
  std::array<Cara::PicaMetalPipelineSelection,3> selected;
  for(unsigned mode=0;mode<3;++mode) {
   Cara::PicaPipelineInputs p{};p.vertex=sources[mode];p.fragment=fragment;p.vertex_mode=Cara::PicaMetalVertexMode::Packed;
   p.packed_arrays=true;p.clip0=true;p.dither=false;p.color_mask=15;p.winding=0x900;p.sampled_unit=Cara::NoMaterialUnit;
   auto units=pipelines.GateMaterialUnits(p.vertex,p.fragment,p.vertex_mode);
   Require(units && (*units)[0]==Cara::NoMaterialUnit,"Production runtime source registration failed");
   Require(pipelines.Prepare(p,{}),"Production SIMD pipeline Prepare failed");
   auto entry=pipelines.Select(p,{});Require(bool(entry) && entry->simd_reference==(mode==1),"PSO SIMD selection/reflection did not match actual source");
   selected[mode]=*entry;
  }
  auto render=[&](bool mixed) {
   Cara::NativeMetalCost::spans={};auto command=[queue commandBuffer];Cara::PicaMetalUploadArena arena(device,256*1024);
   Require(arena.Begin(command),"Production draw arena Begin failed");
   auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
   td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageRenderTarget;auto color=[device newTextureWithDescriptor:td];
   td.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;td.storageMode=MTLStorageModePrivate;auto depth=[device newTextureWithDescriptor:td];
   std::array<std::uint8_t,1024> pixels{};[color replaceRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:64];
   std::array<std::byte,2048> vertices{};
   for(unsigned v=0;v<4;++v) {
    float sign=v&1 ? -1.f : 1.f;
    const std::array<std::array<float,4>,6> input{{{v&1 ? 1.f : -1.f,v&2 ? 1.f : -1.f,-0.5f,1},
     {sign*0.5f,sign*0.25f,sign*0.125f,sign*0.75f},{0.1f,0.2f,0.3f,0.4f},{0.01f,0.02f,0.03f,0.04f},
     {0.02f,0.01f,1.0f,float(v)/8},{float(v%3),0.01f,0,0}}};std::memcpy(vertices.data()+5+v*96,input.data(),96);
   }
   std::array<std::array<std::int32_t,4>,16> layout{};for(unsigned i=0;i<6;++i)layout[i]={int(5+i*16),96,3,4};layout[0][3]|=(8<<3)|(7<<7);
   std::array<std::uint16_t,256> indices{8,7,7,8,9,9,8,10};
   for(unsigned draw_index=0;draw_index<4;++draw_index) {
    const unsigned mode=mixed ? std::array<unsigned,4>{0,1,2,1}[draw_index] : 0;Cara::TexturedMetalDraw draw{};selected[mode].Apply(draw);
    draw.vertex_mode=Cara::MetalRawIndexMode;draw.color=color;draw.depth_stencil=depth;draw.vertex_count=6;
    draw.viewport={0,0,16,16,0,1};draw.scissor={draw_index*4,0,4,16};draw.packed_vertices=vertices;
    draw.expanded_indices=std::as_bytes(std::span(indices));draw.layout_uniform=std::as_bytes(std::span(layout));
    OpenGL::VSUniformData vs{};vs.uniforms.bools[0].b=draw_index&1;vs.uniforms.i[0]={1+(draw_index&1),0,0,0};
    vs.uniforms.f[0]={0.125f,0,0,0};vs.uniforms.f[1]={-0.0625f,0.0625f,0,0};vs.uniforms.f[2]={0.25f,-0.125f,0.125f,0};
    OpenGL::UniformData common{};common.cara_viewport={0,0,160,120};common.clip_coef={0,0,0,1};std::array<std::byte,1040> light{};
    draw.vs_uniform=std::as_bytes(std::span(&vs,1));draw.common_uniform=std::as_bytes(std::span(&common,1));draw.light_uniform=light;
    Require(Cara::EncodeGroupedMetalDraw(command,arena,draw),"Production SIMD grouped draw failed");
   }
   Require(Cara::CloseGroupedMetalDraw(command) && arena.Submit(),"Production grouped pass submit failed");[command waitUntilCompleted];
   Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"Production SIMD GPU command failed");
   Require(Cara::NativeMetalCost::spans[Cara::NativeMetalCost::SimdReferenceDraws].calls==(mixed ? 2u : 0u),"SIMD token/draw receipt count wrong");
   [color getBytes:pixels.data() bytesPerRow:64 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];return pixels;
  };
  const auto baseline=render(false),mixed=render(true);Require(baseline==mixed,"Production SIMD token/state switching changed pixels");++cases;
 }
 std::cout<<"PASS:"<<cases<<" production SIMD/fallback pipeline/Apply/grouped token-binding cases;32draws and exact mixed-pass pixels\n";
 return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
} }
