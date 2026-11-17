// Diagnostic-only immutable draw-local input snapshot. No memory-system API.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include "cara/quad_uv_plan.h"
namespace Cara::QuadInputCapture {
enum class Reason : std::uint8_t { None,NoBinding,NoExactDraw,WrongDraw,SoftwarePath,
    Mode,Shape,AlternateReuse,Program,Mapping,UndefinedSource,DefaultSource,
    Layout,BufferBounds,Triangles,Overflow,ScopeMismatch,PlanChanged };
struct Key {std::uint64_t generation=0,submission=0,draw=0;};
inline bool Equal(Key a,Key b){return a.generation==b.generation&&a.submission==b.submission&&a.draw==b.draw;}
struct Witness {
    bool structural=false,ended=false;std::uint32_t scanned=0,moves=0,arithmetic=0,nops=0;
    std::uint64_t program=0,descriptors=0;std::array<QuadUVPlan::Lane,6> lanes{};
};
inline Witness FromPlan(const QuadUVPlan::Plan& p){
    Witness w;w.structural=p.structural;w.ended=p.ended;w.scanned=p.scanned;w.moves=p.moves;w.arithmetic=p.arithmetic;w.nops=p.nops;
    w.program=p.scanned_program_identity;w.descriptors=p.scanned_descriptor_identity;
    for(unsigned i=0;i<6;++i)w.lanes[i]=p.semantic_sources[i];return w;
}
inline bool Family(const Witness& w){
    if(!w.structural||!w.ended||w.scanned!=6||w.moves!=5||w.arithmetic||w.nops)return false;
    constexpr unsigned index[6]={0,0,95,95,0,0},component[6]={0,1,3,2,2,3};
    for(unsigned i=0;i<6;++i){const auto& l=w.lanes[i];if(l.unknown!=QuadUVPlan::Unknown::None||l.index!=index[i]||l.component!=component[i]||
        l.source!=(i==2||i==3?QuadUVPlan::Source::FloatUniform:QuadUVPlan::Source::InputAttribute))return false;}
    return true;
}
struct Rect {std::int64_t left=0,bottom=0,right=0,top=0;};
struct Planned {
    bool known=false;Rect guest_viewport{},surface{},viewport{},draw_rect{};
    std::uint32_t scale=0,scissor_mode=0;std::array<std::uint32_t,4> guest_scissor{};
};
struct Spec {
    const std::uint8_t* data=nullptr;std::size_t bytes=0;
    bool indexed=false,alternate_reuse=false,source_defined=false,source_default=false;
    std::uint32_t mode=0,span=0,records=0,input_mask=0,shader_register=0,selected_attribute=~0u;
    std::uint32_t source_format=0,source_elements=0,source_stride=0;
    std::array<std::int32_t,4> layout{};
    std::uint32_t first_vertex=0,input_map_low=0,input_map_high=0,max_input=0;std::size_t triangle_count=0,index_count=0;
    std::array<std::uint32_t,6> triangles{};std::array<std::uint16_t,6> indices{};
    Witness witness{};std::array<std::uint32_t,2> uniform95_zw{};
    std::array<std::uint32_t,2> viewport_half_raw{},viewport_half_float_bits{};
    std::array<std::int32_t,2> viewport_corner{};
    bool native_snap_env_present=false,ios_path=false,atlas_path=false;
};
struct Snapshot {
    Reason reason=Reason::NoBinding;bool captured=false,all_finite=false;
    Key key{};std::uint64_t binding=0;Witness witness{};
    std::uint32_t shader_register=0,offset=0,stride=0,first_vertex=0,input_map_low=0,input_map_high=0,max_input=0;
    std::array<std::array<std::uint32_t,4>,4> input_bits{};
    std::array<std::uint32_t,2> uniform95_zw{},viewport_half_raw{},viewport_half_float_bits{};
    std::array<std::int32_t,2> viewport_corner{};std::array<std::uint16_t,6> triangle_order{};
    bool native_snap_env_present=false,ios_path=false,atlas_path=false;
    Planned planned{};
};
static_assert(sizeof(float)==4,"FLOAT4 captures IEEE32 transfer words");
static_assert(sizeof(std::array<std::array<std::uint32_t,4>,4>)==64,"exact four-record capture");
static_assert(sizeof(std::array<std::uint32_t,2>)==8,"exact two selected uniform components");
inline bool Finite(std::uint32_t bits){return (bits&0x7f800000u)!=0x7f800000u;}
inline Snapshot Capture(Key key,const Spec& s){
    Snapshot r;r.key=key;r.witness=s.witness;
    if(s.mode!=2){r.reason=Reason::Mode;return r;}
    if(s.indexed||s.span!=4||s.records!=4){r.reason=Reason::Shape;return r;}
    if(s.alternate_reuse){r.reason=Reason::AlternateReuse;return r;}
    if(!Family(s.witness)){r.reason=Reason::Program;return r;}
    if(s.shader_register>=16||!(s.input_mask&(1u<<s.shader_register))||s.selected_attribute!=0){r.reason=Reason::Mapping;return r;}
    if(!s.source_defined){r.reason=Reason::UndefinedSource;return r;}
    if(s.source_default){r.reason=Reason::DefaultSource;return r;}
    if(s.source_format!=3||s.source_elements!=4||s.source_stride!=16||s.layout[0]<0||s.layout[1]!=16||s.layout[2]!=3||s.layout[3]!=4){r.reason=Reason::Layout;return r;}
    const auto offset=std::uint64_t(s.layout[0]);
    if(!s.data||offset>s.bytes||64u>s.bytes-offset){r.reason=Reason::BufferBounds;return r;}
    if(s.triangle_count!=6||s.index_count<6||s.first_vertex>0xfffffffcu){r.reason=Reason::Triangles;return r;}
    unsigned used=0;
    for(unsigned i=0;i<6;++i){
        if(s.triangles[i]<s.first_vertex||s.triangles[i]-s.first_vertex>=4||s.indices[i]!=s.triangles[i]-s.first_vertex){r.reason=Reason::Triangles;return r;}
        used|=1u<<s.indices[i];
    }
    if(used!=15){r.reason=Reason::Triangles;return r;}
    // These are the ONLY payload reads: four already-coherent FLOAT4 records.
    for(unsigned vertex=0;vertex<4;++vertex)std::memcpy(r.input_bits[vertex].data(),s.data+offset+vertex*16,16);
    r.input_map_low=s.input_map_low;r.input_map_high=s.input_map_high;r.max_input=s.max_input;
    r.uniform95_zw=s.uniform95_zw;r.triangle_order=s.indices;r.shader_register=s.shader_register;r.offset=static_cast<std::uint32_t>(offset);r.stride=16;r.first_vertex=s.first_vertex;
    r.viewport_half_raw=s.viewport_half_raw;r.viewport_half_float_bits=s.viewport_half_float_bits;r.viewport_corner=s.viewport_corner;
    r.native_snap_env_present=s.native_snap_env_present;r.ios_path=s.ios_path;r.atlas_path=s.atlas_path;
    r.all_finite=true;for(const auto& vertex:r.input_bits)for(auto word:vertex)r.all_finite&=Finite(word);for(auto word:r.uniform95_zw)r.all_finite&=Finite(word);
    r.reason=Reason::None;r.captured=true;return r;
}
inline Planned PlanRect(Rect viewport,Rect surface,std::uint32_t scale,std::uint32_t mode,std::array<std::uint32_t,4> scissor){
    Planned p;p.guest_viewport=viewport;p.surface=surface;p.scale=scale;p.scissor_mode=mode;p.guest_scissor=scissor;
    constexpr auto Min=std::numeric_limits<std::int32_t>::min(),Max=std::numeric_limits<std::int32_t>::max();
    if(viewport.left<Min||viewport.left>Max||viewport.right<Min||viewport.right>Max||viewport.bottom<Min||viewport.bottom>Max||viewport.top<Min||viewport.top>Max)return p;
    if(!scale||surface.left<0||surface.bottom<0||surface.right<surface.left||surface.top<surface.bottom||surface.right>Max||surface.top>Max||viewport.right<viewport.left||viewport.top<viewport.bottom)return p;
    const auto x1=surface.left+viewport.left*scale,x2=surface.left+viewport.right*scale;
    const auto y1=surface.bottom+viewport.bottom*scale,y2=surface.bottom+viewport.top*scale;
    if(x1<Min||x1>Max||x2<Min||x2>Max||y1<Min||y1>Max||y2<Min||y2>Max||x2-x1>Max||y2-y1>Max)return p;
    p.viewport={x1,y1,x2,y2};
    const auto clamp=[](std::int64_t v,std::int64_t low,std::int64_t high){return v<low?low:v>high?high:v;};
    p.draw_rect={clamp(x1,surface.left,surface.right),clamp(y1,surface.bottom,surface.top),clamp(x2,surface.left,surface.right),clamp(y2,surface.bottom,surface.top)};p.known=true;return p;
}
class Stack {
public:
    struct Token {std::uint64_t generation=0,id=0;std::size_t depth=0;bool armed=false;};
    Token Push(Snapshot value){
        if(value.key.generation!=generation_){generation_=value.key.generation;depth_=0;serial_=0;poisoned_=false;}
        if(poisoned_||depth_==frames_.size()||serial_==~std::uint64_t{0}){poisoned_=true;return {};}
        value.binding=++serial_;frames_[depth_++]=value;return {generation_,value.binding,depth_,true};
    }
    void Pop(Token t){
        if(!t.armed||t.generation!=generation_)return;
        if(!depth_||t.depth!=depth_||frames_[depth_-1].binding!=t.id){poisoned_=true;return;}
        frames_[--depth_]={};
    }
    Snapshot Read(Key expected,const Witness& current) const {
        Snapshot unknown;unknown.key=expected;
        if(poisoned_){unknown.reason=Reason::Overflow;return unknown;}
        if(!depth_)return unknown;
        const auto& value=frames_[depth_-1];
        if(!Equal(value.key,expected)){unknown.reason=Reason::WrongDraw;return unknown;}
        if(value.captured&&(!Family(current)||current.program!=value.witness.program||current.descriptors!=value.witness.descriptors)){unknown.reason=Reason::PlanChanged;return unknown;}
        return value;
    }
    void PlannedFor(Key key,const Planned& planned){if(!poisoned_&&depth_&&Equal(frames_[depth_-1].key,key))frames_[depth_-1].planned=planned;}
private:
    std::array<Snapshot,8> frames_{};std::size_t depth_=0;std::uint64_t generation_=0,serial_=0;bool poisoned_=false;
};
} // namespace Cara::QuadInputCapture
