// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_index_scan_memo.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

using Memo=Cara::ExactMetalIndexScanMemo;
static unsigned checks=0;
static void Require(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
static Memo::Result Scan(std::span<const std::uint8_t> bytes,unsigned width) {
    std::set<std::uint32_t> values;
    for(std::size_t i=0;i<bytes.size();i+=width)
        values.insert(width==1?bytes[i]:std::uint32_t{bytes[i]}|(std::uint32_t{bytes[i+1]}<<8));
    return {*values.begin(),*values.rbegin(),values.size()};
}
int main() {try {
    Memo memo;
    const auto key=[](unsigned i,unsigned count=6,unsigned width=1) {
        return Memo::Key{0x20000000ull+std::uint64_t{i}*65536,count,width};
    };
    std::vector<std::uint8_t> small{7,8,9,7,9,8};
    // These addresses all collide in the legacy 128-entry direct-map formula.
    for(unsigned i=0;i<128;++i){auto k=key(i);memo.Store(k,small,Scan(small,1));}
    for(unsigned i=0;i<128;++i)Require(memo.Lookup(key(i),small)==Scan(small,1),"Associative key lost on legacy collision");
    Require(memo.SmallStats().entries==128 && memo.SmallStats().evictions==0,"Small tier capacity incorrect");
    // Key 0 becomes most recent, so the next insertion must evict key 1.
    Require(memo.Lookup(key(0),small).has_value(),"LRU touch failed");
    memo.Store(key(128),small,Scan(small,1));
    Require(!memo.Lookup(key(1),small) && memo.Lookup(key(0),small).has_value(),"Bounded small LRU eviction incorrect");
    for(unsigned where:{0u,3u,5u}){
        auto changed=small;changed[where]^=0x71;
        Require(!memo.Lookup(key(0),changed),"Changed first/middle/tail byte incorrectly hit");
        memo.Store(key(0),changed,Scan(changed,1));
        Require(memo.Lookup(key(0),changed)==Scan(changed,1),"Changed-byte scan result stale");
        Require(!memo.Lookup(key(0),small),"Restored bytes used wrong scan result");
        memo.Store(key(0),small,Scan(small,1));
    }
    // A deterministic real hash collision still requires all key fields equal.
    const Memo::Key bytes_key{0x20001000,2,1},words_key{0x20001000,1,2};
    std::vector<std::uint8_t> pair{1,2};
    Require(Memo::Hash{}(bytes_key)==Memo::Hash{}(words_key),"Authored hash collision unavailable");
    memo.Store(bytes_key,pair,Scan(pair,1));memo.Store(words_key,pair,Scan(pair,2));
    Require(memo.Lookup(bytes_key,pair)==Scan(pair,1) && memo.Lookup(words_key,pair)==Scan(pair,2),"Width/count hash collision aliased");
    std::vector<std::uint8_t> large(Memo::LargeBytes);
    for(std::size_t i=0;i<large.size();++i)large[i]=std::uint8_t(i*37+(i>>8));
    const auto large_result=Scan(large,2);
    for(unsigned i=0;i<8;++i)memo.Store(key(i,65536,2),large,large_result);
    for(unsigned i=0;i<8;++i)Require(memo.Lookup(key(i,65536,2),large)==large_result,"Large exact memo lost payload");
    Require(memo.LargeStats().bytes==1024*1024 && memo.LargeStats().entries==8,"Large tier byte/entry bound incorrect");
    for(unsigned where:{0u,65535u,131071u}){
        auto changed=large;changed[where]^=1;
        Require(!memo.Lookup(key(0,65536,2),changed),"Large payload mutation incorrectly hit");
    }
    memo.Store(key(8,65536,2),large,large_result);
    Require(!memo.Lookup(key(0,65536,2),large) && memo.LargeStats().bytes<=1024*1024,"Large eviction/bound incorrect");
    Require(memo.Lookup(key(0),small).has_value(),"Large tier evicted an unrelated small entry");
    // No entry pointers escape Lookup: an outer miss/scan can survive an
    // intervening nested capture evicting every entry before its Store.
    const auto outer=key(500);
    Require(!memo.Lookup(outer,small),"Unexpected outer hit");
    for(unsigned i=0;i<300;++i)memo.Store(key(1000+i),small,Scan(small,1));
    memo.Store(outer,small,Scan(small,1));
    Require(memo.Lookup(outer,small)==Scan(small,1),"Interleaved lookup/store retained stale entry pointer");
    for(const auto invalid:{Memo::Key{0x20000000,0,1},Memo::Key{0x20000000,6,0},
            Memo::Key{0x20000000,6,3},Memo::Key{0x100000000ull,6,1},Memo::Key{0xfffffffcu,6,1}}){
        Require(!Memo::Admits(invalid,small),"Invalid count/width/address admitted");
        memo.Store(invalid,small,{0,0,1});Require(!memo.Lookup(invalid,small),"Invalid input was cached");
    }
    Require(!Memo::Admits(key(0,65537,2),large),"Oversized count admitted");
    Require(!Memo::Admits(key(0,6,2),small),"Truncated input admitted");
    Require(memo.SmallStats().bytes<=4*1024*1024 && memo.LargeStats().bytes<=1024*1024,"Memo budgets exceeded");
    {
        Memo full;
        std::vector<std::uint8_t> maximum_small(Memo::SmallBytes,0x5a);
        const auto result=Scan(maximum_small,2);
        for(unsigned i=0;i<129;++i)full.Store(key(i,16384,2),maximum_small,result);
        Require(full.SmallStats().entries==128 && full.SmallStats().bytes==4*1024*1024 &&
                full.SmallStats().evictions==1,"Maximum small-tier ownership exceeded bound");
    }
    std::cout<<"PASS exact_index_memo checks="<<checks<<" small_entries="<<memo.SmallStats().entries
             <<" large_entries="<<memo.LargeStats().entries<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
