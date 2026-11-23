// SPDX-License-Identifier: GPL-2.0-or-later
// Actual grouped encoder, publication bounds and committed-ticket lifetimes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_draw_group.h"
#include "video_core/renderer_metal/pica_metal_draw_writer.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/pica_metal_submission_queue.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
namespace {
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
constexpr const char* source=R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; };
vertex Out vertex_main(uint id [[vertex_id]]) {
 const float2 p[]={float2(-1,-1),float2(3,-1),float2(-1,3)};
 return {float4(p[id],0,1)};
}
fragment float4 fragment_main() { return float4(1); }
)";
}
int main(int argc, char** argv) { @autoreleasepool { try {
 using namespace Cara;
 setenv("CARA_METAL_GROUPED_PASS","1",1);
 setenv("CARA_METAL_FRAME_COST","1",1);
 const auto limit=NativeMetalCost::DrawGroupLimit();
 Require(limit==32 || limit==64 || limit==128 || limit==256,"Unbounded group limit");
 Require(NativeMetalCost::DrawComputeGroupLimit()==std::min(limit,std::size_t{64}),"Compute budget grew");
 auto device=MTLCreateSystemDefaultDevice();Require(device && device.hasUnifiedMemory,"Apple GPU required");
 auto queue=[device newCommandQueue];NSError* error=nil;
 auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source] options:nil error:&error];
 Require(library!=nil,"Shader compile failed");
 auto descriptor=[MTLRenderPipelineDescriptor new];
 descriptor.vertexFunction=[library newFunctionWithName:@"vertex_main"];
 descriptor.fragmentFunction=[library newFunctionWithName:@"fragment_main"];
 descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
 descriptor.depthAttachmentPixelFormat=descriptor.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
 auto pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
 auto depth_desc=[MTLDepthStencilDescriptor new];depth_desc.depthCompareFunction=MTLCompareFunctionAlways;
 auto depth_state=[device newDepthStencilStateWithDescriptor:depth_desc];
 auto sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
 auto texture=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
 texture.storageMode=MTLStorageModeShared;texture.usage=MTLTextureUsageRenderTarget;
 auto color=[device newTextureWithDescriptor:texture];
 texture.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;texture.storageMode=MTLStorageModePrivate;
 auto depth=[device newTextureWithDescriptor:texture];
 Require(pipeline && depth_state && sampler && color && depth,"GPU resources unavailable");
 std::array<std::uint8_t,1024> zeros{};
 [color replaceRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0 withBytes:zeros.data() bytesPerRow:64];
 std::array<std::byte,288> common{};std::array<std::byte,1040> light{};
 std::array<PicaMetalStageVertex,3> vertices{};
 TexturedMetalDraw draw{};draw.pipeline=pipeline;draw.depth_stencil_state=depth_state;
 draw.texel_fetch_sampler=sampler;draw.color=color;draw.depth_stencil=depth;
 draw.vertex_mode=PicaMetalVertexMode::StageIn;draw.vertex_count=3;
 draw.common_uniform=common;draw.light_uniform=light;draw.packed_vertices=std::as_bytes(std::span(vertices));
 draw.viewport={0,0,16,16,0,1};draw.winding=MTLWindingCounterClockwise;
 draw.lut_texture_slot=draw.lut_sampler_slot=draw.texture0_slot=draw.texture0_sampler_slot=NSNotFound;
 draw.fragment_common_slot=0;draw.fragment_light_slot=NSNotFound;
 PicaMetalDrawViews views{};
 constexpr std::uint64_t base=0x18000000;
 views.color={color,{base,base+1024},11};
 views.depth_stencil={depth,{base+4096,base+5120},12};
 PicaMetalDrawGroup group(queue,2*1024*1024);
 PicaMetalSubmissionQueue submissions(queue, [] {});
 const auto configure=[&](std::size_t i) {
  const auto pixel=i%256;draw.scissor={pixel%16,pixel/16,1,1};
  views.color_dirty={base+pixel*4,base+pixel*4+4};
 };
 for (std::size_t i=0;i<limit;++i) {
  configure(i);Require(group.Append(draw,views)==PicaMetalDrawGroup::Result::Recorded,"Premature capacity boundary");
 }
 configure(limit);
 Require(group.Append(draw,views)==PicaMetalDrawGroup::Result::BoundaryRequired,"Missing capacity boundary");
 auto first=group.Commit();Require(bool(first),"First commit failed");
 submissions.Track(*first);
 ValidateMetalDrawTicket(*first);Require(first->count==limit,"Publication count changed");
 Require(bool(first->extended_publications)==(limit>64),"Unexpected extended allocation");
 for (std::size_t i=0;i<limit;++i) {
  const auto& p=first->Publications()[i];
  Require(p.color.backing_generation==11 && p.color.bytes.begin==base+i*4 && p.color.bytes.end==base+i*4+4,"Publication lost or reordered");
 }
 const auto old_storage=first->extended_publications;
 Require(group.Append(draw,views)==PicaMetalDrawGroup::Result::Recorded,"Next pass admission failed");
 auto second=group.Commit();Require(bool(second) && second->count==1,"Second commit failed");
 submissions.Track(*second);
 if(limit>64) Require(old_storage!=second->extended_publications,"Committed publication storage reused");
 Require(first->Publications()[limit-1].color.bytes.begin==base+(limit-1)*4,"Old publication mutated");
 submissions.Drain();
 Require(first->Poll()==PicaMetalCompletion::Complete && second->Poll()==PicaMetalCompletion::Complete,"GPU completion failed");
 std::array<std::uint8_t,1024> pixels{};
 [color getBytes:pixels.data() bytesPerRow:64 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
 for(std::size_t i=0;i<256;++i) for(unsigned c=0;c<4;++c)
  Require(pixels[i*4+c]==(i<std::min(limit+1,std::size_t{256})?255:0),"Golden raster differs");
 // Abandon a live extended pass under Metal API validation; no hidden commit.
 { PicaMetalDrawGroup aborted(queue,2*1024*1024);
   for(std::size_t i=0;i<std::min(limit,std::size_t{65});++i) {
    configure(i);Require(aborted.Append(draw,views)==PicaMetalDrawGroup::Result::Recorded,"Abort fixture admission failed");
   }
   aborted.Abort();aborted.Abort();
 }
 if (argc==2) ReportNativeMetalCommandTimeline(argv[1]);
 std::cout<<"PASS group_limit="<<limit<<" exact raster/publications, retained old storage, abort; compute limit="<<NativeMetalCost::DrawComputeGroupLimit()<<'\n';
 return 0;
 } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;} } }
