// SPDX-License-Identifier: GPL-2.0-or-later
// Include the production renderer to instantiate its private NativeRasterizer;
// normal VideoCore installation routes real MemorySystem callbacks to it.
#include "../emulator/src/video_core/renderer_metal/pica_metal_native_renderer.mm"
#include "core/frontend/emu_window.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace {
void Check(bool ok,const char* why) { if(!ok)throw std::runtime_error(why); }
class Window final:public Frontend::EmuWindow {
public: void PollEvents()override{} void MakeCurrent()override{} void DoneCurrent()override{} void SwapBuffers()override{}
};
class Renderer final:public VideoCore::RendererBase {
public: explicit Renderer(Frontend::EmuWindow& w):RendererBase(w){}
VideoCore::ResultStatus Init()override{return VideoCore::ResultStatus::Success;}
void SwapBuffers()override{} bool TryPresent()override{return false;}void ResetPresent()override{}
};
}
int main(int argc,char** argv) { @autoreleasepool { try {
    Check(argc==2,"Pass cara-pica.metallib path");
    const bool partial=std::string_view(std::getenv("CARA_METAL_RELEASE_OWNERLESS_FCRAM_PAGES") ?: "0")=="1";
    const bool enabled=partial || std::string_view(std::getenv("CARA_METAL_RELEASE_INVALIDATED_FCRAM_PAGES") ?: "0")=="1";
    setenv("CARA_METAL_FRAME_COST","1",1);
    setenv("CARA_METAL_BATCH_TEXTURE_COPIES","1",1);
    auto device=MTLCreateSystemDefaultDevice(); NSError* error=nil;
    auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    Check(library!=nil,"Library missing");
    Cara::PicaMetalResources resources{};resources.queue=[device newCommandQueue];
    resources.transfer.fill_words=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cara_pica_fill_words"] error:&error];
    Check(resources.transfer.fill_words!=nil,"Fill pipeline missing");
    Memory::MemorySystem memory;auto table=std::make_shared<Memory::PageTable>();
    memory.RegisterPageTable(table);memory.SetCurrentPageTable(table);
    constexpr auto page=Memory::PAGE_SIZE;constexpr PAddr base=Memory::FCRAM_PADDR+0x10000;
    constexpr VAddr virt=Memory::LINEAR_HEAP_VADDR+0x10000;
    memory.MapMemoryRegion(*table,virt,8*page,memory.GetFCRAMRef(0x10000));
    memory.MapMemoryRegion(*table,Memory::VRAM_VADDR,page,memory.GetPhysicalRef(Memory::VRAM_PADDR));
    Window window;Cara::NativeRasterizer* native=nullptr;
    VideoCore::SetNativeRendererFactory([&](Frontend::EmuWindow& w,Memory::MemorySystem& m){
        auto rasterizer=std::make_unique<Cara::NativeRasterizer>(m,resources);native=rasterizer.get();
        return VideoCore::NativeRendererPair{std::make_unique<Renderer>(w),std::move(rasterizer)};
    });
    Check(VideoCore::Init(window,memory)==VideoCore::ResultStatus::Success,"Native installation failed");
    const auto pointer=[&](unsigned p){return table->GetPointerArray()[(virt+p*page)>>Memory::PAGE_BITS];};
    const auto fill=[&](PAddr address,u32 bytes,u32 word){GPU::Regs::MemoryFillConfig cfg{};cfg.address_start=address/8;cfg.address_end=(address+bytes)/8;cfg.value_32bit=word;cfg.fill_32bit.Assign(1);Check(native->AccelerateFill(cfg),"Fill declined");};
    fill(base,2*page,0xa1b2c3d4);Check(!pointer(0)&&!pointer(1),"GPU writes did not mark pages");
    // Partial overwrites retain the rest of the native owner's page.
    native->InvalidateRegion(base+8,8);Check(!pointer(0),"Partial invalidation released page");
    // Entire first page remains a predecessor guard for the live second page.
    native->InvalidateRegion(base,page);Check(!pointer(0),"Adjacent live-page guard removed");
    // A whole invalidated run can be released together, including that guard.
    native->InvalidateRegion(base,2*page);Check(bool(pointer(0))==enabled&&bool(pointer(1))==enabled,"Whole-page release mode mismatch");
    if(enabled){memory.Write32(virt,0x10203040);Check(memory.Read32(virt)==0x10203040,"Restored CPU direct pointer failed");}
    fill(base,page,0x55667788);Check(!pointer(0),"Future GPU write missed cleared range hint");
    Check(memory.Read32(virt)==0x55667788,"Re-marked native ownership read is stale");
    // Pending GPU producer writes a private buffer. Full overwrite invalidates
    // its ownership before CPU pointer restoration without waiting for the gate.
    auto event=[device newSharedEvent];auto gate=[resources.queue commandBuffer];
    [gate encodeWaitForEvent:event value:1];[gate commit];
    fill(base+3*page,page,0x99887766);Check(!pointer(3),"Pending fill not watched");
    native->InvalidateRegion(base+3*page,page);Check(bool(pointer(3))==enabled,"Pending owner release mismatch");
    if(enabled)memory.Write32(virt+3*page,0xabcdef01);
    event.signaledValue=1;[gate waitUntilCompleted];native->FlushDeferredDraws();
    if(enabled)Check(memory.Read32(virt+3*page)==0xabcdef01,"Old GPU producer overwrote guest CPU RAM");
    fill(Memory::VRAM_PADDR,page,0x13572468);native->InvalidateRegion(Memory::VRAM_PADDR,page);
    Check(!table->GetPointerArray()[Memory::VRAM_VADDR>>Memory::PAGE_BITS],"VRAM watch unexpectedly released");
    fill(base+4*page,2*page,0x12345678);
    native->InvalidateRegion(base+4*page,page);Check(!pointer(4),"Next-page native owner lost guard");
    native->InvalidateRegion(base+5*page,page);
    native->InvalidateRegion(base+4*page+3,1);
    Check(bool(pointer(4))==partial,"Ownerless scalar invalidation release mismatch");
    if(partial){memory.Write8(virt+4*page+3,0x79);Check(memory.Read8(virt+4*page+3)==0x79,"Ownerless scalar direct write failed");}
    // Read-only resident attachments also depend on CPU invalidation watches,
    // even when no native authority remains for this page.
    auto all=Cara::LoadPicaMetalResources(device,[NSURL fileURLWithPath:@(argv[1])],MTLPixelFormatBGRA8Unorm,&error);
    Check(bool(all),"Full resource fixture load failed");
    Cara::PicaMetalNativeDrawLane lane(all->queue,65536,[](PAddr,PAddr){});
    const auto read=[&](PAddr address,std::span<u8> out){auto ref=memory.GetPhysicalRef(address);if(!ref||out.size()>ref.GetSize())return false;std::memcpy(out.data(),ref.GetPtr(),out.size());return true;};
    const PAddr surface=base+6*page+128;
    lane.AcquireTiledColor({surface,surface+1024},16,16,MTLPixelFormatRGBA8Unorm,*all->draw.textures,all->draw.tiled_export,all->draw.detile_packed_color,read);
    lane.InvalidateGuest(base+6*page+1,base+6*page+2);
    Check(!lane.CanReleaseFcramWatchPage(base+6*page),"Resident outside scalar overwrite lost watch");
    Check(!lane.CanReleaseFcramWatchPage(base+5*page),"Next-page resident lost predecessor guard");
    lane.InvalidateGuest(surface,surface+1024);
    Check(lane.CanReleaseFcramWatchPage(base+6*page),"Invalidated resident kept ownerless dependency");
    Check(!lane.CanReleaseFcramWatchPage(Memory::VRAM_PADDR),"Query accepted VRAM");
    Check(!lane.CanReleaseFcramWatchPage(base+1),"Query accepted unaligned page");
    // Construct an actual unsubmitted raster group without resident caches:
    // only pending attachment metadata can protect these FCRAM pages.
    auto authored=[device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal; vertex float4 v(uint i [[vertex_id]]) {float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);} fragment float4 f(){return float4(1,0,0,1);}" options:nil error:&error];
    auto pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[authored newFunctionWithName:@"v"];pd.fragmentFunction=[authored newFunctionWithName:@"f"];
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    pd.depthAttachmentPixelFormat=pd.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
    auto pipeline=[device newRenderPipelineStateWithDescriptor:pd error:&error];Check(pipeline!=nil,"Pending fixture pipeline failed");
    auto makeTexture=[&](MTLPixelFormat format){auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:16 height:16 mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;return [device newTextureWithDescriptor:d];};
    auto colorTexture=makeTexture(MTLPixelFormatRGBA8Unorm),depthTexture=makeTexture(MTLPixelFormatDepth32Float_Stencil8);
    const auto exporter=[](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8>){throw std::runtime_error("Unexpected pending fixture export");return false;};
    Cara::PicaMetalNativeAttachment color{{colorTexture,{base+128,base+1152},991},exporter,{}};
    Cara::PicaMetalNativeAttachment depth{{depthTexture,{base+2*page+128,base+2*page+1152},992},exporter,{}};
    std::array<Cara::PicaMetalStageVertex,3> vertices{};std::array<std::byte,288> common{};std::array<std::byte,1040> light{};
    Cara::TexturedMetalDraw draw{};draw.pipeline=pipeline;draw.depth_stencil_state=[device newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
    draw.texel_fetch_sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    draw.color=colorTexture;draw.depth_stencil=depthTexture;draw.vertex_mode=Cara::PicaMetalVertexMode::StageIn;draw.vertex_count=3;
    draw.common_uniform=common;draw.light_uniform=light;draw.packed_vertices=std::as_bytes(std::span(vertices));
    draw.lut_texture_slot=draw.lut_sampler_slot=draw.texture0_slot=draw.texture0_sampler_slot=NSNotFound;
    draw.fragment_common_slot=0;draw.fragment_light_slot=1;draw.viewport={0,0,16,16,0,1};draw.scissor={0,0,16,16};
    Cara::PicaMetalDrawViews views{};views.color=color.view;views.depth_stencil=depth.view;views.color_dirty=color.view.authoritative_bytes;
    lane.Record(draw,views,color,depth);
    Check(!lane.CanReleaseFcramWatchPage(base),"Unsubmitted color attachment lost watch");
    Check(!lane.CanReleaseFcramWatchPage(base+2*page),"Unsubmitted depth attachment lost watch");
    Check(!lane.CanReleaseFcramWatchPage(base+page),"Unsubmitted next-page attachment lost predecessor guard");
    lane.InvalidateGuest(base,base+3*page);
    Check(lane.CanReleaseFcramWatchPage(base),"Published and invalidated pending group kept stale watch");
    Memory::RasterizerClearAll(true);for(unsigned i=0;i<8;++i)Check(pointer(i)!=nullptr,"Global clear left FCRAM watch");
    Check(table->GetPointerArray()[Memory::VRAM_VADDR>>Memory::PAGE_BITS]!=nullptr,"Global clear left VRAM watch");
    std::ostringstream profile;Cara::AppendNativeMetalWatchProfile(profile,true);
    std::cout<<"PASS: actual MemorySystem/native ownership, partial/adjacent/pending/global cases; release="<<enabled<<profile.str()<<'\n';
    VideoCore::Shutdown();VideoCore::SetNativeRendererFactory({});
    Cara::RegisterNativeMetalTextureCopyBatch(lane);
    lane.CopyTexture({Memory::VRAM_PADDR+0x4000,Memory::VRAM_PADDR+0x8000,64,0,0,0,0},all->transfer,read);
    Check(!lane.CanReleaseFcramWatchPage(base),"Deferred copy batch did not conservatively retain watch");
    lane.PublishPending();
    Check(lane.CanReleaseFcramWatchPage(base),"Published unrelated copy retained FCRAM watch");
    lane.Drain();Cara::UnregisterNativeMetalTextureCopyBatch(lane);

    return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;} } }
