// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "core/arm/fastinterp/fastinterp_cache.h"
using namespace Core::FastInterp;
static void Require(bool ok) {if(!ok)throw std::runtime_error("secondary token differential mismatch");}
namespace Core::FastInterp {
struct SecondaryLinkCacheTestAccess {
    static unsigned Slot(const BlockCache& c,const BasicBlock* b){return b-c.blocks_.get();}
    static void Equal(const BlockCache& a,const BlockCache& b) {
        Require(a.capacity_==b.capacity_ &&
            std::equal(a.tags_.begin(),a.tags_.begin()+a.capacity_,b.tags_.begin()) && a.lru_state_==b.lru_state_ &&
            a.observed_executions_==b.observed_executions_ && a.observed_modes_==b.observed_modes_);
        const auto& x=a.profile_;const auto& y=b.profile_;
        Require(x.lookup_calls==y.lookup_calls&&x.lookup_hits==y.lookup_hits&&
            x.lookup_tag_comparisons==y.lookup_tag_comparisons&&
            x.lookup_or_allocate_calls==y.lookup_or_allocate_calls&&
            x.lookup_or_allocate_hits==y.lookup_or_allocate_hits&&
            x.lookup_or_allocate_tag_comparisons==y.lookup_or_allocate_tag_comparisons&&
            x.allocation_misses==y.allocation_misses&&x.empty_allocations==y.empty_allocations&&
            x.live_replacements==y.live_replacements&&x.clears==y.clears&&
            x.invalidation_calls==y.invalidation_calls&&x.invalidated_entries==y.invalidated_entries);
        std::ostringstream sa,sb;a.WriteReplacementProfile(sa);b.WriteReplacementProfile(sb);
        Require(sa.str()==sb.str());
    }
};
}
int main() {
    u64 operations=0;
    for(unsigned capacity:{DEFAULT_CACHE_SIZE,MAX_CACHE_SIZE})for(unsigned ways:{4u,8u})for(bool profiling:{false,true})for(bool recency:{false,true}) {
        auto a=std::make_unique<BlockCache>(capacity),b=std::make_unique<BlockCache>(capacity);
        a->SetEightWay(ways==8);b->SetEightWay(ways==8);
        a->SetChainRecency(recency);b->SetChainRecency(recency);
        a->SetProfiling(profiling);b->SetProfiling(profiling);
        auto index=[ways,capacity](u32 pc){u32 h=pc*0x9e3779b9u;h^=h>>16;return(h>>4)&((capacity/ways)-1);};
        std::vector<u32> pcs;for(u32 pc=0x1000;pcs.size()<ways*4;pc+=2)
            if(index(pc)==index(0x1000))pcs.push_back(pc);
        std::array<u16,64> tokens;tokens.fill(INVALID_BLOCK_SLOT);
        std::mt19937 rng(374291);
        for(unsigned i=0;i<100000;++i) {
            const unsigned caller=rng()%tokens.size();u32 pc=pcs[rng()%pcs.size()];
            if(i%7==0)pc=0x100000+(rng()%256)*4;
            if(i%997==0)pc=INVALID_TAG;
            BasicBlock *x,*y;
            if(i%3==0) {
                bool hx,hy;x=a->LookupOrAllocate(pc,hx);y=b->LookupOrAllocate(pc,hy);Require(hx==hy);
            } else {
                x=a->Lookup(pc);y=b->LookupSecondary(pc,tokens[caller]);
                if(!y)y=b->Lookup(pc,&tokens[caller]);
            }
            Require(bool(x)==bool(y));
            if(x) {
                Require(SecondaryLinkCacheTestAccess::Slot(*a,x)==SecondaryLinkCacheTestAccess::Slot(*b,y));
                if(pc!=INVALID_TAG)Require(x->start_pc==y->start_pc&&x->inst_count==y->inst_count&&x->flags==y->flags);
                // Include mode rejection and poisoned records: token lookup must
                // still touch exactly the same way BEFORE the caller rejects.
                x->flags=y->flags=rng()&1;x->inst_count=y->inst_count=i%5?2:0;
                x->exec_count=y->exec_count=rng();x->end_pc=y->end_pc=pc+8;
            }
            if(i%211==0){a->InvalidateRange(pc,8);b->InvalidateRange(pc,8);}
            if(i%1987==0){a->Clear();b->Clear();}
            if(i%5003==0){bool eight=a->Ways()!=8;a->SetEightWay(eight);b->SetEightWay(eight);}
            if(i%499==0)SecondaryLinkCacheTestAccess::Equal(*a,*b);
            ++operations;
        }
        SecondaryLinkCacheTestAccess::Equal(*a,*b);
        Require(profiling?b->GetProfile().secondary_hits>0:b->GetProfile().secondary_probes==0);
        // Recycled same-PC tokens are current hits; stale/wrong-PC tokens make
        // no logical request or NRU mutation before the ordinary fallback.
        a->Clear();b->Clear();bool ha,hb;
        a->LookupOrAllocate(0x1000,ha);b->LookupOrAllocate(0x1000,hb);
        u16 slot=INVALID_BLOCK_SLOT;a->Lookup(0x1000);b->Lookup(0x1000,&slot);
        a->InvalidateRange(0x1000,4);b->InvalidateRange(0x1000,4);
        // Allocated-but-not-decoded range has end==start; force a full clear.
        a->Clear();b->Clear();a->LookupOrAllocate(0x1000,ha);b->LookupOrAllocate(0x1000,hb);
        a->Lookup(0x1000);auto* y=b->LookupSecondary(0x1000,slot);
        if(!y)y=b->Lookup(0x1000,&slot);
        Require(y);SecondaryLinkCacheTestAccess::Equal(*a,*b);
    }
    std::cout<<operations<<" cache operations: exact tags/NRU/victims/logical counts across 4/8 ways, evictions, recycled PCs, invalidation, clear, mode and poison passed\n";
}
