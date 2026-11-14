// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "core/arm/fastinterp/decode_cost_profile.h"
#include "core/arm/fastinterp/fastinterp_cache.h"
using namespace Core::FastInterp;
static void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct FakeClock {
    u64 cpu=0,wall=0,reads=0;bool fail=false,backwards=false;
    static bool Read(void* context,bool cpu,u64& ns){
        auto& c=*static_cast<FakeClock*>(context);++c.reads;
        if(c.fail)return false;
        auto& value=cpu?c.cpu:c.wall;
        value+=cpu?10:20;ns=c.backwards?1000000-value:value;return true;
    }
};
static void Sampler() {
    FakeClock clock;DecodeCostProfile p(FakeClock::Read,&clock,4);
    for(u64 i=0;i<1000;++i){p.ObserveRunFrame(i);auto t=p.BeforeDecode(true);p.AfterDecode(t);p.ChainSelf();p.ChainPrimary();p.ChainLookup();p.ChainFallback();}
    Require(!clock.reads&&!p.GetStats().decode_calls&&!p.GetStats().run_calls,"disabled profiler read a clock or recorded events");
    p.SetEnabled(true);
    for(u64 frame:{10,10,11,15,2,3})p.ObserveRunFrame(frame);
    auto mode=p.BeforeDecode(false);p.AfterDecode(mode);
    u64 chosen=0;
    for(u64 n=1;n<=4096;++n){chosen+=(DecodeCostProfile::Hash(n)&63)==0;auto t=p.BeforeDecode(true);p.AfterDecode(t);}
    p.ChainSelf();p.ChainPrimary();p.ChainLookup();p.ChainFallback();
    const auto& s=p.GetStats();
    Require(s.decode_calls==4097&&s.allocation_miss_decodes==4096&&s.mode_redecodes==1,"decode reason counts differ");
    Require(s.selected_samples==chosen&&s.timed_samples==4&&s.completed_samples==4&&s.dropped_samples==chosen-4,"hash selection/cap accounting differs");
    Require(s.sampled_cpu_ns==40&&s.sampled_wall_ns==80&&s.calibration_completed==32&&s.calibration_cpu_ns==320&&s.calibration_wall_ns==640,"raw delta/calibration sums differ");
    Require(s.clock_reads==4*(4+32)&&clock.reads==s.clock_reads,"clock read budget exceeded");
    Require(s.observed_frames==5&&s.first_frame==10&&s.last_frame==3&&s.frame_gaps==3&&s.frame_resets==1&&s.run_calls==6,"frame scope/gap/reset accounting differs");
    Require(s.chain_self_dispatches==1&&s.chain_primary_dispatches==1&&s.chain_fallback_lookups==1&&s.chain_fallback_dispatches==1,"chain counters differ");
    std::ostringstream out;p.Write(out);Require(out.str().find("cpu_lifetime_including_warmup_and_state_restore")!=std::string::npos,"missing lifetime scope");
    FakeClock broken;broken.fail=true;DecodeCostProfile bad(FakeClock::Read,&broken,2);bad.SetEnabled(true);
    for(unsigned n=0;n<1024;++n){auto t=bad.BeforeDecode(true);bad.AfterDecode(t);}
    Require(!bad.GetStats().completed_samples&&bad.GetStats().cpu_clock_failures==68&&bad.GetStats().wall_clock_failures==68,"failed clock counts differ");
    FakeClock reverse;reverse.backwards=true;DecodeCostProfile negative(FakeClock::Read,&reverse,2);negative.SetEnabled(true);
    for(unsigned n=0;n<1024;++n){auto t=negative.BeforeDecode(true);negative.AfterDecode(t);}
    Require(negative.GetStats().invalid_delta_samples==2&&!negative.GetStats().sampled_cpu_ns&&!negative.GetStats().calibration_completed,"backwards clocks were accepted");
}
static std::vector<u32> Collisions(unsigned ways,unsigned capacity){
    auto index=[ways,capacity](u32 pc){u32 h=pc*0x9e3779b9;h^=h>>16;return(h>>4)&((capacity/ways)-1);};
    std::vector<u32> result;for(u32 pc=0x1000;result.size()<ways+1;pc+=4)if(index(pc)==index(0x1000))result.push_back(pc);return result;
}
static void Counters() {
    for(unsigned capacity:{DEFAULT_CACHE_SIZE,MAX_CACHE_SIZE})for(unsigned ways:{4u,8u})for(bool enabled:{false,true}){
        auto cache=std::make_unique<BlockCache>(capacity);cache->SetEightWay(ways==8);cache->SetProfiling(enabled);
        const auto pcs=Collisions(ways,capacity);
        for(unsigned i=0;i<ways;++i){bool hit;auto* b=cache->LookupOrAllocate(pcs[i],hit);Require(!hit,"unexpected allocation hit");b->inst_count=1;b->end_pc=b->start_pc+4;}
        for(unsigned i=0;i<ways;++i){Require(cache->Lookup(pcs[i]),"lookup lost tag");bool hit;cache->LookupOrAllocate(pcs[i],hit);Require(hit,"allocation lookup lost tag");}
        Require(!cache->Lookup(pcs.back()),"missing tag became a hit");
        const auto& s=cache->GetProfile();const u64 on=enabled?1:0;
        Require(s.lookup_calls==on*(ways+1)&&s.lookup_hits==on*ways&&s.lookup_tag_comparisons==on*(ways*(ways+1)/2+ways),"Lookup call/hit/probe contract differs");
        Require(s.lookup_or_allocate_calls==on*ways*2&&s.lookup_or_allocate_hits==on*ways&&s.lookup_or_allocate_tag_comparisons==on*(ways*ways+ways*(ways+1)/2),"LookupOrAllocate call/hit/probe contract differs");
    }
}
int main(){Sampler();Counters();std::cout<<"Decode sample hash/cap/clocks/calibration/frame contracts and 4/8-way lookup counter contracts passed\n";}
