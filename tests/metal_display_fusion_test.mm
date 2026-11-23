// SPDX-License-Identifier: GPL-2.0-or-later
// Actual native ownership/transfer paths, with poisoned underlying guest RAM.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "common/color.h"
#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/utils.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace Cara;
using namespace Cara::NativeMetalCost;
constexpr PAddr base=0x18000000,source=base+0x1000,destination=base+0x4000,input=base+0x7000;
void Require(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
template<class F> void Rejected(F&& f) {
    bool rejected=false;try { f(); } catch (const std::exception&) { rejected=true; }
    Require(rejected,"Rejected transfer lane resumed or invalid copy succeeded");
}
u64 Count(Phase phase) { return spans[phase].calls; }
bool Option(const char* name) { const auto* value=std::getenv(name);return value && std::string_view(value)=="1"; }
struct QueueGate {
    id<MTLSharedEvent> event;
    id<MTLCommandBuffer> command;
    QueueGate(id<MTLCommandQueue> queue):event([queue.device newSharedEvent]),command([queue commandBuffer]) {
        Require(event && command,"Pending-producer gate unavailable");
        [command encodeWaitForEvent:event value:1];[command commit];
    }
    void Release() { event.signaledValue=1; }
    ~QueueGate() { Release();[command waitUntilCompleted]; }
};
struct Fixture {
    id<MTLCommandQueue> queue;
    std::vector<u8> ram=std::vector<u8>(65536,0xcd);
    PicaMetalNativeDrawLane lane;
    const MetalTransfer::Pipelines& transfer;
    Fixture(id<MTLDevice> device,const MetalTransfer::Pipelines& pipelines)
        :queue([device newCommandQueue]),lane(queue,256*1024,[](PAddr,PAddr){}),transfer(pipelines) {
        for (unsigned i=0;i<1024;++i) ram[input-base+i]=u8(i*37+(i>>3)*11);
    }
    MetalGuestAuthority::GuestReader Reader() {
        return [this](PAddr first,std::span<u8> bytes) {
            if (first<base || first-base>ram.size() || bytes.size()>ram.size()-(first-base))return false;
            std::memcpy(bytes.data(),ram.data()+first-base,bytes.size());return true;
        };
    }
    void Copy(PAddr from,PAddr to,u32 size=64,u16 in_width=0,u16 in_gap=0,u16 out_width=0,u16 out_gap=0) {
        lane.CopyTexture({from,to,size,in_width,in_gap,out_width,out_gap},transfer,Reader());
    }
    std::vector<u8> Raw(PAddr address,unsigned bytes) {
        Require(address>=base && address-base+bytes<=ram.size(),"Fixture byte bounds invalid");
        return {ram.begin()+address-base,ram.begin()+address-base+bytes};
    }
    std::vector<u8> Read(PAddr address,unsigned bytes) {
        std::vector<u8> result(bytes);
        Require(lane.ReadGuest(address,result,Reader()),"Native guest read failed");return result;
    }
    void Overwrite(PAddr address,unsigned bytes,u8 value) {
        lane.InvalidateGuest(address,address+bytes);
        std::fill_n(ram.begin()+address-base,bytes,value);
    }
};
constexpr char shaders[]=R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; };
vertex Out vert(uint id [[vertex_id]]) {
    const float2 p[]={float2(-1,-1),float2(3,-1),float2(-1,3)};
    return {float4(p[id],0,1)};
}
fragment float4 frag(Out in [[stage_in]]) {
    uint x=uint(in.position.x),y=uint(in.position.y);
    discard_fragment(); return float4(0);
}
)";
struct RenderResources {
    id<MTLRenderPipelineState> pipeline;
    id<MTLDepthStencilState> depth;
    id<MTLSamplerState> sampler;
    PicaMetalTextureDecoder decoder;
    id<MTLComputePipelineState> tiled;
    RenderResources(id<MTLDevice> device,id<MTLLibrary> production):decoder(production) {
        NSError* error=nil;
        const auto authored=[device newLibraryWithSource:[NSString stringWithUTF8String:shaders] options:nil error:&error];
        Require(authored!=nil,"Authored shader compilation failed");
        auto descriptor=[MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction=[authored newFunctionWithName:@"vert"];
        descriptor.fragmentFunction=[authored newFunctionWithName:@"frag"];
        descriptor.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        descriptor.depthAttachmentPixelFormat=descriptor.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
        pipeline=[device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        depth=[device newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        tiled=[device newComputePipelineStateWithFunction:[production newFunctionWithName:@"ExportPicaTiled32"] error:&error];
        Require(pipeline && depth && sampler && tiled && decoder.Prepare(0,false,&error),"Render resources unavailable");
    }
    void Produce(Fixture& fixture, bool probe=false) {
        const auto color=fixture.lane.AcquireTiledColor({source,source+1024},16,16,MTLPixelFormatRGBA8Unorm,
                                                       decoder,tiled,nil,fixture.Reader());
        const auto inspect=[&](const char* label) {
            if(!probe)return;
            const auto readback=[fixture.queue.device newBufferWithLength:256*16 options:MTLResourceStorageModeShared];
            const auto read=[fixture.queue commandBuffer];const auto copy=[read blitCommandEncoder];
            [copy copyFromTexture:color.view.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(16,16,1) toBuffer:readback destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*16];
            [copy endEncoding];[read commit];[read waitUntilCompleted];
            const auto* v=static_cast<const u8*>(readback.contents);
            std::cerr<<"PROBE "<<label<<" rgba="<<unsigned(v[0])<<","<<unsigned(v[1])<<","<<unsigned(v[2])<<","<<unsigned(v[3])<<'\n';
        };
        inspect("import");
        // Seed a known immutable GPU texture on the lane's ordered queue.
        // The authored discard draw then publishes a real native surface owner
        // without coupling this transfer regression to vertex/fragment math.
        const auto seed=[fixture.queue.device newBufferWithLength:256*16
            options:MTLResourceStorageModeShared|MTLResourceHazardTrackingModeTracked];
        auto* data=static_cast<u8*>(seed.contents);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            auto* pixel=data+y*256+x*4;
            pixel[0]=u8(17+x*7);pixel[1]=u8(51+y*5);pixel[2]=u8(85+(x^y)*3);pixel[3]=255;
        }
        const auto command=[fixture.queue commandBuffer];const auto blit=[command blitCommandEncoder];
        [blit copyFromBuffer:seed sourceOffset:0 sourceBytesPerRow:256 sourceBytesPerImage:256*16
                  sourceSize:MTLSizeMake(16,16,1) toTexture:color.view.texture destinationSlice:0
                  destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [blit endEncoding];fixture.lane.SubmitSampledTextureBuild(command);
        inspect("seed");
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                    width:16 height:16 mipmapped:NO];
        descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageRenderTarget;
        descriptor.hazardTrackingMode=MTLHazardTrackingModeTracked;
        auto texture=[fixture.queue.device newTextureWithDescriptor:descriptor];
        Require(texture!=nil,"Depth attachment allocation failed");
        const PicaMetalNativeAttachment depth_attachment{{texture,{base+0xb000,base+0xb400},2},
            [](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8>) {
                throw std::runtime_error("Unused fixture depth was exported");return false;
            },{}};
        std::array<PicaMetalStageVertex,3> vertices{};
        std::array<std::byte,288> common{};std::array<std::byte,1040> light{};
        TexturedMetalDraw draw{};draw.pipeline=pipeline;draw.depth_stencil_state=depth;
        draw.texel_fetch_sampler=sampler;draw.color=color.view.texture;draw.depth_stencil=texture;
        draw.vertex_mode=PicaMetalVertexMode::StageIn;draw.vertex_count=3;
        draw.common_uniform=common;draw.light_uniform=light;draw.packed_vertices=std::as_bytes(std::span(vertices));
        draw.lut_texture_slot=draw.lut_sampler_slot=draw.texture0_slot=draw.texture0_sampler_slot=NSNotFound;
        draw.fragment_common_slot=0;draw.fragment_light_slot=1;draw.viewport={0,0,16,16,0,1};draw.scissor={0,0,16,16};
        PicaMetalDrawViews views{};views.color=color.view;views.depth_stencil=depth_attachment.view;
        views.color_dirty=color.view.authoritative_bytes;
        fixture.lane.Record(draw,views,color,depth_attachment);
        fixture.lane.PublishPending();
        inspect("draw-published");
    }
};
std::vector<u8> ColorBytes() {
    std::vector<u8> result(1024);
    const auto coordinate=[](unsigned bits) { return (bits&1)|((bits>>1)&2)|((bits>>2)&4); };
    for (unsigned pixel=0;pixel<256;++pixel) {
        const unsigned tile=pixel/64,x=(tile%2)*8+coordinate(pixel&63);
        const unsigned guest_y=(tile/2)*8+coordinate((pixel&63)>>1),y=15-guest_y;
        result[pixel*4]=255;result[pixel*4+1]=u8(85+(x^y)*3);
        result[pixel*4+2]=u8(51+y*5);result[pixel*4+3]=u8(17+x*7);
    }
    return result;
}
}
std::vector<u8> CropExpected(const std::vector<u8>& bytes,u32 input_width,u32 output_width,u32 height,u32 bpp,bool flip) {
    std::vector<u8> result(output_width*height*bpp);
    for(u32 y=0;y<height;++y)for(u32 x=0;x<output_width;++x) {
        const u32 sx=2*x, m=(sx&1)|((y&1)<<1)|((sx&2)<<1)|((y&2)<<2)|((sx&4)<<2)|((y&4)<<3);
        const u32 first=4*((y&~7u)*input_width+(sx&~7u)*8+m);
        const u32 to=bpp*((flip?height-1-y:y)*output_width+x);
        if(bpp==3)for(u32 c=0;c<3;++c) result[to+c]=u8((u32(bytes[first+1+c])+bytes[first+5+c])>>1);
        else {
            const u32 r=(u32(bytes[first+3])+bytes[first+7])>>4;
            const u32 g=(u32(bytes[first+2])+bytes[first+6])>>3;
            const u32 b=(u32(bytes[first+1])+bytes[first+5])>>4;
            const u32 packed=(r<<11)|(g<<5)|b;
            result[to]=u8(packed);result[to+1]=u8(packed>>8);
        }
    }
    return result;
}
void Receipt(unsigned id,const std::vector<u8>& bytes) {
    static const char hex[]="0123456789abcdef";
    std::cout<<"CASE "<<id<<" ";for(auto v:bytes)std::cout<<hex[v>>4]<<hex[v&15];std::cout<<'\n';
}
int main(int argc,char** argv) { @autoreleasepool { try {
    Require(argc==2,"Pass production cara-pica.metallib");
    setenv("CARA_METAL_FRAME_COST","1",1);setenv("CARA_METAL_GROUPED_PASS","1",1);
    setenv("CARA_METAL_QUEUE_COLOR_EXPORT","1",1);setenv("CARA_METAL_DIRECT_COLOR_EXPORT","0",1);
    setenv("CARA_METAL_OVERLAP_TRANSFERS","1",1);
    const bool fused_only=Option("CARA_METAL_TEST_FUSED_ONLY"),probes=Option("CARA_METAL_TEST_BOUNDARY_PROBES");
    const bool crop=Option("CARA_METAL_FUSE_TEXTURE_CROP"),linear=Option("CARA_METAL_LINEAR_COLOR_BYTES_ONLY");
    const auto device=MTLCreateSystemDefaultDevice();Require(device && device.hasUnifiedMemory,"Requires Apple unified-memory GPU");
    NSError* error=nil;
    const auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
    Require(library!=nil,"Production Metal library unavailable");
    const auto copy=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cara_pica_copy_rows"] error:&error];
    const auto color=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cara_pica_display_to_linear_color"] error:&error];
    Require(copy && color,"Transfer pipelines unavailable");
    const MetalTransfer::Pipelines transfer{copy,nil,color};
    RenderResources render(device,library);
    unsigned cases=0,executed=0;
    for(bool pending:{false,true})for(bool flip:{false,true})for(u32 bpp:{3u,2u})for(unsigned mutation=0;mutation<5;++mutation) {
        const unsigned id=cases++;
        if(fused_only && mutation!=0)continue;
        Fixture f(device,transfer);
        std::unique_ptr<QueueGate> gate;
        if(pending)gate=std::make_unique<QueueGate>(f.queue);
        auto bytes=ColorBytes();
        if(mutation==4)std::copy(bytes.begin(),bytes.end(),f.ram.begin()+source-base);
        else render.Produce(f,probes&&id==0);
        if(!pending)f.lane.Drain();
        if(probes&&id==0) {
            const auto got=f.Read(source,1024);
            std::cerr<<"PROBE authority abgr="<<unsigned(got[0])<<","<<unsigned(got[1])<<","<<unsigned(got[2])<<","<<unsigned(got[3])<<'\n';
        }
        if(mutation==1) { f.Overwrite(source+1016,8,0x19);std::fill(bytes.end()-8,bytes.end(),0x19); }
        if(mutation==2) { f.Copy(input,source,16);const auto v=f.Raw(input,16);std::copy(v.begin(),v.end(),bytes.begin()); }
        const u16 input_width=mutation==3?8:16,input_height=mutation==3?32:16;
        const u16 crop_width=mutation==3?8:12;
        const MetalTransfer::DisplayTransferRegisters regs{source,destination,input_width,input_height,crop_width,8,
            (bpp==3?0x01001004u:0x01002004u)|u32(flip),false};
        const auto before=Count(DisplayCropFused),hit=Count(DisplayCropOwnerHits),resolve=Count(ResolveBytesHost);
        f.lane.TransferDisplay(regs,transfer,render.decoder,f.Reader());
        const bool owned=mutation==0;
        Require(Count(DisplayCropOwnerHits)==hit+u64(owned),"Crop exact-owner admission differs");
        Require(Count(DisplayCropFused)==before+u64(crop&&owned),"Crop fusion flag/admission differs");
        Require(Count(ResolveBytesHost)==resolve+u64(!(crop&&owned)),"Crop byte-export bypass differs");
        if(gate)gate->Release();
        const auto expected=CropExpected(bytes,input_width,crop_width/2,8,bpp,flip);
        const auto actual=f.Read(destination,expected.size());
        if(actual!=expected) {
            for(unsigned i=0;i<actual.size();++i)if(actual[i]!=expected[i]) {
                std::cerr<<"case="<<cases<<" pending="<<pending<<" flip="<<flip<<" bpp="<<bpp<<" mutation="<<mutation<<" byte="<<i<<" actual="<<unsigned(actual[i])<<" expected="<<unsigned(expected[i])<<'\n';break;
            }
            throw std::runtime_error("Cropped integer guest bytes differ");
        }
        Require(f.Raw(destination-8,8)==std::vector<u8>(8,0xcd) &&
                f.Raw(destination+expected.size(),8)==std::vector<u8>(8,0xcd),"Crop touched RAM neighbors");
        ++executed;Receipt(id,actual);
        f.Overwrite(destination,expected.size(),0x42);
        Require(f.Read(destination,expected.size())==std::vector<u8>(expected.size(),0x42),"CPU invalidation lost after fused crop");
    }
    for(bool pending:{false,true})for(bool flip:{false,true})for(bool scale:{false,true})for(bool rgb8:{false,true}) {
        Fixture f(device,transfer);std::unique_ptr<QueueGate> gate;
        if(pending)gate=std::make_unique<QueueGate>(f.queue);
        render.Produce(f);if(!pending)f.lane.Drain();
        const MetalTransfer::DisplayTransferRegisters regs{source,destination,16,16,16,16,
            (scale?0x01000000u:0u)|(rgb8?0x1000u:0u)|u32(flip),false};
        const auto hit=Count(DisplayLinearBufferOnly),avoided=Count(DisplayLinearTextureBytesAvoided);
        f.lane.TransferDisplay(regs,transfer,render.decoder,f.Reader());
        const unsigned width=scale?8:16,bytes=width*16*(rgb8?3:4);
        Require(Count(DisplayLinearBufferOnly)==hit+u64(linear),"Linear buffer-only admission differs");
        Require(Count(DisplayLinearTextureBytesAvoided)==avoided+(linear?width*16*4:0),"Linear omitted texture extent differs");
        if(gate)gate->Release();++executed;Receipt(cases++,f.Read(destination,bytes));
    }
    for (bool flip : {false, true}) {
        Fixture f(device, transfer);
        const auto bytes = f.Raw(input, 16 * 16 * 2);
        f.Copy(input, source, bytes.size());
        f.lane.TransferDisplay({source, destination, 16, 16, 12, 8, 0x01002204u | u32(flip),
                                false}, transfer, render.decoder, f.Reader());
        std::vector<u8> expected(6 * 8 * 2);
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 6; ++x) {
                const u32 from = VideoCore::GetMortonOffset(2 * x, y, 2);
                const auto left = Color::DecodeRGB565(bytes.data() + from);
                const auto right = Color::DecodeRGB565(bytes.data() + from + 2);
                const u32 to = 2 * ((flip ? 7 - y : y) * 6 + x);
                Color::EncodeRGB565(((left + right) / 2).Cast<u8>(), expected.data() + to);
            }
        }
        const auto actual = f.Read(destination, expected.size());
        Require(actual == expected, "Packed RGB565 crop differs from software PICA");
        ++executed;
        Receipt(cases++, actual);
    }
    for (bool flip : {false, true}) {
        Fixture f(device, transfer);
        const auto bytes = f.Raw(input, 16 * 16 * 2);
        f.Copy(input, source, bytes.size());
        f.lane.TransferDisplay({source, destination, 16, 16, 16, 16, 0x02002220u | u32(flip),
                                false}, transfer, render.decoder, f.Reader());
        std::vector<u8> expected(8 * 8 * 2);
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                const u32 from = ((2 * y) & ~7u) * 16 * 2 +
                                 VideoCore::GetMortonOffset(2 * x, 2 * y, 2);
                const auto top = Color::DecodeRGB565(bytes.data() + from) +
                                 Color::DecodeRGB565(bytes.data() + from + 2);
                const auto bottom = Color::DecodeRGB565(bytes.data() + from + 4) +
                                    Color::DecodeRGB565(bytes.data() + from + 6);
                const u32 to = VideoCore::GetMortonOffset(x, flip ? 7 - y : y, 2);
                Color::EncodeRGB565(((top + bottom) / 4).Cast<u8>(), expected.data() + to);
            }
        }
        const auto actual = f.Read(destination, expected.size());
        Require(actual == expected, "Tiled RGB565 ScaleXY differs from software PICA");
        ++executed;
        Receipt(cases++, actual);
    }
    {
        Fixture f(device, transfer);
        const auto bytes = f.Raw(input, 16 * 16 * 4);
        f.Copy(input, source, bytes.size());
        f.lane.TransferDisplay({source, destination, 16, 16, 16, 16, 0x02000020u, false},
                               transfer, render.decoder, f.Reader());
        std::vector<u8> expected(8 * 8 * 4);
        for (u32 y = 0; y < 8; ++y) {
            for (u32 x = 0; x < 8; ++x) {
                const u32 from = ((2 * y) & ~7u) * 16 * 4 +
                                 VideoCore::GetMortonOffset(2 * x, 2 * y, 4);
                const u32 to = VideoCore::GetMortonOffset(x, y, 4);
                for (u32 c = 0; c < 4; ++c) {
                    const auto sum = u32(bytes[from + c]) + bytes[from + c + 4] +
                                     bytes[from + c + 8] + bytes[from + c + 12];
                    expected[to + c] = u8(sum / 4);
                }
            }
        }
        const auto actual = f.Read(destination, expected.size());
        Require(actual == expected, "Tiled RGBA8 ScaleXY differs from software PICA");
        ++executed;
        Receipt(cases++, actual);
    }
    std::cout<<"PASS "<<executed<<" production lane transfer cases crop="<<crop<<" linear="<<linear<<'\n';
    return 0;
} catch(const std::exception& failure) { std::cerr<<"FAIL: "<<failure.what()<<'\n';return 1; } } }
