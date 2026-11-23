// SPDX-License-Identifier: GPL-2.0-or-later
// Production capture -> shared packed GLSL -> runtime MSL -> production compute wrapper.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_capture_raw_indices.hh"
#include "video_core/renderer_metal/pica_metal_runtime_shader.h"
#include "video_core/renderer_metal/batched_vertex_source.h"
#include "video_core/renderer_metal/pica_metal_upload.h"
#include "video_core/renderer_opengl/gl_packed_vertex.h"
#include <iostream>
#include <stdexcept>
namespace {
void Require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
using Topology=Pica::PipelineRegs::TriangleTopology;
constexpr PAddr base=0x20000000; constexpr unsigned attribute=0x40000;
}
int main(){ @autoreleasepool { try {
    setenv("CARA_METAL_FRAME_COST","1",1);
    auto device=MTLCreateSystemDefaultDevice(); auto queue=[device newCommandQueue];
    Require(device && device.argumentBuffersSupport==MTLArgumentBuffersTier2,"Apple Tier2 GPU required");
    std::string glsl=OpenGL::PackedVertexDecoderGLSL;
    glsl+=R"(
uniform highp usampler2D cara_vertex_indices;
out vec4 primary_color;
vec4 position_value;
void cara_load_vertex(int vertex_id) {
    int input_vertex_id = int(texelFetch(cara_vertex_indices, ivec2(vertex_id % 128, vertex_id / 128), 0).r);
    position_value=cara_decode_vertex(input_vertex_id,0);
}
void main(){cara_load_vertex(gl_VertexID);gl_Position=position_value;primary_color=vec4(1.0);}
)";
    std::string error;
    auto translated=Cara::TranslateRuntimeMetalShaders(glsl,
      "in vec4 primary_color;layout(location=0)out vec4 color;void main(){color=primary_color;}",
      Cara::RuntimeMetalVertexMode::Packed,true,false,error);
    if(!translated) throw std::runtime_error(error);
    auto source=Cara::MakeBatchedVertexSource(translated->vertex,false);
    Require(bool(source),"Production compute wrapper rejected shader");
    *source+=R"(
kernel void canonical_positions(device const uchar* raw [[buffer(0)]],device float4* positions [[buffer(1)]],
 constant uint& count [[buffer(2)]],uint id [[thread_position_in_grid]]) {
 if(id<count) positions[id]=reinterpret_cast<device const CaraComputeOutput*>(raw+ulong(id)*128)->value.gl_Position;
}
)";
    auto options=[MTLCompileOptions new];options.languageVersion=MTLLanguageVersion2_3;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled=NO;
#pragma clang diagnostic pop
    NSError* failure=nil;
    auto library=[device newLibraryWithSource:[[NSString alloc]initWithBytes:source->data() length:source->size() encoding:NSUTF8StringEncoding] options:options error:&failure];
    if(!library) throw std::runtime_error(failure.localizedDescription.UTF8String);
    auto function=[library newFunctionWithName:@"cara_compute_batch"];
    auto run=[device newComputePipelineStateWithFunction:function error:&failure];
    auto canonical=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"canonical_positions"] error:&failure];
    auto arguments=[function newArgumentEncoderWithBufferIndex:0];
    Require(run && canonical && arguments,"Compute pipeline creation failed");
    unsigned resources=0;
    constexpr std::array members{"layout [[id(0)]]","uniforms [[id(1)]]","common [[id(2)]]","vertices [[id(3)]]","indices [[id(4)]]","vertex_sampler [[id(5)]]","index_sampler [[id(6)]]"};
    for(unsigned i=0;i<members.size();++i)if(source->find(members[i])!=source->npos)resources|=1u<<i;
    Require((resources&0x19)==0x19 && !(resources&6),"Unexpected raw decoder resources");
    auto state=std::make_unique<Pica::State>();state->Reset();
    auto& pipe=state->regs.pipeline;
    pipe.vertex_attributes.base_address.Assign(base/16);
    pipe.vertex_attributes.format0.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
    pipe.vertex_attributes.size0.Assign(3);
    auto& loader=pipe.vertex_attributes.attribute_loaders[0];loader.data_offset.Assign(attribute);
    loader.comp0.Assign(0);loader.component_count.Assign(1);loader.byte_count.Assign(16);
    std::vector<u8> memory(2*1024*1024);
    unsigned cases=0;uint64_t vertices_checked=0;
    for(auto topology:{Topology::List,Topology::Strip,Topology::Fan,Topology::Shader})
    for(unsigned width:{1u,2u}) for(bool large:{false,true}) {
        if(large && width==1 && (topology==Topology::Strip || topology==Topology::Fan))continue;
        unsigned count=large ? ((topology==Topology::Strip || topology==Topology::Fan)?17001:40002):192;
        pipe.num_vertices=count;pipe.index_array.format.Assign(width==2?decltype(pipe.index_array)::SHORT:decltype(pipe.index_array)::BYTE);
        state->primitive_assembler.Reconfigure(topology);
        std::vector<u32> indices(count);for(unsigned i=0;i<count;++i)indices[i]=7+i%3;
        for(unsigned change=0;change<5;++change) {
            // Identical second pass, middle/tail mutations, then live attribute mutation.
            if(change==2)indices[count/2]=indices[count/2]==7?8:7;
            if(change==3)indices[count-1]=indices[count-1]==7?8:7;
            unsigned offset=0x100;pipe.index_array.offset.Assign(offset);
            for(unsigned i=0;i<count;++i){memory[offset+i*width]=u8(indices[i]);if(width==2)memory[offset+i*width+1]=u8(indices[i]>>8);}
            std::array<std::array<float,4>,32> values{};
            for(unsigned i=0;i<values.size();++i){values[i]={float(i),float(i)+.25f,float(i)+.5f+(change==4?.125f:0.f),1.f};std::memcpy(memory.data()+attribute+i*16,values[i].data(),16);}
            auto captured=Cara::CaptureMetalRawIndices(*state,true,1,[&](PAddr a,std::span<u8> out){if(a<base||uint64_t(a-base)+out.size()>memory.size())return false;std::memcpy(out.data(),memory.data()+a-base,out.size());return true;});
            Require(bool(captured),"Authored dense indices rejected");
            std::vector<std::array<float,4>> expected;
            auto add=[&](unsigned a,unsigned b,unsigned c){for(auto i:{a,b,c}){auto v=values[indices[i]];v[1]=-v[1];v[2]=(v[2]+v[3])*.5f;expected.push_back(v);}};
            if(topology==Topology::List || topology==Topology::Shader)for(unsigned i=0;i<count/3*3;i+=3)add(i,i+1,i+2);
            else for(unsigned i=2;i<count;++i){if(topology==Topology::Fan)add(0,i-1,i);else if(i&1)add(i-1,i-2,i);else add(i-2,i-1,i);}
            Require(expected.size()==captured->vertex_count,"Independent topology size differs");
            auto command=[queue commandBuffer];Cara::PicaMetalUploadArena arena(device,256*1024);Require(arena.Begin(command),"Arena lease failed");
            auto layout=arena.Copy(std::as_bytes(std::span(captured->layout)),256);
            auto vertex=arena.Copy2D(std::as_bytes(std::span(captured->vertices)),MTLPixelFormatRGBA32Uint,128,captured->vertices.size()/2048,2048);
            auto index=arena.Copy2D(std::as_bytes(std::span(captured->indices)),MTLPixelFormatR16Uint,256,captured->indices.size()/256,512);
            Require(layout && vertex && index,"Raw capture upload failed");
            auto args=[device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
            auto raw=[device newBufferWithLength:expected.size()*128 options:MTLResourceStorageModePrivate];
            auto output=[device newBufferWithLength:expected.size()*16 options:MTLResourceStorageModeShared];
            auto descriptor=[MTLSamplerDescriptor new];descriptor.supportArgumentBuffers=YES;
            auto sampler=[device newSamplerStateWithDescriptor:descriptor];
            auto encoder=[command computeCommandEncoder];[arguments setArgumentBuffer:args startOffset:0 arrayElement:0];
            [arguments setBuffer:layout->buffer offset:layout->offset atIndex:0];[encoder useResource:layout->buffer usage:MTLResourceUsageRead];
            for(unsigned i=3;i<=4;++i){auto texture=i==3?vertex:index;[arguments setTexture:texture atIndex:i];[encoder useResource:texture usage:MTLResourceUsageRead];}
            for(unsigned i=5;i<=6;++i)if(resources&(1u<<i))[arguments setSamplerState:sampler atIndex:i];
            std::array<uint32_t,3> range{0,captured->vertex_count,0};uint32_t jobs=1;
            [encoder setComputePipelineState:run];[encoder setBuffer:args offset:0 atIndex:0];[encoder setBytes:range.data() length:sizeof(range) atIndex:1];[encoder setBuffer:raw offset:0 atIndex:2];[encoder setBytes:&jobs length:4 atIndex:3];
            [encoder dispatchThreadgroups:MTLSizeMake((range[1]+run.threadExecutionWidth-1)/run.threadExecutionWidth,1,1) threadsPerThreadgroup:MTLSizeMake(run.threadExecutionWidth,1,1)];[encoder endEncoding];
            encoder=[command computeCommandEncoder];[encoder setComputePipelineState:canonical];[encoder setBuffer:raw offset:0 atIndex:0];[encoder setBuffer:output offset:0 atIndex:1];[encoder setBytes:&range[1] length:4 atIndex:2];
            [encoder dispatchThreads:MTLSizeMake(range[1],1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];[encoder endEncoding];
            Require(arena.Submit(),"Submission failed");[command waitUntilCompleted];
            Require(command.status==MTLCommandBufferStatusCompleted&&!command.error,"GPU command failed");
            if(std::memcmp(output.contents,expected.data(),expected.size()*16)) {
                auto actual=static_cast<const std::array<float,4>*>(output.contents);
                for(unsigned i=0;i<expected.size();++i)if(std::memcmp(&actual[i],&expected[i],16)) {
                  std::cerr<<"topology="<<unsigned(topology)<<" width="<<width<<" count="<<count<<" change="<<change<<" corner="<<i<<" layout="<<captured->layout[0][0]<<","<<captured->layout[0][1]<<","<<captured->layout[0][2]<<","<<captured->layout[0][3]<<" actual=";
                  for(auto v:actual[i])std::cerr<<v<<",";std::cerr<<" expected=";for(auto v:expected[i])std::cerr<<v<<",";std::cerr<<"\n";break;
                }throw std::runtime_error("GPU decoder differs from independent exact position oracle");
            }
            ++cases;vertices_checked+=expected.size();
        }
    }
    std::cout<<"PASS cases="<<cases<<" exact_positions="<<vertices_checked<<" memo_hits="<<Cara::NativeMetalCost::spans[Cara::NativeMetalCost::IndexMemoHits].calls<<"\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;} } }
