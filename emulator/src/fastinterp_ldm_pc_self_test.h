// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <sstream>
#include "core/mmio.h"
#include "test_memory.h"

static int VerifyFastInterpLdmPc(Core::System& system, Core::Timing& timing) {
    using namespace Core::FastInterp;
    Memory::MemorySystem memory;
    auto table = std::make_shared<Memory::PageTable>();
    std::array<u32,1024> code{}, target{}, data{};
    for (auto [page, bytes] : {std::pair{1u,&code},std::pair{2u,&target},std::pair{4u,&data}}) {
        table->attributes[page] = Memory::PageType::Memory;
        table->pointers[page] = TestMemory(*bytes);
    }
    memory.SetCurrentPageTable(table);
    std::array<std::unique_ptr<ARM_FastInterp>,2> cpus;
    {
        const char* p = std::getenv("CARA_FASTINTERP_PRECISE_BLOCK_END");
        const char* c = std::getenv("CARA_FASTINTERP_CHAIN_BUDGET");
        const bool had_p = p, had_c = c;
        const std::string saved_p = p ? p : "", saved_c = c ? c : "";
        setenv("CARA_FASTINTERP_CHAIN_BUDGET","1",1);
        for (unsigned mode=0;mode<2;++mode) {
            setenv("CARA_FASTINTERP_PRECISE_BLOCK_END",mode ? "1" : "0",1);
            cpus[mode] = std::make_unique<ARM_FastInterp>(system,memory,0,timing.GetTimer(0));
            cpus[mode]->SetPageTable(table);
        }
        if(had_p) setenv("CARA_FASTINTERP_PRECISE_BLOCK_END",saved_p.c_str(),1);
        else unsetenv("CARA_FASTINTERP_PRECISE_BLOCK_END");
        if(had_c) setenv("CARA_FASTINTERP_CHAIN_BUDGET",saved_c.c_str(),1);
        else unsetenv("CARA_FASTINTERP_CHAIN_BUDGET");
    }
    auto reset = [&](ARM_FastInterp& cpu,u32 cpsr=0x60000010) {
        cpu.ClearInstructionCache(); cpu.SetCPSR(cpsr);
        for(unsigned r=0;r<15;++r) cpu.SetReg(r,7+r);
        cpu.SetPC(0x1000);
    };
    auto run = [&](ARM_FastInterp& cpu) {
        timing.GetTimer(0)->Advance(1000);
        const auto before=timing.GetTimer(0)->GetDowncount();
        cpu.Run();
        return before-timing.GetTimer(0)->GetDowncount();
    };
    target = {0xe10f8000,0xe2899001,0xeafffffe}; // MRS r8,CPSR; ADD r9,#1; loop
    unsigned cases=0;
    // All four addressing modes, writeback on/off, legacy S-bit handling,
    // unconditional return and both outcomes of a conditional return.
    // S+PC does not implement hardware SPSR restoration in the existing handler;
    // this fixture checks its unchanged behavior, not exception-return conformance.
    for(bool up:{false,true}) for(bool before:{false,true})
        for(bool wb:{false,true}) for(bool s:{false,true})
            for(unsigned cond:{14u,0u,1u}) {
                const bool passes=cond!=1;
                const u32 base=0x4800;
                const u32 first=base+(up ? 0 : -12)+(up==before ? 4 : 0);
                data.fill(0);
                data[(first-0x4000)/4]=0x12345678;
                data[(first-0x4000)/4+1]=0x87654321;
                data[(first-0x4000)/4+2]=0x2000;
                code.fill(0xeafffffe);
                code[0]=(cond<<28)|0x08128011|(u32(up)<<23)|(u32(before)<<24)|
                    (u32(wb)<<21)|(u32(s)<<22);
                code[1]=0xe3a0a02a; code[2]=0xea0003fc; // fallthrough marker; B target
                for(unsigned mode=0;mode<2;++mode) {
                    auto& cpu=*cpus[mode]; reset(cpu); cpu.SetReg(2,base);
                    BasicBlock block{};block.start_pc=0x1000;
                    DecodeRecordSelfTestAccess::Decode(cpu,block,0x1000);
                    const bool shortened=mode && cond==14;
                    if((shortened && (block.end_pc!=0x1004 || block.inst_count!=1)) ||
                       (!shortened && block.end_pc!=0x100c)) {
                        std::cerr<<"LDM-PC decode span mismatch\n";return 1;
                    }
                    cpu.Step();
                    const u32 expected_base=wb&&passes ? base+(up ? 12 : -12) : base;
                    if(cpu.GetPC()!=(passes ? 0x2000u : 0x1004u) || cpu.GetCPSR()!=0x60000010 ||
                       cpu.GetReg(0)!=(passes ? 0x12345678u : 7u) ||
                       cpu.GetReg(4)!=(passes ? 0x87654321u : 11u) || cpu.GetReg(2)!=expected_base) {
                        std::cerr<<"LDM-PC Step/address-mode mismatch\n";return 1;
                    }
                    reset(cpu);cpu.SetReg(2,base);
                    // Cold and warm real Run paths: equal authored program,
                    // full budget consumed in the one-tick terminal loop.
                    for(unsigned warm=0;warm<2;++warm) {
                        if(warm) {cpu.SetCPSR(0x60000010);cpu.SetPC(0x1000);cpu.SetReg(2,base);cpu.SetReg(9,16);cpu.SetReg(10,17);}
                        const auto ticks=run(cpu);
                        if(ticks!=1000 || cpu.GetPC()!=0x2008 || cpu.GetCPSR()!=0x60000010 ||
                           cpu.GetReg(8)!=0x60000010 || cpu.GetReg(9)!=17 ||
                           cpu.GetReg(10)!=(passes ? 17u : 42u) || cpu.GetReg(2)!=expected_base) {
                            std::cerr<<"LDM-PC Run/tick trace mismatch mode="<<mode<<" warm="<<warm<<'\n';return 1;
                        }
                    }
                    ++cases;
                }
            }
    // Negative decode controls: LDM without PC and STM including PC must
    // continue; only a load into PC is a semantic return.
    for (u32 transfer : {0xe8b20011u,0xe8a28011u}) for (unsigned mode=0;mode<2;++mode) {
        code={transfer,0xe3a0a02a,0xea0003fc};
        auto& cpu=*cpus[mode];reset(cpu);
        BasicBlock block{};block.start_pc=0x1000;
        DecodeRecordSelfTestAccess::Decode(cpu,block,0x1000);
        if(block.end_pc!=0x100c) {std::cerr<<"LDM-PC negative admission failed\n";return 1;}
        ++cases;
    }
    // Independent instruction-Step oracle grouped at the retained legacy
    // block boundaries. A failed conditional LDM still shares its decoded
    // block with the fallthrough instructions, preserving slice/event behavior.
    for(bool passes:{false,true}) for(bool warm:{false,true})
        for(unsigned budget:{1u,2u,3u,7u,8u,9u,31u}) {
            code={passes ? 0x08b28011u : 0x18b28011u,0xe28aa001,0xe28bb001,0xea0003fb};
            target={0xe2899001,0xeafffffe};
            data[0]=0x12345678;data[1]=0x87654321;data[2]=0x2000;
            struct Trace {
                std::array<u32,16> regs{}; u32 cpsr=0; s64 consumed=0; int late=-999;
                bool operator==(const Trace&) const = default;
            };
            std::array<Trace,2> traces;
            for(unsigned actual=0;actual<2;++actual) {
                auto& cpu=*cpus[actual];auto& trace=traces[actual];
                reset(cpu);
                auto seed=[&] {
                    cpu.SetCPSR(0x60000010);
                    for(unsigned r=0;r<15;++r)cpu.SetReg(r,0);
                    cpu.SetReg(2,0x4000);cpu.SetPC(0x1000);
                };
                seed();if(actual&&warm) {run(cpu);seed();}
                const auto* event=timing.RegisterEvent("ldm_pc_budget_"+std::to_string(cases)+"_"+std::to_string(actual),
                    [&](u64,int late){trace.late=late;});
                timing.GetTimer(0)->Advance(budget);
                timing.ScheduleEvent(budget,event,0,0);
                const auto initial=timing.GetTimer(0)->GetDowncount();
                if(actual) cpu.Run();
                else {
                    cpu.Step(); // conditional LDM: existing canonical tick charge
                    if(!passes)
                        for(unsigned instruction=0;instruction<3;++instruction)cpu.Step();
                    if(timing.GetTimer(0)->GetDowncount()>0)
                        for(unsigned instruction=0;instruction<2;++instruction)cpu.Step();
                    while(timing.GetTimer(0)->GetDowncount()>0)cpu.Step();
                }
                trace.consumed=initial-timing.GetTimer(0)->GetDowncount();
                trace.cpsr=cpu.GetCPSR();
                for(unsigned r=0;r<16;++r)trace.regs[r]=cpu.GetReg(r);
                timing.GetTimer(0)->Advance(1);timing.RemoveEvent(event);
            }
            if(!(traces[0]==traces[1])) {
                std::cerr<<"LDM-PC canonical Step/budget/event oracle failed pass="<<passes<<" warm="<<warm
                    <<" budget="<<budget<<" expected/actual ticks="<<traces[0].consumed<<'/'<<traces[1].consumed
                    <<" late="<<traces[0].late<<'/'<<traces[1].late<<'\n';return 1;
            }
            ++cases;
        }
    target={0xe10f8000,0xe2899001,0xeafffffe};
    // A flag definition after an unconditional return must not make the earlier
    // ADDS dead. The old decoder is deliberately retained as the bug witness.
    for(unsigned mode=0;mode<2;++mode) {
        auto& cpu=*cpus[mode];reset(cpu,0x10000010);
        code={0xe2966001,0xe8b28011,0xe3570000,0xeafffffe};
        data[0]=0x12345678;data[1]=0x87654321;data[2]=0x2000;
        cpu.SetReg(2,0x4000);cpu.SetReg(6,0xffffffff);cpu.SetReg(7,1);
        BasicBlock block{};block.start_pc=0x1000;
        DecodeRecordSelfTestAccess::Decode(cpu,block,0x1000);
        const auto ticks=DecodeRecordSelfTestAccess::ExecuteOnce(cpu,block);
        const auto expected_ticks=Core::TicksForInstruction(false,code[0])+Core::TicksForInstruction(false,code[1]);
        if(ticks!=expected_ticks || cpu.GetReg(6)!=0 || cpu.GetPC()!=0x2000 ||
           cpu.GetCPSR()!=(mode ? 0x60000010u : 0x10000010u) ||
           block.end_pc!=(mode ? 0x1008u : 0x1010u)) {
            std::cerr<<"LDM-PC dead-flags regression/witness mismatch\n";return 1;
        }
        ++cases;
    }
    // Conditional returns preserve their old decode boundary but must still
    // keep all flag definitions live at their possible exit. VS reads only V;
    // MOVS defines NZC, which the unreachable CMP incorrectly killed before.
    for(bool passes:{false,true}) for(unsigned mode=0;mode<2;++mode) {
        auto& cpu=*cpus[mode];reset(cpu,passes ? 0x90000010 : 0x80000010);
        code={0xe3b06000,0x68b28011,0xe3570000,0xeafffffe};
        data[0]=0x12345678;data[1]=0x87654321;data[2]=0x2000;
        cpu.SetReg(2,0x4000);cpu.SetReg(7,1);
        BasicBlock block{};block.start_pc=0x1000;
        DecodeRecordSelfTestAccess::Decode(cpu,block,0x1000);
        const auto ticks=DecodeRecordSelfTestAccess::ExecuteOnce(cpu,block);
        const u32 expected_flags=passes ? (mode ? 0x50000010u : 0x90000010u) : 0x20000010u;
        u64 expected_ticks=Core::TicksForInstruction(false,code[0])+Core::TicksForInstruction(false,code[1]);
        if(!passes)expected_ticks+=Core::TicksForInstruction(false,code[2])+Core::TicksForInstruction(false,code[3]);
        if(block.end_pc!=0x1010 || cpu.GetCPSR()!=expected_flags || ticks!=expected_ticks ||
           cpu.GetPC()!=(passes ? 0x2000u : 0x100cu)) {
            std::cerr<<"LDM-PC conditional flags/barrier mismatch\n";return 1;
        }
        ++cases;
    }
    // A source-page boundary followed by MMIO makes speculative tail fetches
    // observable without executing any instruction from that page.
    struct FetchTrap final : Memory::MMIORegion {
        unsigned reads=0;
        u32 Read(VAddr) {++reads;return 0xeafffffe;}
        bool IsValidAddress(VAddr) override{return true;}
        u8 Read8(VAddr a) override{return u8(Read(a));}
        u16 Read16(VAddr a) override{return u16(Read(a));}
        u32 Read32(VAddr a) override{return Read(a);}
        u64 Read64(VAddr a) override{return Read(a);}
        bool ReadBlock(VAddr,void*,std::size_t) override{return false;}
        void Write8(VAddr,u8) override{} void Write16(VAddr,u16) override{}
        void Write32(VAddr,u32) override{} void Write64(VAddr,u64) override{}
        bool WriteBlock(VAddr,const void*,std::size_t) override{return false;}
    };
    auto trap=std::make_shared<FetchTrap>();memory.MapIoRegion(*table,0x3000,4096,trap);
    target[1023]=0xe8b28011;
    for(unsigned mode=0;mode<2;++mode) {
        auto& cpu=*cpus[mode];reset(cpu);trap->reads=0;
        BasicBlock block{};block.start_pc=0x2ffc;
        DecodeRecordSelfTestAccess::Decode(cpu,block,0x2ffc);
        if((mode && (block.end_pc!=0x3000 || trap->reads)) || (!mode && !trap->reads)) {
            std::cerr<<"LDM-PC post-return fetch guard mismatch\n";return 1;
        }
        ++cases;
    }
    // A retained conditional fallthrough span and a shortened unconditional
    // return both cover their executable source text for invalidation.
    auto& cpu=*cpus[1];reset(cpu);cpu.SetReg(2,0x4000);
    code={0x18b28011,0xe3a0a02a,0xea0003fc}; // failed NE return
    run(cpu);code[1]=0xe3a0a02b;cpu.InvalidateCacheRange(0x1004,4);
    cpu.SetPC(0x1000);cpu.SetReg(2,0x4000);run(cpu);
    if(cpu.GetReg(10)!=43) {std::cerr<<"LDM-PC fallthrough invalidation failed\n";return 1;}
    code[0]=0xe8b28011;cpu.InvalidateCacheRange(0x1000,4);
    cpu.SetPC(0x1000);cpu.SetReg(2,0x4000);cpu.SetReg(10,99);run(cpu);
    if(cpu.GetReg(10)!=99) {std::cerr<<"LDM-PC source invalidation failed\n";return 1;}
    std::cout<<cases<<" LDM-PC off/on decode, Step, cold/warm Run, flags, ticks, address modes, legacy S-bit, post-return fetch and canonical Step/tiny-budget/event cases; cache invalidation passed\n";
    return 0;
}
