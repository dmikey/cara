// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

static int VerifyFastInterpVldrPairs(Core::System& system) {
    using namespace Core::FastInterp;
    Memory::MemorySystem memory;
    Core::Timing timing;
    auto cpu = std::make_unique<ARM_FastInterp>(system, memory, 0, timing.GetTimer(0));
    auto table = std::make_shared<Memory::PageTable>();
    std::array<u32,1024> code{};
    std::array<u8,4096> first_page{}, second_page{};
    for (unsigned i=0;i<4096;++i) {
        first_page[i]=static_cast<u8>((i*37+19) ^ (i>>5));
        second_page[i]=static_cast<u8>((i*71+13) ^ (i>>3));
    }
    table->attributes[1]=table->attributes[3]=table->attributes[4]=Memory::PageType::Memory;
    table->pointers[1]=TestMemory(code);table->pointers[3]=TestMemory(first_page);
    table->pointers[4]=TestMemory(second_page);
    memory.SetCurrentPageTable(table);cpu->SetPageTable(table);
    const auto word = [](unsigned sd, unsigned rn, int offset, unsigned condition) {
        return (condition<<28)|0x0d100a00u|(offset>=0?1u<<23:0)|((sd&1)<<22)|
               (rn<<16)|((sd/2)<<12)|unsigned(std::abs(offset)/4);
    };
    const auto expected_word = [&](u32 address) {
        u32 result=0;
        for(unsigned i=0;i<4;++i) {
            const u32 at=address+i;
            const auto byte=at<0x4000?first_page[at-0x3000]:second_page[at-0x4000];
            result|=u32(byte)<<(8*i);
        }
        return result;
    };
    struct Result {
        std::array<u32,16> regs{};std::array<u32,64> vfp{};
        u32 cpsr{},fpscr{};u64 ticks{};
        bool operator==(const Result&) const=default;
    };
    const auto snapshot = [&](u64 ticks) {
        Result r;r.ticks=ticks;r.cpsr=cpu->GetCPSR();r.fpscr=cpu->GetVFPSystemReg(VFP_FPSCR);
        for(unsigned i=0;i<16;++i)r.regs[i]=cpu->GetReg(i);
        for(unsigned i=0;i<64;++i)r.vfp[i]=cpu->GetVFPReg(i);
        return r;
    };
    const auto initialize = [&](u32 address,u32 flags) {
        cpu->SetCPSR(flags|0x10);cpu->SetPC(0x1000);
        for(unsigned i=0;i<15;++i)cpu->SetReg(i,0x70000000u+i);
        cpu->SetReg(1,address);
        for(unsigned i=0;i<64;++i)cpu->SetVFPReg(i,0x7f800001u+i);
        cpu->SetVFPSystemReg(VFP_FPSCR,0x0303009fu);
    };
    unsigned cases=0,selected=0;
    u64 digest=1469598103934665603ull;
    const auto hash=[&](u64 value){digest=(digest^value)*1099511628211ull;};
    for(unsigned prefix=0;prefix<32;++prefix)
    for(u32 address:{0x3020u,0x3fd0u,0x3fe0u,0x3ff0u,0x3ffcu,0x3fffu,0x4000u,0x4001u})
    for(int stride:{-16,-4,0,4,16})
    for(auto destinations:{std::pair{0u,1u},std::pair{9u,8u},std::pair{31u,0u},std::pair{7u,7u}})
    for(unsigned conditions:{0xeeu,0xe0u,0x0eu,0x00u})
    for(u32 flags:{0u,0x40000000u})
    for(bool al:{false,true}) {
        code.fill(0xf0000000u);
        for(unsigned i=0;i<prefix;++i)code[i]=0xe1a05005u;
        code[prefix]=word(destinations.first,1,0,conditions>>4);
        code[prefix+1]=word(destinations.second,1,stride,conditions&15);
        DecodeRecordSelfTestAccess::ConfigureAlDispatch(*cpu,al);
        initialize(address,flags);
        BasicBlock candidate{};candidate.start_pc=0x1000;
        DecodeRecordSelfTestAccess::Decode(*cpu,candidate,0x1000);
        BasicBlock baseline=candidate;
        for(unsigned i=0;i<baseline.inst_count;++i) {
            if(baseline.instructions[i].GetOpcode()!=Opcode::VldrPairS)continue;
            ++selected;
            if(i+1>=baseline.inst_count || baseline.instructions[i].inst_size!=4 ||
               baseline.instructions[i+1].inst_size!=4 || conditions!=0xee) {
                std::cerr<<"VLDR pair altered original decode boundary\n";return 1;
            }
            baseline.instructions[i].SetOpcode(Opcode::VldrS);
            baseline.instructions[i].dispatch_index=unsigned(Opcode::VldrS)+(al?unsigned(Opcode::OpcodeCount):0);
        }
        const auto expected=snapshot(DecodeRecordSelfTestAccess::ExecuteOnce(*cpu,baseline));
        initialize(address,flags);
        // Context round trips must retain all source/destination FP bits and controls.
        if(cases%3==0) {
            auto saved=cpu->NewContext();cpu->SaveContext(saved);
            cpu->SetVFPReg(destinations.first,0);cpu->SetVFPSystemReg(VFP_FPSCR,0);
            cpu->LoadContext(saved);
        }
        const auto actual=snapshot(DecodeRecordSelfTestAccess::ExecuteOnce(*cpu,candidate));
        if(actual!=expected) {
            std::cerr<<"VLDR pair differential mismatch prefix="<<prefix<<" address="<<std::hex<<address
                     <<" stride="<<std::dec<<stride<<" conditions="<<conditions<<'\n';return 1;
        }
        if(prefix==0 && conditions==0xee) {
            if(actual.vfp[destinations.second]!=expected_word(address+stride) ||
               (destinations.first!=destinations.second && actual.vfp[destinations.first]!=expected_word(address))) {
                std::cerr<<"VLDR fixed byte oracle mismatch\n";return 1;
            }
        }
        hash(actual.ticks);hash(actual.regs[15]);hash(actual.vfp[destinations.first]);hash(actual.vfp[destinations.second]);++cases;
    }
    // A PC source requires per-instruction pipeline setup, so is never paired.
    code.fill(0xf0000000u);code[0]=word(0,15,0,14);code[1]=word(1,15,0,14);
    initialize(0x3020,0);BasicBlock pc_block{};pc_block.start_pc=0x1000;
    DecodeRecordSelfTestAccess::Decode(*cpu,pc_block,0x1000);
    for(unsigned i=0;i<pc_block.inst_count;++i)if(pc_block.instructions[i].GetOpcode()==Opcode::VldrPairS){std::cerr<<"PC source paired\n";return 1;}
    // Public Step must stop after the first instruction, even with pairing enabled.
    code[0]=word(0,1,0,14);code[1]=word(1,1,16,14);cpu->ClearInstructionCache();initialize(0x3020,0);
    const auto before=timing.GetTimer(0)->GetDowncount();cpu->Step();
    if(cpu->GetPC()!=0x1004 || cpu->GetVFPReg(0)!=expected_word(0x3020) || cpu->GetVFPReg(1)!=0x7f800002u ||
       before-timing.GetTimer(0)->GetDowncount()!=Core::TicksForInstruction(false,code[0])) {
        std::cerr<<"VLDR pair single-step boundary mismatch\n";return 1;
    }
    // A mapped register array deliberately aliases a load destination with the
    // next load's source. The second load must observe the first store.
    table->attributes[5]=Memory::PageType::Memory;
    table->pointers[5]=DecodeRecordSelfTestAccess::VfpBacking(*cpu);
    code.fill(0xf0000000u);code[0]=word(0,1,4,14);code[1]=word(1,1,0,14);
    initialize(0x5000,0);BasicBlock alias{};alias.start_pc=0x1000;
    DecodeRecordSelfTestAccess::Decode(*cpu,alias,0x1000);
    DecodeRecordSelfTestAccess::ExecuteOnce(*cpu,alias);
    if(cpu->GetVFPReg(0)!=0x7f800002u || cpu->GetVFPReg(1)!=0x7f800002u) {
        std::cerr<<"VLDR register-backed memory ordering mismatch\n";return 1;
    }
    table->pointers[5]=nullptr;table->attributes[5]=Memory::PageType::Unmapped;

    struct Device final : Memory::MMIORegion {
        ARM_FastInterp& cpu;std::vector<u32> reads;bool ordered=true,unexpected=false;
        explicit Device(ARM_FastInterp& cpu):cpu(cpu){}
        bool IsValidAddress(VAddr a) override{return a>=0x6000 && a<0x7000;}
        u32 Read32(VAddr a) override {
            reads.push_back(a);
            if(reads.size()==2)ordered &= cpu.GetVFPReg(0)==0x1234abcd;
            return reads.size()==1?0x1234abcd:0x89abcdef;
        }
        u8 Read8(VAddr) override{unexpected=true;return 0;}
        u16 Read16(VAddr) override{unexpected=true;return 0;}
        u64 Read64(VAddr) override{unexpected=true;return 0;}
        bool ReadBlock(VAddr,void*,std::size_t) override{unexpected=true;return false;}
        void Write8(VAddr,u8) override{unexpected=true;}
        void Write16(VAddr,u16) override{unexpected=true;}
        void Write32(VAddr,u32) override{unexpected=true;}
        void Write64(VAddr,u64) override{unexpected=true;}
        bool WriteBlock(VAddr,const void*,std::size_t) override{unexpected=true;return false;}
    };
    auto device=std::make_shared<Device>(*cpu);memory.MapIoRegion(*table,0x6000,4096,device);
    code[0]=word(0,1,0,14);code[1]=word(1,1,16,14);
    initialize(0x6000,0);BasicBlock mmio{};mmio.start_pc=0x1000;
    DecodeRecordSelfTestAccess::Decode(*cpu,mmio,0x1000);
    DecodeRecordSelfTestAccess::ExecuteOnce(*cpu,mmio);
    if(device->reads!=std::vector<u32>{0x6000,0x6010} || !device->ordered || device->unexpected ||
       cpu->GetVFPReg(0)!=0x1234abcd || cpu->GetVFPReg(1)!=0x89abcdef || table->GetPointerArray()[6]) {
        std::cerr<<"VLDR MMIO access order/fallback mismatch\n";return 1;
    }
    // Real rasterizer tracking clears direct RAM pointers. The host fixture has
    // no renderer; this proves dispatch takes coherent reads, not device flushing.
    memory.RegisterPageTable(table);
    memory.MapMemoryRegion(*table,Memory::VRAM_VADDR,4096,memory.GetPhysicalRef(Memory::VRAM_PADDR));
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR,4096,true);
    auto* tracked=memory.GetPhysicalPointer(Memory::VRAM_PADDR);
    const u32 a=0x7fcabcde,b=0x80000000;
    std::memcpy(tracked,&a,4);std::memcpy(tracked+16,&b,4);
    initialize(Memory::VRAM_VADDR,0);BasicBlock coherent{};coherent.start_pc=0x1000;
    DecodeRecordSelfTestAccess::Decode(*cpu,coherent,0x1000);
    DecodeRecordSelfTestAccess::ExecuteOnce(*cpu,coherent);
    if(table->GetPointerArray()[Memory::VRAM_VADDR>>Memory::PAGE_BITS] ||
       cpu->GetVFPReg(0)!=a || cpu->GetVFPReg(1)!=b) {
        std::cerr<<"VLDR tracked-page fallback mismatch\n";return 1;
    }
    memory.RasterizerMarkRegionCached(Memory::VRAM_PADDR,4096,false);

    // Identical trace across env=0/env=1 runs proves public timer-budget and
    // cached block behavior, including one-tick budgets and original31-slot ends.
    u64 run_digest=1469598103934665603ull;
    const auto run_hash=[&](u64 value){run_digest=(run_digest^value)*1099511628211ull;};
    for(unsigned prefix=0;prefix<32;++prefix) {
        code.fill(0xe1a05005u);
        code[prefix]=word(9,1,0,14);code[prefix+1]=word(8,1,16,14);
        code[prefix+2]=0xe2800001u;
        code[prefix+3]=0xea000000u|(u32(-s32(prefix+5))&0xffffffu);
        cpu->ClearInstructionCache();initialize(0x3020,0);cpu->SetReg(0,0);
        for(unsigned slice=0;slice<96;++slice) {
            const std::array<unsigned,8> budgets{1,2,3,4,5,31,32,127};
            timing.GetTimer(0)->Advance(budgets[slice%budgets.size()]);
            const auto start=timing.GetTimer(0)->GetDowncount();cpu->Run();
            run_hash(start-timing.GetTimer(0)->GetDowncount());
            for(unsigned i=0;i<16;++i)run_hash(cpu->GetReg(i));
            for(unsigned i=0;i<64;++i)run_hash(cpu->GetVFPReg(i));
            run_hash(cpu->GetCPSR());run_hash(cpu->GetVFPSystemReg(VFP_FPSCR));
            if(slice%7==0){auto saved=cpu->NewContext();cpu->SaveContext(saved);cpu->SetReg(0,0);cpu->LoadContext(saved);}
        }
    }
    // Invalidation of only the second original word must re-decode the pair.
    code.fill(0xeafffffeu);code[0]=word(0,1,0,14);code[1]=word(1,1,16,14);
    cpu->ClearInstructionCache();
    for(unsigned changed=0;changed<2;++changed) {
        if(changed){code[1]=word(1,1,-16,14);cpu->InvalidateCacheRange(0x1004,4);}
        initialize(0x3020,0);timing.GetTimer(0)->Advance(1);cpu->Run();
        if(cpu->GetVFPReg(1)!=expected_word(changed?0x3010:0x3030)) {
            std::cerr<<"VLDR consumed-slot invalidation mismatch\n";return 1;
        }
    }
    if(std::getenv("CARA_FASTINTERP_VLDR_PAIR_MICROBENCH")) {
        // Synthetic dispatch measurement only: not a game or GPU benchmark.
        code.fill(0xf0000000u);
        for(unsigned i=0;i<30;++i)code[i]=word((i*7)%32,1,int((i%8)*16),14);
        initialize(0x3020,0);BasicBlock paired{};paired.start_pc=0x1000;
        DecodeRecordSelfTestAccess::Decode(*cpu,paired,0x1000);
        BasicBlock scalar=paired;
        for(unsigned i=0;i<scalar.inst_count;++i)if(scalar.instructions[i].GetOpcode()==Opcode::VldrPairS) {
            scalar.instructions[i].SetOpcode(Opcode::VldrS);
            scalar.instructions[i].dispatch_index=unsigned(Opcode::VldrS)+unsigned(Opcode::OpcodeCount);
        }
        constexpr unsigned iterations=1000000;
        for(unsigned pass=0;pass<6;++pass) {
            auto& block=(pass%2)?paired:scalar;
            const auto start=std::chrono::steady_clock::now();
            u64 ticks=0;
            for(unsigned i=0;i<iterations;++i) {
                cpu->SetPC(0x1000);
                ticks+=DecodeRecordSelfTestAccess::ExecuteOnce(*cpu,block);
            }
            const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
            std::cout<<"VLDR synthetic paired="<<(pass%2)<<" ns_per_block="<<double(ns)/iterations
                     <<" tick_sum="<<ticks<<'\n';
        }
    }
    std::cout<<"VLDR pair cases="<<cases<<" selected="<<selected<<" differential_digest="<<digest
             <<" budget_trace="<<run_digest<<'\n';
    if(const char* enabled=std::getenv("CARA_FASTINTERP_VLDR_PAIRS");enabled && std::strcmp(enabled,"1")==0 && !selected) {
        std::cerr<<"Enabled VLDR pairing was never exercised\n";return 1;
    }
    return 0;
}
