// SPDX-License-Identifier: GPL-2.0-or-later
// Tests production DecodeTexture snapshot policies with complete RAM snapshots,
// exact GPU readback, native producers, cache churn and queue lifetime changes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/exporter-adapter/pica_metal_visibility_export.h"
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace {
using namespace Cara;
using namespace Cara::NativeMetalCost;
constexpr PAddr base = 0x20000000;
void Require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
bool Option(const char* key) {
    const auto* value = std::getenv(key);
    return value && std::string_view(value)=="1";
}
std::uint64_t Counted(Phase phase) { return spans[phase].calls; }
unsigned Morton(unsigned x,unsigned y) {
    const auto spread = [](unsigned value) { return (value&1)|((value&2)<<1)|((value&4)<<2); };
    return spread(x&7)|(spread(y&7)<<1);
}
struct Fixture {
    id<MTLCommandQueue> queue;
    std::map<PAddr,std::vector<u8>> ram;
    unsigned reader_calls=0,view_calls=0;
    enum class ViewMode { Valid, Empty, Short, Long, Null, Throw };
    ViewMode view_mode=ViewMode::Valid;
    PicaMetalNativeDrawLane lane;
    PicaMetalTextureDecoder& decoder;
    const MetalTransfer::Pipelines& transfer;
    Fixture(id<MTLDevice> device,PicaMetalTextureDecoder& decode,const MetalTransfer::Pipelines& pipelines,bool supply_view=true)
        : queue([device newCommandQueue]),lane(queue,256*1024,{},supply_view ?
            PicaMetalNativeDrawLane::GuestRamView{[this](PAddr address,std::size_t bytes) {return View(address,bytes);}} :
            PicaMetalNativeDrawLane::GuestRamView{}),decoder(decode),transfer(pipelines) {
        spans = {};
    }
    std::vector<u8>& Source(PAddr address,std::size_t bytes,std::array<u8,4> rgba={17,34,51,68}) {
        auto& data = ram[address]; data.resize(bytes);
        for (std::size_t i=0;i<bytes;i+=4)
            for (unsigned j=0;j<4;++j) data[i+j] = rgba[3-j];
        return data;
    }
    std::span<const u8> View(PAddr address,std::size_t bytes) {
        ++view_calls;
        if(view_mode==ViewMode::Throw)throw std::runtime_error("Authored unavailable view");
        if(view_mode==ViewMode::Empty)return {};
        if(view_mode==ViewMode::Null)return {static_cast<const u8*>(nullptr),bytes};
        auto after=ram.upper_bound(address);if(after==ram.begin())return {};
        const auto& [begin,data]=*--after;const auto offset=std::size_t(address-begin);
        if(offset>data.size()||bytes>data.size()-offset)return {};
        if(view_mode==ViewMode::Short)return {data.data()+offset,bytes-1};
        if(view_mode==ViewMode::Long)return {data.data()+offset,bytes+1};
        return {data.data()+offset,bytes};
    }
    MetalGuestAuthority::GuestReader Reader() {
        return [this](PAddr address,std::span<u8> output) {
            ++reader_calls;
            auto after = ram.upper_bound(address);
            if (after==ram.begin()) return false;
            const auto& [begin,data] = *--after;
            const std::size_t offset = address-begin;
            if (offset>data.size() || output.size()>data.size()-offset) return false;
            std::memcpy(output.data(),data.data()+offset,output.size()); return true;
        };
    }
    id<MTLTexture> Decode(PAddr address,std::size_t bytes=256,unsigned width=8,unsigned height=8,
                         unsigned row=256,unsigned format=0,bool alpha=false) {
        return lane.DecodeTexture(decoder,{address,PAddr(address+bytes)},row,width,height,
                                  format,alpha,Reader());
    }
    void Copy(PAddr source,PAddr destination,unsigned bytes) {
        lane.CopyTexture({source,destination,bytes,0,0,0,0},transfer,Reader());
    }
    void Verify(id<MTLTexture> texture,const std::vector<u8>& tiled,unsigned row=256,
                unsigned format=0,bool no_alpha=false) {
        Require(texture != nil,"Decode returned no texture");
        const auto width=texture.width,height=texture.height;
        const auto output_row=(width*4+255)&~NSUInteger(255);
        auto output=[queue.device newBufferWithLength:output_row*height options:MTLResourceStorageModeShared];
        auto command=[queue commandBuffer]; auto blit=[command blitCommandEncoder];
        Require(output && blit,"GPU readback allocation failed");
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                   sourceSize:MTLSizeMake(width,height,1) toBuffer:output destinationOffset:0
                   destinationBytesPerRow:output_row destinationBytesPerImage:output_row*height];
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"GPU readback failed");
        const auto* actual=static_cast<const u8*>(output.contents);
        const unsigned texel_bytes=format==0 ? 4 : 3;
        for (NSUInteger y=0;y<height;++y) for (NSUInteger x=0;x<width;++x) {
            const auto guest_y=height-1-y;
            const auto offset=(guest_y/8)*row+(x/8)*64*texel_bytes+Morton(x,guest_y)*texel_bytes;
            Require(offset+texel_bytes<=tiled.size(),"Golden decoder exceeded complete source");
            const auto* data=tiled.data()+offset;
            const std::array<u8,4> expected = format==0 ?
                std::array<u8,4>{data[3],data[2],data[1],u8(no_alpha ? 255 : data[0])} :
                std::array<u8,4>{data[2],data[1],data[0],255};
            Require(std::memcmp(actual+y*output_row+x*4,expected.data(),4)==0,
                    "GPU decoded pixels differ from complete-source CPU golden");
        }
        lane.Drain();
    }
};
}
int main(int argc,char** argv) { @autoreleasepool { try {
    Require(argc==2,"Pass production cara-pica.metallib path");
    const bool borrow=Option("CARA_METAL_BORROW_RAM_SNAPSHOT_COMPARE");
    const bool keyed=Option("CARA_METAL_CACHE_KEYED_TEXTURE_SNAPSHOTS");
    const bool large=Option("CARA_METAL_CACHE_LARGE_TEXTURE_SNAPSHOTS");
    setenv("CARA_METAL_FRAME_COST","1",1);
    setenv("CARA_METAL_REDUCE_COPY_QUEUE_WORK","1",1);
    setenv("CARA_METAL_OVERLAP_TRANSFERS","1",1);
    setenv("CARA_METAL_DRAW_STAGE_PROFILE","0",1);
    auto device=MTLCreateSystemDefaultDevice();
    Require(device && device.hasUnifiedMemory,"Requires supported Apple unified-memory GPU");
    NSError* error=nil;
    auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]]
                                      error:&error];
    if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
    PicaMetalTextureDecoder decoder(library);
    for (unsigned format : {0u,1u}) for (bool alpha : {false,true})
        if (!decoder.Prepare(format,alpha,&error)) throw std::runtime_error(error.localizedDescription.UTF8String);
    auto rows=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cara_pica_copy_rows"]
                                                   error:&error];
    if (!rows) throw std::runtime_error(error.localizedDescription.UTF8String);
    MetalTransfer::Pipelines transfer{rows,nil,nil};
    unsigned cases=0;
    {
        Fixture f(device,decoder,transfer);
        id<MTLTexture> first=nil,second=nil;
        for (unsigned i=0;i<129;++i) {
            f.Source(base+i*512,256);
            auto texture=f.Decode(base+i*512);
            if (i==0) first=texture;
            if (i==1) second=texture;
        }
        auto revisited=f.Decode(base);
        Require((revisited==first)==keyed,"129-texture churn did not distinguish keyed cache from FIFO128");
        f.Verify(revisited,f.ram.at(base));
        for (unsigned i=129;i<512;++i) { f.Source(base+i*512,256); f.Decode(base+i*512); }
        if (keyed) Require(Counted(KeyedSnapshotEvictions)==0,"Keyed cache evicted below 512 keys");
        f.Source(base+512*512,256); f.Decode(base+512*512);
        if (keyed) Require(Counted(KeyedSnapshotEvictions)==1,"Keyed 512-entry bound did not evict exactly one key");
        Require((f.Decode(base)==first)==keyed,"LRU refresh did not preserve the recently reused key");
        auto replaced=f.Decode(base+512);
        Require(replaced!=second,"Oldest key survived beyond cache capacity");
        if (keyed) Require(Counted(KeyedSnapshotEvictions)==2,"Keyed LRU eviction order changed");
        else Require(Counted(KeyedSnapshotHits)==0 && Counted(KeyedSnapshotEvictions)==0,
                     "Disabled keyed cache was active");
        f.Verify(replaced,f.ram.at(base+512)); ++cases;
    }
    {
        Fixture f(device,decoder,transfer);
        auto& source=f.Source(base,272);
        auto original=f.Decode(base);
        const auto before_reads=f.reader_calls,before_views=f.view_calls;
        Require(f.Decode(base)==original,"Unchanged small source was not reused");
        Require(f.reader_calls-before_reads==(borrow?0u:1u),"Unchanged hit did not bypass the copying reader");
        Require(f.view_calls-before_views==(borrow?1u:0u),"Borrow opt-in did not control view callback");
        source[255]^=0xff; // Last used byte: changes red at (7,0).
        auto changed=f.Decode(base);
        Require(changed!=original,"Small cache reused a stale last-byte snapshot");
        f.Verify(changed,source);
        const auto changed_count=Counted(KeyedSnapshotBytesChanged);
        auto extended=f.Decode(base,272);
        Require(extended!=changed,"Source interval end was omitted from snapshot identity");
        source[271]^=0xff; // Undecoded padding is still part of complete source identity.
        auto padding_changed=f.Decode(base,272);
        Require(padding_changed!=extended,"Snapshot equality ignored source tail padding");
        if (keyed) Require(Counted(KeyedSnapshotBytesChanged)==changed_count+1,
                           "Complete padded source mutation was not observed");
        f.Verify(padding_changed,source); ++cases;
    }
    {
        Fixture f(device,decoder,transfer);
        auto& source=f.Source(base,256);
        auto original=f.Decode(base);
        auto alpha=f.Decode(base,256,8,8,256,0,true);
        auto format=f.Decode(base,256,8,8,192,1);
        auto row=f.Decode(base,256,8,8,512);
        auto dimension=f.Decode(base,256,4,8,256);
        f.Source(base+512,256);
        auto address=f.Decode(base+512);
        Require(alpha!=original && format!=original && row!=original && dimension!=original && address!=original,
                "Snapshot key merged alpha, format, row, dimension or address state");
        f.Verify(alpha,source,256,0,true); f.Verify(format,source,192,1);
        f.Verify(row,source,512); f.Verify(dimension,source); f.Verify(address,source); ++cases;
    }
    {
        Fixture f(device,decoder,transfer);
        constexpr unsigned bytes=1024*1024,row=512/8*256;
        auto& source=f.Source(base,bytes);
        auto original=f.Decode(base,bytes,512,512,row);
        auto repeated=f.Decode(base,bytes,512,512,row);
        Require((repeated==original)==large,"Large snapshot opt-in did not control exact reuse");
        if (large) Require(Counted(LargeSnapshotHits)==1 && Counted(LargeSnapshotMisses)==1,
                           "Large snapshot hit/miss counters changed");
        source.back()^=0xff;
        auto changed=f.Decode(base,bytes,512,512,row);
        Require(changed!=repeated,"Large snapshot ignored final source byte");
        f.Verify(changed,source,row);
        auto alpha=f.Decode(base,bytes,512,512,row,0,true);
        Require(alpha!=changed,"Large snapshot key merged alpha state");
        f.Verify(alpha,source,row,0,true);
        if (large) Require(Counted(LargeSnapshotMisses)==3,"Large key/mutation miss count differs");
        else Require(Counted(LargeSnapshotHits)==0 && Counted(SnapshotOutsideLimits)>=4,
                     "Disabled large cache admitted excluded input");
        ++cases;
    }
    if(large) {
        Fixture f(device,decoder,transfer);id<MTLTexture> oldest=nil,newest=nil;
        constexpr unsigned bytes=1024*1024,row=512/8*256;
        for(unsigned i=0;i<33;++i){f.Source(base+i*0x200000,bytes);auto value=f.Decode(base+i*0x200000,bytes,512,512,row);if(i==0)oldest=value;if(i==32)newest=value;}
        Require(f.Decode(base+32*0x200000,bytes,512,512,row)==newest,"Large cache lost latest key below budget");
        auto evicted=f.Decode(base,bytes,512,512,row);Require(evicted!=oldest,"Large cache exceeded 64MiB snapshot+texture budget");
        f.Verify(evicted,f.ram.at(base),row);++cases;
    }
    for (const unsigned bytes : {256u,1024u*1024u}) {
        Fixture f(device,decoder,transfer);
        const unsigned edge=bytes==256 ? 8 : 512,row=edge/8*256;
        const PAddr other=base+0x200000;
        const auto old=f.Source(base,bytes);
        const auto replacement=f.Source(other,bytes,{129,88,77,66});
        auto original=f.Decode(base,bytes,edge,edge,row);
        f.Copy(other,base,bytes); // Source RAM stays old; authority owns new GPU bytes.
        const auto owner_views=f.view_calls;
        auto owned=f.Decode(base,bytes,edge,edge,row);
        Require(f.view_calls==owner_views,"GPU-owned poisoned RAM reached borrowed view");
        Require(owned!=original,"Native producer incorrectly reused a RAM snapshot");
        Require(f.ram.at(base)==old,"Native copy unexpectedly materialized destination RAM");
        Require(Counted(SnapshotNativeOwners)==std::uint64_t(bytes==256 || large),
                "Snapshot admission did not check native owner coverage");
        f.Verify(owned,replacement,row);
        f.lane.InvalidateGuest(base,base+bytes);
        auto& fresh=f.Source(base,bytes,{201,123,45,67});
        auto cpu=f.Decode(base,bytes,edge,edge,row);
        Require(cpu!=owned,"CPU invalidation reused a native producer texture");
        f.Verify(cpu,fresh,row); ++cases;
    }
    {
        Fixture f(device,decoder,transfer);
        auto& source=f.Source(base,272);
        f.Source(base+512,16,{201,123,45,67});
        auto original=f.Decode(base,272);
        f.Copy(base+512,base+256,16); // Only padding, outside decoded texels, is GPU-owned.
        auto owned=f.Decode(base,272);
        Require(owned!=original && Counted(SnapshotNativeOwners)==1,
                "Native owner in source tail padding bypassed full-interval admission");
        f.Verify(owned,source); ++cases;
    }
    {
        id<MTLTexture> previous=nil;
        { Fixture f(device,decoder,transfer); auto& source=f.Source(base,256);
          previous=f.Decode(base); f.Verify(previous,source); }
        Fixture next(device,decoder,transfer); auto& source=next.Source(base,256);
        auto current=next.Decode(base);
        Require(current!=previous,"Snapshot cache survived command-queue ownership change");
        next.Verify(current,source); ++cases;
    }
    {
        Fixture f(device,decoder,transfer);f.Source(base,256,{201,123,45,67});
        auto first=f.Decode(base);
        auto program=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; vertex float4 v(uint i [[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);} fragment float4 f(){return float4(17.0/255,34.0/255,51.0/255,68.0/255);}" options:nil error:&error];
        auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[program newFunctionWithName:@"v"];pd.fragmentFunction=[program newFunctionWithName:@"f"];
        pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;pd.depthAttachmentPixelFormat=pd.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
        auto pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];Require(pipeline!=nil,"Pending draw pipeline failed");
        const auto texture=[&](MTLPixelFormat format){auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:8 height:8 mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;return [device newTextureWithDescriptor:d];};
        auto color_texture=texture(MTLPixelFormatRGBA8Unorm),depth_texture=texture(MTLPixelFormatDepth32Float_Stencil8);
        auto tiled=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"ExportPicaTiled32"] error:&error];
        PicaMetalNativeAttachment color{{color_texture,{base,base+256},991},MakePicaTiledVisibilityExporter(color_texture,991,{base,base+256},PicaExportFormat::Rgba8,tiled,nil),MetalRgba8Layout{base,base+256,8,8,true}};
        const auto unused=[](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8>){throw std::runtime_error("Unexpected depth export");return false;};
        PicaMetalNativeAttachment depth{{depth_texture,{base+4096,base+4352},992},unused,{}};
        std::array<PicaMetalStageVertex,3> vertices{};std::array<std::byte,288> common{};std::array<std::byte,1040> light{};
        TexturedMetalDraw draw{};draw.pipeline=pipeline;draw.depth_stencil_state=[device newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        draw.texel_fetch_sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];draw.color=color_texture;draw.depth_stencil=depth_texture;
        draw.vertex_mode=PicaMetalVertexMode::StageIn;draw.vertex_count=3;draw.common_uniform=common;draw.light_uniform=light;draw.packed_vertices=std::as_bytes(std::span(vertices));
        draw.lut_texture_slot=draw.lut_sampler_slot=draw.texture0_slot=draw.texture0_sampler_slot=NSNotFound;draw.fragment_common_slot=0;draw.fragment_light_slot=1;
        draw.viewport={0,0,8,8,0,1};draw.scissor={0,0,8,8};PicaMetalDrawViews views{};views.color=color.view;views.depth_stencil=depth.view;views.color_dirty=color.view.authoritative_bytes;
        f.lane.Record(draw,views,color,depth);const auto view_calls=f.view_calls;
        auto produced=f.Decode(base);Require(produced!=first && f.view_calls==view_calls,"Pending native write borrowed poisoned RAM");
        std::vector<u8> expected(256);for(unsigned i=0;i<256;i+=4){expected[i]=68;expected[i+1]=51;expected[i+2]=34;expected[i+3]=17;}
        f.Verify(produced,expected);++cases;
    }
    {
        Fixture f(device,decoder,transfer);f.Source(base,256);auto first=f.Decode(base);
        for(auto mode:{Fixture::ViewMode::Empty,Fixture::ViewMode::Short,Fixture::ViewMode::Long,Fixture::ViewMode::Null,Fixture::ViewMode::Throw}) {
            f.view_mode=mode;const auto reads=f.reader_calls;
            Require(f.Decode(base)==first,"Unavailable view changed cache result");
            Require(f.reader_calls==reads+1,"Unavailable view skipped original copying reader");
        }
        f.view_mode=Fixture::ViewMode::Valid;f.ram.at(base).back()^=0x33;
        auto changed=f.Decode(base);Require(changed!=first,"Valid view failed after fallback");f.Verify(changed,f.ram.at(base));++cases;
    }
    {
        Fixture f(device,decoder,transfer,false);f.Source(base,256);auto first=f.Decode(base);auto reads=f.reader_calls;
        Require(f.Decode(base)==first && f.reader_calls==reads+1 && f.view_calls==0,"Absent view callback did not preserve reader behavior");++cases;
    }
    {
        Fixture f(device,decoder,transfer);const auto original=f.Source(base,256);
        auto event=[device newSharedEvent];auto gate=[f.queue commandBuffer];[gate encodeWaitForEvent:event value:1];[gate commit];
        struct Release {id<MTLSharedEvent> event;~Release(){event.signaledValue=1;}} release{event};
        auto texture=f.Decode(base);f.ram.at(base).assign(256,0xee);
        event.signaledValue=1;f.Verify(texture,original);
        auto changed=f.Decode(base);Require(changed!=texture,"Cache retained borrowed mutable RAM");f.Verify(changed,f.ram.at(base));++cases;
    }
    std::cout << "PASS: borrow=" << borrow << ", keyed=" << keyed << ", large=" << large << ", " << cases
              << " production snapshot GPU cases; FIFO/LRU/key identity/full bytes/native owners/queue lifetime\n";
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
} }
