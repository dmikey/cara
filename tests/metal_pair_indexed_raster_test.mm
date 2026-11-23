// SPDX-License-Identifier: GPL-2.0-or-later
// Production indexed encoder versus expanded corner order. Authored shaders/data only.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_draw.h"
#include "video_core/renderer_metal/pica_metal_pair_indices.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
namespace Cara {
bool EncodeGroupedMetalDraw(id<MTLCommandBuffer>,PicaMetalUploadArena&,const TexturedMetalDraw&);
bool CloseGroupedMetalDraw(id<MTLCommandBuffer>);
}
namespace {
void Require(bool v,const char* reason) { if(!v) throw std::runtime_error(reason); }
constexpr char shader[]=R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 p [[position]]; float4 q; float4 color; };
Out build(uint s,uint first,texture2d<uint,access::read> vertices) {
    uint4 a=vertices.read(uint2(s,0)),b=vertices.read(uint2(first,0));
    float q=as_type<float>(a.z),q0=as_type<float>(b.z);
    if(q*q0<0) q=-q;
    return {float4(as_type<float>(a.x),as_type<float>(a.y),0,1),float4(q,q*.5,q*.25,1),
            float4(float((a.w>>16)&255)/255,float((a.w>>8)&255)/255,float(a.w&255)/255,.5)};
}
vertex Out expanded(uint id [[vertex_id]],texture2d<uint,access::read> vertices [[texture(0)]],
                    texture2d<uint,access::read> indices [[texture(1)]]) {
    uint s=indices.read(uint2(id%128,id/128)).x;
    uint n=(id/3)*3;
    uint first=indices.read(uint2(n%128,n/128)).x;
    return build(s,first,vertices);
}
vertex Out paired(uint id [[vertex_id]],texture2d<uint,access::read> vertices [[texture(0)]],
                  texture2d<uint,access::read> pairs [[texture(1)]]) {
    uint2 p=pairs.read(uint2(id%128,id/128)).xy;
    return build(p.x,p.y,vertices);
}
fragment float4 pixel(Out in [[stage_in]]) {
    return float4(in.color.rgb*(.65+in.q.x*.25),in.color.a);
}
)";
using Pixels=std::array<std::uint8_t,32*32*4>;
struct Fixture {
    id<MTLDevice> device;id<MTLCommandQueue> queue;id<MTLRenderPipelineState> baseline,paired;
    id<MTLDepthStencilState> depth_state;id<MTLSamplerState> sampler;
    std::array<std::byte,2048> packed{};
    Fixture(id<MTLDevice> d):device(d),queue([d newCommandQueue]) {
        NSError* error=nil;
        auto library=[device newLibraryWithSource:[NSString stringWithUTF8String:shader] options:nil error:&error];
        Require(library!=nil,"Authored pair raster shader compile failed");
        const auto pipeline=[&](NSString* vertex) {
            auto p=[MTLRenderPipelineDescriptor new];p.vertexFunction=[library newFunctionWithName:vertex];
            p.fragmentFunction=[library newFunctionWithName:@"pixel"];
            auto color=p.colorAttachments[0];color.pixelFormat=MTLPixelFormatRGBA8Unorm;
            color.blendingEnabled=YES;color.sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
            color.destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
            color.sourceAlphaBlendFactor=MTLBlendFactorOne;color.destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
            p.depthAttachmentPixelFormat=p.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
            return [device newRenderPipelineStateWithDescriptor:p error:&error];
        };
        baseline=pipeline(@"expanded");paired=pipeline(@"paired");
        depth_state=[device newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        Require(baseline && paired && depth_state && sampler,"Pair raster pipeline unavailable");
        constexpr float positions[6][2]{{-.9f,-.8f},{.8f,-.8f},{-.1f,.9f},{.9f,.7f},{-.8f,.4f},{.8f,.4f}};
        for(unsigned i=0;i<6;++i) {
            const std::array<std::uint32_t,4> a{std::bit_cast<std::uint32_t>(positions[i][0]),
                std::bit_cast<std::uint32_t>(positions[i][1]),std::bit_cast<std::uint32_t>(i%2 ? -1.f : 1.f),
                (unsigned(31+i*37)<<16)|(unsigned(211-i*29)<<8)|unsigned(63+i*23)};
            std::memcpy(packed.data()+i*16,a.data(),16);
        }
    }
    Pixels Render(const std::vector<std::uint16_t>& corners,bool use_pair,bool grouped) {
        auto descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
        descriptor.storageMode=MTLStorageModeShared;descriptor.usage=MTLTextureUsageRenderTarget;
        auto color=[device newTextureWithDescriptor:descriptor];Pixels zeros{};
        [color replaceRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0 withBytes:zeros.data() bytesPerRow:128];
        descriptor.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;descriptor.storageMode=MTLStorageModePrivate;
        auto depth=[device newTextureWithDescriptor:descriptor];
        std::array<std::byte,288> common{};std::array<std::byte,1040> light{};
        std::array<std::byte,1856> vs{};std::array<std::byte,256> layout{};
        std::vector<std::uint16_t> indices(128);std::copy(corners.begin(),corners.end(),indices.begin());
        auto owner=std::make_shared<Cara::MetalPairIndexPayload>();
        std::map<std::pair<unsigned,unsigned>,std::uint16_t> identities;
        owner->pairs.resize(128*4);
        for(unsigned i=0;i<corners.size();++i) {
            const auto key=std::pair<unsigned,unsigned>{corners[i],corners[(i/3)*3]};
            const auto [it,inserted]=identities.emplace(key,identities.size());
            if(inserted) { owner->pairs[it->second*4]=key.first;owner->pairs[it->second*4+1]=key.second; }
            owner->elements.push_back(it->second);
        }
        owner->pair_count=identities.size();owner->admitted=true;
        auto tex=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint width:128 height:1 mipmapped:NO];
        tex.storageMode=MTLStorageModeShared;tex.usage=MTLTextureUsageShaderRead;
        owner->pair_texture=[device newTextureWithDescriptor:tex];
        [owner->pair_texture replaceRegion:MTLRegionMake2D(0,0,128,1) mipmapLevel:0 withBytes:owner->pairs.data() bytesPerRow:2048];
        owner->element_buffer=[device newBufferWithBytes:owner->elements.data() length:owner->elements.size()*2 options:MTLResourceStorageModeShared];
        Cara::TexturedMetalDraw draw{};
        draw.pipeline=use_pair ? paired : baseline;draw.depth_stencil_state=depth_state;draw.texel_fetch_sampler=sampler;
        draw.color=color;draw.depth_stencil=depth;draw.vertex_mode=use_pair ? Cara::PicaMetalVertexMode::PairIndexed : Cara::PicaMetalVertexMode::Packed;
        draw.vertex_count=corners.size();draw.packed_vertices=packed;draw.expanded_indices=std::as_bytes(std::span(indices));
        draw.pair_indices=use_pair ? owner : nullptr;draw.common_uniform=common;draw.light_uniform=light;
        draw.vs_uniform=vs;draw.layout_uniform=layout;draw.viewport={0,0,32,32,0,1};draw.scissor={0,0,32,32};
        draw.lut_texture_slot=draw.lut_sampler_slot=draw.texture0_slot=draw.texture0_sampler_slot=NSNotFound;
        draw.fragment_common_slot=0;draw.fragment_light_slot=1;
        Cara::PicaMetalUploadArena arena(device,256*1024);const auto command=[queue commandBuffer];
        Require(command.retainedReferences && arena.Begin(command),"Pair lease unavailable");
        Require(grouped ? Cara::EncodeGroupedMetalDraw(command,arena,draw) : Cara::EncodeTexturedMetalDraw(command,arena,draw),"Production pair draw rejected");
        if(grouped) Require(Cara::CloseGroupedMetalDraw(command),"Pair pass close failed");
        // Production Metal references must retain resources after the CPU memo
        // owner and the caller's draw payload are released before completion.
        draw.pair_indices.reset();owner.reset();
        Require(arena.Submit(),"Pair command submit failed");[command waitUntilCompleted];
        Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"Pair GPU command failed");
        Pixels result{};[color getBytes:result.data() bytesPerRow:128 fromRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0];
        return result;
    }
};
}
int main(int argc,char** argv) { @autoreleasepool { try {
    setenv("CARA_METAL_GROUPED_PASS","1",1);
    const auto device=MTLCreateSystemDefaultDevice();Require(device!=nil,"No Metal GPU");Fixture fixture(device);
    const std::vector<std::uint16_t> original{0,1,2,3,1,0,0,2,1,4,5,0};
    auto reverse=original;std::rotate(reverse.begin(),reverse.begin()+3,reverse.end());
    auto winding=original;for(unsigned i=0;i<winding.size();i+=3) std::swap(winding[i+1],winding[i+2]);
    std::vector<Pixels> results;
    for(const auto& corners:{original,reverse,winding}) for(bool grouped:{false,true}) {
        auto baseline=fixture.Render(corners,false,grouped),pair=fixture.Render(corners,true,grouped);
        Require(pair==baseline,"Pair-indexed production pixels differ from full corner order");results.push_back(pair);
    }
    Require(results[0]!=results[2],"Order fixture failed to distinguish overlapping blend order");
    std::set<std::array<std::uint8_t,4>> colors;
    for(unsigned i=0;i<results[0].size();i+=4) colors.insert({results[0][i],results[0][i+1],results[0][i+2],results[0][i+3]});
    Require(colors.size()>16,"Pair oracle raster output is degenerate");
    if(argc==2) { std::ofstream out(argv[1],std::ios::binary);for(const auto& p:results) out.write((const char*)p.data(),p.size());Require(bool(out),"Pair pixel receipt failed"); }
    std::cout<<"PASS6 exact production pair-indexed raster comparisons: duplicate source/different first quaternion, ordering, winding, grouping, owner release\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } } }
