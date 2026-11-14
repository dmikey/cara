// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#include "../emulator/src/video_core/renderer_opengl/gl_vertex_upload_order_reuse.h"
#include <array>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <utility>
using namespace OpenGL::IOSVertexUploadOrderReuse;
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
struct Entry {
    unsigned id=0, rows=4, format=2;
    bool live=true;
    std::vector<std::uint8_t> input_shadow, index_shadow;
    bool operator==(const Entry& b) const {
        return id==b.id && rows==b.rows && format==b.format && live==b.live &&
            input_shadow==b.input_shadow && index_shadow==b.index_shadow;
    }
};
static auto eligible=[](const Entry& e) { return e.rows==4 && e.format==2 && e.live; };
static Result Search(const std::vector<Entry>& p, std::size_t first,
                     const Entry& wanted, std::size_t limit=32) {
    return Find(p,first,limit,wanted.input_shadow.data(),wanted.input_shadow.size(),
                wanted.index_shadow.data(),wanted.index_shadow.size(),eligible);
}
static Entry Make(unsigned id) {
    Entry e; e.id=id; e.input_shadow.resize(80,0); e.index_shadow.resize(40,0);
    e.input_shadow[32]=static_cast<std::uint8_t>(id);
    e.input_shadow[33]=static_cast<std::uint8_t>(id>>8);
    e.index_shadow[20]=static_cast<std::uint8_t>(id*17);
    return e;
}
// Previous helper semantics, independently expressed as an exhaustive bounded
// full-byte reference. Comparison-call counts are diagnostic implementation
// details; selection, candidate count and independent hit flags must agree.
static Result Reference(const std::vector<Entry>& pool, std::size_t first,
                        const Entry& wanted, std::size_t limit) {
    Result result; result.selected=first;
    if (first>=pool.size()) return result;
    const auto same=[](const auto& a,const auto& b) { return !a.empty() && a==b; };
    const auto stop=first+std::min(limit,pool.size()-first-1);
    for (auto i=first;i<=stop;++i) {
        if (!eligible(pool[i])) continue;
        ++result.candidates;
        const bool a=same(pool[i].input_shadow,wanted.input_shadow);
        const bool b=(i==first || a) && same(pool[i].index_shadow,wanted.index_shadow);
        if (i==first) {result.input_equal=a;result.index_equal=b;}
        if (a && b) {result.selected=i;result.input_equal=true;result.index_equal=true;break;}
    }
    return result;
}
int main() {
    std::size_t cases=0;
    std::mt19937 rng(0x4c415241);
    for (unsigned trial=0; trial<512; ++trial) {
        std::vector<Entry> pool;
        for (unsigned i=0;i<24;++i) pool.push_back(Make(i));
        std::shuffle(pool.begin(),pool.end(),rng);
        for (unsigned first=0;first<pool.size();++first) {
            const Entry target=Make(first);
            const auto before=pool;
            const auto result=Search(pool,first,target);
            REQUIRE(pool==before); // actual search never mutates GPU/CPU ownership
            std::size_t expected=first;
            bool found=false;
            for (std::size_t j=first;j<pool.size();++j)
                if (pool[j].input_shadow==target.input_shadow &&
                    pool[j].index_shadow==target.index_shadow) {expected=j;found=true;break;}
            REQUIRE(result.selected==expected);
            REQUIRE(result.input_equal && result.index_equal && found);
            std::swap(pool[first],pool[result.selected]);
            REQUIRE(pool[first].id==target.id);
            // All previously consumed entries retain their exact identity/data.
            for (unsigned j=0;j<first;++j) REQUIRE(pool[j]==before[j]);
            ++cases;
        }
    }
    const auto wanted=Make(200);
    std::vector<Entry> pool{Make(1),Make(2),wanted};
    auto result=Search(pool,0,wanted);
    REQUIRE(result.selected==2); // identical prefix/suffix but unequal middle rejected
    REQUIRE(result.full_comparisons>=4);
    ++cases;
    pool[2].index_shadow[20]^=1;
    auto before=pool;
    result=Search(pool,0,wanted);
    REQUIRE(result.selected==0 && !result.input_equal && pool==before);
    ++cases;
    pool[2]=wanted; pool[2].rows=8;
    REQUIRE(Search(pool,0,wanted).selected==0); ++cases;
    pool[2]=wanted; pool[2].format=1;
    REQUIRE(Search(pool,0,wanted).selected==0); ++cases;
    pool[2]=wanted; pool[2].live=false;
    REQUIRE(Search(pool,0,wanted).selected==0); ++cases;
    pool[2]=wanted; pool[2].input_shadow.pop_back();
    REQUIRE(Search(pool,0,wanted).selected==0); ++cases;
    pool[2]=wanted; pool[2].index_shadow.pop_back();
    REQUIRE(Search(pool,0,wanted).selected==0); ++cases;
    pool={wanted,Make(1),Make(2)};
    REQUIRE(Search(pool,1,wanted).selected==1); ++cases; // used prefix excluded
    pool={Make(1),wanted};
    REQUIRE(Search(pool,0,wanted,0).selected==0); ++cases;
    REQUIRE(Search(pool,pool.size(),wanted).selected==pool.size()); ++cases;
    // Normal miss preserves independent input/index reuse, avoiding extra writes.
    pool={wanted}; pool[0].input_shadow[30]^=1;
    result=Search(pool,0,wanted);
    REQUIRE(!result.input_equal && result.index_equal); ++cases;
    pool={wanted}; pool[0].index_shadow[20]^=1;
    result=Search(pool,0,wanted);
    REQUIRE(result.input_equal && !result.index_equal); ++cases;
    pool.assign(40,Make(1)); pool[33]=wanted;
    REQUIRE(Search(pool,0,wanted).selected==0); ++cases;
    pool[32]=wanted;
    REQUIRE(Search(pool,0,wanted).selected==32); ++cases;
    // Tiny and zero payloads: endpoints stay in bounds; empty never reuses.
    for (unsigned length=0;length<33;++length) {
        Entry tiny=wanted; tiny.input_shadow.resize(length);tiny.index_shadow.resize(length);
        pool={tiny};result=Search(pool,0,tiny);
        REQUIRE(result.input_equal==(length!=0));
        REQUIRE(result.index_equal==(length!=0)); ++cases;
    }
    for (unsigned trial=0;trial<20000;++trial) {
        Entry target=Make(rng()%8);
        target.input_shadow.resize(rng()%257);target.index_shadow.resize(rng()%129);
        pool.clear();
        for (unsigned i=0,n=rng()%48;i<n;++i) {
            auto entry=target;
            const auto change=[&](auto& bytes) {
                if (rng()%4==0) bytes.resize(rng()%257);
                if (!bytes.empty() && rng()%2) bytes[rng()%bytes.size()]^=1;
            };
            change(entry.input_shadow);change(entry.index_shadow);
            if (rng()%17==0) entry.live=false;
            if (rng()%19==0) entry.format=1;
            if (rng()%23==0) entry.rows=8;
            pool.push_back(std::move(entry));
        }
        const auto first=rng()%50, limit=rng()%40;
        const auto before=pool;
        const auto actual=Search(pool,first,target,limit), expected=Reference(pool,first,target,limit);
        REQUIRE(actual.selected==expected.selected && actual.candidates==expected.candidates &&
                actual.input_equal==expected.input_equal && actual.index_equal==expected.index_equal);
        REQUIRE(pool==before);++cases;
    }
    std::cout << "vertex order reuse: " << cases << " cases passed\n";
}
