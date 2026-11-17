// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <string_view>
namespace OpenGL::VertexProvenance {
inline bool Enabled() {
#ifdef CARA_MAC_ATLAS_PIPELINE
    static const bool enabled=[] {const char* v=std::getenv("CARA_GPU_VERTEX_PROVENANCE");
        return v && std::strcmp(v,"1")==0;}();
    return enabled;
#else
    return false;
#endif
}
enum class Decoder { None, Scalar, Reused, Ambiguous };
inline Decoder Classify(std::string_view source) {
    const bool reused=source.find("uint cara_local_word(")!=std::string_view::npos;
    const bool scalar=source.find("uint cara_raw_word(")!=std::string_view::npos;
    return reused ? (scalar?Decoder::Ambiguous:Decoder::Reused) : (scalar?Decoder::Scalar:Decoder::None);
}
inline const char* DecoderName(Decoder decoder) {
    switch(decoder){case Decoder::Scalar:return "scalar_words";case Decoder::Reused:return "reused_texels";
        case Decoder::Ambiguous:return "ambiguous";default:return "none";}
}
struct Record {
    std::uint32_t program=0,managed_vs=0,actual_vs=0,current_program=0;
    std::uint64_t managed_hash=0,actual_source_hash=0,source_bytes=0,draw_attempts=0;
    std::array<std::uint64_t,8> input_modes{};
    bool linked=false,compiled=false,verified=false;
    Decoder decoder=Decoder::None;
    const char* reason="unobserved";
};
template<std::size_t Capacity=256> struct Ledger {
    std::array<Record,Capacity> records{};
    std::size_t size=0;
    std::uint64_t session=0,total_draw_attempts=0,overflow_draw_attempts=0;
    explicit Ledger(std::uint64_t id):session(id) {}
    // Managed program/shader names plus source hash avoid false identity on name
    // reuse. A verified record's actual attached VS must equal its managed VS.
    template<class Inspect>
    Record* Observe(std::uint32_t program,std::uint32_t managed_vs,std::uint64_t hash,
                    unsigned mode,Inspect inspect,bool& first) {
        ++total_draw_attempts;first=false;
        for(std::size_t i=0;i<size;++i){auto& r=records[i];
            if(r.program==program && r.managed_vs==managed_vs && r.managed_hash==hash){
                ++r.draw_attempts;++r.input_modes[mode<7?mode:7];return &r;}}
        if(size==Capacity){++overflow_draw_attempts;return nullptr;}
        first=true;auto& r=records[size++];r.program=program;r.managed_vs=managed_vs;r.managed_hash=hash;
        r.draw_attempts=1;++r.input_modes[mode<7?mode:7];inspect(r);return &r;
    }
};
inline void WriteRecord(std::ostream& out,const char* event,std::uint64_t session,const Record& r) {
    out<<"{\"event\":\""<<event<<"\",\"session\":"<<session
       <<",\"scope\":\"before_accelerated_final_draw\",\"program\":"<<r.program
       <<",\"managed_vs\":"<<r.managed_vs<<",\"actual_vs\":"<<r.actual_vs
       <<",\"current_program\":"<<r.current_program<<",\"managed_source_hash64\":"<<r.managed_hash
       <<",\"actual_source_hash64\":"<<r.actual_source_hash<<",\"source_bytes\":"<<r.source_bytes
       <<",\"link_ok\":"<<(r.linked?"true":"false")<<",\"compile_ok\":"<<(r.compiled?"true":"false")
       <<",\"first_use_verified\":"<<(r.verified?"true":"false")<<",\"decoder\":\""<<DecoderName(r.decoder)
       <<"\",\"reason\":\""<<r.reason<<"\",\"draw_attempts\":"<<r.draw_attempts
       <<",\"followups_are_cached_observations\":true,\"fallback_cause\":\"not_inferred_use_existing_logs\",\"input_modes\":[";
    for(std::size_t i=0;i<r.input_modes.size();++i)out<<(i?",":"")<<r.input_modes[i];
    out<<"]}";
}
} // namespace OpenGL::VertexProvenance
