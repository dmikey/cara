// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <string>
#include <vector>
#include "test_memory.h"

static int VerifyFastInterpDecodeCost(Core::System& system) {
    using namespace Core::FastInterp;
    struct Trace {
        std::vector<u64> state,events;
        bool operator==(const Trace&) const = default;
    };
    for(unsigned ways:{4u,8u}) {
        auto index=[ways](u32 pc) {u32 h=pc*0x9e3779b9u;h^=h>>16;return(h>>4)&((CACHE_SIZE/ways)-1);};
        std::vector<u32> pcs;
        for(u32 pc=0x1000;pcs.size()<ways+5;pc+=16)
            if(index(pc)==index(0x1000))pcs.push_back(pc);
        std::array<Trace,2> traces;
        for(unsigned mode=0;mode<2;++mode) {
            Memory::MemorySystem memory;Core::Timing timing;
            auto table=std::make_shared<Memory::PageTable>();
            auto bytes=std::make_shared<BufferMem>(0x100000);
            for(unsigned p=0;p<256;++p) {
                table->attributes[p]=Memory::PageType::Memory;
                table->pointers[p]=MemoryRef(bytes,p*4096);
            }
            memory.SetCurrentPageTable(table);
            auto put=[&](u32 address,u32 word) {std::memcpy(bytes->GetPtr()+address,&word,4);};
            auto branch=[](u32 pc,u32 target,bool link=false) {
                return (link?0xeb000000u:0xea000000u)|(((target-pc-8)>>2)&0xffffff);
            };
            for(unsigned i=0;i<pcs.size();++i) {
                put(pcs[i],0xe2800001); // ADD r0,#1
                put(pcs[i]+4,branch(pcs[i]+4,0xf0000,true));
                put(pcs[i]+8,branch(pcs[i]+8,pcs[(i+1)%pcs.size()]));
            }
            put(0xf0000,0xe2855001);put(0xf0004,0xe12fff1e); // ADD r5,#1; BX lr
            std::unique_ptr<ARM_FastInterp> cpu;
            {
                const std::array<const char*,3> names{"CARA_FASTINTERP_CACHE_PROFILE","CARA_FASTINTERP_CACHE_WAYS","CARA_FASTINTERP_CHAIN_BUDGET"};
                std::array<std::string,3> saved;std::array<bool,3> present{};
                for(unsigned i=0;i<3;++i)if(const char* value=std::getenv(names[i])){present[i]=true;saved[i]=value;}
                setenv(names[0],mode?"1":"0",1);setenv(names[1],ways==8?"8":"4",1);setenv(names[2],"1",1);
                cpu=std::make_unique<ARM_FastInterp>(system,memory,0,timing.GetTimer(0));
                for(unsigned i=0;i<3;++i) {
                    if(present[i])setenv(names[i],saved[i].c_str(),1);else unsetenv(names[i]);
                }
            }
            cpu->SetPageTable(table);cpu->SetCPSR(0xa0000010);
            for(unsigned r=0;r<15;++r)cpu->SetReg(r,0);
            cpu->SetPC(pcs.front());
            auto& trace=traces[mode];
            const auto* event=timing.RegisterEvent("decode_cost_equivalence",[&](u64 value,int late) {
                trace.events.push_back(value);trace.events.push_back(static_cast<s64>(late));
                trace.events.push_back(cpu->GetPC());
            });
            for(unsigned slice=0;slice<512;++slice) {
                const unsigned budget=std::array<unsigned,8>{1,2,3,7,8,9,31,128}[slice%8];
                timing.GetTimer(0)->Advance(budget);timing.ScheduleEvent(budget,event,slice,0);
                const auto before=timing.GetTimer(0)->GetDowncount();cpu->Run();
                trace.state.push_back(before-timing.GetTimer(0)->GetDowncount());
                for(unsigned r=0;r<16;++r)trace.state.push_back(cpu->GetReg(r));
                trace.state.push_back(cpu->GetCPSR());
                trace.state.push_back(DecodeRecordSelfTestAccess::CacheDigest(*cpu));
                // A genuine text edit/invalidation amid cache churn must retain
                // identical live blocks, stale-chain rejection and guest work.
                if(slice==255){put(0xf0000,0xe2855002);cpu->InvalidateCacheRange(0xf0000,4);}
            }
            timing.GetTimer(0)->Advance(1);timing.RemoveEvent(event);
            // One actual same-PC ARM/Thumb mode redecode, excluded from timing
            // samples but included in the full decode-reason count.
            put(0x80000,0xeafffffe);cpu->SetPC(0x80000);
            timing.GetTimer(0)->Advance(8);
            auto mode_before=timing.GetTimer(0)->GetDowncount();cpu->Run();
            trace.state.push_back(mode_before-timing.GetTimer(0)->GetDowncount());
            put(0x80000,0xe7fee7fe);cpu->SetPC(0x80001);
            timing.GetTimer(0)->Advance(8);mode_before=timing.GetTimer(0)->GetDowncount();cpu->Run();
            trace.state.push_back(mode_before-timing.GetTimer(0)->GetDowncount());
            trace.state.push_back(cpu->GetPC());trace.state.push_back(cpu->GetCPSR());
            trace.state.push_back(DecodeRecordSelfTestAccess::CacheDigest(*cpu));
            const auto& stats=DecodeRecordSelfTestAccess::DecodeCostStats(*cpu);
            const auto& cache=DecodeRecordSelfTestAccess::CacheStats(*cpu);
            if(!mode) {
                if(stats.clock_reads||stats.decode_calls||cache.lookup_calls||cache.lookup_or_allocate_calls) {
                    std::cerr<<"Disabled decode diagnostics recorded work\n";return 1;
                }
            } else if(stats.decode_calls!=stats.allocation_miss_decodes+stats.mode_redecodes ||
                stats.mode_redecodes!=1 || stats.allocation_miss_decodes!=cache.allocation_misses ||
                !stats.timed_samples || stats.clock_reads!=4*(stats.timed_samples+DecodeCostProfile::CalibrationPairs) ||
                stats.clock_reads>4*(DecodeCostProfile::MaxSamples+DecodeCostProfile::CalibrationPairs) ||
                stats.chain_fallback_lookups!=cache.lookup_calls ||
                !stats.chain_primary_dispatches || !stats.chain_self_dispatches ||
                !cache.lookup_hits || !cache.lookup_or_allocate_hits ||
                stats.observed_frames!=1 || stats.frame_resets || stats.frame_gaps) {
                std::cerr<<"Actual decode/lookup/frame counter contract failed ways="<<ways<<'\n';return 1;
            }
            std::ostringstream exported;cpu->WriteBlockProfile(exported);
            if(exported.str().find("\"decode_cost_profile\":{")==std::string::npos) {
                std::cerr<<"Decode diagnostics missing from census\n";return 1;
            }
        }
        if(!(traces[0]==traces[1])) {
            std::cerr<<"Cache profile off/on changed 512-slice register/tick/event/cache-victim trace ways="<<ways<<'\n';return 1;
        }
    }
    std::cout<<"4/8-way cache profile off/on: 1024 collision slices, exact registers/ticks/event lateness, resident/victim/chain identities, mode redecode and text invalidation passed\n";
    return 0;
}
