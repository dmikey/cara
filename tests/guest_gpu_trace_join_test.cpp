// SPDX-License-Identifier: GPL-2.0-or-later
#include "cara/guest_gpu_trace_join.h"
#include <cstdlib>
#include <iostream>
#include <sstream>
using namespace Cara::GuestGPUTrace;
using namespace Cara::GuestGPUTrace::Join;
static unsigned checks=0;
static void Require(bool b,const char* why){++checks;if(!b){std::cerr<<why<<'\n';std::exit(1);}}
static Record GX(std::uint64_t seq,std::uint64_t submission,std::uint64_t cmd=1) {
    return {seq,60,submission,100,200,300,static_cast<std::uint32_t>(Kind::GXBegin),11,2,1,cmd,0,0,(std::uint64_t(0x30001000)<<32)|4096};
}
static Record Draw(std::uint64_t seq,std::uint64_t submission) {
    return {seq,60,submission,101,202,303,static_cast<std::uint32_t>(Kind::Draw),0,0,0,0,0,0,0};
}
int main(){
    State s;
    Require(!s.Read(1,true,true,true).gx_valid,"no stale initial GX");
    auto gx=s.GX(1,GX(10,7),true,11,0);
    auto a=s.Read(1,true,true,true);
    Require(a.gx_valid&&!a.draw_valid&&a.span_valid&&a.gx.sequence==10&&a.gx.submission==7,"GX key");
    Require(a.gx.thread==11&&a.gx.object==2&&a.gx.core==1&&a.execution_thread==11&&a.execution_core==0,"queue/timer/execution owners distinct");
    auto draw=s.Draw(1,Draw(13,7),true,11,0);a=s.Read(1,true,true,true);
    Require(a.draw_valid&&a.anchor.sequence==13&&a.anchor.timer0_ticks==101&&a.anchor.core==0,"draw uses committed anchor timestamps");
    auto nested=s.GX(1,GX(20,8),true,1,1);
    Require(!s.Read(1,true,true,true).draw_valid&&s.Read(1,true,true,true).gx.submission==8,"nested GX clears parent draw");
    s.Pop(nested);Require(s.Read(1,true,true,true).anchor.sequence==13,"nested GX restores exact parent");
    auto deferred=s.Deferred(1);a=s.Read(1,true,true,true);
    Require(!a.gx_valid&&!a.draw_valid&&a.reason==Reason::DeferredBatch,"deferred batch never borrows current draw/list");
    s.Pop(deferred);Require(s.Read(1,true,true,true).draw_valid,"deferred mask scoped");
    s.Pop(draw);a=s.Read(1,true,true,true);
    Require(a.gx_valid&&!a.draw_valid&&a.anchor.sequence==10,"after draw never reuse latest draw");
    auto next=s.Draw(1,Draw(16,7),false,0,0);a=s.Read(1,true,true,true);
    Require(a.draw_valid&&a.anchor.sequence==16&&!a.execution_owner_valid,"unknown actual guest owner explicit");
    s.Pop(next);
    for(auto bad:{Draw(17,8),Draw(9,7),GX(18,7)}) {
        auto t=s.Draw(1,bad,true,11,0);a=s.Read(1,true,true,true);
        Require(!a.gx_valid&&!a.draw_valid&&a.reason==Reason::InvalidAnchor,"reject wrong submission/order/kind");s.Pop(t);
    }
    Require(s.Read(1,false,true,true).reason==Reason::Inactive,"outside active trace fails closed");
    Require(s.Read(1,true,false,true).reason==Reason::ForeignOwner,"foreign host thread fails closed");
    Require(s.Read(1,true,true,false).reason==Reason::TraceOverflow,"trace overflow invalidates join");
    Require(s.Read(2,true,true,true).reason==Reason::GenerationMismatch,"Arm/restart invalidates old state");
    s.Pop(gx);Require(!s.Read(1,true,true,true).gx_valid,"after GX no latest submission leak");
    auto transfer=s.GX(2,GX(30,9,3),true,1,0);
    Require(s.Read(2,true,true,true).gx_valid&&!s.Read(2,true,true,true).span_valid,"transfer union never interpreted as command span");
    auto impossible=s.Draw(2,Draw(31,9),true,1,0);
    Require(!s.Read(2,true,true,true).draw_valid,"draw under non-command-list GX rejected");s.Pop(impossible);s.Pop(transfer);
    std::array<Token,State::Capacity> tokens;
    for(unsigned i=0;i<tokens.size();++i)tokens[i]=s.GX(3,GX(40+i,i+10),true,1,0);
    Require(!s.Poisoned(),"bounded capacity admitted");auto overflow=s.GX(3,GX(60,99),true,1,0);
    Require(!overflow.armed&&s.Poisoned()&&s.Read(3,true,true,true).reason==Reason::ContextOverflow,"bounded overflow poisons capture");
    for(unsigned i=tokens.size();i>0;--i)s.Pop(tokens[i-1]);
    Require(!s.Read(3,true,true,true).gx_valid,"overflow remains fail closed after unwind");
    auto fresh=s.GX(4,GX(70,100),true,1,0);Require(s.Read(4,true,true,true).gx_valid&&!s.Poisoned(),"new Arm generation clears poison");
    s.Pop(tokens[0]);Require(s.Read(4,true,true,true).gx.submission==100,"old RAII token cannot restore stale generation");s.Pop(fresh);
    // Real RAII unwinds correctly through exceptions without retaining a draw.
    state.Reset(5);try {Scope outer;outer.Set(state.GX(5,GX(80,101),true,1,0));Scope inner;inner.Set(state.Draw(5,Draw(81,101),true,1,0));throw 1;}catch(int){}
    Require(!state.Read(5,true,true,true).gx_valid,"exception scope restoration");
    std::ostringstream json;WriteJSON(json,Unknown(Reason::NoGX,5));
    Require(json.str().find("\"gx_valid\":false")!=std::string::npos&&json.str().find("\"submission\"")==std::string::npos,"unknown JSON cannot look joined");
    std::cout<<checks<<" scoped GX/draw join checks passed; no clocks or GL calls\n";
}
