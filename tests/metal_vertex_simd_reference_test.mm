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
#include "video_core/renderer_metal/simd_vertex_source.h"
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
struct Result { std::vector<std::uint32_t> values,stats; };
struct Case { unsigned start,count,instances; };
}
int main() { @autoreleasepool { try {
 setenv("CARA_GPU_QUATERNION_SLICE","0",1);setenv("CARA_GPU_PACKED_FETCH_REUSE","0",1);
 auto device=MTLCreateSystemDefaultDevice();Require(device && device.hasUnifiedMemory,"Apple GPU unavailable");
 auto queue=[device newCommandQueue];Pica::Regs regs{};regs.vs.output_mask.Assign(0x3f);
 OpenGL::PicaFixedGSConfig output(regs);
 for(unsigned i=0;i<16;++i)output.state.semantic_maps[i]={i/4,i%4};
 output.state.semantic_maps[16]={4,2};output.state.semantic_maps[18]={4,3};output.state.semantic_maps[19]={5,0};
 output.state.semantic_maps[20]={5,1};output.state.semantic_maps[22]={4,0};output.state.semantic_maps[23]={4,1};
 auto compile=[&](std::string_view code) {
  auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_3;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  options.fastMathEnabled=NO;
#pragma clang diagnostic pop
  NSError* error=nil;auto text=[[NSString alloc]initWithBytes:code.data() length:code.size() encoding:NSUTF8StringEncoding];
  auto library=[device newLibraryWithSource:text options:options error:&error];
  if(!library)throw std::runtime_error(error.localizedDescription.UTF8String);return library;
 };
 std::uint64_t sequence=1,reused=0,fallbacks=0,vertices_checked=0;unsigned comparisons=0;
 const std::array cases{Case{0,3,1},Case{1,3,1},Case{0,33,1},Case{1,66,1},Case{30,99,1},Case{2,195,2}};
 for(unsigned kind=0;kind<4;++kind) {
  auto setup=Program(kind);OpenGL::PicaVSConfig config(regs,setup);
  auto baseline=OpenGL::GenerateVertexShader(setup,config,false,&output,0x3f,2,true,false);
  auto no_reference=Cara::RemoveVertexReference(baseline);Require(bool(no_reference),"Actual source reference removal failed");auto actual=*no_reference;
  std::array<id<MTLRenderPipelineState>,2> pipelines{};
  for(unsigned mode=0;mode<2;++mode) {
   std::string error;auto stages=Cara::TranslateRuntimeMetalShaders(mode ? actual : baseline,fragment,
        Cara::RuntimeMetalVertexMode::Packed,true,true,error);
   if(!stages)throw std::runtime_error(error);
   if(mode) {
    auto production=Cara::MakeVertexReferenceSource(stages->vertex,true,false);
    Require(production && Cara::ValidSimdVertexMsl(*production),"Production wrapper validation failed");
    auto tagged=baseline;Require(Cara::TagSimdVertexSource(tagged) && Cara::HasSimdVertexTag(tagged),"Versioned tag admission failed");
    auto translated=Cara::TranslateRuntimeMetalShaders(tagged,fragment,Cara::RuntimeMetalVertexMode::Packed,true,true,error);
    Require(translated && translated->vertex==*production,"Tagged production translator differs from shared helper");
    auto damaged=*production;damaged.back()='x';Require(!Cara::ValidSimdVertexMsl(damaged),"Damaged cached SIMD wrapper was accepted");
    Require(!Cara::ValidSimdVertexMsl(stages->vertex),"Full fallback shader was mislabeled SIMD");
   }
   auto wrapped=Cara::MakeVertexReferenceSource(stages->vertex,mode,true);Require(bool(wrapped),"Production SIMD builder failed");
   auto library=compile(*wrapped);auto frag=compile(stages->fragment);
   auto desc=[MTLRenderPipelineDescriptor new];desc.vertexFunction=[library newFunctionWithName:@"cara_pica_vert"];
   desc.fragmentFunction=[frag newFunctionWithName:@"cara_pica_frag"];desc.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
   NSError* failure=nil;pipelines[mode]=[device newRenderPipelineStateWithDescriptor:desc error:&failure];
   if(!pipelines[mode])throw std::runtime_error(failure.localizedDescription.UTF8String);
  }
  for(unsigned offset : {0u,1u,5u,15u})for(unsigned state=0;state<2;++state)for(const auto test:cases) {
   auto render=[&](unsigned mode)->Result { @autoreleasepool {
    auto command=[queue commandBuffer];Cara::PicaMetalUploadArena arena(device,256*1024);Require(arena.Begin(command),"Arena begin failed");
    std::array<std::byte,2048> bytes{};
    for(unsigned v=0;v<4;++v) {
     const float sign=v&1 ? -1.f : 1.f;
     const std::array<std::array<float,4>,6> attributes{{{v&1 ? 1.f : -1.f,v&2 ? 1.f : -1.f,-0.5f,1.f},
       {sign*0.5f,sign*0.25f,sign*0.125f,sign*0.75f},{-0.25f,0.375f,1.5f,0.125f},
       {0.125f,0.25f,0.5f,0.75f},{0.625f,0.875f,1.25f,float(v)/2},{float(v%3),0.875f,0.25f,0.5f}}};
     std::memcpy(bytes.data()+offset+v*96,attributes.data(),96);
    }
    std::array<std::array<std::int32_t,4>,16> layout{};for(unsigned i=0;i<6;++i)layout[i]={int(offset+i*16),96,3,4};
    layout[0][3]|=(8<<3)|(7<<7);std::array<std::uint16_t,256> indices{8,7};
    constexpr std::array<unsigned,6> order{0,1,2,2,1,3};for(unsigned i=0;i<254;++i)indices[i+2]=7+order[i%6];
    OpenGL::VSUniformData uniforms{};uniforms.uniforms.bools[0].b=state;uniforms.uniforms.i[0]={1+state,0,0,0};
    uniforms.uniforms.f[0]={0.125f,0,0,0};uniforms.uniforms.f[1]={-0.0625f,0.0625f,0,0};
    uniforms.uniforms.f[2]={0.25f,-0.125f,0.125f,0};uniforms.uniforms.f[3]={0.375f,0.125f,0,0};
    OpenGL::UniformData common{};common.cara_viewport={0,0,160,120};common.clip_coef={0,0,0,1};
    const auto l=arena.Copy(std::as_bytes(std::span(layout)),256),u=arena.Copy(std::as_bytes(std::span(&uniforms,1)),256),
               c=arena.Copy(std::as_bytes(std::span(&common,1)),256);
    auto vertices=arena.Copy2D(bytes,MTLPixelFormatRGBA32Uint,128,1,2048);
    auto idx=arena.Copy2D(std::as_bytes(std::span(indices)),MTLPixelFormatR16Uint,256,1,512);
    Require(l && u && c && vertices && idx,"Vertex inputs unavailable");
    auto values=[device newBufferWithLength:512*24*4 options:MTLResourceStorageModeShared];
    auto stats=[device newBufferWithLength:512*4*4 options:MTLResourceStorageModeShared];
    std::memset(values.contents,0xcd,values.length);std::memset(stats.contents,0xcd,stats.length);
    auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    desc.storageMode=MTLStorageModePrivate;desc.usage=MTLTextureUsageRenderTarget;
    auto target=[device newTextureWithDescriptor:desc];auto pass=[MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionDontCare;
    auto encoder=[command renderCommandEncoderWithDescriptor:pass];[encoder setRenderPipelineState:pipelines[mode]];
    const std::array ranges{*l,*u,*c};for(unsigned i=0;i<3;++i)[encoder setVertexBuffer:ranges[i].buffer offset:ranges[i].offset atIndex:i];
    const std::uint64_t token=(sequence++<<32)|1u;[encoder setVertexBytes:&token length:8 atIndex:3];
    [encoder setVertexBuffer:values offset:0 atIndex:4];[encoder setVertexBuffer:stats offset:0 atIndex:5];
    [encoder setVertexTexture:vertices atIndex:0];[encoder setVertexTexture:idx atIndex:1];
    auto sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    [encoder setVertexSamplerState:sampler atIndex:0];[encoder setVertexSamplerState:sampler atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:test.start vertexCount:test.count instanceCount:test.instances];
    [encoder endEncoding];Require(arena.Submit(),"SIMD diagnostic submit failed");[command waitUntilCompleted];
    Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"SIMD vertex GPU execution failed");
    Result result;result.values.resize(512*24);result.stats.resize(512*4);
    std::memcpy(result.values.data(),values.contents,values.length);std::memcpy(result.stats.data(),stats.contents,stats.length);return result;
   } };
   const auto reference=render(0),candidate=render(1);
   for(unsigned instance=0;instance<test.instances;++instance)for(unsigned id=test.start;id<test.start+test.count;++id) {
    const auto slot=instance*256+id;
    Require(reference.stats[slot*4+2]==id && candidate.stats[slot*4+2]==id && candidate.stats[slot*4+3]==instance,
            "Vertex stage did not capture each requested vertex/instance");
    Require(std::memcmp(reference.values.data()+slot*24,candidate.values.data()+slot*24,24*4)==0,
            "SIMD reference reuse changed actual PICA output/clip bits");
    if(id%3) {fallbacks+=candidate.stats[slot*4];reused+=candidate.stats[slot*4+1];}
    if(test.start==1 && test.count==3 && id<3)Require(candidate.stats[slot*4]==1,"Missing triangle-first lane did not fall back");
    ++vertices_checked;
   }
   ++comparisons;
  }
 }
 Require(reused>0 && fallbacks>0,"Diagnostic did not exercise both SIMD reuse and original fallback");
 std::cout<<"PASS:"<<comparisons<<" actual vertex-stage PICA pairs, "<<vertices_checked<<" exact all-varying/clip outputs, "
          <<reused<<" reference reuses, "<<fallbacks<<" original fallbacks; raw indices, unaligned inputs, loops, IFU, indirect uniforms\n";
 return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
} }
