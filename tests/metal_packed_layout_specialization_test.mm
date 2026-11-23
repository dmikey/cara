// SPDX-License-Identifier: GPL-2.0-or-later
// Authored packed-layout specialization -> actual GLSL/SPIR-V/MSL -> GPU equivalence of source
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
#include <array>
#include <cstring>
#include <iostream>
#include <fstream>
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
constexpr char canonical[] = R"(
kernel void canonical_vertex_outputs(device const uchar* raw [[buffer(0)]],
                                     device float4* values [[buffer(1)]], uint id [[thread_position_in_grid]]) {
    if (id>=6) return;
    auto v=reinterpret_cast<device const CaraComputeOutput*>(raw+ulong(id)*128)->value;
    values[id*6+0]=v.gl_Position;
    values[id*6+1]=v.primary_color;
    values[id*6+2]=float4(v.texcoord0,v.texcoord1);
    values[id*6+3]=float4(v.texcoord2,v.texcoord0_w,v.gl_ClipDistance[0]);
    values[id*6+4]=v.normquat;
    values[id*6+5]=float4(v.view,v.gl_ClipDistance[1]);
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
struct Pipelines { id<MTLComputePipelineState> run, canonical; id<MTLArgumentEncoder> arguments; unsigned resources; };
}
int main(int argc, char** argv) { @autoreleasepool { try {
    const bool dump_only = argc == 2 && std::string_view(argv[1]) == "--dump-msl";
    auto device=dump_only ? nil : MTLCreateSystemDefaultDevice();
    if (!dump_only)
        Require(device && device.hasUnifiedMemory && device.argumentBuffersSupport==MTLArgumentBuffersTier2,
                "Requires supported Apple Tier2 unified-memory GPU");
    auto queue=[device newCommandQueue];
    Pica::Regs regs{}; regs.vs.output_mask.Assign(0x3f);
    OpenGL::PicaFixedGSConfig output(regs);
    for (unsigned i=0;i<16;++i) output.state.semantic_maps[i]={i/4,i%4};
    output.state.semantic_maps[16]={4,2}; output.state.semantic_maps[18]={4,3};
    output.state.semantic_maps[19]={5,0}; output.state.semantic_maps[20]={5,1};
    output.state.semantic_maps[22]={4,0}; output.state.semantic_maps[23]={4,1};
    auto compile=[&](std::string_view code) {
        auto options=[MTLCompileOptions new]; options.languageVersion=MTLLanguageVersion2_3;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled=NO;
#pragma clang diagnostic pop
        NSError* error=nil;
        auto text=[[NSString alloc] initWithBytes:code.data() length:code.size() encoding:NSUTF8StringEncoding];
        auto library=[device newLibraryWithSource:text options:options error:&error];
        if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
        return library;
    };
    unsigned comparisons=0;
    Cara::PicaMetalSourcePool sources(64*1024*1024);
    for (unsigned format=0;format<4;++format) for (unsigned offset : {1u,15u})
    for (unsigned kind=0;kind<4;++kind) {
        if (dump_only && kind>1) continue;
        auto setup=Program(kind);
        OpenGL::PicaVSConfig config(regs,setup);
        std::array<std::array<std::uint32_t,144>,8> reference{};
        for (unsigned policy=0;policy<4;++policy) {
            const bool specialized = (policy & 1) != 0;
            OpenGL::PackedVertexLayout known{};
            for (unsigned i=0;i<6;++i)
                known[i]={int(offset+i*16), i==4 ? 0 : 96,
                          i==5 ? 3 : int(format), i==5 ? 4 : int((i+format)%5)};
            setenv("CARA_GPU_QUATERNION_SLICE","0",1);
            setenv("CARA_GPU_PACKED_FETCH_REUSE",policy&2 ? "1" : "0",1);
            auto glsl=OpenGL::GenerateVertexShader(setup,config,false,&output,0x3f,2,true,false,false,false,
                                                      specialized ? &known : nullptr);
            Require(!glsl.empty(),"Actual PICA vertex generation failed");
            Require((glsl.find("void cara_run_quaternion(")!=glsl.npos)==false,
                    "Quaternion slice option did not reach actual generated source");
            Require((glsl.find("cara_local_word(")!=glsl.npos)==bool(policy&2),
                    "Packed fetch option did not reach actual generated source");
            std::string error;
            Require(bool(Cara::MakeUniquePackedVertexSource(glsl)),
                    "Specialized decoder broke production unique-vertex rewrite");
            auto translated=Cara::TranslateRuntimeMetalShaders(glsl,fragment,
                Cara::RuntimeMetalVertexMode::Packed,true,kind!=0,error);
            if (!translated) throw std::runtime_error(error);
            if (dump_only) {
                const auto name = "work/portable-metal-checks/packed-layout-specialization/msl-format-" +
                    std::to_string(format) + "-offset-" + std::to_string(offset) +
                    "-program-" + std::to_string(kind) + "-policy-" + std::to_string(policy) + ".metal";
                std::ofstream file(name);
                file << translated->vertex;
                Require(bool(file), "Could not write authored translated MSL");
                continue; // CPU translation only: no Metal device/compiler/dispatch.
            }
            const auto owned=sources.Intern({glsl,fragment,translated->vertex,translated->fragment});
            Require(bool(owned),"Generated shader source pool admission failed");
            translated.reset(); std::string{}.swap(glsl);
            auto source=Cara::MakeBatchedVertexSource(owned->sources[2],false);
            Require(bool(source),"Actual generated MSL was rejected by production wrapper");
            *source+=canonical;
            auto library=compile(*source);
            auto function=[library newFunctionWithName:@"cara_compute_batch"];
            NSError* failure=nil;
            Pipelines p{[device newComputePipelineStateWithFunction:function error:&failure],
                [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"canonical_vertex_outputs"]
                                                       error:&failure],
                [function newArgumentEncoderWithBufferIndex:0],0};
            if (!p.run || !p.canonical || !p.arguments) throw std::runtime_error(failure.localizedDescription.UTF8String);
            constexpr std::array members{"layout [[id(0)]]","uniforms [[id(1)]]","common [[id(2)]]",
                "vertices [[id(3)]]","indices [[id(4)]]","vertex_sampler [[id(5)]]","index_sampler [[id(6)]]"};
            for (unsigned i=0;i<members.size();++i) if (source->find(members[i])!=source->npos) p.resources|=1u<<i;
            unsigned input_case=0;
            for (unsigned header_case=0;header_case<2;++header_case)
            for (unsigned state=0;state<2;++state,++input_case) {
                auto command=[queue commandBuffer]; Cara::PicaMetalUploadArena arena(device,256*1024);
                Require(arena.Begin(command),"Input arena lease failed");
                std::array<std::byte,2048> bytes{};
                for (unsigned v=0;v<4;++v) {
                    const float sign=(v&1) ? -1.f : 1.f;
                    const std::array<std::array<float,4>,6> attributes{{
                        {v&1 ? 1.f : -1.f,v&2 ? 1.f : -1.f,-0.5f,1.f},
                        {sign*(state ? 0.375f : 0.5f),sign*0.25f,sign*0.125f,sign*0.75f},
                        {-0.25f,0.375f,1.5f,0.125f}, {0.125f,0.25f,0.5f,0.75f},
                        {0.625f,0.875f,1.25f,float(v)/2}, {float(v%3),0.875f,0.25f,0.5f}}};
                    std::memcpy(bytes.data()+offset+v*96,attributes.data(),96);
                }
                auto layout=known;
                const unsigned first=header_case ? 11 : 7;
                if (kind!=0) layout[0][3]|=(8<<3)|(first<<7);
                std::array<std::uint16_t,256> indices{8,std::uint16_t(first),
                    std::uint16_t(first),std::uint16_t(first+1),std::uint16_t(first+2),
                    std::uint16_t(first+2),std::uint16_t(first+1),std::uint16_t(first+3)};
                OpenGL::VSUniformData uniforms{};
                uniforms.uniforms.bools[0].b=state;
                uniforms.uniforms.i[0]={1+state,0,0,0};
                uniforms.uniforms.f[0]={0.125f,0,0,0}; uniforms.uniforms.f[1]={-0.0625f,0.0625f,0,0};
                uniforms.uniforms.f[2]={0.25f,-0.125f,0.125f,0}; uniforms.uniforms.f[3]={0.375f,0.125f,0,0};
                OpenGL::UniformData common{}; common.cara_viewport={0,0,160,120}; common.clip_coef={0,0,0,1};
                const auto l=arena.Copy(std::as_bytes(std::span(layout)),256);
                const auto u=arena.Copy(std::as_bytes(std::span(&uniforms,1)),256);
                const auto c=arena.Copy(std::as_bytes(std::span(&common,1)),256);
                auto vertices=arena.Copy2D(bytes,MTLPixelFormatRGBA32Uint,128,1,2048);
                auto index_texture=arena.Copy2D(std::as_bytes(std::span(indices)),MTLPixelFormatR16Uint,256,1,512);
                Require(l && u && c && vertices && index_texture,"Generated-shader input upload failed");
                auto args=[device newBufferWithLength:p.arguments.encodedLength options:MTLResourceStorageModeShared];
                auto raw=[device newBufferWithLength:6*128 options:MTLResourceStorageModePrivate];
                auto values=[device newBufferWithLength:144*4 options:MTLResourceStorageModeShared];
                auto sampler_descriptor=[MTLSamplerDescriptor new];
                sampler_descriptor.supportArgumentBuffers=YES;
                auto sampler=[device newSamplerStateWithDescriptor:sampler_descriptor];
                auto encoder=[command computeCommandEncoder];
                [p.arguments setArgumentBuffer:args startOffset:0 arrayElement:0];
                const std::array ranges{*l,*u,*c};
                for (unsigned i=0;i<3;++i) if (p.resources&(1u<<i)) {
                    [p.arguments setBuffer:ranges[i].buffer offset:ranges[i].offset atIndex:i];
                    [encoder useResource:ranges[i].buffer usage:MTLResourceUsageRead];
                }
                for (unsigned i=3;i<=4;++i) if (p.resources&(1u<<i)) {
                    auto texture=i==3 ? vertices : index_texture;
                    [p.arguments setTexture:texture atIndex:i]; [encoder useResource:texture usage:MTLResourceUsageRead];
                }
                for (unsigned i=5;i<=6;++i) if (p.resources&(1u<<i)) [p.arguments setSamplerState:sampler atIndex:i];
                const std::array<std::uint32_t,3> range{0,6,0}; const std::uint32_t count=1;
                [encoder setComputePipelineState:p.run]; [encoder setBuffer:args offset:0 atIndex:0];
                [encoder setBytes:range.data() length:sizeof(range) atIndex:1];
                [encoder setBuffer:raw offset:0 atIndex:2]; [encoder setBytes:&count length:4 atIndex:3];
                [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(p.run.threadExecutionWidth,1,1)];
                [encoder endEncoding]; encoder=[command computeCommandEncoder];
                [encoder setComputePipelineState:p.canonical]; [encoder setBuffer:raw offset:0 atIndex:0];
                [encoder setBuffer:values offset:0 atIndex:1];
                [encoder dispatchThreads:MTLSizeMake(6,1,1) threadsPerThreadgroup:MTLSizeMake(6,1,1)];
                [encoder endEncoding]; Require(arena.Submit(),"Generated-shader submit failed");
                [command waitUntilCompleted];
                Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"Generated-shader GPU execution failed");
                if (!specialized) std::memcpy(reference[input_case].data(),values.contents,144*4);
                else { Require(std::memcmp(reference[input_case].data(),values.contents,144*4)==0,
                               "Optional generated shader changed a varying, position or clip output"); ++comparisons; }
            }
        }
    }
    if (dump_only) { std::cout << "PASS: wrote 64 authored MSL fixtures; CPU translation only\n"; return 0; }
    std::cout << "PASS: " << comparisons << " exact packed-layout Metal GPU comparisons; four formats, 0-4 components, stride-zero, split words, live headers, both decoder policies, all varyings/clips\n";
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
} }
