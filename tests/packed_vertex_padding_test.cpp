// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_packed_vertex_padding.h"
#include "video_core/renderer_opengl/gl_vertex_upload_ranges.h"
#include "video_core/renderer_opengl/gl_vertex_upload_order_reuse.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>
using Byte=std::uint8_t;
using Ranges=OpenGL::BoundedVertexUploadRanges<16>;
static std::size_t cases=0,checks=0,selector_cases=0;
static std::uint64_t legacy_zero_bytes=0,padding_zero_bytes=0,source_copy_bytes=0;
#define CHECK(x) do{++checks;if(!(x))throw std::runtime_error(#x);}while(false)
struct Span {std::size_t begin,end;};
struct Entry {std::vector<Byte> input_shadow,index_shadow;bool eligible=true;};
static void SameFind(const std::vector<Byte>& reference,const std::vector<Byte>& actual,
                     std::mt19937& random){
    std::vector<Byte> indices(32);for(auto& b:indices)b=Byte(random());
    std::vector<Entry> pool(6);
    for(auto& e:pool){e.input_shadow=reference;e.index_shadow=indices;}
    pool[0].index_shadow[random()%indices.size()]^=1;
    pool[1].input_shadow.pop_back();
    pool[2].index_shadow[indices.size()/2]^=1;
    pool[4].input_shadow[reference.size()/2]^=1; // equal endpoints, unequal middle
    pool[5].eligible=false;
    const auto eligible=[&](const Entry& e){return e.eligible;};
    for(std::size_t first=0;first<pool.size();++first){
        const auto old=OpenGL::IOSVertexUploadOrderReuse::Find(pool,first,32,
            reference.data(),reference.size(),indices.data(),indices.size(),eligible);
        const auto now=OpenGL::IOSVertexUploadOrderReuse::Find(pool,first,32,
            actual.data(),actual.size(),indices.data(),indices.size(),eligible);
        CHECK(old.selected==now.selected&&old.candidates==now.candidates);
        CHECK(old.full_comparisons==now.full_comparisons);
        CHECK(old.input_equal==now.input_equal&&old.index_equal==now.index_equal);
        ++selector_cases;
    }
}
// Independent oracle: define a fresh all-zero output and copy every span. It
// never calls ZeroPackedVertexPadding or infers bytes from the reused buffer.
static std::vector<Byte> Oracle(const std::array<Byte,256>& constants,
                               const std::vector<Byte>& guest,const Ranges& ranges,
                               std::size_t total){
    std::vector<Byte> result(total,0);
    std::copy(constants.begin(),constants.end(),result.begin());
    for(const auto& s:ranges)
        std::copy(guest.begin()+s.begin,guest.begin()+s.end,result.begin()+s.destination);
    return result;
}
static void Exercise(const std::vector<Span>& raw,std::vector<Byte>& guest,
                     std::array<Byte,256>& constants,std::vector<Byte>& reused,
                     std::mt19937& random,bool selector=true){
    ++cases;Ranges ranges;
    for(const auto& s:raw)CHECK(ranges.Add(s.begin,s.end));
    ranges.SortAndMerge();
    // Independent per-source-byte coverage verifies overlap/adjacent merging.
    std::vector<bool> expected_coverage(guest.size()),merged_coverage(guest.size());
    for(const auto& s:raw)for(std::size_t j=s.begin;j<s.end;++j)expected_coverage[j]=true;
    for(const auto& s:ranges)for(std::size_t j=s.begin;j<s.end;++j)merged_coverage[j]=true;
    CHECK(expected_coverage==merged_coverage);
    std::size_t bytes=constants.size(),copied=0;
    for(auto& s:ranges){bytes=(bytes+3)&~std::size_t(3);s.destination=bytes;bytes+=s.end-s.begin;copied+=s.end-s.begin;}
    const std::size_t total=(bytes+4095)/4096*4096;
    CHECK(total>=bytes&&total<=8*1024*1024);
    // Exercise capacity reuse, growth and shrinkage. Poison even newly resized
    // bytes so neither path can rely on allocator/resize zero initialization.
    reused.resize(total);std::fill(reused.begin(),reused.end(),Byte(0xa5));
    CHECK(ranges.ClearPadding(reused.data(),total,constants.size()));
    std::vector<bool> overwritten(total);
    std::fill_n(overwritten.begin(),constants.size(),true);
    for(const auto& s:ranges)std::fill_n(overwritten.begin()+s.destination,s.end-s.begin,true);
    std::size_t zeros=0;
    for(std::size_t j=0;j<total;++j){CHECK(reused[j]==(overwritten[j]?0xa5:0));zeros+=!overwritten[j];}
    CHECK(zeros==total-constants.size()-copied);
    // Mutate live input and defaults AFTER clearing padding, before the same
    // fresh per-range copies. A cached/stale snapshot would fail this witness.
    for(const auto& s:ranges)guest[s.begin+(s.end-s.begin)/2]^=Byte(1+(cases%255));
    constants[cases%constants.size()]^=Byte(1+(cases%255));
    std::memcpy(reused.data(),constants.data(),constants.size());
    for(const auto& s:ranges)std::memcpy(reused.data()+s.destination,guest.data()+s.begin,s.end-s.begin);
    const auto expected=Oracle(constants,guest,ranges,total);
    CHECK(reused==expected);
    if(selector)SameFind(expected,reused,random);
    legacy_zero_bytes+=total;padding_zero_bytes+=zeros;source_copy_bytes+=copied;
}
static void InvalidPlans() {
    for(unsigned bad=0;bad<10;++bad) for(bool enabled:{false,true}) {
        Ranges ranges;CHECK(ranges.Add(0,8));CHECK(ranges.Add(16,20));
        ranges.SortAndMerge();ranges.begin()[0].destination=32;ranges.begin()[1].destination=64;
        std::size_t prefix=16;
        switch(bad) {
        case 0:prefix=257;break;
        case 1:ranges.begin()[0].destination=8;break;
        case 2:ranges.begin()[0].destination=257;break;
        case 3:ranges.begin()[0].begin=9;break;
        case 4:ranges.begin()[0].end=300;break;
        case 5:ranges.begin()[1].destination=36;break; // partial clear, then invalid order
        case 6:ranges.begin()[1].destination=257;break;
        case 7:ranges.begin()[1].end=15;break;
        case 8:ranges.begin()[1].destination=std::numeric_limits<std::size_t>::max();break;
        case 9:ranges.begin()[1].end=std::numeric_limits<std::uint64_t>::max();break;
        }
        std::array<Byte,320> guarded;guarded.fill(0xd3);Byte* output=guarded.data()+32;
        // The exact production fallback. Failure may have cleared earlier gaps;
        // full zeroing must erase that partial state without touching canaries.
        const bool success=enabled&&ranges.ClearPadding(output,256,prefix);
        CHECK(!success);
        if(!success)std::memset(output,0,256);
        for(unsigned i=0;i<guarded.size();++i)CHECK(guarded[i]==(i>=32&&i<288?0:0xd3));
        ++cases;
    }
}
int main(){try{
    InvalidPlans();
    std::mt19937 random(0x50414430);std::vector<Byte> guest(16384),reused;
    std::array<Byte,256> constants{};
    for(auto& b:guest)b=Byte(random());
    // All source-byte alignments and range lengths; overlapping and adjacent
    // spans, deterministic zero/default constants, and zero-range draws.
    for(unsigned alignment=0;alignment<4;++alignment)
        for(unsigned length=1;length<=17;++length){
            constants.fill(0);
            Exercise({{alignment,alignment+length},{64,65},{80,82},{96,99},{112,116}},guest,constants,reused,random);
            Exercise({{alignment,alignment+length},{alignment,alignment+length+1},{alignment+length+1,alignment+length+3}},guest,constants,reused,random);
            Exercise({},guest,constants,reused,random);
        }
    for(unsigned iteration=0;iteration<4096;++iteration){
        std::vector<Span> spans;
        const unsigned count=iteration%17;
        for(unsigned i=0;i<count;++i){
            const std::size_t start=random()%8192,length=1+random()%1024;
            spans.push_back({start,start+length});
            if(i&&iteration%3==0)spans.back()={spans[0].begin+(i%4),spans[0].end+(i%4)};
        }
        std::shuffle(spans.begin(),spans.end(),random);
        for(auto& b:constants)b=iteration%4==0?0:Byte(random());
        Exercise(spans,guest,constants,reused,random);
    }
    // A real-plan near8MiB payload, then immediate shrink, then regrowth. Keep
    // large byte-oracle cases separate from bounded selector pool allocations.
    guest.resize(8*1024*1024);for(auto& b:guest)b=Byte(random());
    Exercise({{0,guest.size()-256}},guest,constants,reused,random,false);
    Exercise({},guest,constants,reused,random,false);
    Exercise({{3,guest.size()-256}},guest,constants,reused,random,false);
    std::cout<<"PASS cases="<<cases<<" checks="<<checks<<" selector_cases="<<selector_cases
        <<" planned_legacy_zero_bytes="<<legacy_zero_bytes<<" planned_padding_zero_bytes="<<padding_zero_bytes
        <<" unchanged_source_copy_bytes="<<source_copy_bytes<<"\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}
