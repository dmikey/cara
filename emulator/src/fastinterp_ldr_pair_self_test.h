// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

static int VerifyFastInterpLdrPairs(Core::System& system) {
    using namespace Core::FastInterp;
    Memory::MemorySystem memory;Core::Timing timing;
    auto cpu=std::make_unique<ARM_FastInterp>(system,memory,0,timing.GetTimer(0));
    auto table=std::make_shared<Memory::PageTable>();
    std::array<u32,1024> code{};std::array<u8,4096> first{},second{},wrapped{};
    for(unsigned i=0;i<4096;++i){first[i]=u8(i*37+19);second[i]=u8(i*71+13);wrapped[i]=u8(i*13+7);}
    table->attributes[0]=table->attributes[1]=table->attributes[3]=table->attributes[4]=Memory::PageType::Memory;
    table->pointers[0]=TestMemory(wrapped);table->pointers[1]=TestMemory(code);
    table->pointers[3]=TestMemory(first);table->pointers[4]=TestMemory(second);
    memory.SetCurrentPageTable(table);cpu->SetPageTable(table);
    const auto word=[](unsigned rd,unsigned rn,int offset,unsigned cond) {
        return (cond<<28)|0x05100000u|(offset>=0?1u<<23:0)|(rn<<16)|(rd<<12)|unsigned(std::abs(offset));
    };
    struct Result {std::array<u32,16> regs{};std::array<u32,64> vfp{};u32 cpsr{},fpscr{};u64 ticks{};
        bool operator==(const Result&)const=default;};
    const auto snapshot=[&](u64 ticks){Result r;r.ticks=ticks;r.cpsr=cpu->GetCPSR();r.fpscr=cpu->GetVFPSystemReg(VFP_FPSCR);
        for(unsigned i=0;i<16;++i)r.regs[i]=cpu->GetReg(i);
        for(unsigned i=0;i<64;++i)r.vfp[i]=cpu->GetVFPReg(i);return r;};
    const auto init=[&](u32 address,u32 flags){cpu->SetCPSR(flags|0x10);cpu->SetPC(0x1000);
        for(unsigned i=0;i<15;++i)cpu->SetReg(i,0x70000000u+i);cpu->SetReg(14,address);
        for(unsigned i=0;i<64;++i)cpu->SetVFPReg(i,0x7f800001u+i);
        cpu->SetVFPSystemReg(VFP_FPSCR,0x0303009fu);};
    const auto expected_word=[&](u32 address){u32 value=0;for(unsigned i=0;i<4;++i){const auto at=address+i;
        const auto byte=at<0x1000?wrapped[at]:at<0x4000?first[at-0x3000]:second[at-0x4000];value|=u32(byte)<<(8*i);}return value;};
    u64 digest=1469598103934665603ull;unsigned cases=0,selected=0;
    const auto hash=[&](u64 value){digest=(digest^value)*1099511628211ull;};
    for(unsigned prefix:{0u,1u,15u,30u,31u})
    for(u32 address:{0x3020u,0x3fd0u,0x3fe0u,0x3ff0u,0x3ffcu,0x3fffu,0x4000u,0x4001u})
    for(int stride:{-16,0,4,16})
    for(auto destinations:{std::pair{0u,1u},std::pair{9u,8u},std::pair{7u,7u},std::pair{3u,14u}})
    for(unsigned conditions:{0xeeu,0xe0u,0x0eu,0x00u})
    for(u32 flags:{0u,0x40000000u})for(bool al:{false,true})for(unsigned budget:{1u,1000000u}) {
        code.fill(0xf0000000u);for(unsigned i=0;i<prefix;++i)code[i]=0xe1a05005u;
        code[prefix]=word(destinations.first,14,0,conditions>>4);code[prefix+1]=word(destinations.second,14,stride,conditions&15);
        DecodeRecordSelfTestAccess::ConfigureAlDispatch(*cpu,al);init(address,flags);
        BasicBlock paired{};paired.start_pc=0x1000;DecodeRecordSelfTestAccess::Decode(*cpu,paired,0x1000);
        BasicBlock scalar=paired;
        for(unsigned i=0;i<scalar.inst_count;++i)if(scalar.instructions[i].GetOpcode()==Opcode::LdrPairImm){
            ++selected;
            if(i+1>=scalar.inst_count || scalar.instructions[i].inst_size!=4 || scalar.instructions[i+1].inst_size!=4 || conditions!=0xee){
                std::cerr<<"LDR pairing changed retained decode boundary\n";return 1;}
            scalar.instructions[i].SetOpcode(Opcode::LdrImm);
            scalar.instructions[i].dispatch_index=unsigned(Opcode::LdrImm)+(al?unsigned(Opcode::OpcodeCount):0);
        }
        timing.GetTimer(0)->Advance(budget);const auto expected=snapshot(DecodeRecordSelfTestAccess::ExecuteWithoutStep(*cpu,scalar));
        init(address,flags);timing.GetTimer(0)->Advance(budget);
        const auto actual=snapshot(DecodeRecordSelfTestAccess::ExecuteWithoutStep(*cpu,paired));
        if(actual!=expected){std::cerr<<"LDR pair differential mismatch prefix="<<prefix<<" address="<<std::hex<<address
            <<" stride="<<std::dec<<stride<<" conditions="<<conditions<<" budget="<<budget<<'\n';return 1;}
        if(prefix==0 && conditions==0xee && (actual.regs[destinations.second]!=expected_word(address+stride) ||
            (destinations.first!=destinations.second && actual.regs[destinations.first]!=expected_word(address)))){
            std::cerr<<"LDR fixed byte oracle mismatch\n";return 1;}
        hash(actual.ticks);for(auto value:actual.regs)hash(value);hash(actual.cpsr);hash(actual.fpscr);++cases;
    }
    const auto compare_once=[&](u32 base,u32 a,u32 b) {
        code.fill(0xf0000000u);code[0]=a;code[1]=b;cpu->ClearInstructionCache();init(base,0);
        BasicBlock paired{};paired.start_pc=0x1000;DecodeRecordSelfTestAccess::Decode(*cpu,paired,0x1000);BasicBlock scalar=paired;
        for(unsigned i=0;i<scalar.inst_count;++i)if(scalar.instructions[i].GetOpcode()==Opcode::LdrPairImm){
            scalar.instructions[i].SetOpcode(Opcode::LdrImm);scalar.instructions[i].dispatch_index=unsigned(Opcode::LdrImm)+unsigned(Opcode::OpcodeCount);}
        timing.GetTimer(0)->Advance(1000000);const auto expected=snapshot(DecodeRecordSelfTestAccess::ExecuteWithoutStep(*cpu,scalar));
        init(base,0);timing.GetTimer(0)->Advance(1000000);const auto actual=snapshot(DecodeRecordSelfTestAccess::ExecuteWithoutStep(*cpu,paired));
        return actual==expected;
    };
    // Unsigned address wrap can produce valid page-zero addresses. Each guest
    // address is wrapped independently, rather than widening across an extent.
    if(!compare_once(0xfffffffcu,word(0,14,8,14),word(1,14,12,14)) ||
       !compare_once(4,word(0,14,-4,14),word(1,14,0,14))){std::cerr<<"LDR address wrap mismatch\n";return 1;}
    // Decode dependency declines: first load changes the next base; PC sources
    // and destinations retain their original branch/pipeline setup.
    for(auto pair:{std::pair{word(14,14,0,14),word(0,14,4,14)},
                   std::pair{word(0,15,0,14),word(1,15,4,14)},
                   std::pair{word(15,14,0,14),word(1,14,4,14)}}){
        code.fill(0xf0000000u);code[0]=pair.first;code[1]=pair.second;BasicBlock declined{};declined.start_pc=0x1000;
        DecodeRecordSelfTestAccess::Decode(*cpu,declined,0x1000);
        for(unsigned i=0;i<declined.inst_count;++i)if(declined.instructions[i].GetOpcode()==Opcode::LdrPairImm){
            std::cerr<<"LDR PC/dependent-base pair admitted\n";return 1;}
    }
    code.fill(0xf0000000u);code[0]=word(0,14,0,14);code[1]=word(1,14,4,14);
    cpu->ClearInstructionCache();init(0x3020,0);timing.GetTimer(0)->Advance(100);
    const auto tick_start=timing.GetTimer(0)->GetDowncount();cpu->Step();
    if(cpu->GetPC()!=0x1004 || cpu->GetReg(0)!=expected_word(0x3020) || cpu->GetReg(1)!=0x70000001 ||
       tick_start-timing.GetTimer(0)->GetDowncount()!=Core::TicksForInstruction(false,code[0])){
        std::cerr<<"LDR public single-step boundary mismatch\n";return 1;}
    // Synthetic register-backed source must keep the original two reads and
    // the intervening register write through the scalar fallback.
    table->attributes[5]=Memory::PageType::Memory;table->pointers[5]=DecodeRecordSelfTestAccess::RegisterBacking(*cpu);
    code[0]=word(0,14,4,14);code[1]=word(1,14,0,14);cpu->ClearInstructionCache();init(0x5000,0);
    BasicBlock aliases{};aliases.start_pc=0x1000;DecodeRecordSelfTestAccess::Decode(*cpu,aliases,0x1000);timing.GetTimer(0)->Advance(1000000);
    DecodeRecordSelfTestAccess::ExecuteWithoutStep(*cpu,aliases);
    if(cpu->GetReg(0)!=0x70000001u || cpu->GetReg(1)!=0x70000001u){std::cerr<<"LDR register-source sequencing mismatch\n";return 1;}
    table->pointers[5]=nullptr;table->attributes[5]=Memory::PageType::Unmapped;
    struct Device final:Memory::MMIORegion {
        ARM_FastInterp& cpu;std::vector<u32> reads,pcs;bool ordered=true,throw_second=false;
        explicit Device(ARM_FastInterp& c):cpu(c){}
        bool IsValidAddress(VAddr a)override{return a>=0x6000&&a<0x7000;}
        u32 Read32(VAddr a)override{reads.push_back(a);pcs.push_back(cpu.GetPC());
            if(reads.size()==2){ordered&=cpu.GetReg(0)==0x1234abcd;if(throw_second)throw std::runtime_error("authored MMIO read failure");}
            return reads.size()==1?0x1234abcd:0x89abcdef;}
        u8 Read8(VAddr)override{return 0;}u16 Read16(VAddr)override{return 0;}u64 Read64(VAddr)override{return 0;}
        bool ReadBlock(VAddr,void*,std::size_t)override{return false;}
        void Write8(VAddr,u8)override{}void Write16(VAddr,u16)override{}void Write32(VAddr,u32)override{}void Write64(VAddr,u64)override{}
        bool WriteBlock(VAddr,const void*,std::size_t)override{return false;}
    };
    auto device=std::make_shared<Device>(*cpu);memory.MapIoRegion(*table,0x6000,4096,device);
    for(bool throws:{false,true}){
        code.fill(0xf0000000u);code[0]=word(0,14,0,14);code[1]=word(1,14,16,14);cpu->ClearInstructionCache();init(0x6000,0);
        BasicBlock paired{};paired.start_pc=0x1000;DecodeRecordSelfTestAccess::Decode(*cpu,paired,0x1000);BasicBlock scalar=paired;
        for(unsigned i=0;i<scalar.inst_count;++i)if(scalar.instructions[i].GetOpcode()==Opcode::LdrPairImm){
            scalar.instructions[i].SetOpcode(Opcode::LdrImm);scalar.instructions[i].dispatch_index=unsigned(Opcode::LdrImm)+unsigned(Opcode::OpcodeCount);}
        Result reference{};std::vector<u32> expected_pcs;
        for(unsigned pass=0;pass<2;++pass){init(0x6000,0);device->reads.clear();device->pcs.clear();device->ordered=true;device->throw_second=throws;
            bool caught=false;u64 ticks=0;timing.GetTimer(0)->Advance(1000000);
            try{ticks=DecodeRecordSelfTestAccess::ExecuteWithoutStep(*cpu,pass?paired:scalar);}catch(const std::runtime_error&){caught=true;}
            if(caught!=throws || !device->ordered || device->reads!=std::vector<u32>{0x6000,0x6010}){
                std::cerr<<"LDR MMIO order/exception mismatch\n";return 1;}
            const auto state=snapshot(ticks);if(!pass){reference=state;expected_pcs=device->pcs;}
            else if(state!=reference || device->pcs!=expected_pcs){std::cerr<<"LDR MMIO observable state changed\n";return 1;}
        }
    }
    // Real tracking removes direct pointers; the renderer-free fixture checks
    // coherent read dispatch, rather than claiming GPU flush coverage.
    memory.RegisterPageTable(table);memory.MapMemoryRegion(*table,Memory::VRAM_VADDR,4096,memory.GetPhysicalRef(Memory::VRAM_PADDR));
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR,4096,true);
    auto* tracked=memory.GetPhysicalPointer(Memory::VRAM_PADDR);const u32 x=0x0badcafe,y=0x12345678;
    std::memcpy(tracked,&x,4);std::memcpy(tracked+16,&y,4);
    if(!compare_once(Memory::VRAM_VADDR,word(0,14,0,14),word(1,14,16,14)) ||
       table->GetPointerArray()[Memory::VRAM_VADDR>>Memory::PAGE_BITS] || cpu->GetReg(0)!=x || cpu->GetReg(1)!=y){
        std::cerr<<"LDR tracked-page fallback mismatch\n";return 1;}
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR,4096,false);
    u64 run_digest=1469598103934665603ull;const auto run_hash=[&](u64 v){run_digest=(run_digest^v)*1099511628211ull;};
    for(unsigned prefix:{0u,1u,15u,30u,31u}){
        code.fill(0xe1a05005u);code[prefix]=word(9,14,0,14);code[prefix+1]=word(8,14,16,14);
        code[prefix+2]=0xe2800001u;code[prefix+3]=0xea000000u|(u32(-s32(prefix+5))&0xffffffu);
        cpu->ClearInstructionCache();init(0x3020,0);cpu->SetReg(0,0);
        for(unsigned slice=0;slice<96;++slice){const std::array<unsigned,8> budgets{1,2,3,4,5,31,32,127};
            timing.GetTimer(0)->Advance(budgets[slice%8]);const auto start=timing.GetTimer(0)->GetDowncount();cpu->Run();
            run_hash(start-timing.GetTimer(0)->GetDowncount());for(unsigned i=0;i<16;++i)run_hash(cpu->GetReg(i));run_hash(cpu->GetCPSR());
            if(slice%7==0){auto saved=cpu->NewContext();cpu->SaveContext(saved);cpu->SetReg(0,0);cpu->LoadContext(saved);}
        }
    }
    code.fill(0xeafffffeu);code[0]=word(0,14,0,14);code[1]=word(1,14,16,14);cpu->ClearInstructionCache();
    for(unsigned changed=0;changed<2;++changed){if(changed){code[1]=word(1,14,-16,14);cpu->InvalidateCacheRange(0x1004,4);}
        init(0x3020,0);timing.GetTimer(0)->Advance(1);cpu->Run();if(cpu->GetReg(1)!=expected_word(changed?0x3010:0x3030)){
            std::cerr<<"LDR second-slot invalidation mismatch\n";return 1;}}
    const auto counts=DecodeRecordSelfTestAccess::LdrPairCounts();
    const bool enabled=std::getenv("CARA_FASTINTERP_LDR_PAIRS")&&std::strcmp(std::getenv("CARA_FASTINTERP_LDR_PAIRS"),"1")==0;
    if(enabled && (!selected || !counts[0] || !counts[1])){std::cerr<<"LDR selected/fused/fallback paths not exercised\n";return 1;}
    std::cout<<"LDR pair cases="<<cases<<" selected="<<selected<<" fused="<<counts[0]<<" fallback="<<counts[1]
             <<" differential_digest="<<digest<<" budget_trace="<<run_digest<<'\n';return 0;
}
