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
#include <thread>
#include <chrono>

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
    void Produce(Fixture& fixture, PAddr address=source) {
        const auto color=fixture.lane.AcquireTiledColor({address,address+1024},16,16,MTLPixelFormatRGBA8Unorm,
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

// Authored status-only producer failure. It is never submitted to Metal and
// does not claim to reproduce a real driver fault; it exercises lane rejection.
@interface LeaseFailureCommand : NSObject {
    id<MTLCommandQueue> owner_queue;
    MTLCommandBufferStatus test_status;
}
- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue;
- (void)fail;
@end
@implementation LeaseFailureCommand
- (instancetype)initWithQueue:(id<MTLCommandQueue>)queue {
    self=[super init];if(self){owner_queue=queue;test_status=MTLCommandBufferStatusNotEnqueued;}return self;
}
- (id<MTLCommandQueue>)commandQueue { return owner_queue; }
- (id<MTLDevice>)device { return owner_queue.device; }
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferStatus)status { return test_status; }
- (NSError*)error { return test_status==MTLCommandBufferStatusError ?
    [NSError errorWithDomain:@"AuthoredLeaseProducerFailure" code:1 userInfo:nil] : nil; }
- (void)commit { test_status=MTLCommandBufferStatusCommitted; }
- (void)waitUntilCompleted { if(test_status==MTLCommandBufferStatusCommitted)test_status=MTLCommandBufferStatusCompleted; }
- (void)fail { test_status=MTLCommandBufferStatusError; }
@end
namespace {
struct ReleaseGuard {
    QueueGate& gate;
    std::thread thread;
    explicit ReleaseGuard(QueueGate& value) : gate(value),thread([&value] {
        // This bounded hold only releases a known blocked GPU event. No test
        // assumes GPU completion based on an elapsed duration.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));value.Release();
    }) {}
    ~ReleaseGuard(){gate.Release();thread.join();}
};
PicaMetalNativeAttachment Acquire(Fixture& f,RenderResources& render,PAddr address=source) {
    return f.lane.AcquireTiledColor({address,address+1024},16,16,MTLPixelFormatRGBA8Unorm,
                                  render.decoder,render.tiled,nil,f.Reader());
}
void FillQueue(Fixture& f,unsigned count=8) {
    for(unsigned i=0;i<count;++i) {
        const PAddr address=base+0xc000+i*64;
        f.lane.Fill({address,address+64,0xff000000u+i,false,true},f.transfer);
    }
}
void AppendLease(Fixture& f,RenderResources& render,const PicaMetalNativeAttachment& color) {
    const auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                            width:16 height:16 mipmapped:NO];
    descriptor.storageMode=MTLStorageModePrivate;descriptor.usage=MTLTextureUsageRenderTarget;
    descriptor.hazardTrackingMode=MTLHazardTrackingModeTracked;
    const auto texture=[f.queue.device newTextureWithDescriptor:descriptor];
    const PicaMetalNativeAttachment depth{{texture,{base+0xb000,base+0xb400},2},
        [](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8>) {
            throw std::runtime_error("Unused depth was exported");return false;
        },{}};
    std::array<PicaMetalStageVertex,3> vertices{};
    std::array<std::byte,288> common{};std::array<std::byte,1040> light{};
    TexturedMetalDraw draw{};draw.pipeline=render.pipeline;draw.depth_stencil_state=render.depth;
    draw.texel_fetch_sampler=render.sampler;draw.color=color.view.texture;draw.depth_stencil=texture;
    draw.vertex_mode=PicaMetalVertexMode::StageIn;draw.vertex_count=3;
    draw.common_uniform=common;draw.light_uniform=light;draw.packed_vertices=std::as_bytes(std::span(vertices));
    draw.lut_texture_slot=draw.lut_sampler_slot=draw.texture0_slot=draw.texture0_sampler_slot=NSNotFound;
    draw.fragment_common_slot=0;draw.fragment_light_slot=1;
    draw.viewport={0,0,16,16,0,1};draw.scissor={0,0,16,16};
    PicaMetalDrawViews views{};views.color=color.view;views.depth_stencil=depth.view;
    views.color_dirty=color.view.authoritative_bytes;
    f.lane.Record(draw,views,color,depth);
}
}
int main(int argc,char** argv) { @autoreleasepool { try {
    Require(argc==2,"Pass production Metal library");
    setenv("CARA_METAL_FRAME_COST","1",1);setenv("CARA_METAL_GROUPED_PASS","1",1);
    setenv("CARA_METAL_QUEUE_COLOR_EXPORT","1",1);setenv("CARA_METAL_DIRECT_COLOR_EXPORT","0",1);
    setenv("CARA_METAL_OVERLAP_TRANSFERS","0",1);setenv("CARA_METAL_SUBMISSION_WINDOW","8",1);
    const bool skip=Option("CARA_METAL_COLOR_LEASE_NO_RESERVE"),late=Option("CARA_METAL_LATE_DRAW_RESERVE");
    const bool retire_at_record=Option("CARA_METAL_RETIRE_CACHED_INPUTS_AT_RECORD");
    const auto device=MTLCreateSystemDefaultDevice();Require(device!=nil,"Metal device unavailable");
    NSError* error=nil;
    const auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:&error];
    const auto copy=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cara_pica_copy_rows"] error:&error];
    const auto fill=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cara_pica_fill_words"] error:&error];
    Require(copy && fill,"Transfer kernels unavailable");
    const MetalTransfer::Pipelines transfers{copy,fill,nil};
    RenderResources render(device,library);
    unsigned cases=0;
    // Real same-queue A->B->A authority recovery with all8 slots held by
    // unrelated committed GPU fills behind a shared-event gate.
    for(bool pending_source:{false,true}) {
        Fixture f(device,transfers);render.Produce(f);const auto original=Acquire(f,render);
        render.Produce(f,destination);f.lane.Drain();
        QueueGate gate(f.queue);
        if(pending_source) {
            // A real raster write is committed behind the gate and published
            // as A's current owner. B becomes resident again without a draw.
            const auto pending=Acquire(f,render);
            AppendLease(f,render,pending);f.lane.PublishPending();
            (void)Acquire(f,render,destination);
        }
        FillQueue(f,pending_source?7:8);
        const auto waits=Count(QueueReserveFull);
        std::optional<PicaMetalNativeAttachment> lease;
        if(skip) {
            lease=Acquire(f,render);
            Require(gate.event.signaledValue==0,"Metadata acquisition waited for a GPU slot");
            Require(Count(QueueReserveFull)==waits,"Metadata lease reserved a submission slot");
        } else {
            ReleaseGuard release(gate);lease=Acquire(f,render);
            Require(gate.event.signaledValue==1 && Count(QueueReserveFull)==waits+1,
                    "Control did not reserve the full submission queue");
        }
        Require(lease->view.texture==original.view.texture &&
                lease->view.backing_generation==original.view.backing_generation,
                "Authority recovery allocated or changed an immutable backing");
        if(skip) {
            ReleaseGuard release(gate);
            const auto bytes=f.Read(source,1024);
            Require(gate.event.signaledValue==1,"Guest read bypassed pending GPU ordering");
            Require(bytes==ColorBytes(),"Recovered authority bytes differ after queue drain");
        } else Require(f.Read(source,1024)==ColorBytes(),"Control authority bytes differ");
        Require(f.Raw(source,1024)==std::vector<u8>(1024,0xcd),"Metadata lease materialized guest RAM");
        f.lane.Drain();++cases;
    }
    // New imports and incomplete authority still need their own submission slot.
    for(bool partial:{false,true}) {
        Fixture f(device,transfers);render.Produce(f);render.Produce(f,destination);f.lane.Drain();
        if(partial)f.Overwrite(source+8,8,0x66);
        QueueGate gate(f.queue);FillQueue(f);
        const auto waits=Count(QueueReserveFull);ReleaseGuard release(gate);
        const auto imported=Acquire(f,render,partial?source:input);
        Require(imported.view.texture && gate.event.signaledValue==1 && Count(QueueReserveFull)==waits+1,
                "New import bypassed submission reservation");
        f.lane.Drain();++cases;
    }
    // The combination with late draw reservation permits upload/encoding, not
    // a9th submitted command. Final publication still reserves before commit.
    {
        Fixture f(device,transfers);render.Produce(f);render.Produce(f,destination);f.lane.Drain();
        QueueGate gate(f.queue);FillQueue(f);
        const auto waits=Count(QueueReserveFull);
        if(skip) {
            const auto lease=Acquire(f,render);
            Require(gate.event.signaledValue==0,"Combined lease waited early");
            if(late) {
                AppendLease(f,render,lease);
                Require(gate.event.signaledValue==0 && Count(QueueReserveFull)==waits,
                        "Late record reserved before bounded arena encoding");
                ReleaseGuard release(gate);f.lane.PublishPending();
            } else {
                ReleaseGuard release(gate);AppendLease(f,render,lease);f.lane.PublishPending();
            }
        } else {
            ReleaseGuard release(gate);const auto lease=Acquire(f,render);
            AppendLease(f,render,lease);f.lane.PublishPending();
        }
        Require(gate.event.signaledValue==1 && Count(QueueReserveFull)==waits+1,
                "Final record/publication failed to reserve exactly one slot");
        Require(f.Read(source,1024)==ColorBytes(),"Late-record pixels changed");
        f.lane.Drain();++cases;
    }
    {
        Fixture f(device,transfers);render.Produce(f);render.Produce(f,destination);f.lane.Drain();
        auto failure=[[LeaseFailureCommand alloc] initWithQueue:f.queue];
        f.lane.SubmitSampledTextureBuild((id<MTLCommandBuffer>)failure);
        [failure fail];
        Rejected([&]{(void)Acquire(f,render);});
        Rejected([&]{(void)Acquire(f,render);});
        Rejected([&]{(void)f.Read(source,4);});
        ++cases;
    }
    {
        Fixture f(device, transfers);
        const auto resident = Acquire(f, render);
        f.lane.Drain();
        auto failure = [[LeaseFailureCommand alloc] initWithQueue:f.queue];
        f.lane.SubmitSampledTextureBuild((id<MTLCommandBuffer>)failure);
        [failure fail];
        if (retire_at_record) {
            const auto cached = Acquire(f, render);
            Require(cached.view.texture == resident.view.texture, "Cached lease changed before Record");
            Rejected([&]{ AppendLease(f, render, cached); });
        } else {
            Rejected([&]{ (void)Acquire(f, render); });
        }
        Rejected([&]{ (void)f.Read(source, 4); });
        ++cases;
    }
    std::cout<<"PASS: color_lease="<<skip<<", late_record="<<late<<", "<<cases
             <<" production lane cases; full8queue, exact bytes, bounded encoding/publication and authored producer rejection\n";
    return 0;
} catch(const std::exception& failure) {std::cerr<<"FAIL: "<<failure.what()<<'\n';return 1;} } }
