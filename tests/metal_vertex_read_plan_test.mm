// SPDX-License-Identifier: GPL-2.0-or-later
// Extend the existing production capture oracle without copying its fixtures.
#define main OriginalRawCaptureSuite
#include "metal_raw_index_capture_test.mm"
#undef main
#include "common/logging/log.h"
#include "video_core/renderer_metal/pica_metal_upload.h"
#import <Metal/Metal.h>
namespace {
unsigned loader_errors=0;
struct GpuCaptureOracle {
    id<MTLDevice> device=nil;id<MTLCommandQueue> queue=nil;id<MTLCommandBuffer> command=nil;
    std::unique_ptr<Cara::PicaMetalUploadArena> arena;
    struct Snapshot { id<MTLTexture> texture;std::vector<u8> bytes; };
    std::vector<Snapshot> snapshots;
    explicit GpuCaptureOracle(bool enabled) {
        if(!enabled) return;
        device=MTLCreateSystemDefaultDevice();Require(device!=nil,"GPU capture oracle requires Metal");
        queue=[device newCommandQueue];command=[queue commandBuffer];
        arena=std::make_unique<Cara::PicaMetalUploadArena>(device,1024*1024);
        Require(arena->Begin(command),"GPU capture upload lease failed");
    }
    void Observe(const Cara::PicaMetalPackedInputs& captured) {
        if(!arena) return;
        const auto bytes=std::as_bytes(std::span(captured.vertices));
        const auto texture=arena->Copy2D(bytes,MTLPixelFormatRGBA32Uint,128,bytes.size()/2048,2048);
        Require(texture!=nil,"Production captured-payload texture upload failed");
        snapshots.push_back({texture,captured.vertices});
    }
    void Finish() {
        if(!arena) return;
        std::size_t total=0;for(const auto& s:snapshots) total+=s.bytes.size();
        const auto buffer=[device newBufferWithLength:total options:MTLResourceStorageModeShared];
        const auto blit=[command blitCommandEncoder];Require(buffer && blit,"GPU capture readback resources failed");
        std::size_t offset=0;
        for(const auto& s:snapshots) {
            [blit copyFromTexture:s.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                       sourceSize:MTLSizeMake(128,s.texture.height,1) toBuffer:buffer destinationOffset:offset
                       destinationBytesPerRow:2048 destinationBytesPerImage:s.bytes.size()];
            offset+=s.bytes.size();
        }
        [blit endEncoding];Require(arena->Submit(),"GPU capture submit failed");[command waitUntilCompleted];
        Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"GPU capture command failed");
        offset=0;const auto* bytes=static_cast<const u8*>(buffer.contents);
        for(const auto& s:snapshots) {
            Require(!std::memcmp(bytes+offset,s.bytes.data(),s.bytes.size()),"GPU consumed stale captured payload/default bytes");
            offset+=s.bytes.size();
        }
        std::cerr<<"gpu_captured_snapshots="<<snapshots.size()<<" exact_gpu_bytes="<<total<<'\n';
    }
};
}
namespace Log {
void FmtLogMessageImpl(Class,Level,const char*,unsigned,const char*,const char* format,
                      const fmt::format_args&) {
    if(std::string_view(format).find("Overflow in the vertex attribute loader")!=std::string_view::npos)
        ++loader_errors;
}
}
int main(int argc,char**) { @autoreleasepool { try {
    Require(OriginalRawCaptureSuite()==0,"Original raw capture oracle failed");
    const auto* reuse_env=std::getenv("CARA_METAL_REUSE_VERTEX_READ_PLAN");
    const bool reuse=reuse_env && std::string_view(reuse_env)=="1";
    const auto* wide_env=std::getenv("CARA_METAL_VERTEX_READ_PLAN_CACHE_128");
    const bool wide=wide_env && std::string_view(wide_env)=="1";
    const auto* relative_env=std::getenv("CARA_METAL_VERTEX_READ_PLAN_RELATIVE_BASE");
    const bool relative=reuse && wide && relative_env && std::string_view(relative_env)=="1";
    setenv("CARA_METAL_DRAW_INPUT_REUSE","1",1);
    GpuCaptureOracle gpu(argc>1);
    Fixture f;
    for(auto& attribute:f.state->input_default_attributes.attr)
        attribute={Pica::float24::FromFloat32(0),Pica::float24::FromFloat32(0),
                   Pica::float24::FromFloat32(0),Pica::float24::FromFloat32(1)};
    auto& pipe=f.state->regs.pipeline;auto& vs=f.state->regs.vs;
    pipe.num_vertices=6;pipe.vertex_offset=0;
    pipe.vertex_attributes.attribute_mask.Assign(0xffe);
    const auto original_pipe=pipe;const auto original_vs=vs;
    auto emit=[&](u16 mask=1,bool indexed=false) {
        auto value=Cara::CaptureMetalRawIndices(*f.state,indexed,mask,
            [&](PAddr address,std::span<u8> bytes) { return f.Read(address,bytes); });
        Require(value.has_value(),"Read-plan mutation rejected valid capture");
        gpu.Observe(*value);
        HashObject(value->layout);HashObject(value->vertex_count);HashObject(value->unique_dense_vertices);
        Hash(value->vertices);Hash({reinterpret_cast<const u8*>(value->indices.data()),value->indices.size()*2});
        ++cases;return *value;
    };
    const auto hits=Cara::raw_read_plan_hits,misses=Cara::raw_read_plan_misses;
    Require(VideoCore::GetCurrentFrame()==0,"Fixture should be outside historical720..840 diagnostic window");
    for(unsigned field=0;field<9;++field) {
        pipe=original_pipe;vs=original_vs;
        pipe.vertex_attributes.attribute_loaders[0].data_offset.Assign(attributes+field*0x100);
        emit();emit();
        const auto before=Cara::raw_read_plan_misses;
        switch(field) {
        case 0:pipe.vertex_attributes.attribute_loaders[0].data_offset.Assign(attributes+field*0x100+32);break;
        case 1:vs.input_attribute_to_register_map_low=1;break;
        case 2:vs.max_input_attribute_index.Assign(8);vs.input_attribute_to_register_map_low=0x76543210;
               vs.input_attribute_to_register_map_high=0;emit();
               vs.input_attribute_to_register_map_high=1;break;
        case 3:vs.max_input_attribute_index.Assign(1);break;
        case 4:pipe.vertex_offset=1;break;
        case 5:pipe.num_vertices=9;break;
        case 6:pipe.vertex_attributes.base_address.Assign((base+16)/16);break;
        case 7:vs.max_input_attribute_index.Assign(1);vs.input_attribute_to_register_map_low=0x10;emit(1);
               (void)emit(3);Require(!reuse || Cara::raw_read_plan_misses>before,"Mask key mutation incorrectly hit");continue;
        case 8: {
            pipe.num_vertices=6;pipe.index_array.format.Assign(decltype(pipe.index_array)::BYTE);
            const std::array<u8,6> dense{0,1,2,3,4,5},sparse{0,1,2,3,0,5};
            std::copy(dense.begin(),dense.end(),f.memory.begin()+0x100);emit(1,true);
            const auto sparse_before=Cara::raw_read_plan_misses;
            std::copy(sparse.begin(),sparse.end(),f.memory.begin()+0x100);
            auto rejected=Cara::CaptureMetalRawIndices(*f.state,true,1,
                [&](PAddr address,std::span<u8> bytes) { return f.Read(address,bytes); });
            Require(!rejected,"Same first/span with fewer unique indices bypassed sparse budget");
            Require(!reuse || Cara::raw_read_plan_misses>sparse_before,"Unique-count mutation incorrectly reused plan");
            HashObject(u32{0xdeadbeef});++cases;continue;
        }
        }
        emit();Require(!reuse || (relative && field==6) || Cara::raw_read_plan_misses>before,"Explicit plan key mutation incorrectly hit");
    }
    pipe=original_pipe;vs=original_vs;
    auto first=emit();f.memory[attributes+23]^=0x63;auto live=emit();
    Require(first.vertices!=live.vertices,"Cached plan retained old guest attribute bytes");
    pipe.vertex_attributes.attribute_mask.Assign(0xfff);
    pipe.vertex_attributes.attribute_loaders[0].component_count.Assign(0);
    f.state->input_default_attributes.attr[0].x=Pica::float24::FromFloat32(1.25f);
    first=emit();f.state->input_default_attributes.attr[0].x=Pica::float24::FromFloat32(-2.5f);live=emit();
    Require(first.vertices!=live.vertices,"Cached plan retained old default attributes");
    // Invalid loader diagnostics must still run on EVERY draw; such plans are
    // explicitly excluded from admission even if capture itself can continue.
    pipe=original_pipe;vs=original_vs;
    pipe.vertex_attributes.attribute_loaders[0].component_count.Assign(13);
    const auto errors=loader_errors;
    for(unsigned i=0;i<3;++i) {
        (void)Cara::CaptureMetalRawIndices(*f.state,false,1,
            [&](PAddr address,std::span<u8> bytes) { return f.Read(address,bytes); });
        Require(loader_errors==errors+i+1,"Invalid loader diagnostic suppressed by cached plan");
    }
    // Uniform base translation must preserve the plan while reading CURRENT
    // indices and attribute bytes at every base. Index order changes too.
    pipe=original_pipe;vs=original_vs;
    pipe.vertex_attributes.attribute_loaders[0].data_offset.Assign(0x50000);
    std::vector<u16> preceding_indices;
    for(unsigned i=0;i<16;++i) {
        const auto address=base+i*0x1000;
        pipe.vertex_attributes.base_address.Assign(address/16);
        for(unsigned j=0;j<6;++j) f.memory[i*0x1000+0x100+j]=i%2 ? 5-j : j;
        const auto before_hits=Cara::raw_read_plan_hits;
        auto value=emit(1,true);
        if(relative && i) Require(Cara::raw_read_plan_hits==before_hits+1,"Base translation failed exact relative-plan reuse");
        const auto bytes=6*16;
        Require(!std::memcmp(value.vertices.data()+value.layout[0][0],
                             f.memory.data()+i*0x1000+0x50000,bytes),
                "Base-relative plan read old physical guest bytes");
        if(i) Require(value.indices!=preceding_indices,"Cached plan retained index data across base translation");
        preceding_indices=value.indices;
    }
    // The base-address field is bits1..28; reserved bits must remain part of
    // exact key equality even though GetPhysicalBaseAddress ignores them.
    pipe=original_pipe;vs=original_vs;
    pipe.vertex_attributes.attribute_loaders[0].data_offset.Assign(0x55000);emit();
    for(unsigned bit:{0u,29u,30u,31u}) {
        u32 word;std::memcpy(&word,&pipe.vertex_attributes,sizeof(word));word^=u32{1}<<bit;
        std::memcpy(&pipe.vertex_attributes,&word,sizeof(word));
        const auto before_misses=Cara::raw_read_plan_misses;emit();
        Require(!reuse || Cara::raw_read_plan_misses==before_misses+1,"Reserved base-register bit was removed from key");
    }
    // A cached two-range plan has a valid first range but overflowing later
    // range at the new base. No attribute reader may run before ALL checks pass.
    pipe=original_pipe;vs=original_vs;
    pipe.vertex_attributes.max_attribute_index.Assign(1);
    pipe.vertex_attributes.format1.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
    pipe.vertex_attributes.size1.Assign(3);
    pipe.vertex_attributes.attribute_loaders[0].data_offset.Assign(0x1000);
    auto& second=pipe.vertex_attributes.attribute_loaders[1];
    second.data_offset.Assign(0x3000);second.comp0.Assign(1);
    second.component_count.Assign(1);second.byte_count.Assign(16);
    vs.max_input_attribute_index.Assign(1);vs.input_attribute_to_register_map_low=0x10;
    emit(3);emit(3);
    pipe.vertex_attributes.base_address.Assign(0xffffd000u/16);
    auto before_reads=f.reads;
    auto invalid=Cara::CaptureMetalRawIndices(*f.state,false,3,
        [&](PAddr address,std::span<u8> bytes) { return f.Read(address,bytes); });
    Require(!invalid && f.reads==before_reads,"Later rebased overflow allowed an earlier payload callback");
    HashObject(u32{0xbad00001});++cases;
    // Empty/default-only ranges remain valid even at the highest legal base.
    pipe=original_pipe;vs=original_vs;
    pipe.vertex_attributes.attribute_mask.Assign(0xfff);
    pipe.vertex_attributes.attribute_loaders[0].component_count.Assign(0);
    pipe.vertex_attributes.base_address.Assign(0xfffffff0u/16);
    before_reads=f.reads;emit();Require(f.reads==before_reads,"Default-only plan read guest payload");
    invalid=Cara::CaptureMetalRawIndices(*f.state,true,1,
        [&](PAddr address,std::span<u8> bytes) { return f.Read(address,bytes); });
    Require(!invalid && f.reads==before_reads,"Overflowing current index address reached callback");
    HashObject(u32{0xbad00002});++cases;
    // Reentrant readers can replace the cache node. A local plan copy must
    // keep the second outer range intact after thousands of nested captures.
    pipe=original_pipe;vs=original_vs;
    pipe.vertex_attributes.max_attribute_index.Assign(1);
    pipe.vertex_attributes.format1.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
    pipe.vertex_attributes.size1.Assign(3);
    pipe.vertex_attributes.attribute_loaders[0].data_offset.Assign(0x51000);
    second.data_offset.Assign(0x61000);second.comp0.Assign(1);second.component_count.Assign(1);second.byte_count.Assign(16);
    vs.max_input_attribute_index.Assign(1);vs.input_attribute_to_register_map_low=0x10;
    auto outer_reference=emit(3);
    Fixture nested;nested.state->regs.pipeline.num_vertices=6;
    nested.state->regs.vs.max_input_attribute_index.Assign(1);
    nested.state->regs.vs.input_attribute_to_register_map_low=0x10;
    bool reentered=false;
    auto outer=Cara::CaptureMetalRawIndices(*f.state,false,3,[&](PAddr address,std::span<u8> bytes) {
        if(!reentered) {
            reentered=true;
            for(unsigned i=0;i<4096;++i) {
                nested.state->regs.pipeline.vertex_attributes.attribute_loaders[0].data_offset.Assign(0x80000+i*32);
                auto inner=Cara::CaptureMetalRawIndices(*nested.state,false,3,
                    [&](PAddr a,std::span<u8> out) { return nested.Read(a,out); });
                Require(inner.has_value(),"Reentrant cache pressure capture failed");
            }
        }
        return f.Read(address,bytes);
    });
    Require(outer && reentered && outer->layout==outer_reference.layout && outer->vertices==outer_reference.vertices,
            "Reentrant cache eviction changed outer range/layout payload");
    const auto evicted=Cara::raw_read_plan_misses;
    emit(3);Require(!reuse || Cara::raw_read_plan_misses==evicted+1,"Reentrant pressure failed to evict outer cache node");
    // All64 layouts collide in the original hash: only their loader offset
    // differs. Repeating them demonstrates the full-field hash has a purpose.
    pipe=original_pipe;vs=original_vs;
    const auto collision_hits=Cara::raw_read_plan_hits;
    for(unsigned round=0;round<2;++round) for(unsigned i=0;i<64;++i) {
        pipe.vertex_attributes.attribute_loaders[0].data_offset.Assign(0x10000+i*64);emit();
    }
    if(reuse && wide) Require(Cara::raw_read_plan_hits>collision_hits,"Full-field hash did not recover any old-hash-colliding layout");
    gpu.Finish();
    if(reuse) Require(Cara::raw_read_plan_hits>hits && Cara::raw_read_plan_misses>misses,
                      "Outside-window read-plan diagnostic counters stayed zero");
    std::cout<<"read_plan_cases="<<cases<<" digest="<<std::hex<<digest<<std::dec<<'\n';
    std::cerr<<"plan_reuse="<<reuse<<" wide="<<wide<<" relative="<<relative<<" plan_hits="<<Cara::raw_read_plan_hits-hits
             <<" plan_misses="<<Cara::raw_read_plan_misses-misses<<" loader_errors="<<loader_errors<<'\n';
    return 0;
} catch(const std::exception& failure) { std::cerr<<failure.what()<<'\n';return 1; } } }
