// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

static int VerifyFastInterpAlDispatch(Core::System& system) {
    using namespace Core::FastInterp;
    // Independent CPUs/timers exercise real decode and dispatch with identical
    // code/data. The oracle is the unchanged predicate entry and tick accounting.
    struct Fixture {
        Memory::MemorySystem memory;
        Core::Timing timing;
        std::array<u32,1024> words{};
        std::shared_ptr<Memory::PageTable> table = std::make_shared<Memory::PageTable>();
        std::unique_ptr<ARM_FastInterp> cpu;
        Fixture(Core::System& system, bool enabled) {
            table->attributes[1] = Memory::PageType::Memory;
            table->pointers[1] = TestMemory(words);
            memory.SetCurrentPageTable(table);
            cpu = std::make_unique<ARM_FastInterp>(system,memory,0,timing.GetTimer(0));
            DecodeRecordSelfTestAccess::ConfigureAlDispatch(*cpu,enabled);
            cpu->SetPageTable(table);
        }
    };
    std::array<std::unique_ptr<Fixture>,2> fixtures{
        std::make_unique<Fixture>(system,false),std::make_unique<Fixture>(system,true)};
    const std::array<u32,20> instructions{
        0x01a00001, // MOV r0,r1
        0x02810003, // ADD r0,r1,#3
        0x02910003, // ADDS r0,r1,#3
        0x02510003, // SUBS r0,r1,#3
        0x00010003, // AND r0,r1,r3
        0x01810003, // ORR r0,r1,r3
        0x01b00081, // MOVS r0,r1,LSL #1
        0x01510003, // CMP r1,r3
        0x03110001, // TST r1,#1
        0x00b10003, // ADCS r0,r1,r3
        0x00d10003, // SBCS r0,r1,r3
        0x00000391, // MUL r0,r1,r3
        0x05920000, // LDR r0,[r2]
        0x05821000, // STR r1,[r2]
        0x05d20000, // LDRB r0,[r2]
        0x05c21000, // STRB r1,[r2]
        0x08b20011, // LDMIA r2!,{r0,r4}
        0x08a20012, // STMIA r2!,{r1,r4}
        0x0e000a10, // VMOV s0,r0
        0x0e100a10  // VMOV r0,s0
    };
    struct Result {
        std::array<u32,16> regs{};std::array<u32,64> vfp{};
        std::array<u32,1024> words{};u32 cpsr{};u64 ticks{};
        bool operator==(const Result&) const = default;
    };
    unsigned cases=0;
    for(u32 raw:instructions) for(unsigned cond=0;cond<15;++cond)
        for(unsigned flags=0;flags<16;++flags) for(bool step:{false,true}) {
        std::array<Result,2> results;
        for(unsigned mode=0;mode<2;++mode) {
            auto& f=*fixtures[mode];auto& cpu=*f.cpu;
            f.words.fill(0xeafffffe);f.words[0]=(cond<<28)|raw;
            f.words[512]=0x12345678;f.words[513]=0x87654321;
            cpu.ClearInstructionCache();cpu.SetCPSR((flags<<28)|0x10);
            for(unsigned r=0;r<15;++r)cpu.SetReg(r,0x80000000u+r);
            cpu.SetReg(2,0x1800);cpu.SetPC(0x1000);
            for(unsigned r=0;r<64;++r)cpu.SetVFPReg(r,0x3f800000+r);
            if(step) {
                const auto before=f.timing.GetTimer(0)->GetDowncount();cpu.Step();
                results[mode].ticks=before-f.timing.GetTimer(0)->GetDowncount();
            } else {
                BasicBlock block{};block.start_pc=0x1000;
                DecodeRecordSelfTestAccess::Decode(cpu,block,0x1000);
                for(unsigned i=0;i<=block.inst_count;++i) {
                    const auto& inst=block.instructions[i];
                    const u16 expected=static_cast<u16>(inst.opcode)+
                        ((mode&&inst.cond==14)?static_cast<u16>(Opcode::OpcodeCount):0);
                    if(inst.dispatch_index!=expected) {std::cerr<<"AL dispatch index mismatch\n";return 1;}
                }
                results[mode].ticks=DecodeRecordSelfTestAccess::ExecuteOnce(cpu,block);
            }
            auto& result=results[mode];
            for(unsigned r=0;r<16;++r)result.regs[r]=cpu.GetReg(r);
            for(unsigned r=0;r<64;++r)result.vfp[r]=cpu.GetVFPReg(r);
            result.cpsr=cpu.GetCPSR();result.words=f.words;
        }
        if(results[0]!=results[1]) {
            std::cerr<<"AL dispatch differential mismatch raw="<<std::hex<<raw
                     <<" cond="<<cond<<" flags="<<flags<<" step="<<step<<'\n';return 1;
        }
        ++cases;
    }
    // A conditional instruction must still materialize pending lazy flags;
    // an AL entry must bypass the gate without discarding that pending state.
    for(unsigned cond=0;cond<15;++cond) for(u32 a:{0u,1u,0x80000000u,0xffffffffu}) {
        std::array<Result,2> results;
        for(unsigned mode=0;mode<2;++mode) {
            auto& f=*fixtures[mode];auto& cpu=*f.cpu;
            f.words.fill(0xeafffffe);f.words[0]=(cond<<28)|0x01a00001;
            cpu.SetCPSR(0x10);cpu.SetPC(0x1000);cpu.SetReg(0,17);cpu.SetReg(1,42);
            BasicBlock block{};block.start_pc=0x1000;
            DecodeRecordSelfTestAccess::Decode(cpu,block,0x1000);
            DecodeRecordSelfTestAccess::PendingCompare(cpu,a,1);
            results[mode].ticks=DecodeRecordSelfTestAccess::ExecuteOnce(cpu,block);
            results[mode].regs[0]=cpu.GetReg(0);results[mode].cpsr=cpu.GetCPSR();
        }
        if(results[0]!=results[1]) {std::cerr<<"AL pending flags mismatch\n";return 1;}
        ++cases;
    }
    // Hot-loop opcode rewrites must refresh the dispatch index, including
    // synthetic records not emitted by the ordinary final decode pass.
    for(unsigned mode=0;mode<2;++mode) for(Opcode opcode:{Opcode::ThumbFusedSubsBcc,
            Opcode::ThumbFusedCmpRegBcc,Opcode::FusedSubsBcc}) {
        BasicBlock block{};block.start_pc=0x1000;block.inst_count=1;
        auto& inst=block.instructions[0];inst.opcode=opcode;inst.cond=14;
        inst.fused.branch_cond=1;inst.fused.branch_target=0x1000;
        DecodeRecordSelfTestAccess::Reoptimize(*fixtures[mode]->cpu,block);
        if(inst.opcode==opcode || inst.dispatch_index!=static_cast<u16>(inst.opcode)+
            (mode?static_cast<u16>(Opcode::OpcodeCount):0)) {
            std::cerr<<"AL hot rewrite index mismatch\n";return 1;
        }
        ++cases;
    }
    std::cout<<"AL dispatch differential cases="<<cases<<"; 20-byte records retained\n";
    return 0;
}
