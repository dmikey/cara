// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
static int VerifyFastInterpSecondaryLink(Core::System& system) {
    using namespace Core::FastInterp;
    unsigned cases=0;u64 hits=0;
    for(unsigned ways:{4u,8u})for(bool al:{false,true})for(unsigned scale:{75u,100u,133u})
        for(bool svc:{false,true}) {
        std::array<std::vector<u64>,2> traces;
        for(unsigned mode=0;mode<2;++mode) {
            Memory::MemorySystem memory;Core::Timing timing;timing.UpdateClockSpeed(scale);
            std::array<u32,1024> words{},alternate{};auto table=std::make_shared<Memory::PageTable>();
            table->attributes[1]=Memory::PageType::Memory;table->pointers[1]=TestMemory(words);
            memory.SetCurrentPageTable(table);
            auto branch=[](u32 pc,u32 target){return 0xea000000u|(((target-pc-8)>>2)&0xffffff);};
            words.fill(0xeafffffe);
            words[0]=0xe2200001;words[1]=0xe3100001; // EOR r0,#1; TST r0,#1
            words[2]=(branch(0x1008,0x1100)&0x0fffffff)|0x10000000;
            words[3]=branch(0x100c,0x1200);
            words[64]=0xe2811001;words[65]=svc?0xef0000ff:branch(0x1104,0x1000);
            words[66]=branch(0x1108,0x1000);
            words[128]=0xe2822001;words[129]=branch(0x1204,0x1000);
            const char* env=std::getenv("CARA_FASTINTERP_CACHE_WAYS");
            const bool present=env;const std::string saved=env?env:"";
            setenv("CARA_FASTINTERP_CACHE_WAYS",ways==8?"8":"4",1);
            auto cpu=std::make_unique<ARM_FastInterp>(system,memory,0,timing.GetTimer(0));
            if(present)setenv("CARA_FASTINTERP_CACHE_WAYS",saved.c_str(),1);else unsetenv("CARA_FASTINTERP_CACHE_WAYS");
            DecodeRecordSelfTestAccess::ConfigureAlDispatch(*cpu,al);
            DecodeRecordSelfTestAccess::ConfigureSecondary(*cpu,mode);
            cpu->SetPageTable(table);cpu->SetCPSR(0xa0000010);cpu->SetPC(0x1000);
            for(unsigned r=0;r<15;++r)cpu->SetReg(r,0);
            auto& trace=traces[mode];
            const auto* event=timing.RegisterEvent("secondary_link_equivalence",[&](u64 value,int late){
                trace.push_back(value);trace.push_back(static_cast<s64>(late));trace.push_back(cpu->GetPC());
            });
            auto& counters=Common::FrameTiming::counters;counters.enabled=true;
            counters.guest_ticks=0;counters.guest_core_ticks.fill(0);counters.last_guest_pc.fill(0);
            for(unsigned slice=0;slice<512;++slice) {
                unsigned budget=std::array<unsigned,10>{1,2,3,7,8,17,31,128,257,1000}[slice%10];
                timing.GetTimer(0)->Advance(budget);timing.ScheduleEvent(budget,event,slice,0);
                const auto before=timing.GetTimer(0)->GetDowncount();cpu->Run();
                trace.push_back(before-timing.GetTimer(0)->GetDowncount());
                for(unsigned r=0;r<16;++r)trace.push_back(cpu->GetReg(r));
                trace.push_back(cpu->GetCPSR());trace.push_back(counters.guest_ticks);
                trace.push_back(counters.guest_core_ticks[0]);trace.push_back(counters.last_guest_pc[0]);
                trace.push_back(DecodeRecordSelfTestAccess::CacheDigest(*cpu));
                const auto& p=DecodeRecordSelfTestAccess::CacheStats(*cpu);
                for(u64 value:{p.lookup_calls,p.lookup_hits,p.lookup_tag_comparisons,p.lookup_or_allocate_calls,
                        p.lookup_or_allocate_hits,p.lookup_or_allocate_tag_comparisons,p.allocation_misses,
                        p.live_replacements,p.invalidated_entries})trace.push_back(value);
                if(slice==255){words[64]=0xe2811002;cpu->InvalidateCacheRange(0x1100,4);}
                if(slice==319) {
                    words[0]=0xe7fe3001;cpu->SetPC(0x1001); // Same-PC ARM -> Thumb redecode.
                }
                if(slice==321) {
                    words[0]=0xe2200001;cpu->SetPC(0x1000); // Same-PC Thumb -> ARM redecode.
                }
                if(slice==383)cpu->ClearInstructionCache();
                if(slice==447) {
                    alternate=words;alternate[64]=0xe2811003;
                    auto changed=std::make_shared<Memory::PageTable>();
                    changed->attributes[1]=Memory::PageType::Memory;
                    changed->pointers[1]=TestMemory(alternate);
                    memory.SetCurrentPageTable(changed);cpu->SetPageTable(changed);
                }
            }
            timing.GetTimer(0)->Advance(1);timing.RemoveEvent(event);counters.enabled=false;
            const auto& p=DecodeRecordSelfTestAccess::CacheStats(*cpu);
            if(!mode&&p.secondary_probes){std::cerr<<"Disabled secondary made a probe\n";return 1;}
            hits+=p.secondary_hits;
        }
        if(traces[0]!=traces[1]) {std::cerr<<"Secondary CPU trace mismatch ways="<<ways<<" al="<<al<<" scale="<<scale<<" svc="<<svc<<'\n';return 1;}
        cases+=512;
    }
    if(!hits){std::cerr<<"Secondary CPU fixture did not exercise a shortcut\n";return 1;}
    std::cout<<cases<<" CPU slices: secondary off/on exact registers/flags/ticks/events/counters/victims, AL combinations, 4/8 ways and 75/100/133 percent clocks; hits="<<hits<<'\n';return 0;
}
