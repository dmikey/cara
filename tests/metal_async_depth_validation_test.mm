// SPDX-License-Identifier: GPL-2.0-or-later
// GPU tests for checked depth export and reimport on the production lane.
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
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
using namespace Cara;
using namespace Cara::NativeMetalCost;
constexpr PAddr base=0x18000000,source=base+0x1000;
void Require(bool ok,const char* text) { if (!ok) throw std::runtime_error(text); }
template<class F> void Rejected(F&& f) {
    bool rejected=false;try { f(); } catch (const std::exception&) { rejected=true; }
    Require(rejected,"Rejected transfer lane resumed or invalid copy succeeded");
}
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
    explicit Fixture(id<MTLDevice> device)
        :queue([device newCommandQueue]),lane(queue,256*1024,[](PAddr,PAddr){}) {}
    MetalGuestAuthority::GuestReader Reader() {
        return [this](PAddr first,std::span<u8> bytes) {
            if (first<base || first-base>ram.size() || bytes.size()>ram.size()-(first-base))return false;
            std::memcpy(bytes.data(),ram.data()+first-base,bytes.size());return true;
        };
    }
    std::vector<u8> Raw(PAddr address,unsigned bytes) {
        Require(address>=base && address-base+bytes<=ram.size(),"Fixture byte bounds invalid");
        return {ram.begin()+address-base,ram.begin()+address-base+bytes};
    }
    std::vector<u8> Read(PAddr address,unsigned bytes) {
        std::vector<u8> result(bytes);
        Require(lane.ReadGuest(address,result,Reader()),"Native guest read failed");return result;
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
};
template<class F> bool Watchdog(QueueGate& gate,F&& operation) {
    std::mutex mutex;std::condition_variable cv;bool returned=false,released=false;
    std::thread thread([&] {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock,std::chrono::milliseconds(100),[&]{return returned;})) {
            released=true;gate.Release();
        }
    });
    try { operation(); } catch (...) {
        {std::lock_guard lock(mutex);returned=true;}cv.notify_one();gate.Release();thread.join();throw;
    }
    {std::lock_guard lock(mutex);returned=true;}cv.notify_one();thread.join();return released;
}
id<MTLComputePipelineState> Compute(id<MTLDevice> device,id<MTLLibrary> lib,NSString* name) {
    auto pipeline=[device newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:nullptr];
    Require(pipeline!=nil,"Production depth compute pipeline unavailable");return pipeline;
}
struct DepthFixture {
    Fixture f;
    RenderResources render;
    PicaMetalDepthImportPipelines imports;
    id<MTLComputePipelineState> pack;
    explicit DepthFixture(id<MTLDevice> device,id<MTLLibrary> lib)
        :f(device),render(device,lib),
         imports{Compute(device,lib,@"DetilePicaDepth24Stencil8"),Compute(device,lib,@"unpack_pica_depth")},
         pack(Compute(device,lib,@"pack_pica_depth")) {}
    static constexpr PAddr a=base+0xb000,b=base+0xc000;
    PicaMetalNativeAttachment Acquire(PAddr address) {
        return f.lane.AcquireTiledDepthStencil({address,address+1024},16,16,imports,render.tiled,pack,f.Reader());
    }
    void Publish(const PicaMetalNativeAttachment& depth) {
        auto color=f.lane.AcquireTiledColor({source,source+1024},16,16,MTLPixelFormatRGBA8Unorm,
                                           render.decoder,render.tiled,nil,f.Reader());
        std::array<PicaMetalStageVertex,3> vertices{};
        std::array<std::byte,288> common{};std::array<std::byte,1040> light{};
        TexturedMetalDraw draw{};draw.pipeline=render.pipeline;draw.depth_stencil_state=render.depth;
        draw.texel_fetch_sampler=render.sampler;draw.color=color.view.texture;draw.depth_stencil=depth.view.texture;
        draw.vertex_mode=PicaMetalVertexMode::StageIn;draw.vertex_count=3;
        draw.common_uniform=common;draw.light_uniform=light;draw.packed_vertices=std::as_bytes(std::span(vertices));
        draw.lut_texture_slot=draw.lut_sampler_slot=draw.texture0_slot=draw.texture0_sampler_slot=NSNotFound;
        draw.fragment_common_slot=0;draw.fragment_light_slot=1;draw.viewport={0,0,16,16,0,1};draw.scissor={0,0,16,16};
        PicaMetalDrawViews views{};views.color=color.view;views.depth_stencil=depth.view;
        views.color_dirty=color.view.authoritative_bytes;views.depth_stencil_dirty=depth.view.authoritative_bytes;
        f.lane.Record(draw,views,color,depth);f.lane.PublishPending();
    }
    void SeedInvalid(const PicaMetalNativeAttachment& depth) {
        auto buffer=[f.queue.device newBufferWithLength:256*16 options:MTLResourceStorageModeShared];
        auto* words=static_cast<u32*>(buffer.contents);
        std::fill_n(words,256*16/4,0x7fc00000u);
        auto command=[f.queue commandBuffer];auto blit=[command blitCommandEncoder];
        [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:256 sourceBytesPerImage:256*16
                 sourceSize:MTLSizeMake(16,16,1) toTexture:depth.view.texture destinationSlice:0
          destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0) options:MTLBlitOptionDepthFromDepthStencil];
        [blit endEncoding];[command commit];[command waitUntilCompleted];
        Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"Invalid-depth setup faulted GPU");
    }
};
// Real GPU writes to each independent check buffer, without GPU command faults.
void CheckReceipt(id<MTLDevice> device,unsigned bad) {
    const auto queue=[device newCommandQueue];unsigned rejected=0;
    MetalGuestAuthority authority;
    PicaMetalSubmissionQueue submissions(queue,[&]{++rejected;authority.RejectSubmittedWrites();});
    const auto first=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    const auto second=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *static_cast<u32*>(first.contents)=0;*static_cast<u32*>(second.contents)=0;
    QueueGate gate(queue);
    auto command=[queue commandBuffer];auto blit=[command blitCommandEncoder];
    [blit fillBuffer:(bad==1 ? first : second) range:NSMakeRange(0,4) value:(bad ? 1 : 0)];
    [blit endEncoding];[command commit];
    auto receipt=std::make_shared<PicaMetalValidationReceipt>(command,std::array<id<MTLBuffer>,2>{first,second});
    PicaMetalDrawTicket ticket;ticket.command=command;ticket.validation=receipt;submissions.Track(ticket);
    Require(ticket.Poll()==PicaMetalCompletion::Pending,"Gated receipt not pending");
    // Fill every remaining queue slot; the immutable dependency snapshot must
    // retain all commands and the validation receipt in the first slot.
    auto capacity = std::stoul(std::getenv("CARA_METAL_SUBMISSION_WINDOW") ?
                               std::getenv("CARA_METAL_SUBMISSION_WINDOW") : "8");
    for(unsigned i=1;i<capacity;++i) {auto c=[queue commandBuffer];[c commit];PicaMetalDrawTicket t;t.command=c;submissions.Track(t);}
    const auto before=submissions.Predecessors();Require(before && before->count==capacity,"Missing full queue dependencies");
    const auto raw=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    const auto descendant=before->commands[capacity-1];
    bool exported=false;
    const auto writer=std::make_shared<MetalSurfaceWriter>(raw,descendant,1,base,base+4,
        [&](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8> out) {
            exported=true;std::fill(out.begin(),out.end(),0x5a);return true;
        },0,before,raw);
    MetalGuestWrite write{base,base+4,writer};authority.Publish({&write,1});
    gate.Release();[descendant waitUntilCompleted];
    Require(descendant.status==MTLCommandBufferStatusCompleted && !descendant.error,"Validation test faulted GPU");
    auto successor=[queue commandBuffer];[successor commit];
    const auto success=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *static_cast<u32*>(success.contents)=0;
    PicaMetalValidationReceipt dependent(successor, {success,success}, writer);
    Require(dependent.Wait()==(bad ? PicaMetalCompletion::Failed : PicaMetalCompletion::Complete),
            "Joined consumer lost the original producer's validation");
    PicaMetalValidationReceipt single(successor, bad == 1 ? first : second);
    Require(single.Wait() == (bad ? PicaMetalCompletion::Failed : PicaMetalCompletion::Complete),
            "Single-buffer sample receipt accepted the wrong result");
    PicaMetalValidationReceipt single_dependent(successor, success, writer);
    Require(single_dependent.Wait() == (bad ? PicaMetalCompletion::Failed : PicaMetalCompletion::Complete),
            "Sample receipt lost the original producer's validation");
    const auto valid_writer = std::make_shared<MetalSurfaceWriter>(
        raw, successor, 2, base, base + 4,
        [](id<MTLCommandQueue>, id<MTLResource>, u64, PAddr, PAddr, std::span<u8>) {
            return true;
        });
    const PicaMetalValidationReceipt multiple(successor, success, valid_writer, {writer});
    Require(multiple.Wait() == (bad ? PicaMetalCompletion::Failed : PicaMetalCompletion::Complete),
            "A later source dependency escaped sample validation");
    std::array<u8,4> bytes{};
    if (bad) {
        Require(writer->Status()==MetalGuestWriteStatus::Failed,"Descendant blessed invalid checks");
        Require(!writer->ExportGuestBytes(base,base+4,bytes) && !exported,"Invalid bytes reached CPU");
        Rejected([&]{authority.NativeSegments(base,base+4);});
        {
            QueueGate untracked_gate(queue);
            auto untracked=[queue commandBuffer];auto fill=[untracked blitCommandEncoder];
            [fill fillBuffer:raw range:NSMakeRange(0,4) value:0x7b];[fill endEncoding];[untracked commit];
            PicaMetalDrawTicket unslotted;unslotted.command=untracked;
            Require(Watchdog(untracked_gate,[&]{Rejected([&]{submissions.Track(unslotted);});}),
                    "Track failure exposed an untracked pending GPU command");
            Require(untracked.status==MTLCommandBufferStatusCompleted && !untracked.error &&
                    *static_cast<const u32*>(raw.contents)==0x7b7b7b7b,
                    "Committed unslotted command was not drained before failure");
        }
        Rejected([&]{submissions.Retire(false);});Require(rejected!=0,"Queue did not reject guest writes");
        *static_cast<u32*>(first.contents)=0;*static_cast<u32*>(second.contents)=0;
        Require(receipt->Poll()==PicaMetalCompletion::Failed,"Validation fault did not latch");
        submissions.Abort();Rejected([&]{submissions.Drain();});
        Require(writer->Status()==MetalGuestWriteStatus::Failed,"Abort lost retained validation failure");
    } else {
        submissions.Drain();Require(writer->ExportGuestBytes(base,base+4,bytes) && exported,"Valid receipt blocked CPU export");
        Require(receipt->Poll()==PicaMetalCompletion::Complete,"Valid receipt not complete");
    }
    auto other=[queue commandBuffer];[other commit];PicaMetalDrawTicket mismatched;mismatched.command=other;mismatched.validation=receipt;
    Require(mismatched.Poll()==PicaMetalCompletion::Failed,"Mismatched receipt accepted");[other waitUntilCompleted];
}
}
int main(int argc,char** argv) { @autoreleasepool {try {
    Require(argc==2,"Pass production Metal library");
    setenv("CARA_METAL_FRAME_COST","1",1);setenv("CARA_METAL_GROUPED_PASS","1",1);
    setenv("CARA_METAL_JOIN_CHECKED_DEPTH_IMPORT","1",1);setenv("CARA_METAL_FUSE_DEPTH_CLEAR","0",1);
    setenv("CARA_METAL_EXACT_DEPTH_CLEAR","0",1);setenv("CARA_METAL_QUEUE_COLOR_EXPORT","1",1);
    const bool async=Option("CARA_METAL_ASYNC_CHECKED_DEPTH_IMPORT");
    const bool pending=async && Option("CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT");
    auto device=MTLCreateSystemDefaultDevice();Require(device!=nil,"No Metal device");
    auto lib=[device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] error:nullptr];
    Require(lib!=nil,"No production library");
    for(unsigned bad=0;bad<3;++bad)CheckReceipt(device,bad);
    {
        DepthFixture fixture(device, lib);
        QueueGate gate(fixture.f.queue);
        PicaMetalNativeAttachment attachment;
        Require(Watchdog(gate, [&] { attachment = fixture.Acquire(DepthFixture::a); }) != async,
                "RAM-backed depth import must validate through the queue in async mode");
        gate.Release();
        fixture.Publish(attachment);
        fixture.f.lane.Drain();
    }
    {
        DepthFixture fixture(device, lib);
        constexpr u32 size = 16 * 16 * 3;
        constexpr auto address = DepthFixture::a;
        auto* ram = fixture.f.ram.data() + address - base;
        for (u32 i = 0; i < size; ++i)
            ram[i] = u8(i * 23 + i / 7);
        const auto expected = fixture.f.Raw(address, size);
        auto imports = fixture.imports;
        imports.detile_depth = Compute(device, lib, @"DetilePicaDepth24");
        const auto tiled = Compute(device, lib, @"ExportPicaTiledDepth24");
        const auto acquire = [&](PAddr start) {
            return fixture.f.lane.AcquireTiledDepthStencil(
                {start, start + size}, 16, 16, imports, tiled, fixture.pack,
                fixture.f.Reader(), false);
        };
        const auto attachment = acquire(address);
        fixture.Publish(attachment);
        fixture.f.lane.Drain();
        (void)acquire(DepthFixture::b);
        QueueGate gate(fixture.f.queue);
        fixture.Publish(attachment);
        PicaMetalNativeAttachment restored;
        Require(Watchdog(gate, [&] { restored = acquire(address); }) != pending,
                "Packed D24 reimport did not preserve queue ordering");
        gate.Release();
        fixture.Publish(restored);
        const auto partial = fixture.f.Read(address + 1, size - 3);
        Require(std::equal(partial.begin(), partial.end(), expected.begin() + 1),
                "Guest D24 partial read changed packed bytes");
        Require(fixture.f.Read(address, size) == expected, "Guest D24 round-trip differs");
    }
    {
        DepthFixture fixture(device,lib);
        for(unsigned i=0;i<1024;++i)fixture.f.ram[DepthFixture::a-base+i]=u8(i*37+(i>>2)*11);
        const auto expected=fixture.f.Raw(DepthFixture::a,1024);
        auto a=fixture.Acquire(DepthFixture::a);fixture.Publish(a);fixture.f.lane.Drain();
        (void)fixture.Acquire(DepthFixture::b);
        QueueGate gate(fixture.f.queue);PicaMetalNativeAttachment restored;
        const bool blocked=Watchdog(gate,[&]{restored=fixture.Acquire(DepthFixture::a);});
        Require(blocked!=async,"Async depth acquisition did not preserve expected gate behavior");
        if(async) {
            fixture.Publish(restored);
            Require(Watchdog(gate,[&]{Require(fixture.f.Read(DepthFixture::a,1024)==expected,"Async guest depth/stencil bytes changed");}),
                    "CPU read escaped pending validation");
        } else Require(fixture.f.Read(DepthFixture::a,1024)==expected,"Control depth/stencil bytes changed");
        fixture.f.lane.Drain();
    }
    {
        DepthFixture fixture(device,lib);auto a=fixture.Acquire(DepthFixture::a);
        fixture.SeedInvalid(a);fixture.Publish(a);fixture.f.lane.Drain();(void)fixture.Acquire(DepthFixture::b);
        if(async) {
            QueueGate gate(fixture.f.queue);auto restored=fixture.Acquire(DepthFixture::a);
            fixture.Publish(restored);
            Rejected([&]{(void)Watchdog(gate,[&]{(void)fixture.f.Read(DepthFixture::a,1024);});});
        } else Rejected([&]{(void)fixture.Acquire(DepthFixture::a);fixture.f.lane.Drain();});
        fixture.f.lane.Abort();Rejected([&]{fixture.f.lane.Drain();});
        Rejected([&]{(void)fixture.f.Read(DepthFixture::a,1024);});
    }
    {
        DepthFixture fixture(device,lib);
        for(unsigned i=0;i<1024;++i)fixture.f.ram[DepthFixture::a-base+i]=u8(i*17+(i>>2)*23);
        const auto expected=fixture.f.Raw(DepthFixture::a,1024);
        auto a=fixture.Acquire(DepthFixture::a);
        fixture.Publish(a);fixture.f.lane.Drain();
        (void)fixture.Acquire(DepthFixture::b);
        QueueGate gate(fixture.f.queue);
        fixture.Publish(a); // Its writer is pending behind the closed gate.
        PicaMetalNativeAttachment restored;
        Require(Watchdog(gate,[&]{restored=fixture.Acquire(DepthFixture::a);})!=pending,
                "Acquisition did not distinguish the tracked pending producer");
        fixture.Publish(restored);
        const auto read=[&]{Require(fixture.f.Read(DepthFixture::a,1024)==expected,
                                    "Pending GPU export changed depth/stencil bytes");};
        if(pending)
            Require(Watchdog(gate,read),"Guest read escaped the pending export/import receipt");
        else
            read();
        fixture.f.lane.Drain();
    }
    std::cout<<"PASS async_depth="<<async<<" pending_depth="<<pending
             <<" receipt_cases=3 production_joined_cases=3 all_stencil_codes=256\n";
    return 0;
} catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;} } }
