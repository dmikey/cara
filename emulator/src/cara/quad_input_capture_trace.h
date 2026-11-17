#pragma once
#include <cstdlib>
#include <ostream>
#include "cara/quad_input_capture.h"
#include "cara/quad_uv_plan_census.h"
#include "cara/guest_gpu_trace.h"
namespace Cara::QuadInputCapture {
inline bool Enabled(){
    static const bool enabled=[] {
        const auto on=[](const char* key){const char* s=std::getenv(key);return s&&std::strcmp(s,"1")==0;};
        return on("CARA_QUAD_UV_INPUT_CENSUS")&&on("CARA_QUAD_UV_PLAN_CENSUS")&&
            on("CARA_SURFACE_VALIDATION_COPY_TRACE")&&on("CARA_SURFACE_VALIDATION_COPY_JOIN");
    }();return enabled;
}
inline thread_local Stack bindings;
inline Key JoinKey(const GuestGPUTrace::Join::Snapshot& j){return {j.generation,j.gx.submission,j.anchor.sequence};}
inline bool Exact(const GuestGPUTrace::Join::Snapshot& j){return j.reason==GuestGPUTrace::Join::Reason::None&&j.gx_valid&&j.draw_valid&&j.span_valid&&j.execution_owner_valid;}
inline std::uint32_t FloatBits(float value){std::uint32_t bits;std::memcpy(&bits,&value,4);return bits;}
class Scope {
public:
    Scope()=default;Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    ~Scope(){if(token_.armed)bindings.Pop(token_);}
    template<class Factory>
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline,cold))
#elif defined(_MSC_VER)
    __declspec(noinline)
#endif
    void Bind(Factory&& factory){
        const auto join=GuestGPUTrace::CurrentValidationCopyJoin();Snapshot value;value.key=JoinKey(join);
        if(!Exact(join))value.reason=Reason::NoExactDraw;
        else if(std::uint32_t(join.anchor.b)!=4||join.anchor.b>>32!=2)value.reason=Reason::Shape;
        else value=Capture(JoinKey(join),factory(join));
        token_=bindings.Push(value);
    }
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline,cold))
#elif defined(_MSC_VER)
    __declspec(noinline)
#endif
    void RejectSoftware(){const auto join=GuestGPUTrace::CurrentValidationCopyJoin();Snapshot value;value.key=JoinKey(join);value.reason=Reason::SoftwarePath;token_=bindings.Push(value);}
private:Stack::Token token_{};
};
inline Snapshot Current(const GuestGPUTrace::Join::Snapshot& join,const QuadUVPlan::Observation& observation){
    Snapshot result;if(!Enabled())return result;
    if(!Exact(join)){result.reason=Reason::NoExactDraw;return result;}
    result=bindings.Read(JoinKey(join),FromPlan(observation.plan));
    const auto& config=Pica::g_state.regs.vs;
    if(result.captured&&(result.input_map_low!=config.input_attribute_to_register_map_low||result.input_map_high!=config.input_attribute_to_register_map_high||result.max_input!=config.max_input_attribute_index)){
        Snapshot unknown;unknown.key=JoinKey(join);unknown.reason=Reason::PlanChanged;return unknown;
    }
    return result;
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline,cold))
#elif defined(_MSC_VER)
__declspec(noinline)
#endif
inline void PublishPlanned(Rect viewport,Rect surface,std::uint32_t scale,std::uint32_t mode,std::array<std::uint32_t,4> scissor){
    const auto join=GuestGPUTrace::CurrentValidationCopyJoin();if(Exact(join))bindings.PlannedFor(JoinKey(join),PlanRect(viewport,surface,scale,mode,scissor));
}
inline void RectJSON(std::ostream& out,const Rect& r){out<<"{\"left\":"<<r.left<<",\"bottom\":"<<r.bottom<<",\"right\":"<<r.right<<",\"top\":"<<r.top<<'}';}
inline void WriteJSON(std::ostream& out,const Snapshot& r){
    out<<"{\"schema\":1,\"diagnostic_only\":true,\"footprint_certified\":false,\"reason\":"<<unsigned(r.reason)<<",\"captured\":"<<(r.captured?"true":"false")
       <<",\"binding\":"<<r.binding<<",\"capture_generation\":"<<r.key.generation<<",\"submission\":"<<r.key.submission<<",\"draw_sequence\":"<<r.key.draw
       <<",\"all_selected_values_finite\":"<<(r.all_finite?"true":"false")<<",\"source\":\"post_PrepareIOSVertices_immutable_FLOAT4_copy\""
       <<",\"shader_register\":"<<r.shader_register<<",\"attribute\":0,\"byte_offset\":"<<r.offset<<",\"byte_stride\":"<<r.stride<<",\"first_vertex\":"<<r.first_vertex
       <<",\"input_map_low\":"<<r.input_map_low<<",\"input_map_high\":"<<r.input_map_high<<",\"max_input\":"<<r.max_input;
    out<<",\"float32_bits\":";if(!r.captured)out<<"null";else {out<<'[';for(unsigned v=0;v<4;++v){out<<(v?",":"")<<'[';for(unsigned c=0;c<4;++c)out<<(c?",":"")<<r.input_bits[v][c];out<<']';}out<<']';}
    out<<",\"uniform95_float32_bits_zw\":["<<r.uniform95_zw[0]<<','<<r.uniform95_zw[1]<<"],\"triangle_order\":[";for(unsigned i=0;i<6;++i)out<<(i?",":"")<<r.triangle_order[i];out<<']';
    out<<",\"native_snap_env_present\":"<<(r.native_snap_env_present?"true":"false")<<",\"ios_shader_path\":"<<(r.ios_path?"true":"false")<<",\"atlas_shader_path\":"<<(r.atlas_path?"true":"false")
       <<",\"viewport_half_raw\":["<<r.viewport_half_raw[0]<<','<<r.viewport_half_raw[1]<<"],\"viewport_half_float32_bits\":["<<r.viewport_half_float_bits[0]<<','<<r.viewport_half_float_bits[1]
       <<"],\"viewport_corner\":["<<r.viewport_corner[0]<<','<<r.viewport_corner[1]<<"],\"planned\":{\"known\":"<<(r.planned.known?"true":"false")<<",\"scale\":"<<r.planned.scale<<",\"guest_viewport\":";
    RectJSON(out,r.planned.guest_viewport);out<<",\"surface\":";RectJSON(out,r.planned.surface);out<<",\"prospective_gl_viewport\":";RectJSON(out,r.planned.viewport);out<<",\"prospective_draw_rect\":";RectJSON(out,r.planned.draw_rect);
    out<<",\"guest_scissor_mode\":"<<r.planned.scissor_mode<<",\"guest_scissor_xyxy\": [";for(unsigned i=0;i<4;++i)out<<(i?",":"")<<r.planned.guest_scissor[i];out<<"]}}";
}
} // namespace Cara::QuadInputCapture
