// Copyright 2026 Cara contributors; GPL-2.0-or-later.
// Synthetic instructions/data only. Explicit test, before any guest Run; caller exits afterward.
#include <array>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/archive/binary_oarchive.hpp>
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/core.h"
#include "core/memory.h"
#include "core/mmio.h"

#include "cara/native_mac/arm_dynarmic_adapter_test.h"
namespace Cara::CPUJitTest {
static void Require(bool value, Result& r) {
    if(!value) throw std::runtime_error("actual Dynarmic adapter fixture assertion failed");
    ++r.checks;
}
struct Registers {
    std::array<u32,16> reg{}; std::array<u32,64> vfp{};
    u32 cpsr{},fpscr{},fpexc{},uprw{},uro{};
    bool operator==(const Registers&) const=default;
};
static Registers Capture(ARM_Interface& c) {
    Registers r;for(int i=0;i<16;++i)r.reg[i]=c.GetReg(i);
    for(int i=0;i<64;++i)r.vfp[i]=c.GetVFPReg(i);
    r.cpsr=c.GetCPSR();r.fpscr=c.GetVFPSystemReg(VFP_FPSCR);
    r.fpexc=c.GetVFPSystemReg(VFP_FPEXC);r.uprw=c.GetCP15Register(CP15_THREAD_UPRW);
    r.uro=c.GetCP15Register(CP15_THREAD_URO);return r;
}
static void Pattern(ARM_Interface& c,u32 seed) {
    for(int i=0;i<16;++i)c.SetReg(i,seed+i*4);
    for(int i=0;i<64;++i)c.SetVFPReg(i,0x3f000000+seed+i);
    c.SetCPSR(0x60000010);c.SetVFPSystemReg(VFP_FPSCR,0x01000000);
    c.SetVFPSystemReg(VFP_FPEXC,0x40000000);
    c.SetCP15Register(CP15_THREAD_UPRW,seed+0x100);c.SetCP15Register(CP15_THREAD_URO,seed+0x200);
}
class IO final:public Memory::MMIORegion {
public: unsigned reads=0,writes=0;u32 value=0x12345678;
 bool IsValidAddress(VAddr a) override{return a==0x3000;}
 u32 Read32(VAddr a) override{if(a!=0x3000)throw std::runtime_error("unexpected MMIO read");++reads;return value;}
 void Write32(VAddr a,u32 v) override{if(a!=0x3000)throw std::runtime_error("unexpected MMIO write");++writes;value=v;}
 u8 Read8(VAddr) override{throw std::runtime_error("unexpected MMIO width");}
 u16 Read16(VAddr) override{throw std::runtime_error("unexpected MMIO width");}
 u64 Read64(VAddr) override{throw std::runtime_error("unexpected MMIO width");}
 void Write8(VAddr,u8) override{throw std::runtime_error("unexpected MMIO width");}
 void Write16(VAddr,u16) override{throw std::runtime_error("unexpected MMIO width");}
 void Write64(VAddr,u64) override{throw std::runtime_error("unexpected MMIO width");}
 bool ReadBlock(VAddr,void*,std::size_t) override{throw std::runtime_error("unexpected MMIO block");}
 bool WriteBlock(VAddr,const void*,std::size_t) override{throw std::runtime_error("unexpected MMIO block");}
};
void RunFactorySystemArchiveTest(Core::System& system,Result& r) {
 const unsigned count=system.GetNumCores();Require(count==4,r);
 std::array<Registers,4> expected;
 for(unsigned i=0;i<count;++i) {
  Require(dynamic_cast<ARM_Dynarmic*>(&system.GetCore(i))!=nullptr,r);
  expected[i]=Capture(system.GetCore(i));
 }
 // Caller preflight requires an empty private user directory; no user slot is overwritten.
 system.SaveState(1);
 for(unsigned i=0;i<count;++i) {
  auto& cpu=system.GetCore(i);cpu.SetReg(0,expected[i].reg[0]^0x01020304);
  cpu.SetVFPReg(63,expected[i].vfp[63]^1);
  cpu.SetVFPSystemReg(VFP_FPEXC,expected[i].fpexc^0x40000000);
  cpu.SetCP15Register(CP15_THREAD_UPRW,expected[i].uprw^0x100);
 }
 system.LoadState(1);Require(system.GetNumCores()==count,r);
 for(unsigned i=0;i<count;++i) {
  Require(dynamic_cast<ARM_Dynarmic*>(&system.GetCore(i))!=nullptr,r);
  Require(Capture(system.GetCore(i))==expected[i],r);
 }
 r.full_system_archive=true;
}
Result RunActualAdapterTest(Core::System& system) {
 Result r;auto& memory=system.Memory();const auto original_table=memory.GetCurrentPageTable();
 struct Restore { Memory::MemorySystem& mem;std::shared_ptr<Memory::PageTable> table;
  ~Restore(){mem.SetCurrentPageTable(table);} } restore{memory,original_table};
 // Actual adapter construction and all four dormant architectural states, not a storage mirror.
 for(unsigned id=0;id<4;++id) {
  auto timer=std::make_shared<Core::Timing::Timer>(system.CoreTiming());
  ARM_Dynarmic cpu(&system,id,timer);cpu.SetPageTable(nullptr);
  cpu.PrepareReschedule();cpu.InvalidateCacheRange(0x1000,4);cpu.PurgeState();
  Pattern(cpu,0x10000+id*0x100);auto expected=Capture(cpu);
  auto ctx=cpu.NewContext();cpu.SaveContext(ctx);cpu.SetReg(0,0x20000);cpu.SetVFPReg(63,0);
  cpu.SetVFPSystemReg(VFP_FPEXC,0);cpu.LoadContext(ctx);
  Require(Capture(cpu)==expected,r);
  // Exercise exact generic ARM_Interface save/load, including same-null SetPageTable and CP15.
  std::stringstream bytes;{boost::archive::binary_oarchive a(bytes);a<<static_cast<const ARM_Interface&>(cpu);}
  Pattern(cpu,0x30000);{boost::archive::binary_iarchive a(bytes);a>>static_cast<ARM_Interface&>(cpu);}
  Require(Capture(cpu)==expected,r);
  auto A=std::make_shared<Memory::PageTable>(),B=std::make_shared<Memory::PageTable>();
  for(auto p:{A,std::shared_ptr<Memory::PageTable>{},B,A}) {
   cpu.SetPageTable(p);Require(Capture(cpu)==expected,r);
   cpu.PrepareReschedule();cpu.InvalidateCacheRange(0x1000,4);cpu.ClearInstructionCache();
  }
  cpu.SetPageTable(nullptr);Require(Capture(cpu)==expected,r);
 }
 r.adapter_state=true;
 // Actual mapped RAM, memory callbacks and MMIO handler through ARM_Dynarmic::Step.
 auto timer=std::make_shared<Core::Timing::Timer>(system.CoreTiming());ARM_Dynarmic cpu(&system,0,timer);
 auto p=std::make_shared<Memory::PageTable>();auto backing=std::make_shared<BufferMem>(0x2000);
 memory.MapMemoryRegion(*p,0x1000,0x2000,MemoryRef(backing));auto io=std::make_shared<IO>();
 memory.MapIoRegion(*p,0x3000,0x1000,io);memory.SetCurrentPageTable(p);cpu.SetPageTable(p);
 auto step=[&](u32 word){memory.Write32(0x1000,word);cpu.InvalidateCacheRange(0x1000,4);cpu.SetPC(0x1000);cpu.SetCPSR(0x10);cpu.Step();};
 cpu.SetReg(2,0x3000);step(0xe5920000);Require(cpu.GetReg(0)==0x12345678&&io->reads==1,r);
 cpu.SetReg(0,0x89abcdef);step(0xe5820000);Require(io->value==0x89abcdef&&io->writes==1,r);r.mmio=true;
 step(0xe3a00007);Require(cpu.GetReg(0)==7,r);step(0xe3a00009);Require(cpu.GetReg(0)==9,r);
 // LoadContext must clear the real monitor; STREX after it must fail and preserve RAM.
 memory.Write32(0x2000,41);cpu.SetReg(2,0x2000);step(0xe1920f9f);
 Require(cpu.GetReg(0)==41,r);auto ctx=cpu.NewContext();cpu.SaveContext(ctx);cpu.LoadContext(ctx);
 cpu.SetReg(0,42);step(0xe1821f90);Require(cpu.GetReg(1)==1&&memory.Read32(0x2000)==41,r);
 step(0xe1920f9f);cpu.SetReg(0,42);step(0xe1821f90);
 Require(cpu.GetReg(1)==0&&memory.Read32(0x2000)==42,r);r.monitor=true;
 // Do not claim GPU-cached-page coherence or whole-System save/load from this synthetic RAM test.
 return r;
}
} // namespace Cara::CPUJitTest
