// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#include "wifi_envelope.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
using namespace Cara::Relay;
static unsigned checks=0;
static void Check(bool b) { ++checks;if(!b){std::cerr<<"check "<<checks<<" failed\n";std::abort();} }
static void Put(Bytes& p,std::size_t n,unsigned x) {p[n]=x>>8;p[n+1]=x&255;}
static Packet P(Kind k,Bytes data) {return {k,6,{2,1,2,3,4,5},Broadcast,std::move(data)};}
static Bytes LLC(unsigned type,std::size_t size) {Bytes p(size);p[0]=p[1]=0xaa;p[2]=3;Put(p,6,type);return p;}
int main() {
    Bytes auth{0,0,1,0,0,0};
    Bytes assoc{0x31,4,0,0,1,0xc0,0,8,'0','0','0','0','0','0','0','1'};
    Bytes beacon(12);beacon.insert(beacon.end(),{0,8,'0','0','0','0','0','0','0','1'});
    Bytes map{0,2,2,1,2,3,4,5,0,1,2,6,7,8,9,10,0,2};
    auto secure=LLC(0x876d,22+1478);Put(secure,8,14+1478);Put(secure,12,10+1478);secure[15]=1;Put(secure,18,0xffff);Put(secure,20,1);
    auto start=LLC(0x888e,56);Put(start,8,0x201);Put(start,10,1);Put(start,12,1);
    auto logoff=LLC(0x888e,672);Put(logoff,8,0x202);Put(logoff,12,2);logoff[26]=2;logoff[27]=16;Put(logoff,64,1);Put(logoff,104,2);
    std::vector<Packet> fixtures{P(Kind::Beacon,beacon),P(Kind::Data,secure),P(Kind::Authentication,auth),P(Kind::AssociationResponse,assoc),P(Kind::Deauthentication,{}),P(Kind::NodeMap,map),P(Kind::Data,start),P(Kind::Data,logoff)};
    if(const char* dir=std::getenv("CORPUS_DIR")) {
        unsigned i=0;for(const auto& p:fixtures){auto b=Encode(p);Check(bool(b));std::ofstream out(std::string(dir)+"/seed"+std::to_string(i++),std::ios::binary);Check(out.good());out.write(reinterpret_cast<const char*>(b->data()),b->size());Check(out.good());}
    }
    const Mac local{2,9,8,7,6,5};
    for(const auto& p:fixtures) {
        auto b=Encode(p);Check(bool(b));Check(b->size()==18+p.payload.size());Check((*b)[0]==1&&(*b)[16]==(p.payload.size()>>8)&&(*b)[17]==(p.payload.size()&255));
        auto decoded=Decode(*b);Check(bool(decoded));Check(decoded->payload==p.payload&&decoded->transmitter==p.transmitter&&decoded->destination==p.destination&&decoded->kind==p.kind);
        Check(ShouldDeliver(*decoded,local,6));Check(!ShouldDeliver(*decoded,local,7));Check(!ShouldDeliver(*decoded,p.transmitter,6));
        auto q=*decoded;q.destination={2,3,3,3,3,3};Check(!ShouldDeliver(q,local,6));q.destination=local;Check(ShouldDeliver(q,local,6));
        Check(ShouldDeliver(*decoded,local,0)==(p.kind==Kind::Beacon));
        for(std::size_t n=0;n<b->size();++n) { auto cut=*b;cut.resize(n);Check(!Decode(cut)); }
        auto bad=*b;bad.push_back(0);Check(!Decode(bad));
        for(auto pos:{0,1,2,3,16,17}) {bad=*b;bad[pos]=255;Check(!Decode(bad));}
        for(unsigned ch=0;ch<256;++ch){auto q=p;q.channel=ch;Check(Valid(q)==(ch>=1&&ch<=14));}
        for(std::size_t n=0;n<p.payload.size();++n) {
            if(p.kind==Kind::Beacon || p.kind==Kind::AssociationResponse) continue; // valid complete tag prefixes may exist
            auto q=p;q.payload.resize(n);Check(!Valid(q));
        }
    }
    auto bad=secure;Put(bad,8,13);Check(!PayloadValid(Kind::Data,bad));
    bad=secure;Put(bad,12,0);Check(!PayloadValid(Kind::Data,bad));
    bad=secure;bad[14]=1;Check(!PayloadValid(Kind::Data,bad));
    bad=secure;Put(bad,20,0);Check(!PayloadValid(Kind::Data,bad));
    bad=secure;bad.push_back(0);Put(bad,8,1493);Put(bad,12,1489);Check(!PayloadValid(Kind::Data,bad));
    for(auto offset:{0,1,2,3,4,5,6,7}){bad=secure;bad[offset]^=1;Check(!PayloadValid(Kind::Data,bad));}
    for(unsigned count=0;count<256;++count) {bad=logoff;bad[26]=count;Check(PayloadValid(Kind::Data,bad)==(count>=1&&count<=16));}
    bad=logoff;Put(bad,104,1);Check(!PayloadValid(Kind::Data,bad));
    bad=logoff;Put(bad,64,17);Check(!PayloadValid(Kind::Data,bad));
    bad=logoff;Put(bad,64,0);Put(bad,104,0);Check(PayloadValid(Kind::Data,bad)); // generator reset slots
    bad=logoff;bad[27]=17;Check(!PayloadValid(Kind::Data,bad));
    bad=map;bad[17]=1;Check(!PayloadValid(Kind::NodeMap,bad));
    bad=map;std::copy_n(bad.begin()+2,6,bad.begin()+10);Check(!PayloadValid(Kind::NodeMap,bad));
    bad=map;bad[0]=255;Check(!PayloadValid(Kind::NodeMap,bad));
    bad=beacon;bad.back()=255;Check(PayloadValid(Kind::Beacon,bad));bad[13]=255;Check(!PayloadValid(Kind::Beacon,bad));
    auto native=WireNodeMapToNative(map);Check(bool(native));std::size_t count=0;std::memcpy(&count,native->data(),sizeof(count));Check(count==2);
    std::uint16_t id=0;std::memcpy(&id,native->data()+sizeof(count)+6,2);Check(id==1);
    Check(NativeNodeMapToWire(*native)==std::optional<Bytes>(map));
    for(std::size_t n=0;n<native->size();++n){bad=*native;bad.resize(n);Check(!NativeNodeMapToWire(bad));}
    bad=*native;count=~std::size_t(0);std::memcpy(bad.data(),&count,sizeof(count));Check(!NativeNodeMapToWire(bad));
    Bytes full_map{0,16};
    for(unsigned i=1;i<=16;++i) full_map.insert(full_map.end(),{2,0,0,0,0,static_cast<unsigned char>(i),0,static_cast<unsigned char>(i)});
    Check(PayloadValid(Kind::NodeMap,full_map));
    Check(NativeNodeMapToWire(*WireNodeMapToNative(full_map))==std::optional<Bytes>(full_map));
    Check(WireNodeMapToNative(Bytes{0,0})->size()==sizeof(std::size_t));
    Bytes max_beacon(12);
    for(unsigned i=0;i<15;++i) {max_beacon.insert(max_beacon.end(),{221,255});max_beacon.resize(max_beacon.size()+255);}
    max_beacon.insert(max_beacon.end(),{221,201});max_beacon.resize(max_beacon.size()+201);
    auto max_packet=P(Kind::Beacon,max_beacon);Check(max_beacon.size()==MaxData);
    auto max_wire=Encode(max_packet);Check(bool(max_wire)&&max_wire->size()==MaxOpaque);Check(bool(Decode(*max_wire)));
    max_packet.payload.push_back(0);Check(!Encode(max_packet));
    std::mt19937 rng(0xcaba2026);
    for(unsigned i=0;i<100000;++i) {
        auto valid=Encode(fixtures[rng()%fixtures.size()]);bad=*valid;
        for(unsigned j=0,changes=1+rng()%8;j<changes;++j)bad[rng()%bad.size()]=rng();
        if(auto result=Decode(bad)){Check(Encode(*result)==std::optional<Bytes>(bad));Check(Valid(*result));}
    }
    std::cout<<"PASS "<<checks<<" assertions; 8 forms; 100000 deterministic mutations\n";
}
