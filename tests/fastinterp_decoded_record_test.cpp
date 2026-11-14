// SPDX-License-Identifier: GPL-2.0-or-later
// Original Cara storage-format witness. No ROM/OpenGL or timing-policy dependency.
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include "core/arm/fastinterp/fastinterp_types.h"
using namespace Core::FastInterp;
static unsigned checks;
static void Check(bool value) { ++checks; if (!value) { std::cerr << "record check " << checks << " failed\n"; std::exit(1); } }
int main() {
 static_assert(std::is_trivially_copyable_v<DecodedInst>);
 static_assert(std::is_standard_layout_v<DecodedInst>);
 static_assert(MAX_BLOCK_SIZE == 32);
 static_assert(sizeof(BasicBlock) == (CARA_FASTINTERP_COMPACT_DECODED ? 544 : 672));
 constexpr unsigned count=static_cast<unsigned>(Opcode::OpcodeCount);
 for(unsigned op=0;op<count;++op) for(unsigned cond=0;cond<16;++cond)
 for(unsigned al=0;al<2;++al) for(unsigned initial_bank=0;initial_bank<2;++initial_bank) {
  DecodedInst inst{}; inst.dispatch_index=initial_bank*count+op;
  inst.cond=cond;inst.rd=15;inst.inst_size=16;inst.ticks=255;
  std::memset(&inst.vfp4,0xa5,sizeof(inst.vfp4));
  // Setter must make semantic identity immediate, before PrepareDispatch.
  const auto next=static_cast<Opcode>((op+1)%count);inst.SetOpcode(next);
  Check(inst.GetOpcode()==next);
#if CARA_FASTINTERP_COMPACT_DECODED
  Check(inst.dispatch_index==static_cast<unsigned>(next));
#endif
  inst.dispatch_index=static_cast<unsigned>(inst.GetOpcode())+((al&&cond==14)?count:0);
  Check(inst.GetOpcode()==next);
  Check(inst.dispatch_index==static_cast<unsigned>(next)+((al&&cond==14)?count:0));
  Check(inst.cond==cond&&inst.rd==15&&inst.inst_size==16&&inst.ticks==255);
  const auto* bytes=reinterpret_cast<const unsigned char*>(&inst.vfp4);
  for(unsigned i=0;i<12;++i) Check(bytes[i]==0xa5);
  DecodedInst copy=inst;Check(std::memcmp(&copy,&inst,sizeof(inst))==0);
 }
 // Orthogonal field boundaries, including unexecuted high tick costs and raw-size 31.
 for(unsigned dispatch=0;dispatch<2*count;++dispatch) for(unsigned value=0;value<256;++value) {
  DecodedInst inst{};inst.SetOpcode(static_cast<Opcode>(dispatch%count));inst.dispatch_index=dispatch;inst.cond=value&15;inst.rd=(value>>4)&15;
  inst.inst_size=value&31;inst.ticks=value;inst.imm32=0x89abcdef;
  Check(inst.dispatch_index==dispatch);Check(inst.GetOpcode()==static_cast<Opcode>(dispatch%count));
  Check(inst.cond==(value&15));Check(inst.rd==((value>>4)&15));
  Check(inst.inst_size==(value&31));Check(inst.ticks==value);Check(inst.imm32==0x89abcdef);
 }
 DecodedInst zero{};Check(zero.GetOpcode()==Opcode::Invalid&&zero.ticks==0&&zero.inst_size==0);
 zero.SetOpcode(Opcode::EndBlock);Check(zero.GetOpcode()==Opcode::EndBlock&&zero.ticks==0&&zero.inst_size==0);
 zero.SetOpcode(Opcode::PcSetup);zero.cond=14;Check(zero.GetOpcode()==Opcode::PcSetup&&zero.ticks==0&&zero.inst_size==0);
 std::cout << checks << " decoded-record field/setter/copy/sentinel checks passed; record=" << sizeof(DecodedInst) << " block=" << sizeof(BasicBlock) << "\n";
}
