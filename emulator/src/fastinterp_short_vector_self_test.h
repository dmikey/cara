// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/arm/skyeye_common/vfp/vfp.h"

// Real decode/dispatch coverage, fixed arithmetic answers, then differential
// register-bank/FP-control coverage against the existing software VFP engine.
static int VerifyFastInterpShortVectors(Core::System& system) {
    using namespace Core::FastInterp;
    Memory::MemorySystem memory;
    Core::Timing timing;
    std::array<u32, 1024> code{};
    auto table = std::make_shared<Memory::PageTable>();
    table->attributes[1] = Memory::PageType::Memory;
    table->pointers[1] = TestMemory(code);
    memory.SetCurrentPageTable(table);
    ARM_FastInterp cpu(system, memory, 0, timing.GetTimer(0));
    cpu.SetPageTable(table);
    ARMul_State oracle(system, memory, USER32MODE);
    struct Operation { u32 base; bool unary; std::array<double, 4> answer; };
    const std::array<Operation, 13> operations{{
        {0xee300a00, false, {6,8,10,12}}, {0xee300a40, false, {-2,0,2,4}},
        {0xee200a00, false, {8,16,24,32}}, {0xee800a00, false, {.5,1,1.5,2}},
        {0xee000a00, false, {9,18,27,36}}, {0xee000a40, false, {-7,-14,-21,-28}},
        {0xee100a40, false, {-9,-18,-27,-36}}, {0xee100a00, false, {7,14,21,28}},
        {0xee200a40, false, {-8,-16,-24,-32}}, {0xeeb00ac0, true, {4,4,4,4}},
        {0xeeb10a40, true, {-4,-4,-4,-4}}, {0xeeb10ac0, true, {2,2,2,2}},
        {0xeeb00a40, true, {4,4,4,4}},
    }};
    auto encode = [](const Operation& op, bool wide, unsigned d, unsigned n, unsigned m) {
        if (wide) return op.base | 0x100u | ((d & 15) << 12) | ((d >> 4) << 22) |
            (op.unary ? 0 : ((n & 15) << 16) | ((n >> 4) << 7)) |
            (m & 15) | ((m >> 4) << 5);
        return op.base | ((d >> 1) << 12) | ((d & 1) << 22) |
            (op.unary ? 0 : ((n >> 1) << 16) | ((n & 1) << 7)) |
            (m >> 1) | ((m & 1) << 5);
    };
    using Registers = std::array<u32,64>;
    auto write = [](Registers& regs, bool wide, unsigned index, double value) {
        if (wide) {
            const u64 bits = std::bit_cast<u64>(value);
            regs[index*2] = u32(bits); regs[index*2+1] = u32(bits >> 32);
        } else regs[index] = std::bit_cast<u32>(float(value));
    };
    unsigned fixed = 0, differential = 0, fusion = 0;
    auto run = [&](u32 instruction, const Registers& before, u32 fpscr,
                   const Registers& expected, u32 expected_fpscr, bool al, bool thumb = false) {
        DecodeRecordSelfTestAccess::ConfigureAlDispatch(cpu, al);
        code.fill(0xeafffffe); code[0] = thumb ? (instruction << 16) | (instruction >> 16) : instruction;
        const u32 cpsr = 0x60000010 | (thumb ? 0x20 : 0);
        cpu.SetCPSR(cpsr); cpu.SetPC(0x1000 | unsigned(thumb));
        for (unsigned i=0;i<64;++i) cpu.SetVFPReg(i,before[i]);
        cpu.SetVFPSystemReg(VFP_FPSCR,fpscr);
        const auto start = timing.GetTimer(0)->GetDowncount();
        cpu.Step();
        bool valid = cpu.GetPC()==0x1004 && cpu.GetCPSR()==cpsr &&
            cpu.GetVFPSystemReg(VFP_FPSCR)==expected_fpscr &&
            start-timing.GetTimer(0)->GetDowncount()==Core::TicksForInstruction(thumb,instruction);
        for (unsigned i=0;i<64;++i) if (cpu.GetVFPReg(i)!=expected[i]) {
            std::cerr << "VFP register mismatch s" << i << " expected=" << std::hex
                      << expected[i] << " actual=" << cpu.GetVFPReg(i) << '\n'; valid=false; break;
        }
        if (!valid) std::cerr << "Short-vector case failed inst=" << std::hex << instruction
            << " fpscr=" << fpscr << " actual=" << cpu.GetVFPSystemReg(VFP_FPSCR)
            << " expected=" << expected_fpscr << " AL=" << al << '\n';
        return valid;
    };
    for (bool wide : {false,true}) for (bool al : {false,true})
        for (bool scalar_dest : {false,true}) for (unsigned cond : {0u,1u,14u})
            for (const auto& op : operations) {
                Registers before{};
                const unsigned d=scalar_dest ? 0 : wide ? 4 : 8, n=wide ? 8 : 16, m=wide ? 12 : 24;
                for(unsigned i=0;i<4;++i) {
                    write(before,wide,d+i,i+1); write(before,wide,n+i,2*(i+1)); write(before,wide,m+i,4);
                }
                auto expected=before;
                if(cond!=1) for(unsigned i=0;i<(scalar_dest?1u:4u);++i)
                    write(expected,wide,d+i,op.answer[i]);
                const u32 raw=(encode(op,wide,d,n,m)&0x0fffffffu)|(cond<<28);
                if(!run(raw,before,0xa3030000,expected,0xa3030000,al)) return 1;
                ++fixed;
                if (cond==14) {
                    if(!run(raw,before,0xa3030000,expected,0xa3030000,al,true)) return 1;
                    ++fixed;
                }
            }
    // Matrix-column form observed in the affected game, with independently
    // chosen inputs (no guest code/data asset): four vector*scalar columns,
    // then four multiply-accumulates, broadcasting s0..s3 within bank zero.
    for(bool al:{false,true}) {
        Registers before{};
        for(unsigned i=0;i<4;++i) {
            write(before,false,i,i+2);write(before,false,24+i,i+1);write(before,false,28+i,i+5);
        }
        for(unsigned pass=0;pass<2;++pass) for(unsigned column=0;column<4;++column) {
            auto expected=before;
            for(unsigned row=0;row<4;++row)
                write(expected,false,8+column*4+row,(column+2)*(pass ? 2*row+6 : row+1));
            const auto raw=encode(operations[pass?4:2],false,8+column*4,pass?28:24,column);
            if(!run(raw,before,0x23030000,expected,0x23030000,al))return 1;
            before=expected;++fixed;
        }
    }
    // Independent bank-zero FP-control answers under LEN=3. Each operation
    // changes exactly one destination, retains existing sticky DZC, and observes
    // guest DN, input FZ/IDC, and all four rounding modes with cumulative IXC.
    for(bool wide:{false,true}) for(bool al:{false,true}) for(unsigned kind=0;kind<6;++kind) {
        Registers before{};for(unsigned i=0;i<64;++i)before[i]=0x12345678u+i;
        const unsigned d=2,n=wide?8:16,m=wide?12:24;
        const u64 one=wide?0x3ff0000000000000ull:0x3f800000ull;
        const auto put_bits=[&](Registers& regs,unsigned reg,u64 bits) {
            if(wide){regs[reg*2]=u32(bits);regs[reg*2+1]=u32(bits>>32);}else regs[reg]=u32(bits);
        };
        u32 fpscr=0xa0030002u,exceptions=0;u64 answer=one;
        put_bits(before,n,one);
        if(kind==0) {
            fpscr|=1u<<25;put_bits(before,m,wide?0x7ff8123456789abcull:0x7fc12345ull);
            answer=wide?0x7ff8000000000000ull:0x7fc00000ull;
        } else if(kind==1) {
            fpscr|=1u<<24;put_bits(before,m,1);exceptions=0x80;
        } else {
            const unsigned rounding=kind-2;fpscr|=rounding<<22;
            put_bits(before,m,wide?0x3ca0000000000000ull:0x33800000ull);
            answer=one+(rounding==1);exceptions=0x10;
        }
        auto expected=before;put_bits(expected,d,answer);
        if(!run(encode(operations[0],wide,d,n,m),before,fpscr,expected,fpscr|exceptions,al))return 1;
        ++fixed;
    }
    // Explicit wrap and overlap answers independent of the software oracle.
    for(bool al:{false,true}) {
        Registers before{}; for(unsigned i=0;i<32;++i) write(before,false,i,i+1);
        auto expected=before;
        for(auto [reg,value]:std::array<std::pair<unsigned,double>,4>{{{14,54},{15,56},{8,42},{9,44}}})
            write(expected,false,reg,value);
        if(!run(encode(operations[0],false,14,22,30),before,3u<<16,expected,3u<<16,al)) return 1;
        ++fixed;
        write(before,false,0,2); expected=before;
        for(auto [reg,value]:std::array<std::pair<unsigned,double>,4>{{{9,18},{10,36},{11,72},{12,144}}})
            write(expected,false,reg,value);
        if(!run(encode(operations[2],false,9,8,0),before,3u<<16,expected,3u<<16,al)) return 1;
        ++fixed;
    }
    // Legal vector sizes only: stride encoding 00=1, 11=2. Reserved stride
    // encodings and repeated-register lengths are not asserted as hardware behavior.
    struct BankCase { unsigned d,n,m,length,stride; };
    const std::array<BankCase,7> singles{{{14,22,30,4,1},{9,17,25,4,2},
        {8,16,0,8,1},{9,8,0,4,1},{8,0,24,4,1},{2,22,30,4,1},{7,0,2,4,1}}};
    const std::array<BankCase,7> doubles{{{7,11,15,4,1},{5,9,13,2,2},
        {4,8,0,4,1},{5,4,0,3,1},{4,0,12,4,1},{2,11,15,4,1},{3,0,1,4,1}}};
    const std::array<u32,12> single_bits{0,0x80000000,1,0x80000001,0x3f800000,
        0xbf800000,0x7f800000,0xff800000,0x7fc12345,0x7fa12345,0x3eaaaaab,0x7f7fffff};
    const std::array<u64,12> double_bits{0,0x8000000000000000ull,1,0x8000000000000001ull,
        0x3ff0000000000000ull,0xbff0000000000000ull,0x7ff0000000000000ull,
        0xfff0000000000000ull,0x7ff8123456789abcull,0x7ff0123456789abcull,
        0x3fd5555555555555ull,0x7fefffffffffffffull};
    for(bool wide:{false,true}) for(bool al:{false,true}) for(const auto& op:operations)
        for(const auto& bank:wide?doubles:singles) for(unsigned mode=0;mode<16;++mode)
            for(unsigned cond:{1u,14u}) {
                Registers before{};
                for(unsigned i=0;i<(wide?32u:64u);++i) {
                    if(wide) {const auto bits=double_bits[(i+mode)%double_bits.size()];
                        before[i*2]=u32(bits);before[i*2+1]=u32(bits>>32);}
                    else before[i]=single_bits[(i+mode)%single_bits.size()];
                }
                const u32 fpscr=0xa0000010u|((bank.length-1)<<16)|((bank.stride==2?3u:0u)<<20)|
                    ((mode&3)<<22)|((mode&4)<<22)|((mode&8)<<22);
                const u32 raw=(encode(op,wide,bank.d,bank.n,bank.m)&0x0fffffffu)|(cond<<28);
                auto expected=before;u32 expected_fpscr=fpscr;
                if(cond!=1) {
                    oracle.ExtReg=before;oracle.VFP[VFP_FPSCR]=fpscr;
                    const u32 exceptions=wide?vfp_double_cpdo(&oracle,raw,fpscr):vfp_single_cpdo(&oracle,raw,fpscr);
                    vfp_raise_exceptions(&oracle,exceptions,raw,fpscr);
                    expected=oracle.ExtReg;expected_fpscr=oracle.VFP[VFP_FPSCR];
                }
                if(!run(raw,before,fpscr,expected,expected_fpscr,al)) return 1;
                ++differential;
            }
    // Compare actual cached/fused block execution with individual guest steps.
    // VMSR changes LEN twice inside that same block, without re-decoding it.
    for(bool al:{false,true}) for(bool accumulate:{false,true}) for(unsigned pattern=0;pattern<5;++pattern) {
        code.fill(0xeafffffe);code[0]=0xeee10a10; // VMSR FPSCR,r0
        for(unsigned i=0;i<4;++i) {
            unsigned d=8+i,n=16+i,m=0;
            if(pattern==1) {n=0;m=16+i;}
            if(pattern==2) {d=9+4*i;n=8+4*i;}
            if(pattern==3) {d=i;n=16+i;m=4;}
            if(pattern==4) {d=std::array<unsigned,4>{0,8,1,12}[i];n=16+i;m=4;}
            code[1+i]=encode(operations[accumulate?4:2],false,d,n,m);
            code[6+i]=pattern>=3 ? encode(operations[accumulate?4:2],false,24+i,16+i,4) : code[1+i];
        }
        code[5]=0xeee11a10; // VMSR FPSCR,r1 (scalar)
        struct Result {Registers regs;u32 fpscr,cpsr,pc;u64 ticks;bool operator==(const Result&)const=default;};
        std::array<Result,2> results;
        DecodeRecordSelfTestAccess::ConfigureAlDispatch(cpu,al);
        BasicBlock block{};DecodeRecordSelfTestAccess::Decode(cpu,block,0x1000);
        const char* simd_env=std::getenv("CARA_FASTINTERP_VFP_SIMD");
        const unsigned simd=simd_env?unsigned(std::atoi(simd_env)):0;
        bool expect_fusion=simd>=1&&simd<=3&&(!accumulate||simd==3)&&((pattern!=2&&pattern!=4)||simd>=2);
#ifndef __aarch64__
        if(accumulate)expect_fusion=false;
#endif
        unsigned fused=0;
        for(unsigned i=0;i<block.inst_count;++i) {
            const auto op=block.instructions[i].opcode;
            fused+=op==Opcode::Vmul4S||op==Opcode::Vmul4Gather||op==Opcode::Vmla4S||op==Opcode::Vmla4Gather;
        }
        const unsigned expected_fused=expect_fusion?2u:
            (pattern==4&&simd==1&&!accumulate?1u:0u);
        if(fused!=expected_fused) {std::cerr<<"Short-vector fixture fusion coverage mismatch\n";return 1;}
        for(unsigned path=0;path<2;++path) {
            for(unsigned i=0;i<64;++i) cpu.SetVFPReg(i,std::bit_cast<u32>(float(i+1)));
            if(pattern>=3)for(unsigned i=16;i<20;++i)cpu.SetVFPReg(i,0x7fc12345);
            cpu.SetReg(0,0x03030000);cpu.SetReg(1,0x03000000);
            cpu.SetVFPSystemReg(VFP_FPSCR,0x03000000);cpu.SetCPSR(0x60000010);cpu.SetPC(0x1000);
            auto& result=results[path];
            if(path==0) {const auto start=timing.GetTimer(0)->GetDowncount();
                for(unsigned i=0;i<11;++i)cpu.Step();result.ticks=start-timing.GetTimer(0)->GetDowncount();}
            else result.ticks=DecodeRecordSelfTestAccess::ExecuteOnce(cpu,block);
            for(unsigned i=0;i<64;++i)result.regs[i]=cpu.GetVFPReg(i);
            result.fpscr=cpu.GetVFPSystemReg(VFP_FPSCR);result.cpsr=cpu.GetCPSR();result.pc=cpu.GetPC();
        }
        if(pattern>=3 && (results[1].regs[0]!=0x7fc00000 || results[1].regs[1]!=0x7fc00000)) {
            std::cerr<<"Fused bank-zero guest DN handling failed\n";return 1;
        }
        if(results[0]!=results[1]) {std::cerr<<"VFP vector/scalar cached fusion mismatch pattern="<<pattern
            <<" accumulate="<<accumulate<<" AL="<<al<<'\n';return 1;}
        ++fusion;
    }
    // The added host work must not alter guest event delivery or block budgets.
    struct Trace {std::vector<s64> consumed,events,pcs; bool operator==(const Trace&)const=default;};
    for(bool al:{false,true}) {
        std::array<Trace,2> traces;
        for(unsigned vector=0;vector<2;++vector) {
            Core::Timing clock;
            ARM_FastInterp scheduled(system,memory,0,clock.GetTimer(0));
            scheduled.SetPageTable(table);
            DecodeRecordSelfTestAccess::ConfigureAlDispatch(scheduled,al);
            code.fill(0xeafffffe);
            code[0]=encode(operations[12],false,8,0,16); // VMOV, no control-flow dependence
            code[1]=encode(operations[0],false,12,16,0); // VADD
            code[2]=0xeafffffc; // B 0x1000
            for(unsigned i=0;i<64;++i)scheduled.SetVFPReg(i,std::bit_cast<u32>(float(i+1)));
            scheduled.SetVFPSystemReg(VFP_FPSCR,vector ? 0x03030000 : 0x03000000);
            scheduled.SetCPSR(0x60000010);scheduled.SetPC(0x1000);
            auto& trace=traces[vector];
            const auto* event=clock.RegisterEvent("short_vector_event",[&](u64 value,int late){
                trace.events.push_back(s64(value));trace.events.push_back(late);
            });
            for(unsigned slice=0;slice<64;++slice) {
                const unsigned budget=std::array<unsigned,8>{1,2,7,8,9,11,31,128}[slice%8];
                clock.ScheduleEvent(budget,event,slice,0);clock.GetTimer(0)->Advance(budget);
                const auto before=clock.GetTimer(0)->GetDowncount();scheduled.Run();
                trace.consumed.push_back(before-clock.GetTimer(0)->GetDowncount());
                trace.pcs.push_back(scheduled.GetPC());
            }
            clock.GetTimer(0)->Advance(1);clock.RemoveEvent(event);
        }
        if(traces[0]!=traces[1] || traces[0].events.empty()) {
            std::cerr<<"Short-vector scalar/vector scheduling trace mismatch AL="<<al<<'\n';return 1;
        }
    }
    std::cout<<"VFP short-vector fixed="<<fixed<<" software differential="<<differential
             <<" cached/fused transition="<<fusion<<" cases passed; 128 event slices, PC/ticks/FPSCR checked\n";
    return 0;
}
