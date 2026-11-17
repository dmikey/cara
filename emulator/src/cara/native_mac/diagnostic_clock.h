// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
// Native diagnostic host policy only. The core's existing clock API is unchanged.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <ostream>
#include <stdexcept>

namespace Cara::NativeDiagnosticClock {
struct Config {
    bool requested=false;
    unsigned percent=100;
};
inline bool One(const char* s) {return s&&std::strcmp(s,"1")==0;}
inline Config Parse(const char* value,const char* diagnostics,const char* benchmark,const char* script_only) {
    if(!value)return {};
    if(std::strcmp(value,"100")!=0 && std::strcmp(value,"50")!=0)
        throw std::invalid_argument("CARA_NATIVE_DIAGNOSTIC_CLOCK_PERCENT must be exactly 100 or 50");
    if(!One(diagnostics)||!One(benchmark)||!One(script_only))
        throw std::invalid_argument("Native diagnostic clock requires DIAGNOSTICS=1, BENCHMARK=1 and SCRIPT_ONLY_INPUT=1");
    return {true,std::strcmp(value,"50")==0?50u:100u};
}
inline const Config& Get() {
    static const Config config=Parse(std::getenv("CARA_NATIVE_DIAGNOSTIC_CLOCK_PERCENT"),
        std::getenv("CARA_NATIVE_DIAGNOSTICS"),std::getenv("CARA_NATIVE_BENCHMARK"),
        std::getenv("CARA_NATIVE_SCRIPT_ONLY_INPUT"));
    return config;
}
struct Receipt {
    std::uint64_t applications=0;
    const char* last_boundary="none";
    bool script_only_verified_at_apply=false;
};
inline Receipt receipt;
inline unsigned RequestedPercent() {return Get().percent;}
template<class Timing> double EffectivePercent(Timing& timing,unsigned core=0) {
    const double scale=timing.GetTimer(core)->GetClockScaleForDiagnostics();
    if(!std::isfinite(scale)||scale<=0)throw std::runtime_error("Invalid actual native guest clock scale");
    return 100.0/scale;
}
template<class Timing> void Verify(Timing& timing,unsigned cores,unsigned target) {
    if(!cores||cores>4)throw std::runtime_error("Invalid native clock timer count");
    const double expected=100.0/target;
    for(unsigned i=0;i<cores;++i)
        if(timing.GetTimer(i)->GetClockScaleForDiagnostics()!=expected)
            throw std::runtime_error("Native guest clock differs from the requested diagnostic/baseline clock");
}
// Called only on the emulation owner after a successful load, never from a
// report or frame callback. No pending events/ticks/budget are rescheduled here.
template<class Timing> void Apply(const Config& config,Timing& timing,unsigned cores,
                                  bool script_only_effective,const char* boundary,Receipt& state) {
    if(!cores||cores>4 || (config.percent!=50&&config.percent!=100) || (!config.requested&&config.percent!=100))
        throw std::runtime_error("Invalid native diagnostic clock policy");
    if(config.requested) {
        if(!script_only_effective)throw std::runtime_error("Native diagnostic clock requires effective script-only input");
        timing.UpdateClockSpeed(config.percent); // existing core API, including its remainder-reset semantics
        if(state.applications!=std::numeric_limits<std::uint64_t>::max())++state.applications;
        state.last_boundary=boundary;
        state.script_only_verified_at_apply=true;
    }
    // Absent mode performs no clock update. A diagnostic50 saved state cannot
    // silently become an ordinary host50 session through normal state loading.
    Verify(timing,cores,config.percent);
}
template<class Timing> void ApplyAfterLoad(Timing& timing,unsigned cores,bool script_only_effective,const char* boundary) {
    Apply(Get(),timing,cores,script_only_effective,boundary,receipt);
}
template<class Timing> void WriteJSON(std::ostream& out,const Config& config,Timing& timing,unsigned cores,const Receipt& state) {
    out<<"{\"requested\":"<<(config.requested?"true":"false")<<",\"requested_percent\":";
    if(config.requested)out<<config.percent;else out<<"null";
    out<<",\"baseline_ini_clock_percent\":100,\"target_guest_cpu_percent\":"<<config.percent
       <<",\"actual_guest_cpu_percent\":"<<EffectivePercent(timing)
       <<",\"applications\":"<<state.applications<<",\"last_apply_boundary\":\""<<state.last_boundary
       <<"\",\"script_only_verified_at_apply\":"<<(state.script_only_verified_at_apply?"true":"false")
       <<",\"existing_core_clock_api\":true,\"remainder_reset_on_scale_change\":true"
         ",\"hardware_events_rescheduled_by_native_control\":false,\"timers\":[";
    bool match=cores>0&&cores<=4;
    if(cores>4)throw std::runtime_error("Invalid native clock report timer count");
    for(unsigned i=0;i<cores;++i) {
        const double effective=EffectivePercent(timing,i);
        match=match&&(timing.GetTimer(i)->GetClockScaleForDiagnostics()==100.0/config.percent);
        out<<(i?",":"")<<"{\"core\":"<<i<<",\"effective_percent\":"<<effective<<'}';
    }
    out<<"],\"all_active_timers_match_target\":"<<(match?"true":"false")<<'}';
}
template<class Timing> void WriteJSON(std::ostream& out,Timing& timing,unsigned cores) {
    WriteJSON(out,Get(),timing,cores,receipt);
}
} // namespace Cara::NativeDiagnosticClock
