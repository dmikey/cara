// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_upload.h"
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
void Check(bool value, const char* text) {
    if (!value) { std::cerr << text << '\n'; std::exit(1); }
}
const char* shader = R"(
#include <metal_stdlib>
using namespace metal;
kernel void inspect(constant uint* source [[buffer(0)]], device uint* out [[buffer(1)]],
 texture2d<uint,access::read> a [[texture(0)]], texture2d<uint,access::read> b [[texture(1)]],
 texture2d<float,access::read> c [[texture(2)]], uint i [[thread_position_in_grid]]) {
 if (i<6) { uint2 p=uint2(i%3,i/3); uint4 x=b.read(p), y=as_type<uint4>(c.read(p));
 out[i]=source[i]+a.read(p).x+x.x+x.y+x.z+x.w+y.x+y.y+y.z+y.w; }
}
vertex float4 vertex_read(uint i [[vertex_id]], constant uint* source [[buffer(0)]]) {
 float2 positions[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};
 return float4(positions[i],float(source[0]&1u)*0.1f,1);
}
fragment float4 solid() { return float4(0.25,0.5,0.75,1); }
)";
}
int main() { @autoreleasepool {
    const bool untracked=std::string_view(std::getenv("CARA_METAL_UNTRACKED_UPLOAD_SOURCES") ?: "0")=="1";
    const auto expected=untracked?MTLHazardTrackingModeUntracked:MTLHazardTrackingModeTracked;
    const auto device=MTLCreateSystemDefaultDevice(); Check(device!=nil,"Metal device missing");
    const auto queue=[device newCommandQueue]; NSError* error=nil;
    const auto library=[device newLibraryWithSource:@(shader) options:nil error:&error];
    Check(library!=nil,error.localizedDescription.UTF8String ?: "Library failed");
    const auto compute=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"inspect"] error:&error];
    auto pd=[MTLRenderPipelineDescriptor new]; pd.vertexFunction=[library newFunctionWithName:@"vertex_read"];
    pd.fragmentFunction=[library newFunctionWithName:@"solid"]; pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    const auto raster=[device newRenderPipelineStateWithDescriptor:pd error:&error];
    Check(compute && raster,"Pipelines failed");
    Cara::PicaMetalUploadArena arena(device,65536);
    const auto gate=[device newSharedEvent];
    std::vector<id<MTLCommandBuffer>> commands;
    std::vector<id<MTLBuffer>> results;
    std::vector<std::array<uint32_t,6>> goldens;
    for(unsigned round=0;round<12;++round) {
        if(round==3) { gate.signaledValue=1; for(auto command:commands)[command waitUntilCompleted]; }
        auto command=[queue commandBuffer]; Check(arena.Begin(command),"Begin failed");
        std::array<uint32_t,6> words{}; for(unsigned i=0;i<6;++i)words[i]=round*31+i;
        auto bytes=std::as_bytes(std::span(words)); auto range=arena.Copy(bytes,256);
        Check(range && range->buffer.hazardTrackingMode==expected,"Buffer hazard mode mismatch");
        std::array<std::byte,128> input{};
        // Finite float bit patterns also exercise exact integer source reads.
        for(unsigned i=0;i<32;++i) { uint32_t v=0x3f000000u+round*97+i; std::memcpy(input.data()+4*i,&v,4); }
        const auto a=arena.Copy2D(input,MTLPixelFormatR16Uint,3,2,64);
        const auto b=arena.Copy2D(input,MTLPixelFormatRGBA32Uint,3,2,64);
        const auto c=arena.Copy2D(input,MTLPixelFormatRGBA32Float,3,2,64);
        Check(a && b && c,"Textures failed");
        for(auto t:{a,b,c})Check(t.hazardTrackingMode==expected && t.buffer.hazardTrackingMode==expected,"Texture/backing tracking mismatch");
        auto output=[device newBufferWithLength:24 options:MTLResourceStorageModeShared];
        Check(output.hazardTrackingMode==MTLHazardTrackingModeTracked,"GPU output became untracked");
        if(round<3)[command encodeWaitForEvent:gate value:1];
        auto encoder=[command computeCommandEncoder]; [encoder setComputePipelineState:compute];
        [encoder setBuffer:range->buffer offset:range->offset atIndex:0]; [encoder setBuffer:output offset:0 atIndex:1];
        [encoder setTexture:a atIndex:0]; [encoder setTexture:b atIndex:1]; [encoder setTexture:c atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(6,1,1) threadsPerThreadgroup:MTLSizeMake(6,1,1)]; [encoder endEncoding];
        auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        td.storageMode=MTLStorageModeShared; td.usage=MTLTextureUsageRenderTarget; auto target=[device newTextureWithDescriptor:td];
        auto pass=[MTLRenderPassDescriptor renderPassDescriptor]; pass.colorAttachments[0].texture=target;
        pass.colorAttachments[0].loadAction=MTLLoadActionClear; pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        auto render=[command renderCommandEncoderWithDescriptor:pass]; [render setRenderPipelineState:raster];
        [render setVertexBuffer:range->buffer offset:range->offset atIndex:0];
        [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [render endEncoding];
        std::array<uint32_t,6> golden{};
        for(unsigned i=0;i<6;++i) { unsigned row=i/3,col=i%3; uint16_t short_value;std::memcpy(&short_value,input.data()+row*64+col*2,2);
            uint32_t sum=words[i]+short_value;for(unsigned j=0;j<4;++j){uint32_t v;std::memcpy(&v,input.data()+row*64+col*16+j*4,4);sum+=2*v;}golden[i]=sum; }
        if(round==0 && std::string_view(std::getenv("CARA_METAL_DRAW_INPUT_REUSE") ?: "0")=="1") {
            // Evict the already-encoded textures before their gated GPU reads.
            // The retained command must keep the old immutable backing alive.
            for(unsigned eviction=0;eviction<390;++eviction) {
                auto churn=input;std::memcpy(churn.data(),&eviction,sizeof(eviction));
                Check(arena.Copy2D(churn,MTLPixelFormatRGBA32Uint,3,2,64)!=nil,"Eviction upload failed");
            }
        }
        Check(arena.Submit(),"Submit failed"); commands.push_back(command);results.push_back(output);goldens.push_back(golden);
        if(round>=3) { [command waitUntilCompleted]; std::array<unsigned char,4> pixel{};
            [target getBytes:pixel.data() bytesPerRow:4 fromRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0];
            Check(pixel==std::array<unsigned char,4>{64,128,191,255},"Direct vertex-input raster mismatch"); }
    }
    for(unsigned i=0;i<commands.size();++i) { [commands[i] waitUntilCompleted];Check(commands[i].status==MTLCommandBufferStatusCompleted && !commands[i].error,"GPU command failed");
        Check(std::memcmp(results[i].contents,goldens[i].data(),24)==0,"GPU texture/buffer read mismatch"); }
    // Cancellation returns an unsubmitted lease without changing cached snapshots.
    Check(arena.Begin([queue commandBuffer]),"Cancel begin failed");
    std::array<std::byte,128> source{};auto previous=arena.Copy2D(source,MTLPixelFormatRGBA32Uint,3,2,64);arena.Cancel();
    Check(arena.Begin([queue commandBuffer]),"Begin after cancel failed");
    auto same=arena.Copy2D(source,MTLPixelFormatRGBA32Uint,3,2,64);
    const bool reuse=std::string_view(std::getenv("CARA_METAL_DRAW_INPUT_REUSE") ?: "0")=="1";
    if(reuse)Check(same==previous,"Immutable cache lost canceled snapshot");
    source[0]=std::byte{17};auto changed=arena.Copy2D(source,MTLPixelFormatRGBA32Uint,3,2,64);
    Check(changed!=previous,"Mutated source reused stale texture");arena.Cancel();
    std::cout<<"PASS: 12 GPU leases, three formats, pending reads, page rotation, raster input and cancellation; untracked="<<untracked<<" reuse="<<reuse<<'\n';
} }
