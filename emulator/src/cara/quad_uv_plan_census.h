#pragma once
#include <ostream>
#include "cara/quad_uv_plan.h"
#include "cara/guest_gpu_trace_join.h"
#include "video_core/pica_state.h"
namespace Cara::QuadUVPlan {
constexpr std::uint32_t SampleCapacity=128;
struct Observation {
    bool captured=false;
    std::uint32_t entry=0,output_mask=0,output_total=0,max_input=0;
    Plan plan{};
};
inline const char* ReasonName(Reason r){
    switch(r){
    case Reason::None:return "bounded_straightline_metadata_only";
    case Reason::NoExactDraw:return "no_exact_current_draw_owner";
    case Reason::DrawShape:return "not_unindexed_four_vertex_strip_nogs";
    case Reason::InputConfig:return "unsupported_input_configuration";
    case Reason::OutputConfig:return "unsupported_output_compaction";
    case Reason::EntryBounds:return "entry_or_host_span_unavailable";
    case Reason::ProgramBounds:return "program_bounds_without_end";
    case Reason::ScanLimit:return "scan_limit_without_end";
    case Reason::FlowOrGeometry:return "flow_or_geometry_opcode";
    case Reason::UnsupportedOpcode:return "unsupported_opcode_including_cmp_mad_mova";
    case Reason::RelativeAddressing:return "relative_source_addressing";
    case Reason::SwizzleBounds:return "descriptor_bounds";
    case Reason::DuplicateSemantic:return "duplicate_output_semantic";
    case Reason::InvalidSemantic:return "unsupported_output_semantic";
    case Reason::CensusCapacity:return "diagnostic_capacity";
    }
    return "unknown";
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline,cold))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
inline Observation Observe(const GuestGPUTrace::Join::Snapshot& join,Pica::State& state) {
    Observation result;
    if(join.reason!=GuestGPUTrace::Join::Reason::None || !join.gx_valid || !join.draw_valid ||
       !join.span_valid || !join.execution_owner_valid){result.plan.reason=Reason::NoExactDraw;return result;}
    if(std::uint32_t(join.anchor.b)!=4 || join.anchor.b>>32!=2){result.plan.reason=Reason::DrawShape;return result;}
    const auto& config=state.regs.vs;
    if(config.shader_mode!=Pica::ShaderRegs::ShaderMode::VS || config.input_to_uniform!=0){result.plan.reason=Reason::InputConfig;return result;}
    View view;view.program=state.vs.program_code.data();view.program_words=state.vs.program_code.size();
    view.swizzles=state.vs.swizzle_data.data();view.swizzle_words=state.vs.swizzle_data.size();
    view.entry=config.main_offset;view.max_input=config.max_input_attribute_index;
    view.input_map_low=config.input_attribute_to_register_map_low;view.input_map_high=config.input_attribute_to_register_map_high;
    view.output_mask=config.output_mask;view.output_total=state.regs.rasterizer.vs_output_total;
    for(unsigned i=0;i<view.semantics.size();++i)view.semantics[i]=state.regs.rasterizer.vs_output_attributes[i].raw;
    result.entry=view.entry;result.output_mask=view.output_mask;result.output_total=view.output_total;result.max_input=view.max_input;
    result.plan=Classify(view);result.captured=true;return result;
}
inline void WriteJSON(std::ostream& out,const Observation& o){
    const auto& p=o.plan;
    out<<"{\"schema\":1,\"diagnostic_only\":true,\"footprint_certified\":false,\"captured\":"<<(o.captured?"true":"false")
       <<",\"reason\":"<<unsigned(p.reason)<<",\"reason_name\":\""<<ReasonName(p.reason)<<"\",\"structural\":"<<(p.structural?"true":"false")
       <<",\"ended\":"<<(p.ended?"true":"false")<<",\"entry\":"<<o.entry<<",\"output_mask\":"<<o.output_mask<<",\"output_total\":"<<o.output_total<<",\"max_input\":"<<o.max_input
       <<",\"scan_limit\":"<<ScanLimit<<",\"scanned\":"<<p.scanned<<",\"stop_pc\":"<<p.stop_pc<<",\"moves\":"<<p.moves<<",\"nops\":"<<p.nops<<",\"tainting_arithmetic\":"<<p.arithmetic;
    out<<",\"identity_scope\":\"scanned_prefix_and_referenced_descriptors_noncryptographic_not_enforcement_key\""
       <<",\"scanned_program_identity\":"<<p.scanned_program_identity<<",\"scanned_descriptor_identity\":"<<p.scanned_descriptor_identity;
    out<<",\"opcode_counts\":{";bool comma=false;
    for(unsigned i=0;i<p.opcode_counts.size();++i)if(p.opcode_counts[i]){out<<(comma?",":"")<<'"'<<i<<"\":"<<p.opcode_counts[i];comma=true;}out<<'}';
    out<<",\"uv_selector_plan\":[";
    for(unsigned i=0;i<3;++i)out<<(i?",":"")<<(p.uv_selector_plan[i]?"true":"false");
    out<<"],\"semantic_sources\":[";
    for(unsigned i=0;i<Semantics.size();++i){const auto& l=p.semantic_sources[i];out<<(i?",":"")<<"{\"semantic\":"<<Semantics[i]<<",\"source\":"<<unsigned(l.source)<<",\"unknown\":"<<unsigned(l.unknown)<<",\"index\":"<<unsigned(l.index)<<",\"component\":"<<unsigned(l.component)<<'}';}
    out<<"]}";
}
} // namespace Cara::QuadUVPlan
