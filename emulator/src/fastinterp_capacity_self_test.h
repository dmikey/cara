// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
static int VerifyFastInterpCapacity(Core::System& system) {
    using namespace Core::FastInterp;
    unsigned slices=0;u64 small_misses=0,large_misses=0;
    for(unsigned ways:{4u,8u})for(bool al:{false,true})for(bool secondary:{false,true})
        for(unsigned scale:{75u,100u,133u}) {
        // Force collisions under 16k; the extra set bit at 32k splits this
        // working set. Victim identities intentionally differ across capacities.
        auto index=[ways](u32 pc){u32 h=pc*0x9e3779b9u;h^=h>>16;return(h>>4)&(DEFAULT_CACHE_SIZE/ways-1);};
        std::vector<u32> pcs;for(u32 pc=0x1000;pcs.size()<ways+5;pc+=16)
            if(index(pc)==index(0x1000))pcs.push_back(pc);
        if(pcs.back()+8>=0x100000){std::cerr<<"Capacity fixture code exceeds RAM\n";return 1;}
        std::array<std::vector<u64>,2> public_traces;
        for(unsigned mode=0;mode<2;++mode) {
            const unsigned capacity=mode?MAX_CACHE_SIZE:DEFAULT_CACHE_SIZE;
            Memory::MemorySystem memory;Core::Timing timing,oracle;
            timing.UpdateClockSpeed(scale);oracle.UpdateClockSpeed(scale);
            auto table=std::make_shared<Memory::PageTable>();auto bytes=std::make_shared<BufferMem>(0x100000);
            for(unsigned page=0;page<256;++page){table->attributes[page]=Memory::PageType::Memory;table->pointers[page]=MemoryRef(bytes,page*4096);}
            memory.SetCurrentPageTable(table);
            auto put=[&](u32 pc,u32 word){std::memcpy(bytes->GetPtr()+pc,&word,4);};
            std::vector<unsigned> increments(pcs.size(),1),ticks(pcs.size());
            for(unsigned i=0;i<pcs.size();++i){
                u32 branch=0xea000000u|(((pcs[(i+1)%pcs.size()]-pcs[i]-12)>>2)&0xffffff);
                put(pcs[i],0xe2800001);put(pcs[i]+4,branch);
                ticks[i]=Core::TicksForInstruction(false,0xe2800001)+Core::TicksForInstruction(false,branch);
            }
            const std::array<const char*,3> names{"CARA_FASTINTERP_CACHE_ENTRIES","CARA_FASTINTERP_CACHE_WAYS","CARA_FASTINTERP_CHAIN_BUDGET"};
            std::array<std::string,3> saved;std::array<bool,3> present{};
            for(unsigned i=0;i<3;++i)if(const char* value=std::getenv(names[i])){present[i]=true;saved[i]=value;}
            setenv(names[0],mode?"32768":"16384",1);setenv(names[1],ways==8?"8":"4",1);setenv(names[2],"1",1);
            auto cpu=std::make_unique<ARM_FastInterp>(system,memory,0,timing.GetTimer(0));
            for(unsigned i=0;i<3;++i)if(present[i])setenv(names[i],saved[i].c_str(),1);else unsetenv(names[i]);
            DecodeRecordSelfTestAccess::ConfigureAlDispatch(*cpu,al);
            DecodeRecordSelfTestAccess::ConfigureSecondary(*cpu,secondary);
            cpu->SetPageTable(table);cpu->SetCPSR(0xa0000010);for(unsigned r=0;r<15;++r)cpu->SetReg(r,0);cpu->SetPC(pcs.front());
            if(DecodeRecordSelfTestAccess::CacheCapacity(*cpu)!=capacity){std::cerr<<"Capacity changed after construction\n";return 1;}
            u32 counter=0;unsigned current=0;u64 guest_ticks=0;std::vector<u64> events,expected_events;
            const auto* event=timing.RegisterEvent("capacity_actual",[&](u64 v,int late){events.insert(events.end(),{v,u64(s64(late)),cpu->GetPC()});});
            const auto* expected_event=oracle.RegisterEvent("capacity_oracle",[&](u64 v,int late){expected_events.insert(expected_events.end(),{v,u64(s64(late)),pcs[current]});});
            auto& counters=Common::FrameTiming::counters;counters.enabled=true;counters.guest_ticks=0;counters.guest_core_ticks.fill(0);counters.last_guest_pc.fill(0);
            auto& trace=public_traces[mode];
            for(unsigned slice=0;slice<512;++slice) {
                unsigned budget=std::array<unsigned,10>{1,2,3,7,8,17,31,128,257,1000}[slice%10];
                timing.GetTimer(0)->Advance(budget);oracle.GetTimer(0)->Advance(budget);
                timing.ScheduleEvent(budget,event,slice,0);oracle.ScheduleEvent(budget,expected_event,slice,0);
                const auto before=timing.GetTimer(0)->GetDowncount(),expected_before=oracle.GetTimer(0)->GetDowncount();
                cpu->Run();
                // Independent oracle advances one complete original ADD+B
                // block at a time, not cache hits, chains or decoded handlers.
                while(oracle.GetTimer(0)->GetDowncount()>0){counter+=increments[current];guest_ticks+=ticks[current];oracle.GetTimer(0)->AddTicks(ticks[current]);current=(current+1)%pcs.size();}
                if(cpu->GetReg(0)!=counter||cpu->GetPC()!=pcs[current]||cpu->GetCPSR()!=0xa0000010||
                    before-timing.GetTimer(0)->GetDowncount()!=expected_before-oracle.GetTimer(0)->GetDowncount()||
                    counters.guest_ticks!=guest_ticks||counters.guest_core_ticks[0]!=guest_ticks||
                    counters.last_guest_pc[0]!=pcs[current]||events!=expected_events) {
                    std::cerr<<"Capacity timing oracle mismatch cap="<<capacity<<" ways="<<ways<<" scale="<<scale<<" slice="<<slice<<'\n';return 1;
                }
                for(unsigned r=1;r<15;++r)if(cpu->GetReg(r)){std::cerr<<"Capacity oracle changed unrelated register\n";return 1;}
                trace.insert(trace.end(),{counter,pcs[current],guest_ticks,u64(timing.GetTimer(0)->GetDowncount())});
                if(slice==255){put(pcs[0],0xe2800002);increments[0]=2;cpu->InvalidateCacheRange(pcs[0],4);}
                if(slice==383)cpu->ClearInstructionCache();
                ++slices;
            }
            timing.GetTimer(0)->Advance(1);oracle.GetTimer(0)->Advance(1);timing.RemoveEvent(event);oracle.RemoveEvent(expected_event);counters.enabled=false;
            if(events!=expected_events){std::cerr<<"Capacity final event lateness mismatch\n";return 1;}
            trace.insert(trace.end(),events.begin(),events.end());
            const auto misses=DecodeRecordSelfTestAccess::CacheStats(*cpu).allocation_misses;
            if(mode)large_misses+=misses;else small_misses+=misses;
            std::ostringstream census;cpu->WriteBlockProfile(census);
            if(census.str().find(mode?"\"cache_entries\":32768":"\"cache_entries\":16384")==std::string::npos){std::cerr<<"Capacity census differs\n";return 1;}
        }
        if(public_traces[0]!=public_traces[1]){std::cerr<<"16k/32k guest trace differs\n";return 1;}
    }
    if(large_misses>=small_misses){std::cerr<<"Capacity fixture did not reduce forced conflict misses\n";return 1;}
    std::cout<<slices<<" 16k/32k slices match independent block/timer/event oracle; AL/secondary combinations, 4/8 ways, 75/100/133 percent clocks, invalidation/clear; misses="<<small_misses<<'/'<<large_misses<<'\n';return 0;
}
