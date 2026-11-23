// SPDX-License-Identifier: GPL-2.0-or-later
// Actual native ownership/transfer paths, with poisoned underlying guest RAM.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
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
    return float4(float(17+x*7)/255.0,float(51+y*5)/255.0,float(85+(x^y)*3)/255.0,1);
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
    void Produce(Fixture& fixture) {
        const auto color=fixture.lane.AcquireTiledColor({source,source+1024},16,16,MTLPixelFormatRGBA8Unorm,
                                                       decoder,tiled,nil,fixture.Reader());
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
int main(int argc,char** argv) { @autoreleasepool { try {
    Require(argc==2,"Pass production cara-pica.metallib");
    setenv("CARA_METAL_FRAME_COST","1",1);setenv("CARA_METAL_GROUPED_PASS","1",1);
    setenv("CARA_METAL_QUEUE_COLOR_EXPORT","1",1);setenv("CARA_METAL_DIRECT_COLOR_EXPORT","0",1);
    setenv("CARA_METAL_OVERLAP_TRANSFERS","1",1);setenv("CARA_METAL_BATCH_TEXTURE_COPIES","0",1);
    const bool striped=Option("CARA_METAL_BORROW_STRIPED_SOURCE"),direct=Option("CARA_METAL_DIRECT_EXPORT_CONSUMPTION");
    const auto device=MTLCreateSystemDefaultDevice();Require(device && device.hasUnifiedMemory,"Requires Apple unified-memory GPU");
    NSError* error=nil;
    const auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
    Require(library!=nil,"Production Metal library unavailable");
    const auto copy=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cara_pica_copy_rows"] error:&error];
    Require(copy!=nil,"Transfer pipeline unavailable");
    const MetalTransfer::Pipelines transfer{copy,nil,nil};
    unsigned cases=0;
    for (bool pending : {false,true}) {
        Fixture f(device,transfer);
        std::unique_ptr<QueueGate> gate;
        if (pending)gate=std::make_unique<QueueGate>(f.queue);
        const auto expected=f.Raw(input,64);
        f.Copy(input,source,64,0,0,1,2);
        if (!pending)f.lane.Drain();
        const auto hits=Count(TextureCopyStripedSourceReuse),avoided=Count(TextureCopyStagingBytesAvoided);
        f.Copy(source,destination,64,1,2);
        Require(Count(TextureCopyStripedSourceReuse)==hits+u64(striped),"Pending/completed striped admission differs");
        Require(Count(TextureCopyStagingBytesAvoided)==avoided+(striped ? 160u : 0u),"Avoided staging footprint incorrect");
        if (gate)gate->Release();
        Require(f.Read(destination,64)==expected,"Striped native producer bytes differ");
        Require(f.Raw(source,160)==std::vector<u8>(160,0xcd),"Striped source was materialized into poisoned RAM");
        ++cases;
    }
    for (unsigned mutation=0;mutation<3;++mutation) {
        Fixture f(device,transfer);auto expected=f.Raw(input,64);
        f.Copy(input,source,64,0,0,1,2);f.lane.Drain();
        if (mutation==0)f.Overwrite(source+16,32,0x12); // Unconsumed padding does not prohibit borrowing.
        if (mutation==1) { f.Overwrite(source+4,4,0x33);std::fill(expected.begin()+4,expected.begin()+8,0x33); }
        if (mutation==2) { f.Copy(input+256,source+48,16);auto replacement=f.Raw(input+256,16);std::copy(replacement.begin(),replacement.end(),expected.begin()+16); }
        const auto hits=Count(TextureCopyStripedSourceReuse);
        f.Copy(source,destination,64,1,2);
        Require(Count(TextureCopyStripedSourceReuse)==hits+u64(striped && mutation==0),"Stripe ownership admission incorrect");
        Require(f.Read(destination,64)==expected,"Consumed CPU/different-owner overwrite lost");++cases;
    }
    {
        Fixture first(device,transfer);first.Copy(input,source,64,0,0,1,2);first.lane.Drain();
        Fixture other(device,transfer);const auto hits=Count(TextureCopyStripedSourceReuse);
        other.Copy(source,destination,64,1,2);
        Require(Count(TextureCopyStripedSourceReuse)==hits,"Foreign queue borrowed another lane's ownership");
        Require(other.Read(destination,64)==std::vector<u8>(64,0xcd),"Foreign lane read unrelated native bytes");++cases;
    }
    RenderResources render(device,library);
    for (unsigned mutation=0;mutation<4;++mutation) {
        Fixture f(device,transfer);render.Produce(f);
        const unsigned bytes=mutation==0 ? 1024 : 64;
        const PAddr begin=source+(mutation==1 ? 8 : 0);
        const auto full=ColorBytes();
        auto expected=std::vector<u8>(full.begin()+(begin-source),full.begin()+(begin-source)+bytes);
        if (mutation==3) {
            for (unsigned row=0;row<4;++row)
                std::copy_n(full.begin()+row*32,16,expected.begin()+row*16);
        }
        if (mutation==2) { f.Overwrite(source+8,8,0x66);std::fill(expected.begin()+8,expected.begin()+16,0x66); }
        const auto hits=Count(DirectQueuedExport),copied=Count(DirectQueuedExportBytes);
        f.Copy(begin,destination,bytes,mutation==3 ? 1 : 0,mutation==3 ? 1 : 0);
        const bool admitted=direct && mutation<2;
        Require(Count(DirectQueuedExport)==hits+u64(admitted),"Direct export admission incorrect");
        Require(Count(DirectQueuedExportBytes)==copied+(admitted ? bytes : 0),"Direct export byte interval differs");
        Require(f.Read(destination,bytes)==expected,"Queued texture export differs from canonical guest bytes");
        Require(f.Raw(source,8)==std::vector<u8>(8,0xcd),"Texture source unexpectedly materialized into RAM");++cases;
    }
    {
        Fixture f(device,transfer);render.Produce(f);
        const auto direct_hits=Count(DirectQueuedExport),stripe_hits=Count(TextureCopyStripedSourceReuse);
        Rejected([&]{f.Copy(source+1,destination,64);});
        Require(Count(DirectQueuedExport)==direct_hits && Count(TextureCopyStripedSourceReuse)==stripe_hits,
                "Misaligned rejected request entered optimized consumption");
        Rejected([&]{f.Copy(input,destination);});Rejected([&]{(void)f.Read(source,4);});++cases;
    }
    std::cout<<"PASS: direct="<<direct<<", striped="<<striped<<", "<<cases
             <<" production source-consumption GPU cases; exact bytes and admission counters\n";
    return 0;
} catch (const std::exception& failure) { std::cerr<<"FAIL: "<<failure.what()<<'\n';return 1; } } }
