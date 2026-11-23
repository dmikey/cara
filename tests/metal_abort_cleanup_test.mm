// SPDX-License-Identifier: GPL-2.0-or-later
// Failure-only production lane cleanup. No ROM, captured shader, or guest state.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/pica_metal_raw_index_format.hh"
#include <array>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace Cara { void RegisterNativeMetalTextureCopyBatch(PicaMetalNativeDrawLane&); }
namespace {
using namespace Cara;
void Require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void Rejected(F&& operation) {
    bool rejected=false;
    try { operation(); } catch (const std::exception&) { rejected=true; }
    Require(rejected,"Aborted lane resumed execution");
}
constexpr char source[]=R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; };
vertex Out vertex_main(uint i [[vertex_id]]) {
    const float2 p[]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return {float4(p[i%3],0,1)};
}
fragment float4 fragment_main() { return float4(1,0,0,1); }
struct Args { device const float4* layout [[id(0)]]; };
kernel void compute_main(constant Args* args [[buffer(0)]],
                         device const uint4* ranges [[buffer(1)]],
                         device float4* output [[buffer(2)]],
                         constant uint& count [[buffer(3)]], uint i [[thread_position_in_grid]]) {
    if (i<count) output[i]=args[i].layout[0]+float4(ranges[i].x);
}
)";
struct Fixture {
    PicaMetalNativeDrawLane lane;
    PicaMetalNativeAttachment color,depth;
    PicaMetalDrawViews views{};
    TexturedMetalDraw draw{};
    std::array<std::byte,288> common{};
    std::array<std::byte,1040> light{};
    std::array<std::byte,1856> vs{};
    std::array<std::byte,256> layout{};
    std::array<std::byte,2048> packed{};
    std::array<std::byte,512> indices{};
    std::array<PicaMetalStageVertex,3> vertices{};
    Fixture(id<MTLCommandQueue> queue,id<MTLRenderPipelineState> pipeline,
            id<MTLDepthStencilState> depth_state,id<MTLSamplerState> sampler)
        :lane(queue,256*1024) {
        const auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                width:16 height:16 mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;
        descriptor.hazardTrackingMode=MTLHazardTrackingModeTracked;
        descriptor.usage=MTLTextureUsageRenderTarget;
        auto color_texture=[queue.device newTextureWithDescriptor:descriptor];
        descriptor.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;
        descriptor.storageMode=MTLStorageModePrivate;
        auto depth_texture=[queue.device newTextureWithDescriptor:descriptor];
        Require(color_texture && depth_texture,"Attachment allocation failed");
        std::array<u8,1024> zero{};
        [color_texture replaceRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0
                          withBytes:zero.data() bytesPerRow:64];
        const MetalGuestExport exporter=[](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8>) {
            throw std::runtime_error("Aborted write was exported"); return false;
        };
        color={{color_texture,{0x18000000,0x18000400},1},exporter,
               MetalRgba8Layout{0x18000000,0x18000400,16,16,true}};
        depth={{depth_texture,{0x18001000,0x18001400},2},exporter,{}};
        views.color=color.view;views.depth_stencil=depth.view;
        views.color_dirty=color.view.authoritative_bytes;
        draw.pipeline=pipeline;draw.depth_stencil_state=depth_state;
        draw.texel_fetch_sampler=sampler;draw.color=color_texture;draw.depth_stencil=depth_texture;
        draw.lut_texture_slot=draw.lut_sampler_slot=NSNotFound;
        draw.texture0_slot=draw.texture0_sampler_slot=NSNotFound;
        draw.fragment_common_slot=0;draw.fragment_light_slot=1;
        draw.viewport={0,0,16,16,0,1};draw.scissor={0,0,16,16};
        draw.vertex_count=3;draw.vertex_mode=PicaMetalVertexMode::StageIn;
        draw.common_uniform=common;draw.light_uniform=light;
        draw.packed_vertices=std::as_bytes(std::span(vertices));
    }
    void Record() { lane.Record(draw,views,color,depth); }
    void Defer(const std::shared_ptr<const BatchedMetalVertexProgram>& batch) {
        draw.vertex_mode=MetalRawIndexMode;draw.batched_vertex=batch;
        draw.vertex_count=6;draw.unique_source_vertices=3;draw.packed_vertices=packed;
        draw.expanded_indices=indices;draw.layout_uniform=layout;draw.vs_uniform=vs;
        Record();Record();
    }
    void CheckUnexecuted() {
        std::array<u8,1024> bytes{};
        [color.view.texture getBytes:bytes.data() bytesPerRow:64
                         fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
        Require(bytes==std::array<u8,1024>{},"Abandoned draw reached the GPU");
    }
};
}
int main(int argc,char** argv) { @autoreleasepool { try {
    Require(argc==2,"Pass the retained production cara-pica.metallib path");
    setenv("CARA_METAL_GROUPED_PASS","1",1);
    setenv("CARA_METAL_REUSE_UNIFORM_UPLOADS","0",1);
    setenv("CARA_METAL_BATCH_TEXTURE_COPIES","1",1);
    setenv("CARA_METAL_FRAME_COST","1",1);
    const auto device=MTLCreateSystemDefaultDevice();
    Require(device && device.hasUnifiedMemory,"Requires supported Apple unified-memory GPU");
    const auto queue=[device newCommandQueue];NSError* error=nil;
    const auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source] options:nil error:&error];
    Require(library!=nil,"Authored shader compilation failed");
    const auto descriptor=[MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction=[library newFunctionWithName:@"vertex_main"];
    descriptor.fragmentFunction=[library newFunctionWithName:@"fragment_main"];
    descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    descriptor.depthAttachmentPixelFormat=descriptor.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
    const auto pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    const auto depth=[device newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
    const auto sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    const auto function=[library newFunctionWithName:@"compute_main"];
    const auto compute=[device newComputePipelineStateWithFunction:function error:&error];
    const auto arguments=[function newArgumentEncoderWithBufferIndex:0];
    Require(pipeline && depth && sampler && compute && arguments,"Pipeline preparation failed");
    const auto batch=std::make_shared<const BatchedMetalVertexProgram>(
        BatchedMetalVertexProgram{compute,arguments,pipeline,1,true});
    {
        Fixture f(queue,pipeline,depth,sampler);
        // A real committed prefix must finish even when an unsubmitted group is rejected.
        const auto completed=[device newBufferWithLength:64 options:MTLResourceStorageModeShared];
        const auto command=[queue commandBuffer];const auto blit=[command blitCommandEncoder];
        [blit fillBuffer:completed range:NSMakeRange(0,64) value:0xab];[blit endEncoding];
        f.lane.SubmitSampledTextureBuild(command);
        f.Record(); // Open render encoder, still uncommitted.
        const auto direct_draws=NativeMetalCost::spans[NativeMetalCost::EncoderDraw].calls;
        f.Defer(batch); // Two unique-vertex packets remain unencoded beside it.
        Require(NativeMetalCost::spans[NativeMetalCost::EncoderDraw].calls==direct_draws,
                "Fixture did not retain deferred packets");
        const auto draws=NativeMetalCost::spans[NativeMetalCost::EncoderDraw].calls;
        const auto dispatches=NativeMetalCost::spans[NativeMetalCost::BatchedVertexDispatches].calls;
        try { throw std::runtime_error("injected pre-submit binding failure"); }
        catch (const std::exception&) { f.lane.Abort(); }
        Require(NativeMetalCost::spans[NativeMetalCost::EncoderDraw].calls==draws &&
                NativeMetalCost::spans[NativeMetalCost::BatchedVertexDispatches].calls==dispatches,
                "Abort encoded deferred guest work");
        Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"Committed prefix was not drained");
        const auto bytes=static_cast<const u8*>(completed.contents);
        for (unsigned i=0;i<64;++i) Require(bytes[i]==0xab,"Committed prefix data lost");
        f.CheckUnexecuted();
        Rejected([&]{f.Record();});Rejected([&]{f.lane.Drain();});
        f.lane.Abort(); // Failure cleanup is idempotent.
    }
    try {
        // Destruction is a fallback for stack unwinding before explicit Stop.
        Fixture f(queue,pipeline,depth,sampler);f.Record();f.Defer(batch);
        throw std::runtime_error("injected destructor-unwind failure");
    } catch (const std::runtime_error& error) {
        Require(std::string_view(error.what())=="injected destructor-unwind failure",
                "Unexpected exception in destructor-unwind fixture");
    }
    {
        NSError* load_error=nil;
        const auto transfer_library=[device newLibraryWithURL:[NSURL fileURLWithPath:
            [NSString stringWithUTF8String:argv[1]]] error:&load_error];
        const auto copy=[device newComputePipelineStateWithFunction:
            [transfer_library newFunctionWithName:@"cara_pica_copy_rows"] error:&load_error];
        Require(copy!=nil,"Production transfer library unavailable");
        const MetalTransfer::Pipelines transfer{copy,nil,nil};
        PicaMetalNativeDrawLane lane(queue,256*1024,[](PAddr,PAddr){});
        RegisterNativeMetalTextureCopyBatch(lane);
        const MetalGuestAuthority::GuestReader reader=[](PAddr,std::span<u8> output) {
            std::fill(output.begin(),output.end(),u8{0x42});return true;
        };
        lane.CopyTexture({0x20000000,0x18002000,64,0,0,0,0},transfer,reader);
        const auto abandoned=NativeMetalCost::spans[NativeMetalCost::TextureCopyBatchAbandoned].calls;
        lane.Abort();
        Require(NativeMetalCost::spans[NativeMetalCost::TextureCopyBatchAbandoned].calls==abandoned+1,
                "Pending copy batch was not abandoned");
        Rejected([&]{lane.CopyTexture({0x20000000,0x18002000,64,0,0,0,0},transfer,reader);});
        Rejected([&]{std::array<u8,4> bytes{};lane.ReadGuest(0x18002000,bytes,reader);});
    }
    // A new owner on the same thread must not inherit stale deferred/encoder state.
    { Fixture f(queue,pipeline,depth,sampler);f.Record();f.lane.Drain(); }
    std::cout<<"PASS: abort open render/deferred unique/copy batch; committed prefix drained; rejected lanes cannot resume\n";
    return 0;
} catch (const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n';return 1; } } }
