// Real architectural register storage before an address space/JIT is bound.
// No guest page table is invented and no CPU is run to obtain dormant state.
#pragma once
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/context.h>
class DynarmicRegisterState {
    Dynarmic::A32::Context dormant;
public:
    DynarmicRegisterState() { dormant.Regs()={};dormant.ExtRegs()={};dormant.SetCpsr(0x10);dormant.SetFpscr(0); }
    auto& Regs(Dynarmic::A32::Jit* jit) { return jit?jit->Regs():dormant.Regs(); }
    const auto& Regs(const Dynarmic::A32::Jit* jit) const { return jit?jit->Regs():dormant.Regs(); }
    auto& ExtRegs(Dynarmic::A32::Jit* jit) { return jit?jit->ExtRegs():dormant.ExtRegs(); }
    const auto& ExtRegs(const Dynarmic::A32::Jit* jit) const { return jit?jit->ExtRegs():dormant.ExtRegs(); }
    auto Cpsr(const Dynarmic::A32::Jit* jit) const { return jit?jit->Cpsr():dormant.Cpsr(); }
    void SetCpsr(Dynarmic::A32::Jit* jit,unsigned v) { if(jit)jit->SetCpsr(v);else dormant.SetCpsr(v); }
    auto Fpscr(const Dynarmic::A32::Jit* jit) const { return jit?jit->Fpscr():dormant.Fpscr(); }
    void SetFpscr(Dynarmic::A32::Jit* jit,unsigned v) { if(jit)jit->SetFpscr(v);else dormant.SetFpscr(v); }
    void Capture(const Dynarmic::A32::Jit* jit,Dynarmic::A32::Context& out) const { if(jit)jit->SaveContext(out);else out=dormant; }
    void Restore(Dynarmic::A32::Jit* jit,const Dynarmic::A32::Context& in) { if(jit)jit->LoadContext(in);else dormant=in; }
};
