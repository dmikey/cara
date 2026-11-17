// Diagnostic symbolic provenance only: no float evaluation or shader execution.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <nihstro/shader_bytecode.h>
namespace Cara::QuadUVPlan {
constexpr std::size_t ScanLimit=128;
enum class Reason : std::uint8_t {
    None, NoExactDraw, DrawShape, InputConfig, OutputConfig, EntryBounds,
    ProgramBounds, ScanLimit, FlowOrGeometry, UnsupportedOpcode,
    RelativeAddressing, SwizzleBounds, DuplicateSemantic, InvalidSemantic, CensusCapacity
};
enum class Source : std::uint8_t { Unknown, InputAttribute, FloatUniform };
enum class Unknown : std::uint8_t { None, Uninitialized, Arithmetic, NegatedMove, MissingSemantic };
struct Lane {
    Source source=Source::Unknown;Unknown unknown=Unknown::Uninitialized;
    std::uint8_t index=0,component=0;
};
struct View {
    const std::uint32_t* program=nullptr;std::size_t program_words=0;
    const std::uint32_t* swizzles=nullptr;std::size_t swizzle_words=0;
    std::uint32_t entry=0,input_map_low=0,input_map_high=0,max_input=0;
    std::uint32_t output_mask=0,output_total=0;
    std::array<std::uint32_t,7> semantics{};
};
// Exported semantics: position.xyzw, tc0.xy, tc1.xy, tc0.w, tc2.xy.
constexpr std::array<unsigned,11> Semantics{{0,1,2,3,12,13,14,15,16,22,23}};
struct Plan {
    Reason reason=Reason::EntryBounds;
    bool ended=false,structural=false;
    std::uint32_t scanned=0,moves=0,nops=0,arithmetic=0,stop_pc=0;
    std::uint64_t scanned_program_identity=14695981039346656037ULL;
    std::uint64_t scanned_descriptor_identity=14695981039346656037ULL;
    std::array<std::uint32_t,64> opcode_counts{};
    std::array<Lane,11> semantic_sources{};
    std::array<bool,3> uv_selector_plan{};
    // This type deliberately contains NO values, UV bounds or permission bit.
};
inline void IdentityWord(std::uint64_t& hash,std::uint32_t value){
    for(unsigned byte=0;byte<4;++byte){hash^=(value>>(8*byte))&255u;hash*=1099511628211ULL;}
}
inline bool Known(const Lane& l){return l.source!=Source::Unknown && l.unknown==Unknown::None;}
inline Plan Classify(const View& view) {
    Plan result;
    for(auto& lane:result.semantic_sources)lane.unknown=Unknown::MissingSemantic;
    if(view.max_input>=16){result.reason=Reason::InputConfig;return result;}
    unsigned output_count=0;for(unsigned b=0;b<16;++b)output_count+=(view.output_mask>>b)&1u;
    if(view.output_mask>0xffffu || !view.output_total || view.output_total>7 || output_count!=view.output_total){result.reason=Reason::OutputConfig;return result;}
    if(!view.program || !view.swizzles || view.entry>=view.program_words){result.reason=Reason::EntryBounds;return result;}
    std::array<std::array<Lane,4>,16> input{},temp{},output{};
    const std::uint64_t input_map=std::uint64_t(view.input_map_low)|(std::uint64_t(view.input_map_high)<<32);
    // UnitState::LoadInput overwrites duplicate mapped registers in attr order.
    for(unsigned attr=0;attr<=view.max_input;++attr){
        const auto reg=(input_map>>(4*attr))&15u;
        for(unsigned c=0;c<4;++c)input[reg][c]={Source::InputAttribute,Unknown::None,static_cast<std::uint8_t>(attr),static_cast<std::uint8_t>(c)};
    }
    for(std::size_t step=0;step<ScanLimit;++step){
        const auto pc=std::uint64_t(view.entry)+step;result.stop_pc=static_cast<std::uint32_t>(pc);
        if(pc>=view.program_words){result.reason=Reason::ProgramBounds;return result;}
        const nihstro::Instruction ins={view.program[pc]};
        IdentityWord(result.scanned_program_identity,static_cast<std::uint32_t>(pc));
        IdentityWord(result.scanned_program_identity,view.program[pc]);
        const unsigned op=(view.program[pc]>>26)&63u;
        ++result.scanned;++result.opcode_counts[op];
        if(op==0x22){result.ended=true;break;}
        if(op==0x21){++result.nops;continue;}
        if(op>=0x20 && op<=0x2d){result.reason=Reason::FlowOrGeometry;return result;}
        const bool ordinary_arithmetic=op<=0x0f || (op>=0x18&&op<=0x1b);
        if(op!=0x13 && !ordinary_arithmetic){result.reason=Reason::UnsupportedOpcode;return result;}
        if(ins.common.address_register_index!=0){result.reason=Reason::RelativeAddressing;return result;}
        const auto swizzle_id=ins.common.operand_desc_id.Value();
        if(swizzle_id>=view.swizzle_words){result.reason=Reason::SwizzleBounds;return result;}
        const nihstro::SwizzlePattern swizzle={view.swizzles[swizzle_id]};
        IdentityWord(result.scanned_descriptor_identity,swizzle_id);
        IdentityWord(result.scanned_descriptor_identity,view.swizzles[swizzle_id]);
        const auto dest=ins.common.dest.Value();
        auto& target=dest.GetRegisterType()==nihstro::RegisterType::Output?output[dest.GetIndex()]:temp[dest.GetIndex()];
        if(op==0x13){
            ++result.moves;std::array<Lane,4> source{};
            const auto src=ins.common.GetSrc1(false);
            // Snapshot all source lanes before aliasing partial/swizzled writes.
            for(unsigned c=0;c<4;++c){
                const auto component=static_cast<unsigned>(swizzle.GetSelectorSrc1(c));
                if(src.GetRegisterType()==nihstro::RegisterType::Input)source[c]=input[src.GetIndex()][component];
                else if(src.GetRegisterType()==nihstro::RegisterType::Temporary)source[c]=temp[src.GetIndex()][component];
                else source[c]={Source::FloatUniform,Unknown::None,static_cast<std::uint8_t>(src.GetIndex()),static_cast<std::uint8_t>(component)};
                if(swizzle.negate_src1)source[c]={Source::Unknown,Unknown::NegatedMove,0,0};
            }
            for(unsigned c=0;c<4;++c)if(swizzle.DestComponentEnabled(c))target[c]=source[c];
        }else{
            ++result.arithmetic;
            for(unsigned c=0;c<4;++c)if(swizzle.DestComponentEnabled(c))target[c]={Source::Unknown,Unknown::Arithmetic,0,0};
        }
    }
    if(!result.ended){result.reason=Reason::ScanLimit;return result;}
    std::array<bool,32> seen{};unsigned attribute=0;
    for(unsigned reg=0;reg<16;++reg){
        if(!(view.output_mask&(1u<<reg)))continue;
        const auto map=view.semantics[attribute++];
        for(unsigned c=0;c<4;++c){
            const auto semantic=(map>>(8*c))&31u;
            if(semantic==31)continue;
            if(semantic>=24 || semantic==17 || semantic==21){result.reason=Reason::InvalidSemantic;return result;}
            if(seen[semantic]){result.reason=Reason::DuplicateSemantic;return result;}seen[semantic]=true;
            for(unsigned n=0;n<Semantics.size();++n)if(Semantics[n]==semantic)result.semantic_sources[n]=output[reg][c];
        }
    }
    result.reason=Reason::None;result.structural=true;
    result.uv_selector_plan[0]=Known(result.semantic_sources[4])&&Known(result.semantic_sources[5]);
    result.uv_selector_plan[1]=Known(result.semantic_sources[6])&&Known(result.semantic_sources[7]);
    result.uv_selector_plan[2]=Known(result.semantic_sources[9])&&Known(result.semantic_sources[10]);
    return result;
}
} // namespace Cara::QuadUVPlan
