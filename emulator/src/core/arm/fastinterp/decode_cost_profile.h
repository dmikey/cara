// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <ctime>
#include <ostream>

namespace Core::FastInterp {
// Raw, bounded diagnostics only. This class never reads guest memory or guest clocks,
// changes scheduling, estimates unsampled cost, or subtracts calibration time.
class DecodeCostProfile {
public:
    static constexpr std::uint64_t CalibrationPairs = 32, MaxSamples = 65536;
    using Clock = bool (*)(void*, bool, std::uint64_t&); // true=thread CPU, false=monotonic wall
    struct Stats {
        std::uint64_t decode_calls=0, allocation_miss_decodes=0, mode_redecodes=0;
        std::uint64_t selected_samples=0, timed_samples=0, completed_samples=0, dropped_samples=0;
        std::uint64_t sampled_cpu_ns=0, sampled_wall_ns=0, invalid_delta_samples=0;
        std::uint64_t clock_reads=0, cpu_clock_failures=0, wall_clock_failures=0;
        std::uint64_t calibration_pairs=0, calibration_completed=0, calibration_cpu_ns=0, calibration_wall_ns=0;
        std::uint64_t run_calls=0, observed_frames=0, first_frame=0, last_frame=0, frame_gaps=0, frame_resets=0;
        std::uint64_t chain_self_dispatches=0, chain_primary_dispatches=0;
        std::uint64_t chain_fallback_lookups=0, chain_fallback_dispatches=0;
    };
    struct Ticket { bool active=false, valid=false; std::uint64_t cpu=0, wall=0; };
    explicit DecodeCostProfile(Clock clock=ReadClock, void* context=nullptr,
                               std::uint64_t sample_limit=MaxSamples)
        : clock_(clock), context_(context), sample_limit_(std::min(sample_limit,MaxSamples)) {}
    void SetEnabled(bool enabled) { enabled_=enabled; }
    bool Enabled() const { return enabled_; }
    const Stats& GetStats() const { return stats_; }
    static std::uint64_t Hash(std::uint64_t value) {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }
    void ObserveRunFrame(std::uint64_t frame) {
        if(!enabled_) return;
        ++stats_.run_calls;
        if(!stats_.observed_frames) {
            stats_.first_frame=stats_.last_frame=frame;stats_.observed_frames=1;return;
        }
        if(frame==stats_.last_frame) return;
        if(frame<stats_.last_frame) ++stats_.frame_resets;
        else stats_.frame_gaps += frame-stats_.last_frame-1;
        stats_.last_frame=frame;++stats_.observed_frames;
    }
    Ticket BeforeDecode(bool allocation_miss) {
        if(!enabled_) return {};
        ++stats_.decode_calls;
        if(!calibrated_) Calibrate();
        if(!allocation_miss) {++stats_.mode_redecodes;return {};}
        const auto ordinal=++stats_.allocation_miss_decodes;
        if(Hash(ordinal)&63) return {};
        ++stats_.selected_samples;
        if(stats_.timed_samples==sample_limit_) {++stats_.dropped_samples;return {};}
        ++stats_.timed_samples;
        Ticket ticket;ticket.active=true;
        const bool cpu=Read(true,ticket.cpu), wall=Read(false,ticket.wall);
        ticket.valid=cpu&&wall;
        return ticket;
    }
    void AfterDecode(const Ticket& ticket) {
        if(!ticket.active) return;
        std::uint64_t cpu=0,wall=0;
        const bool cpu_ok=Read(true,cpu), wall_ok=Read(false,wall);
        if(!ticket.valid||!cpu_ok||!wall_ok) return;
        if(cpu<ticket.cpu||wall<ticket.wall) {++stats_.invalid_delta_samples;return;}
        ++stats_.completed_samples;
        stats_.sampled_cpu_ns+=cpu-ticket.cpu;stats_.sampled_wall_ns+=wall-ticket.wall;
    }
    void ChainSelf() {if(enabled_)++stats_.chain_self_dispatches;}
    void ChainPrimary() {if(enabled_)++stats_.chain_primary_dispatches;}
    void ChainLookup() {if(enabled_)++stats_.chain_fallback_lookups;}
    void ChainFallback() {if(enabled_)++stats_.chain_fallback_dispatches;}
    void Write(std::ostream& out) const {
        out << "{\"enabled\":" << (enabled_?"true":"false")
            << ",\"scope\":\"cpu_lifetime_including_warmup_and_state_restore\""
            << ",\"sample_policy\":\"splitmix64_allocation_miss_ordinal_low6_zero\""
            << ",\"frame_policy\":\"Run_frame_observations_not_PC_profile_duty_windows\""
            << ",\"sample_limit\":" << sample_limit_
            << ",\"clock_read_budget\":" << 4*(sample_limit_+CalibrationPairs);
#define DECODE_COST_FIELD(name) out << ",\"" #name "\":" << stats_.name
        DECODE_COST_FIELD(decode_calls); DECODE_COST_FIELD(allocation_miss_decodes); DECODE_COST_FIELD(mode_redecodes);
        DECODE_COST_FIELD(selected_samples); DECODE_COST_FIELD(timed_samples); DECODE_COST_FIELD(completed_samples);
        DECODE_COST_FIELD(dropped_samples); DECODE_COST_FIELD(sampled_cpu_ns); DECODE_COST_FIELD(sampled_wall_ns);
        DECODE_COST_FIELD(invalid_delta_samples); DECODE_COST_FIELD(clock_reads); DECODE_COST_FIELD(cpu_clock_failures);
        DECODE_COST_FIELD(wall_clock_failures); DECODE_COST_FIELD(calibration_pairs); DECODE_COST_FIELD(calibration_completed);
        DECODE_COST_FIELD(calibration_cpu_ns); DECODE_COST_FIELD(calibration_wall_ns); DECODE_COST_FIELD(run_calls);
        DECODE_COST_FIELD(observed_frames); DECODE_COST_FIELD(first_frame); DECODE_COST_FIELD(last_frame);
        DECODE_COST_FIELD(frame_gaps); DECODE_COST_FIELD(frame_resets); DECODE_COST_FIELD(chain_self_dispatches);
        DECODE_COST_FIELD(chain_primary_dispatches); DECODE_COST_FIELD(chain_fallback_lookups); DECODE_COST_FIELD(chain_fallback_dispatches);
#undef DECODE_COST_FIELD
        out << '}';
    }
private:
    static bool ReadClock(void*,bool cpu,std::uint64_t& ns) {
#if defined(CLOCK_THREAD_CPUTIME_ID) && defined(CLOCK_MONOTONIC)
        timespec value{};
        if(clock_gettime(cpu?CLOCK_THREAD_CPUTIME_ID:CLOCK_MONOTONIC,&value)!=0||
           value.tv_sec<0||value.tv_nsec<0||value.tv_nsec>=1000000000L) return false;
        const auto seconds=static_cast<std::uint64_t>(value.tv_sec);
        if(seconds>(UINT64_MAX-static_cast<std::uint64_t>(value.tv_nsec))/1000000000ULL)return false;
        ns=seconds*1000000000ULL+static_cast<std::uint64_t>(value.tv_nsec);return true;
#else
        (void)cpu;(void)ns;return false;
#endif
    }
    bool Read(bool cpu,std::uint64_t& ns) {
        ++stats_.clock_reads;
        if(clock_(context_,cpu,ns)) return true;
        if(cpu)++stats_.cpu_clock_failures;else++stats_.wall_clock_failures;
        return false;
    }
    void Calibrate() {
        calibrated_=true;
        for(std::uint64_t i=0;i<CalibrationPairs;++i) {
            ++stats_.calibration_pairs;
            std::uint64_t a=0,b=0,c=0,d=0;
            const bool ac=Read(true,a),bw=Read(false,b),cc=Read(true,c),dw=Read(false,d);
            if(!ac||!bw||!cc||!dw||c<a||d<b)continue;
            ++stats_.calibration_completed;stats_.calibration_cpu_ns+=c-a;stats_.calibration_wall_ns+=d-b;
        }
    }
    Clock clock_; void* context_; std::uint64_t sample_limit_;
    bool enabled_=false,calibrated_=false; Stats stats_{};
};
} // namespace Core::FastInterp
