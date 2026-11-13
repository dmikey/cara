// Copyright 2026 Cara contributors; GPL-2.0-or-later.
// Synthetic guest instructions only; no ROM data.
#include <array>
#include <cassert>
#include <iostream>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/context.h>
#include <dynarmic/interface/exclusive_monitor.h>
struct Callbacks : Dynarmic::A32::UserCallbacks {
    std::array<uint32_t,4> code{0xe3a00007,0xe2801005,0xef000000,0xeafffffe};
    uint32_t data=41; int64_t ticks=1000; Dynarmic::A32::Jit* jit=nullptr;
    uint32_t MemoryRead32(uint32_t a) override { if(a==0x2000)return data;assert(a>=0x1000&&a<0x1010);return code[(a-0x1000)/4]; }
    uint8_t MemoryRead8(uint32_t) override {assert(false);return 0;}
    uint16_t MemoryRead16(uint32_t) override {assert(false);return 0;}
    uint64_t MemoryRead64(uint32_t) override {assert(false);return 0;}
    void MemoryWrite8(uint32_t,uint8_t) override {assert(false);}
    void MemoryWrite16(uint32_t,uint16_t) override {assert(false);}
    void MemoryWrite32(uint32_t a,uint32_t v) override {assert(a==0x2000);data=v;}
    void MemoryWrite64(uint32_t,uint64_t) override {assert(false);}
    bool MemoryWriteExclusive32(uint32_t a,uint32_t v,uint32_t e) override {assert(a==0x2000);return __sync_bool_compare_and_swap(&data,e,v);}
    void InterpreterFallback(uint32_t,size_t) override {assert(false);}
    void ExceptionRaised(uint32_t,Dynarmic::A32::Exception) override {assert(false);}
    void CallSVC(uint32_t) override {jit->HaltExecution();}
    void AddTicks(uint64_t n) override {ticks-=n;}
    uint64_t GetTicksRemaining() override {return ticks>0?ticks:0;}
};
int main(){
 Callbacks cb;Dynarmic::ExclusiveMonitor monitor(1);Dynarmic::A32::UserConfig c{};c.callbacks=&cb;c.processor_id=0;c.global_monitor=&monitor;c.arch_version=Dynarmic::A32::ArchVersion::v6K;
 Dynarmic::A32::Jit jit(c);cb.jit=&jit;jit.Regs()[15]=0x1000;jit.Run();assert(jit.Regs()[0]==7&&jit.Regs()[1]==12);
 jit.ExtRegs()[0]=0x3f800000;auto ctx=jit.SaveContext();jit.Regs()[0]=99;jit.ExtRegs()[0]=0;jit.LoadContext(ctx);assert(jit.Regs()[0]==7&&jit.ExtRegs()[0]==0x3f800000);
 cb.code[0]=0xe3a00009;jit.InvalidateCacheRange(0x1000,4);jit.Regs()[15]=0x1000;cb.ticks=1000;jit.Run();assert(jit.Regs()[0]==9&&jit.Regs()[1]==14);
 cb.code={0xe1920f9f,0xe2800001,0xe1821f90,0xef000000};jit.ClearCache();jit.Regs()[15]=0x1000;jit.Regs()[2]=0x2000;cb.ticks=1000;jit.Run();assert(jit.Regs()[0]==42&&jit.Regs()[1]==0&&cb.data==42);
 cb.code={0xe1920f9f,0xe5823000,0xe1821f90,0xef000000};jit.ClearCache();jit.Regs()[15]=0x1000;jit.Regs()[2]=0x2000;jit.Regs()[3]=99;cb.data=41;cb.ticks=1000;jit.Run();assert(jit.Regs()[1]==1&&cb.data==99);
 std::cout<<"ARM64 JIT: arithmetic, context restore, cache invalidation and exclusive-store smoke checks passed\n";
}
