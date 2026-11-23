// SPDX-License-Identifier: GPL-2.0-or-later
// Authored topology/mutation/immutable lease fixtures. No commercial data.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_pair_indices.h"
#include "video_core/renderer_metal/pica_metal_raw_index_format.hh"
#include "video_core/pica_state.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
namespace {
using Topology=Pica::PipelineRegs::TriangleTopology;
void Require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Fixture {
    std::unique_ptr<Pica::State> state=std::make_unique<Pica::State>();
    Cara::PicaMetalPackedInputs captured;
    std::vector<u32> source;
    unsigned width=2;
    Fixture() { state->Reset(); state->regs.lighting.disable.Assign(0); }
    void Set(Topology topology,unsigned index_width,unsigned count=96) {
        width=index_width; source.resize(count);
        for(unsigned i=0;i<count;++i) source[i]=index_width ? 10+i%8 : 10+i;
        auto& pipe=state->regs.pipeline;
        pipe.vertex_attributes.base_address.Assign(0x20000000/16);
        pipe.index_array.offset.Assign(256);
        pipe.index_array.format.Assign(index_width==2 ? decltype(pipe.index_array)::SHORT : decltype(pipe.index_array)::BYTE);
        pipe.vertex_offset=10; pipe.num_vertices=count;
        state->primitive_assembler.Reconfigure(topology);
        captured={}; captured.layout[0]={256,16,3,4};
        captured.vertex_count=(topology==Topology::Strip||topology==Topology::Fan) ? (count-2)*3 : count/3*3;
        Rewrite();
    }
    void Rewrite() {
        captured.indices.assign((2+source.size()*width/2+255)/256*256,0);
        captured.indices[0]=u16(state->primitive_assembler.GetTopology())|u16(width<<2);
        captured.indices[1]=width ? *std::min_element(source.begin(),source.end()) : 0;
        auto* raw=reinterpret_cast<u8*>(captured.indices.data()+2);
        for(unsigned i=0;i<source.size()&&width;++i) {
            raw[i*width]=u8(source[i]); if(width==2) raw[i*width+1]=u8(source[i]>>8);
        }
    }
    auto Capture(id<MTLDevice> device) {
        return Cara::CaptureMetalPairIndices(device,*state,width!=0,1,captured);
    }
    void Check(const std::shared_ptr<const Cara::MetalPairIndexPayload>& payload) {
        Require(bool(payload)&&payload->admitted,"expected admitted topology");
        Require(payload->elements.size()==captured.vertex_count,"wrong hardware element count");
        const auto topology=state->primitive_assembler.GetTopology();
        const auto first=width ? captured.indices[1] : source.front();
        auto slot=[&](unsigned corner) {
            const unsigned tri=corner/3, lane=corner%3;
            if(topology==Topology::Strip) return tri+(lane==2 ? 2 : lane^(tri&1));
            if(topology==Topology::Fan) return lane==0 ? 0 : tri+lane;
            return corner;
        };
        for(unsigned corner=0;corner<captured.vertex_count;++corner) {
            const auto pair=payload->elements[corner];
            Require(pair<payload->pair_count,"out of bounds pair ID");
            Require(payload->pairs[pair*4]==source[slot(corner)]-first,"changed source ID");
            Require(payload->pairs[pair*4+1]==source[slot(corner-corner%3)]-first,"changed original triangle-first ID");
        }
        Require(std::memcmp(payload->element_buffer.contents,payload->elements.data(),payload->elements.size()*2)==0,
                "immutable hardware index buffer changed");
        std::vector<u32> texels(payload->pairs.size());
        [payload->pair_texture getBytes:texels.data() bytesPerRow:128*4*sizeof(u32)
            fromRegion:MTLRegionMake2D(0,0,128,payload->pair_texture.height) mipmapLevel:0];
        Require(texels==payload->pairs,"immutable pair texture changed");
        Require(u64(payload->predicted_runs)*4<=u64(payload->baseline_runs)*3,"unprofitable admission");
    }
};
}
int main(int argc,char**) { @autoreleasepool { try {
    const bool disabled=argc>1;
    setenv("CARA_METAL_PAIR_INDEXED_VERTICES",disabled?"0":"1",1);
    setenv("CARA_METAL_RAW_INDICES","1",1);
    auto device=MTLCreateSystemDefaultDevice(); Require(bool(device),"Metal device missing");
    Fixture f; unsigned checks=0;
    if(disabled) {
        f.Set(Topology::List,2); Require(!f.Capture(device),"disabled path admitted");
        std::cout<<"PASS: pair opt-in disabled control\n"; return 0;
    }
    for(auto topology:{Topology::List,Topology::Strip,Topology::Fan,Topology::Shader})
    for(unsigned width:{1u,2u}) {
        f.Set(topology,width); auto original=f.Capture(device); f.Check(original); ++checks;
        Require(f.Capture(device)==original,"full-byte memo hit missed");
        f.source[48]=17; f.Rewrite(); auto middle=f.Capture(device); f.Check(middle); ++checks;
        Require(middle!=original,"interior mutation reused old owner");
        f.source.back()=16; f.Rewrite(); auto tail=f.Capture(device); f.Check(tail); ++checks;
        Require(tail!=middle,"tail mutation reused old owner");
        f.captured.layout[0][0]+=4; auto layout=f.Capture(device); f.Check(layout); ++checks;
        Require(layout!=tail,"layout mutation reused old owner");
        f.captured.layout[0][3]|=(8<<3)|(65000<<7);
        Require(f.Capture(device)==layout,"canonical layout did not preserve live header independence");
        ++f.captured.indices[1];
        Require(!f.Capture(device),"changed rebase metadata reused an old pair owner");
        --f.captured.indices[1];
        f.captured.indices[0]^=1;
        Require(!f.Capture(device),"mismatched raw topology metadata accepted");
        f.captured.indices[0]^=1;
        Require(std::memcmp(original->element_buffer.contents,original->elements.data(),original->elements.size()*2)==0,
                "mutation overwrote retained hardware buffer");
    }
    f.Set(Topology::List,2); auto retained=f.Capture(device); f.Check(retained);
    for(unsigned i=0;i<180;++i) {
        f.captured.layout[15][0]=1000+i; auto next=f.Capture(device); f.Check(next); ++checks;
    }
    Require(std::memcmp(retained->element_buffer.contents,retained->elements.data(),retained->elements.size()*2)==0,
            "eviction invalidated an outstanding immutable owner");
    f.Check(retained);
    f.Set(Topology::List,2); f.state->regs.lighting.disable.Assign(1);
    Require(!f.Capture(device),"unlit path did not fall back");
    f.state->regs.lighting.disable.Assign(0); f.captured.vertex_count=65538;
    Require(!f.Capture(device),"UInt16 corner bound accepted overflow");
    f.Set(Topology::List,2,65535); for(unsigned i=0;i<f.source.size();++i) f.source[i]=i;
    f.Rewrite(); Require(!f.Capture(device),"unprofitable unique topology admitted");
    Require(!f.Capture(device),"negative exact-payload memo did not fall back");
    f.Set(Topology::List,0); Require(!f.Capture(device),"unprofitable non-indexed topology admitted");
    std::ostringstream report; Cara::ReportMetalPairIndices(report); std::cout<<report.str();
    std::cout<<"PASS: "<<checks<<" pair topology/mutation/eviction owner checks; list/strip/fan/Shader8/16bit, conservative fallbacks\n";
    return 0;
} catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; return 1; } } }
