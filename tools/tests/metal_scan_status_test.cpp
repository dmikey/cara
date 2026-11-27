// Clearly synthetic CPU contract tests, not native GPU completion evidence.
#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include "video_core/renderer_metal/pica_metal_guest_authority.h"
using namespace Cara;
struct Writer final : MetalGuestWriter {
 mutable unsigned statuses=0,covers=0;
 mutable MetalGuestWriteStatus state=MetalGuestWriteStatus::Pending;
 unsigned fail_at=0,reject_at=0,complete_at=0;
 MetalGuestWriteStatus Status() const override { ++statuses;return state; }
 bool Covers(PAddr a,PAddr b) const override {
  ++covers;
  if (fail_at && covers>=fail_at)state=MetalGuestWriteStatus::Failed;
  if (complete_at && covers>=complete_at)state=MetalGuestWriteStatus::Complete;
  return a<b && a>=100 && b<=300 && (!reject_at || covers<reject_at);
 }
 bool ExportGuestBytes(PAddr,PAddr,std::span<u8>) const override { return false; }
};
void require(bool ok){if(!ok)throw std::runtime_error("contract failed");}
template<class F>void reject(F f){bool bad=false;try{f();}catch(const std::exception&){bad=true;}require(bad);}
int main(){
 const bool opt=std::getenv("CARA_METAL_REUSE_SCAN_STATUS") && std::string_view(std::getenv("CARA_METAL_REUSE_SCAN_STATUS"))=="1";
 unsigned tests=0;
 for (unsigned kind=0;kind<6;++kind) {
  MetalGuestAuthority a;auto x=std::make_shared<Writer>();auto y=std::make_shared<Writer>();
  std::array<MetalGuestWrite,16> writes{};
  for(unsigned i=0;i<16;++i)writes[i]={100+8*i,108+8*i,i%2?y:x};
  a.Publish(writes);x->statuses=x->covers=y->statuses=y->covers=0;
  if(kind==1)x->fail_at=3;
  if(kind==2)x->reject_at=3;
  if(kind==3)x->complete_at=3;
  if(kind==4)x->state=MetalGuestWriteStatus::Failed;
  if(kind==5)y->fail_at=8;
  if(kind==1||kind==2||kind==4||kind==5)reject([&]{a.NativeSegments(100,228);});
  else {
   auto rows=a.NativeSegments(100,228);require(rows.size()==16);
   for(unsigned i=0;i<16;++i)require(rows[i].begin==writes[i].begin&&rows[i].end==writes[i].end&&rows[i].writer==writes[i].writer);
   require(x->covers==8&&y->covers==8);
   require(x->statuses==(opt?2:8)&&y->statuses==(opt?2:8));
  }
  ++tests;
 }
 {
  MetalGuestAuthority a;auto x=std::make_shared<Writer>(),y=std::make_shared<Writer>(),z=std::make_shared<Writer>();
  std::array<MetalGuestWrite,6> w{{{100,108,x},{108,116,y},{116,124,z},{124,132,x},{132,140,y},{140,148,z}}};
  a.Publish(w);x->covers=x->statuses=y->covers=y->statuses=z->covers=z->statuses=0;
  auto rows=a.NativeSegments(104,144);require(rows.size()==6&&rows.front().begin==104&&rows.back().end==144);
  require(z->covers==2&&z->statuses==2);require(x->covers==2&&y->covers==2);++tests;
 }
 {MetalGuestAuthority a;require(a.NativeSegments(100,100).empty());reject([&]{a.NativeSegments(110,100);});++tests;}
 std::cout<<"SYNTHETIC_CPU_CONTRACT_PASS cases="<<tests<<" scan_status="<<opt<<'\n';
}
