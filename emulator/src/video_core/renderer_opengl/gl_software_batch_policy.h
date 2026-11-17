// Adjacent post-transform software-triangle batching, never a guest-time policy.
#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <ostream>
namespace OpenGL::SoftwareBatching {
inline bool Enabled(){static const bool on=[] {auto p=std::getenv("CARA_SW_ADJACENT_BATCH");return p&&std::strcmp(p,"1")==0;}();return on;}
inline bool CensusEnabled(){static const bool on=[] {auto p=std::getenv("CARA_SW_BATCH_CENSUS");return p&&std::strcmp(p,"1")==0;}();return on;}
inline bool Active(){return Enabled()||CensusEnabled();}
// Independent default-off experiment; never enables batching or census by itself.
inline bool DeferFramebufferFlushEnabled(){static const bool requested=[] {auto p=std::getenv("CARA_SW_DEFER_FRAMEBUFFER_FLUSH");return p&&std::strcmp(p,"1")==0;}();return requested&&Active();}
inline constexpr std::uint64_t Capacity=4095; // whole triangles; existing vertex_batch only
// Predicted reasons describe a census planner, actual reasons describe real draws.
enum class Reason:unsigned {ListBegin,ListEnd,FragmentOrUnknownWrite,IRQ,Jump,HardwareDraw,
 TextureAlias,VertexIndexAlias,Unsupported,SizeCap,FlushAll,FlushRegion,Invalidate,
 Transfer,Present,ConfigOrFrame,ExplicitVisibility,FramebufferFlush,TargetAlias,Count};
inline const char* ReasonName(Reason r){static constexpr const char* names[]={"list_begin","list_end","fragment_or_unknown_write","irq","jump","hardware_draw","texture_alias","vertex_index_alias","unsupported","size_cap","flush_all","flush_region","invalidate","transfer","present","config_or_frame","explicit_visibility","framebuffer_flush","target_alias"};return names[unsigned(r)];}
// With no queued host outputs there is nothing to protect before current CPU
// inputs are consumed. Census preserves its original eager virtual model.
inline bool LazySoftwareAdmission(bool enabled,bool census,bool batch_empty,
                                  bool owned_deferred,bool visibility_pending) {
 return enabled&&!census&&batch_empty&&!owned_deferred&&!visibility_pending;
}
struct Stats {
 std::uint64_t lazy_empty_input_skips=0,first_software_admission_checks=0,pre_input_resource_checks=0;
 std::uint64_t framebuffer_flush_triggers=0,framebuffer_flush_deferred=0;
 std::uint64_t framebuffer_flush_obligations=0,framebuffer_flush_materialized=0;
 std::uint64_t framebuffer_flush_census_materialized=0,framebuffer_flush_pending=0;
 std::array<std::uint64_t,unsigned(Reason::Count)> framebuffer_flush_retire_reasons{};
 std::array<std::uint64_t,0x300> post_draw_other_writes{},post_draw_other_unchanged{};
 std::array<std::uint64_t,0x300> fence_registers{},unchanged_fence_registers{};
 std::uint64_t software_requests=0,software_nonempty_requests=0,software_vertices=0;
 std::uint64_t eligible_software_requests=0,mergeable_software_transitions=0,predicted_groups=0;
 std::uint64_t hardware_attempts=0,adjacent_hardware_attempts_same_fragment_epoch=0;
 std::uint64_t actual_flushes=0,actual_flushed_vertices=0,actual_merged_completed_requests=0;
 std::uint64_t max_deferred_vertices=0,unsafe_texture=0,unsafe_vertex_index=0,unsafe_target_alias=0,unsupported=0;
 std::array<std::uint64_t,unsigned(Reason::Count)> predicted_flush_reasons{},actual_flush_reasons{};
};
inline Stats stats;
inline void Add(std::uint64_t& n,std::uint64_t v=1){n=v>UINT64_MAX-n?UINT64_MAX:n+v;}
inline void WriteJSON(std::ostream& out){
 out<<"{\"enabled\":"<<(Enabled()?"true":"false")<<",\"census\":"<<(CensusEnabled()?"true":"false")
    <<",\"defer_framebuffer_flush\":"<<(DeferFramebufferFlushEnabled()?"true":"false")
    <<",\"deferred_vertex_bound\":"<<Capacity<<",\"hardware_merge_implemented\":false";
#define SW_BATCH_JSON(n) out<<",\"" #n "\":"<<stats.n
 SW_BATCH_JSON(software_requests);SW_BATCH_JSON(software_nonempty_requests);SW_BATCH_JSON(software_vertices);
 SW_BATCH_JSON(eligible_software_requests);SW_BATCH_JSON(mergeable_software_transitions);SW_BATCH_JSON(predicted_groups);
 SW_BATCH_JSON(hardware_attempts);SW_BATCH_JSON(adjacent_hardware_attempts_same_fragment_epoch);
 SW_BATCH_JSON(actual_flushes);SW_BATCH_JSON(actual_flushed_vertices);SW_BATCH_JSON(actual_merged_completed_requests);
 SW_BATCH_JSON(max_deferred_vertices);SW_BATCH_JSON(unsafe_texture);SW_BATCH_JSON(unsafe_vertex_index);SW_BATCH_JSON(unsafe_target_alias);SW_BATCH_JSON(unsupported);
 SW_BATCH_JSON(framebuffer_flush_triggers);SW_BATCH_JSON(framebuffer_flush_deferred);
 SW_BATCH_JSON(framebuffer_flush_obligations);SW_BATCH_JSON(framebuffer_flush_materialized);
 SW_BATCH_JSON(framebuffer_flush_census_materialized);SW_BATCH_JSON(framebuffer_flush_pending);
 SW_BATCH_JSON(lazy_empty_input_skips);SW_BATCH_JSON(first_software_admission_checks);SW_BATCH_JSON(pre_input_resource_checks);
#undef SW_BATCH_JSON
 out<<",\"framebuffer_flush_retire_reasons\":{";
 for(unsigned i=0;i<unsigned(Reason::Count);++i){if(i)out<<',';out<<'"'<<ReasonName(Reason(i))<<"\":"<<stats.framebuffer_flush_retire_reasons[i];}
 out<<'}';
 out<<",\"predicted_flush_reasons\":{";
 for(unsigned i=0;i<unsigned(Reason::Count);++i){if(i)out<<',';out<<'"'<<ReasonName(Reason(i))<<"\":"<<stats.predicted_flush_reasons[i];}
 out<<"},\"actual_flush_reasons\":{";
 for(unsigned i=0;i<unsigned(Reason::Count);++i){if(i)out<<',';out<<'"'<<ReasonName(Reason(i))<<"\":"<<stats.actual_flush_reasons[i];}
 out<<"},\"fence_registers\":[";bool first=true;
 for(unsigned i=0;i<stats.fence_registers.size();++i)if(stats.fence_registers[i]){
  if(!first)out<<',';first=false;out<<"{\"register\":"<<i<<",\"fences\":"<<stats.fence_registers[i]<<",\"unchanged\":"<<stats.unchanged_fence_registers[i]<<'}';}
 out<<"],\"post_draw_other_writes\":[";first=true;
 for(unsigned i=0;i<stats.post_draw_other_writes.size();++i)if(stats.post_draw_other_writes[i]){
  if(!first)out<<',';first=false;out<<"{\"register\":"<<i<<",\"writes\":"<<stats.post_draw_other_writes[i]<<",\"unchanged\":"<<stats.post_draw_other_unchanged[i]<<'}';}
 out<<"]}";
}
struct Range {std::uint64_t begin=0,end=0;};
inline bool MakeRange(std::uint64_t address,std::uint64_t size,Range& r){if(!size||address>UINT32_MAX||size>std::uint64_t(UINT32_MAX)+1-address)return false;r={address,address+size};return true;}
inline bool Overlap(Range a,Range b){return a.begin<b.end&&b.begin<a.end;}
// Only post-transform input/geometry state is allowed to change without a fence.
// All fragment/raster/framebuffer/LUT and unknown writes fence, even equal writes.
inline bool InputOnlyRegister(std::uint32_t id){return id==0x253||id==0x231||id==0x245||(id>=0x200&&id<=0x22d)||(id>=0x232&&id<=0x235)||(id>=0x280&&id<=0x2df)||id==0x25e||id==0x25f;}
// Represents visibility work, not an input register or a removable guest command.
class FramebufferVisibility {
public:
 bool pending=false;
 bool Request(std::uint32_t value,std::uint32_t mask,bool enabled,bool in_list,
              bool safe,bool debug_observer,std::uint64_t owned_vertices) {
  if(!(mask&1)||!(value&1))return false;
  Add(stats.framebuffer_flush_triggers);
  if(!enabled||!in_list||!safe||debug_observer||!owned_vertices||
     owned_vertices>Capacity||owned_vertices%3)return false;
  Add(stats.framebuffer_flush_deferred);
  if(!pending){pending=true;Add(stats.framebuffer_flush_obligations);Add(stats.framebuffer_flush_pending);}
  return true;
 }
 // Call only after successful owned-vertex submission (or the census witness
 // that its original immediate submission already succeeded). Exceptions retain it.
 void Materialized(Reason why,bool census_already_submitted=false) {
  if(!pending)return;
  pending=false;if(stats.framebuffer_flush_pending)--stats.framebuffer_flush_pending;
  Add(stats.framebuffer_flush_materialized);
  if(census_already_submitted)Add(stats.framebuffer_flush_census_materialized);
  Add(stats.framebuffer_flush_retire_reasons[unsigned(why)]);
 }
};
class Planner {
public:
 bool in_list=false;bool last_hardware=false;std::uint64_t pending=0;
 void Boundary(Reason why){if(pending){Add(stats.predicted_groups);Add(stats.predicted_flush_reasons[unsigned(why)]);pending=0;}last_hardware=false;}
 void Begin(){Boundary(Reason::ListBegin);in_list=true;}
 void End(){Boundary(Reason::ListEnd);in_list=false;}
 void Hardware(){Add(stats.hardware_attempts);if(last_hardware)Add(stats.adjacent_hardware_attempts_same_fragment_epoch);Boundary(Reason::HardwareDraw);last_hardware=true;}
 void Software(std::uint64_t vertices,bool safe){
  Add(stats.software_requests);Add(stats.software_vertices,vertices);last_hardware=false;if(!vertices)return;
  Add(stats.software_nonempty_requests);if(!in_list||!safe){Boundary(Reason::Unsupported);return;}
  Add(stats.eligible_software_requests);
  if(pending&&vertices<=Capacity-pending){Add(stats.mergeable_software_transitions);pending+=vertices;return;}
  if(pending)Boundary(Reason::SizeCap);
  const auto whole=(vertices-1)/Capacity;
  Add(stats.predicted_groups,whole);Add(stats.predicted_flush_reasons[unsigned(Reason::SizeCap)],whole);
  pending=vertices-whole*Capacity;
 }
};
} // namespace OpenGL::SoftwareBatching
