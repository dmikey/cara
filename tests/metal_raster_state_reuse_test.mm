// SPDX-License-Identifier: GPL-2.0-or-later
// Exact production encoder state transitions; run OFF and ON in fresh processes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_draw.h"
#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace Cara {
bool EncodeGroupedMetalDraw(id<MTLCommandBuffer>,PicaMetalUploadArena&,const TexturedMetalDraw&);
bool CloseGroupedMetalDraw(id<MTLCommandBuffer>);
}
namespace {
void Require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
constexpr char source[]=R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; };
vertex Out vertex_main(uint id [[vertex_id]]) {
    const float2 p[]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return {float4(p[id],0,1)};
}
fragment float4 white_main() { return float4(1); }
fragment float4 blue_main() { return float4(0,0,1,1); }
)";
void CheckFloatIdentity() {
    std::array<float,4> a{},b{};
    Require(Cara::EqualMetalBlendColorBits(a,b),"Equal blend rejected");
    b[0]=-0.0f;
    Require(!Cara::EqualMetalBlendColorBits(a,b),"Signed blend zero merged");
    a[0]=std::bit_cast<float>(std::uint32_t{0x7fc00001});b=a;
    Require(Cara::EqualMetalBlendColorBits(a,b),"Identical NaN payload missed");
    b[0]=std::bit_cast<float>(std::uint32_t{0x7fc00002});
    Require(!Cara::EqualMetalBlendColorBits(a,b),"Distinct NaN payload merged");
    const MTLViewport base{0,0,16,16,0,1};
    for (unsigned i=0;i<6;++i) {
        auto x=base,y=base;
        const auto field=[&](MTLViewport& v)->double& {
            switch(i) { case 0:return v.originX;case 1:return v.originY;case 2:return v.width;
                case 3:return v.height;case 4:return v.znear;default:return v.zfar; }
        };
        field(x)=0.0;field(y)=-0.0;
        Require(!Cara::EqualMetalViewportBits(x,y),"Viewport signed zero merged");
        field(x)=field(y)=std::bit_cast<double>(std::uint64_t{0x7ff8000000000001});
        Require(Cara::EqualMetalViewportBits(x,y),"Identical viewport NaN missed");
        field(y)=std::bit_cast<double>(std::uint64_t{0x7ff8000000000002});
        Require(!Cara::EqualMetalViewportBits(x,y),"Distinct viewport NaN merged");
    }
}
}
int main(int argc,char** argv) { @autoreleasepool { try {
    CheckFloatIdentity();
    const auto* option=std::getenv("CARA_METAL_REUSE_RASTER_STATE");
    const bool reuse=option && std::string_view(option)=="1";
    setenv("CARA_METAL_GROUPED_PASS","1",1);setenv("CARA_METAL_FRAME_COST","1",1);
    const auto device=MTLCreateSystemDefaultDevice();
    Require(device && device.hasUnifiedMemory,"Requires supported Apple unified-memory GPU");
    const auto queue=[device newCommandQueue];NSError* error=nil;
    const auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:source] options:nil error:&error];
    Require(library!=nil,"Authored shader compile failed");
    const auto make_pipeline=[&](bool blend) {
        auto p=[MTLRenderPipelineDescriptor new];p.vertexFunction=[library newFunctionWithName:@"vertex_main"];
        p.fragmentFunction=[library newFunctionWithName:blend ? @"white_main" : @"blue_main"];
        auto color=p.colorAttachments[0];color.pixelFormat=MTLPixelFormatRGBA8Unorm;
        color.blendingEnabled=blend;color.sourceRGBBlendFactor=MTLBlendFactorBlendColor;
        color.destinationRGBBlendFactor=MTLBlendFactorZero;
        p.depthAttachmentPixelFormat=p.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
        return [device newRenderPipelineStateWithDescriptor:p error:&error];
    };
    const auto first=make_pipeline(true),second=make_pipeline(false);
    auto depth_desc=[MTLDepthStencilDescriptor new];depth_desc.depthCompareFunction=MTLCompareFunctionAlways;
    const auto always=[device newDepthStencilStateWithDescriptor:depth_desc];
    depth_desc.depthCompareFunction=MTLCompareFunctionNever;
    const auto never=[device newDepthStencilStateWithDescriptor:depth_desc];
    const auto sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    Require(first && second && always && never && sampler,"Pipeline state unavailable");
    auto texture=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    texture.storageMode=MTLStorageModeShared;texture.usage=MTLTextureUsageRenderTarget;
    const auto color=[device newTextureWithDescriptor:texture];
    texture.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;texture.storageMode=MTLStorageModePrivate;
    const auto depth=[device newTextureWithDescriptor:texture];
    Require(color && depth,"Attachments unavailable");
    std::array<std::uint8_t,1024> zeros{};
    [color replaceRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0 withBytes:zeros.data() bytesPerRow:64];
    std::array<std::byte,288> common{};std::array<std::byte,1040> light{};
    std::array<Cara::PicaMetalStageVertex,3> vertices{};
    Cara::TexturedMetalDraw d{};
    d.pipeline=first;d.depth_stencil_state=always;d.texel_fetch_sampler=sampler;
    d.color=color;d.depth_stencil=depth;d.vertex_mode=Cara::PicaMetalVertexMode::StageIn;
    d.vertex_count=3;d.common_uniform=common;d.light_uniform=light;
    d.packed_vertices=std::as_bytes(std::span(vertices));d.viewport={0,0,16,16,0,1};
    d.scissor={0,0,1,1};d.blend_color={1,0,0,1};
    d.winding=MTLWindingCounterClockwise;
    d.lut_texture_slot=d.lut_sampler_slot=d.texture0_slot=d.texture0_sampler_slot=NSNotFound;
    d.fragment_common_slot=0;d.fragment_light_slot=1;
    Cara::PicaMetalUploadArena arena(device,256*1024);
    auto command=[queue commandBuffer];Require(arena.Begin(command),"Arena admission failed");
    unsigned draws=0;
    const auto emit=[&](unsigned unchanged,bool grouped=true) {
        auto before=Cara::ReadNativeMetalRasterStateProfile();
        Require(grouped ? Cara::EncodeGroupedMetalDraw(command,arena,d) : Cara::EncodeTexturedMetalDraw(command,arena,d),
                "Production draw encoder failed");
        auto after=Cara::ReadNativeMetalRasterStateProfile();
        for (unsigned i=0;i<8;++i) {
            Require(after[i][0]==before[i][0]+1,"State request count mismatch");
            Require(after[i][1]==before[i][1]+unsigned(reuse && grouped && (unchanged&(1u<<i))),
                    "State skip count mismatch");
        }
        ++draws;
    };
    constexpr unsigned all=255;
    emit(0);emit(all);
    d.pipeline=second;d.scissor.x=1;emit(all^1^32);emit(all);
    d.depth_stencil_state=never;d.scissor.x=2;emit(all^2^32);emit(all);
    d.stencil_reference=1;d.scissor.x=3;emit(all^4^32);emit(all);
    d.blend_color={0,1,0,1};d.scissor.x=4;emit(all^8^32);emit(all);
    d.viewport.originX=1;d.scissor.x=5;emit(all^16^32);emit(all);
    d.scissor.x=6;emit(all^32);emit(all);
    d.cull=MTLCullModeFront;d.scissor.x=7;emit(all^64^32);emit(all);
    d.winding=MTLWindingClockwise;d.scissor.x=8;emit(all^128^32);emit(all);
    // Restore visible output, then test encoder boundaries independently.
    d.depth_stencil_state=always;d.cull=MTLCullModeNone;d.scissor.x=9;emit(all^2^64^32);
    d.pipeline=first;d.scissor.x=10;emit(all^1^32);emit(all);
    Require(Cara::CloseGroupedMetalDraw(command),"Closing pass failed");emit(0);emit(all);
    Require(Cara::CloseGroupedMetalDraw(command),"Closing before ungrouped draw failed");emit(0,false);emit(0,false);
    Require(arena.Submit(),"Command submission failed");[command waitUntilCompleted];
    Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"GPU command failed");
    // Abort must discard the memo along with its unsubmitted encoder.
    command=[queue commandBuffer];Require(arena.Begin(command),"Abort lease failed");emit(0);emit(all);
    Cara::AbortGroupedMetalDraw(command);arena.Cancel();
    command=[queue commandBuffer];Require(arena.Begin(command),"Post-abort lease failed");emit(0);emit(all);
    Require(Cara::CloseGroupedMetalDraw(command) && arena.Submit(),"Post-abort submit failed");
    [command waitUntilCompleted];Require(!command.error,"Post-abort command failed");
    std::array<std::uint8_t,1024> pixels{};
    [color getBytes:pixels.data() bytesPerRow:64 fromRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0];
    // Retain earlier state-dependent pixels: red blend, blue pipeline, failed
    // depth tests, restored depth state and the later green blend constant.
    std::array<std::uint8_t,1024> golden{};
    golden[0]=255;golden[3]=255;
    for (unsigned pixel : {1u,9u}) { golden[pixel*4+2]=255;golden[pixel*4+3]=255; }
    golden[10*4+1]=255;golden[10*4+3]=255;
    Require(pixels==golden,"Golden GPU pixels differ");
    if (argc==2) { std::ofstream out(argv[1],std::ios::binary);out.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());Require(bool(out),"Pixel receipt write failed"); }
    std::cout<<"PASS: raster_reuse="<<reuse<<", "<<draws<<" production draws, all8 state transitions, pass/abort reset, exact golden pixels\n";
    return 0;
} catch (const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n';return 1; } } }
