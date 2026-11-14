// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include "core/arm/fastinterp/fastinterp_cache.h"
using namespace Core::FastInterp;
static void Check(bool ok){if(!ok)throw std::runtime_error("cache capacity oracle mismatch");}
namespace Core::FastInterp {
struct SecondaryLinkCacheTestAccess {
    static unsigned Slot(const BlockCache& c,const BasicBlock* b){return b-c.blocks_.get();}
    static void Mapping(const BlockCache& c,const std::vector<u32>& tags,const std::vector<u8>& lru) {
        Check(std::equal(tags.begin(),tags.end(),c.tags_.begin()));
        Check(std::equal(lru.begin(),lru.end(),c.lru_state_.begin()));
    }
};
}
// Independent tag/NRU model retains the pre-heap replacement algorithm, not
// block storage or the implementation's lookup helpers.
struct Oracle {
    unsigned capacity,ways;std::vector<u32> tags,end;std::vector<u8> lru;
    Oracle(unsigned n,unsigned w):capacity(n),ways(w),tags(n,INVALID_TAG),end(n),lru(n/w){}
    void Clear(){std::fill(tags.begin(),tags.end(),INVALID_TAG);std::fill(lru.begin(),lru.end(),0);}
    unsigned Set(u32 pc)const{u32 h=pc*0x9e3779b9u;h^=h>>16;return(h>>4)&(capacity/ways-1);}
    void Touch(unsigned set,unsigned way){auto& bits=lru[set];bits|=1u<<way;if(bits==(1u<<ways)-1)bits=1u<<way;}
    unsigned Lookup(u32 pc,bool allocate,bool& hit) {
        unsigned set=Set(pc),base=set*ways,way=0;
        while(way<ways&&tags[base+way]!=pc)++way;
        hit=way<ways;
        if(!hit){if(!allocate)return capacity;way=0;while(way<ways&&(lru[set]&(1u<<way)))++way;if(way==ways)way=0;tags[base+way]=pc;end[base+way]=pc;}
        Touch(set,way);return base+way;
    }
    void Invalidate(u32 pc,unsigned length){for(unsigned i=0;i<capacity;++i)if(tags[i]!=INVALID_TAG&&u64(tags[i])<u64(pc)+length&&end[i]>pc)tags[i]=INVALID_TAG;}
};
int main() {
    const char* old=std::getenv("CARA_FASTINTERP_CACHE_ENTRIES");bool present=old;std::string saved=old?old:"";
    for(const char* value:{"","16384","32768","65536","32k","32768 "}) {
        if(*value)setenv("CARA_FASTINTERP_CACHE_ENTRIES",value,1);else unsetenv("CARA_FASTINTERP_CACHE_ENTRIES");
        auto c=std::make_unique<BlockCache>();unsigned expected=std::strcmp(value,"32768")==0?32768:16384;
        Check(c->Capacity()==expected&&c->BlockStorageBytes()==expected*sizeof(BasicBlock));
        Check(c->OwnedBytes()==sizeof(BlockCache)+c->BlockStorageBytes());
        setenv("CARA_FASTINTERP_CACHE_ENTRIES",expected==16384?"32768":"16384",1);
        Check(c->Capacity()==expected); // Immutable after construction.
    }
    if(present)setenv("CARA_FASTINTERP_CACHE_ENTRIES",saved.c_str(),1);else unsetenv("CARA_FASTINTERP_CACHE_ENTRIES");
    u64 operations=0;
    for(unsigned capacity:{DEFAULT_CACHE_SIZE,MAX_CACHE_SIZE})for(unsigned ways:{4u,8u}) {
        auto c=std::make_unique<BlockCache>(capacity);c->SetEightWay(ways==8);c->SetChainRecency(false);c->SetSimdTags(false);c->SetProfiling(true);
        Oracle oracle(capacity,ways);std::vector<u32> pcs;
        for(u32 pc=0x1000;pcs.size()<ways*4;pc+=2)if(oracle.Set(pc)==oracle.Set(0x1000))pcs.push_back(pc);
        std::mt19937 rng(124135);std::array<u16,64> tokens;tokens.fill(INVALID_BLOCK_SLOT);
        for(unsigned i=0;i<100000;++i) {
            u32 pc=pcs[rng()%pcs.size()];if(i%11==0)pc=(rng()%0x3fffffff)*4;
            bool allocate=i%3==0,expected_hit;unsigned slot=oracle.Lookup(pc,allocate,expected_hit);BasicBlock* actual;
            if(allocate){bool hit;actual=c->LookupOrAllocate(pc,hit);Check(hit==expected_hit);}
            else {auto& token=tokens[rng()%tokens.size()];actual=c->LookupSecondary(pc,token);if(!actual)actual=c->Lookup(pc,&token);}
            Check(bool(actual)==(slot!=capacity));
            if(actual){Check(SecondaryLinkCacheTestAccess::Slot(*c,actual)==slot);actual->inst_count=2;actual->end_pc=pc+8;oracle.end[slot]=pc+8;}
            if(i%211==0){c->InvalidateRange(pc,8);oracle.Invalidate(pc,8);}
            if(i%1987==0){c->Clear();oracle.Clear();}
            if(i%499==0)SecondaryLinkCacheTestAccess::Mapping(*c,oracle.tags,oracle.lru);
            ++operations;
        }
        SecondaryLinkCacheTestAccess::Mapping(*c,oracle.tags,oracle.lru);
        std::cout<<"entries="<<capacity<<" ways="<<ways<<" metadata_bytes="<<sizeof(BlockCache)
                 <<" block_bytes="<<c->BlockStorageBytes()<<" owned_bytes="<<c->OwnedBytes()<<'\n';
    }
    std::cout<<operations<<" independent tag/NRU/victim operations; exact env/default parsing, immutable capacity and owned storage passed\n";
}
