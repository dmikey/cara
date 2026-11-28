#include "pica_metal_copy_row_runs.hh"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace Cara::MetalTransfer;
struct Copy { unsigned source; std::uint64_t from, to, size; };
using Copies=std::vector<Copy>;
auto plan(const Copies& c) {return PlanTwoSourceCopyRows<unsigned>(std::span<const Copy>{c});}
unsigned checks=0;
void equivalent(const Copies& c) {
    auto p=plan(c);assert(p);std::vector<unsigned char> src[2],a(32768,0xA5),b=a;
    for(unsigned k=0;k<2;++k){src[k].resize(32768);for(unsigned j=0;j<src[k].size();++j)src[k][j]=(j*37+k*71)^(j>>3);}
    for(auto x:c)std::memcpy(a.data()+x.to,src[x.source].data()+x.from,x.size);
    for(unsigned k=0;k<p->count;++k){auto x=p->runs[k];const auto words=x.count*x.width/4;
      for(std::uint64_t w=0;w<words;++w){const auto row=w/(x.width/4),column=w%(x.width/4);
        std::memcpy(b.data()+x.to+row*x.destination_stride+column*4,src[x.source].data()+x.from+row*x.source_stride+column*4,4);}}
    assert(a==b);++checks;
}
void no(const Copies& c){assert(!plan(c));++checks;}
int main(){
    Copies base{{0,0,0,8},{1,0,8,8},{0,16,32,8},{1,16,40,8}};
    equivalent(base);equivalent({{0,0,0,8},{0,16,16,8},{0,32,32,8},{0,48,48,8}});
    for(unsigned rows=2;rows<=40;++rows)for(unsigned a=1;a<=8;++a)for(unsigned b=1;b<=8;++b){
       Copies c;const unsigned stride=4*(a+b+3);
       for(unsigned j=0;j<rows;++j){c.push_back({0,8+j*4*(a+2),16+j*stride,4*a});c.push_back({1,12+j*4*(b+1),16+j*stride+4*(a+1),4*b});}equivalent(c);
    }
    no({});no({base[0],base[1],base[2]});
    auto bad=[&](auto fn){auto c=base;fn(c);no(c);};
    bad([](auto&c){c[0].size=0;});bad([](auto&c){c[0].from=1;});bad([](auto&c){c[0].to=1;});bad([](auto&c){c[0].size=7;});
    bad([](auto&c){c[2].source=2;});bad([](auto&c){c[1].to=4;});bad([](auto&c){c[2].to=0;});bad([](auto&c){c[2].size=4;});
    bad([](auto&c){c[2].from=4;});bad([](auto&c){c[2].from=0;});bad([](auto&c){c[0].from=~std::uint64_t{0}-3;});
    bad([](auto&c){c[3].to=~std::uint64_t{0}-3;});bad([](auto&c){c[2].from=(std::uint64_t{1}<<32);});
    bad([](auto&c){c[2].to=(std::uint64_t{1}<<32);c[3].to=c[2].to+8;});
    auto irregular=base;irregular.push_back({0,36,64,8});irregular.push_back({1,32,72,8});no(irregular);
    irregular=base;irregular.push_back({0,32,68,8});irregular.push_back({1,32,76,8});no(irregular);
    auto single=base;single[3].source=0;no(single);
    std::cout<<"PASS "<<checks<<" exact complete destination bytes including untouched gaps; negative shape/alignment/overlap/overflow guards\n";
}
