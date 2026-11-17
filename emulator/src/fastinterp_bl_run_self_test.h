// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/arm/skyeye_common/vfp/vfp.h"
// Original synthetic ARM code; no ROM or guest-state input. In particular Run
// must call the actual separately compiled DecodeBlockInto before AL dispatch.
static int VerifyFastInterpImmediateBLRun(Core::System& system) {
    using namespace Core::FastInterp;
    constexpr u32 entry=0x100000,target=0x100024,return_pc=0x100004;
    constexpr u32 bl=0xeb000007,bx_lr=0xe12fff1e,add=0xe2800001,back=0xeafffffc;
    const u64 bl_ticks=Core::TicksForInstruction(false,bl);
    const u64 bx_ticks=Core::TicksForInstruction(false,bx_lr);
    const u64 tail_ticks=Core::TicksForInstruction(false,add)+Core::TicksForInstruction(false,back);
    const u64 loop_ticks=bl_ticks+bx_ticks+tail_ticks;
    struct Fixture {
        Memory::MemorySystem memory;
        Core::Timing timing;
        std::array<u32,1024> code{};
        std::shared_ptr<Memory::PageTable> table=std::make_shared<Memory::PageTable>();
        std::unique_ptr<ARM_FastInterp> cpu;
        Fixture(Core::System& system,bool al,bool chain) {
            code.fill(0xeafffffe);
            code[0]=0xeb000007;code[9]=0xe12fff1e;
            code[1]=0xe2800001;code[2]=0xeafffffc;
            table->attributes[0x100]=Memory::PageType::Memory;
            table->pointers[0x100]=TestMemory(code);memory.SetCurrentPageTable(table);
            cpu=std::make_unique<ARM_FastInterp>(system,memory,0,timing.GetTimer(0));cpu->SetPageTable(table);
            DecodeRecordSelfTestAccess::ConfigureAlDispatch(*cpu,al);
            DecodeRecordSelfTestAccess::ConfigureBLRunChain(*cpu,chain);
        }
        void Reset() {
            for(unsigned i=0;i<15;++i)cpu->SetReg(i,i?0x20000000u+i:0);
            cpu->SetReg(13,0x10000000);cpu->SetReg(14,0);
            for(unsigned i=0;i<64;++i)cpu->SetVFPReg(i,0x12340000+i);
            cpu->SetCPSR(0xa0000010);cpu->SetVFPSystemReg(VFP_FPSCR,0x03000002);cpu->SetPC(0x100000);
        }
    };
    struct State {
        std::array<u32,16> regs{};std::array<u32,64> fp{};
        u32 cpsr{},fpscr{};s64 consumed{};
        bool operator==(const State&)const=default;
    };
    const auto save=[](Fixture& f,s64 consumed) {
        State s;s.consumed=consumed;
        for(unsigned i=0;i<16;++i)s.regs[i]=f.cpu->GetReg(i);
        for(unsigned i=0;i<64;++i)s.fp[i]=f.cpu->GetVFPReg(i);
        s.cpsr=f.cpu->GetCPSR();s.fpscr=f.cpu->GetVFPSystemReg(VFP_FPSCR);return s;
    };
    const auto phase=[](u32 pc)->int {return pc==0x100000?0:pc==0x100024?1:pc==0x100004?2:-1;};
    const auto fail=[](const char* why,bool al,bool chain,unsigned slice) {
        std::cerr<<"Immediate BL Run regression: "<<why<<" AL="<<al<<" chain="<<chain<<" slice="<<slice<<'\n';return 1;
    };
    unsigned cases=0;
    for(bool chain:{false,true}) {
        std::array<std::unique_ptr<Fixture>,2> f{
            std::make_unique<Fixture>(system,false,chain),std::make_unique<Fixture>(system,true,chain)};
        std::array<std::vector<State>,2> traces;
        for(unsigned mode=0;mode<2;++mode) {
            auto& x=*f[mode];auto& cpu=*x.cpu;x.Reset();
            BasicBlock block{};block.start_pc=entry;
            DecodeRecordSelfTestAccess::Decode(cpu,block,entry);
            const auto& first=block.instructions[0];
            const unsigned expected=unsigned(Opcode::Bl)+(mode?unsigned(Opcode::OpcodeCount):0);
            if(block.inst_count!=1||block.end_pc!=entry+4||first.GetOpcode()!=Opcode::Bl||
               first.branch.target!=target||first.branch.lr!=return_pc||first.branch.rm!=0xff||first.ticks!=bl_ticks||
               first.dispatch_index!=expected) {
                std::cerr<<"Immediate BL decoded ABI: source OpcodeCount="<<unsigned(Opcode::OpcodeCount)
                         <<" expected dispatch="<<expected<<" actual="<<first.dispatch_index
                         <<" target="<<std::hex<<first.branch.target<<" LR="<<first.branch.lr<<std::dec<<'\n';
                return fail("decoded postpass/table ABI mismatch",mode,chain,0);
            }
            // Step is a distinct path and must not substitute for the Run test.
            auto before=x.timing.GetTimer(0)->GetDowncount();cpu.Step();
            auto stepped=save(x,before-x.timing.GetTimer(0)->GetDowncount());
            if(stepped.regs[15]!=target||stepped.regs[14]!=return_pc||stepped.consumed!=s64(bl_ticks)||
               stepped.regs[0]!=0||stepped.cpsr!=0xa0000010||stepped.fpscr!=0x03000002)
                return fail("Step PC/LR/tick answer",mode,chain,0);
            ++cases;
            // Cold target is deliberately absent from cache. One tick executes
            // exactly the first real decoded BL block even in legacy chain mode.
            x.Reset();cpu.ClearInstructionCache();x.timing.GetTimer(0)->Advance(1);
            before=x.timing.GetTimer(0)->GetDowncount();cpu.Run();
            auto ran=save(x,before-x.timing.GetTimer(0)->GetDowncount());
            if(ran!=stepped)return fail("cold Run1 differs from exact Step result",mode,chain,0);
            ++cases;
            // Keep caches warm between slices. Independently derive work from
            // completed ADD iterations and one of three decoded-block phases.
            x.Reset();cpu.ClearInstructionCache();
            constexpr std::array<s64,8> budgets{1,4,5,7,14,17,31,100};
            const std::array<u64,3> prefix{0,bl_ticks,bl_ticks+bx_ticks};
            for(unsigned slice=0;slice<64;++slice) {
                const u32 old_r0=cpu.GetReg(0);const int old_phase=phase(cpu.GetPC());
                x.timing.GetTimer(0)->Advance(budgets[slice%budgets.size()]);
                before=x.timing.GetTimer(0)->GetDowncount();cpu.Run();
                auto result=save(x,before-x.timing.GetTimer(0)->GetDowncount());
                const int new_phase=phase(result.regs[15]);
                if(old_phase<0||new_phase<0||result.regs[0]<old_r0||result.regs[14]!=return_pc)
                    return fail("mixed-budget control flow",mode,chain,slice);
                const s64 expected_ticks=s64(result.regs[0]-old_r0)*s64(loop_ticks)+
                    s64(prefix[new_phase])-s64(prefix[old_phase]);
                if(result.consumed!=expected_ticks||result.consumed<budgets[slice%budgets.size()]||
                   result.cpsr!=0xa0000010||result.fpscr!=0x03000002||result.regs[13]!=0x10000000)
                    return fail("mixed-budget independent canonical work/ticks",mode,chain,slice);
                for(unsigned r=1;r<13;++r)if(result.regs[r]!=0x20000000u+r)
                    return fail("unexpected ARM register write",mode,chain,slice);
                for(unsigned r=0;r<64;++r)if(result.fp[r]!=0x12340000u+r)
                    return fail("unexpected FP register write",mode,chain,slice);
                traces[mode].push_back(result);++cases;
            }
            // Warm the old branch, change only its source word and invalidate
            // that range. A stale cached target must not survive the write.
            x.Reset();cpu.ClearInstructionCache();x.timing.GetTimer(0)->Advance(1);cpu.Run();
            x.code[0]=0xeb000008; // Synthetic BL target moves from+0x24 to+0x28.
            x.code[10]=bx_lr;cpu.InvalidateCacheRange(entry,4);x.Reset();
            x.timing.GetTimer(0)->Advance(1);before=x.timing.GetTimer(0)->GetDowncount();cpu.Run();
            if(cpu.GetPC()!=entry+0x28||cpu.GetReg(14)!=return_pc||
               before-x.timing.GetTimer(0)->GetDowncount()!=s64(Core::TicksForInstruction(false,x.code[0])))
                return fail("branch word invalidation",mode,chain,0);
            ++cases;
        }
        if(traces[0]!=traces[1])return fail("AL0/AL1 full-state execution traces",true,chain,0);
    }
    std::cout<<cases<<" immediate BL Step/realRun/mixed-budget/invalidation cases passed; AL0/AL1 traces match\n";
    return 0;
}
