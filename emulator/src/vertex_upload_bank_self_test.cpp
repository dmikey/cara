// Cara upload bank bounds/accounting oracle; SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_vertex_upload_bank.h"
#include "video_core/renderer_opengl/gl_vertex_upload_ranges.h"
#include <stdexcept>
#define REQUIRE(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
#include <array>
#include <cassert>
#include <iostream>
#include <limits>
using namespace OpenGL::IOSVertexUploadBank;
struct Entry { unsigned tag=0; };
static void Add(Bank<Entry>& bank,std::size_t index) {
    REQUIRE(index<bank.sizes.size() && index<bank.used.size());
    bank.sizes[index].emplace_back();
    bank.bytes+=AllocationBytes(index,128); ++bank.textures;
}
static void CheckPackedRanges() {
    // Independent byte-coverage oracle: merging must preserve exactly the
    // touched bytes and yield one range for each connected coverage component.
    for (unsigned seed=0; seed<256; ++seed) {
        OpenGL::BoundedVertexUploadRanges<16> ranges;
        std::array<bool,256> expected{}, actual{};
        for (unsigned i=0; i<seed%17; ++i) {
            const unsigned begin=(seed*29+i*37)%240;
            const unsigned end=begin+(seed*13+i*11)%16+1;
            REQUIRE(ranges.Add(begin,end));
            for (unsigned byte=begin; byte<end; ++byte) expected[byte]=true;
        }
        ranges.SortAndMerge();
        std::size_t components=0;
        for (std::size_t byte=0; byte<expected.size(); ++byte)
            if (expected[byte] && (!byte || !expected[byte-1])) ++components;
        REQUIRE(ranges.Size()==components);
        std::uint64_t previous_end=0;
        bool first=true;
        for (const auto& range:ranges) {
            REQUIRE(range.begin<range.end && range.end<=actual.size());
            REQUIRE(first || range.begin>previous_end);
            REQUIRE(range.destination==0);
            for (auto byte=range.begin; byte<range.end; ++byte) {
                REQUIRE(!actual[byte]); actual[byte]=true;
            }
            previous_end=range.end; first=false;
        }
        REQUIRE(actual==expected);
    }
    OpenGL::BoundedVertexUploadRanges<16> full;
    for (unsigned i=0; i<16; ++i) REQUIRE(full.Add(i*2,i*2+1));
    REQUIRE(!full.Add(40,41) && full.Size()==16);
    full.SortAndMerge(); REQUIRE(full.Size()==16);
    OpenGL::BoundedVertexUploadRanges<16> edges;
    REQUIRE(!edges.Add(5,5) && !edges.Add(6,5) && edges.Size()==0);
    const auto high=std::numeric_limits<std::uint64_t>::max();
    REQUIRE(edges.Add(high-6,high-3) && edges.Add(high-3,high));
    edges.SortAndMerge();
    REQUIRE(edges.Size()==1 && edges.begin()->begin==high-6 && edges.begin()->end==high);
    std::cout<<"Packed ranges: 256 byte-coverage cases, capacity fallback and address edges passed.\n";
}
int main() {
    CheckPackedRanges();
    static_assert(FormatCount==5 && HeightClasses==14 && ClassCount==70);
    static_assert(ClassIndex(4,13)==69 && ClassIndex(4,0)==56);
    static_assert(AllocationBytes(69,128)==8192*128*22);
    std::array<bool,ClassCount> visited{};
    Bank<Entry> all;
    std::size_t reference_bytes=0;
    for(std::size_t order=0;order<ClassCount;++order) {
        const auto index=ReclaimIndex(order);
        REQUIRE(index<ClassCount && !visited[index]); visited[index]=true;
    }
    for(std::size_t format=0;format<FormatCount;++format) {
        for(std::size_t height=0;height<HeightClasses;++height) {
            const auto index=ClassIndex(format,height);
            Add(all,index); all.used[index]=1;
            const auto independent=(std::size_t(1)<<height)*128*(format==0?16:format==4?22:18);
            REQUIRE(AllocationBytes(index,128)==independent); reference_bytes+=independent;
        }
    }
    REQUIRE(all.sizes[69].size()==1 && all.used[69]==1 && all.bytes==reference_bytes);
    Reclaim(all,0,0,1,128); // Every entry in flight: none may be reclaimed.
    REQUIRE(all.bytes==reference_bytes && all.textures==70 && all.sizes[69].size()==1);
    all.used.fill(0); // New frame makes every slot unused.
    Reclaim(all,0,0,1,128);
    REQUIRE(all.bytes==0 && all.textures==0);
    for(std::size_t i=0;i<ClassCount;++i) REQUIRE(all.sizes[i].empty());
    Bank<Entry> pair;
    Add(pair,56); Add(pair,69); pair.used[56]=1;
    const auto before=pair.bytes;
    const auto required=AllocationBytes(68,128);
    Reclaim(pair,required,before,100,128); // Reclaims 69, retains active 56.
    REQUIRE(pair.sizes[69].empty() && pair.sizes[56].size()==1 && pair.used[56]==1);
    REQUIRE(pair.bytes==AllocationBytes(56,128) && pair.textures==1);
    Add(pair,68); pair.used[68]=1;
    const auto active=pair.bytes;
    Reclaim(pair,required,active,1,128); // Entry cap cannot evict active slots.
    REQUIRE(pair.bytes==active && pair.textures==2);
    pair.used.fill(0); Reclaim(pair,AllocationBytes(69,128),AllocationBytes(69,128),1,128);
    REQUIRE(pair.bytes==0 && pair.textures==0); Add(pair,69); pair.used[69]++;
    REQUIRE(pair.used[69]==1 && pair.bytes==AllocationBytes(69,128));
    std::cout<<"Upload bank: all 70 classes, 56/69 allocation, active/unused reclamation, byte/entry budgets passed.\n";
}
