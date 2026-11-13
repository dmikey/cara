// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone: c++ -std=c++17 -O3 -Iemulator/src tests/fastinterp_simd_tags_test.cpp
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "core/arm/fastinterp/fastinterp_cache.h"
using namespace Core::FastInterp;
static void Check(bool ok) { if (!ok) throw std::runtime_error("SIMD/scalar mismatch"); }
static u32 Index(u32 pc, unsigned ways) {
    u32 h = pc * 0x9e3779b9u; h ^= h >> 16;
    return (h >> 4) & ((CACHE_SIZE / ways) - 1);
}
static std::vector<u32> PCs(unsigned ways, unsigned sets = 64) {
    std::vector<u32> pcs; std::vector<unsigned> counts(sets);
    for (u32 pc = 0x1000; pcs.size() < sets * ways; pc += 4) {
        auto set = Index(pc, ways);
        if (set < sets && counts[set] < ways) { pcs.push_back(pc); ++counts[set]; }
    }
    return pcs;
}
static void Differential(unsigned ways, bool profiling, bool recency) {
    auto a = std::make_unique<BlockCache>(), b = std::make_unique<BlockCache>();
    a->SetEightWay(ways == 8); b->SetEightWay(ways == 8);
    a->SetChainRecency(recency); b->SetChainRecency(recency);
    a->SetSimdTags(false); b->SetSimdTags(true);
    a->SetProfiling(profiling); b->SetProfiling(profiling);
    auto pcs = PCs(ways, 4); std::mt19937 rng(7231);
    std::unordered_map<BasicBlock*, BasicBlock*> slots;
    for (unsigned i = 0; i < 100000; ++i) {
        auto pc = pcs[rng() % pcs.size()];
        if (i % 7 == 0) pc = rng() & ~3u;
        if (i % 997 == 0) pc = INVALID_TAG;
        BasicBlock *x, *y;
        if (i % 3) {
            bool ha = false, hb = false;
            x = a->LookupOrAllocate(pc, ha); y = b->LookupOrAllocate(pc, hb); Check(ha == hb);
        } else { x = a->Lookup(pc); y = b->Lookup(pc); }
        Check(bool(x) == bool(y));
        if (x) {
            auto entry = slots.emplace(x, y); Check(entry.first->second == y);
            Check(pc == INVALID_TAG || (x->start_pc == y->start_pc && x->end_pc == y->end_pc &&
                  x->inst_count == y->inst_count && x->flags == y->flags && x->exec_count == y->exec_count));
            // Same PC may be undecoded or change ARM/Thumb mode; callers own
            // these checks. The tag probe must return the same entry either way.
            x->flags = y->flags = rng() & 1;
            x->inst_count = y->inst_count = i % 5 ? 2 : 0;
            x->end_pc = y->end_pc = pc + 8; x->exec_count = y->exec_count = rng();
        }
        if (i % 211 == 0) { a->InvalidateRange(pc, 8); b->InvalidateRange(pc, 8); }
        if (i % 1987 == 0) { a->Clear(); b->Clear(); }
        if (i % 311 == 0) b->SetSimdTags(!b->SimdTagsEnabled());
        if (i % 5003 == 0) { bool eight = a->Ways() != 8; a->SetEightWay(eight); b->SetEightWay(eight); }
        if (i % 499 == 0) {
            std::ostringstream sa, sb; a->WriteReplacementProfile(sa); b->WriteReplacementProfile(sb);
            Check(sa.str() == sb.str());
            const auto& pa = a->GetProfile(); const auto& pb = b->GetProfile();
            Check(pa.lookup_calls == pb.lookup_calls && pa.lookup_hits == pb.lookup_hits &&
                  pa.lookup_tag_comparisons == pb.lookup_tag_comparisons &&
                  pa.lookup_or_allocate_calls == pb.lookup_or_allocate_calls &&
                  pa.lookup_or_allocate_hits == pb.lookup_or_allocate_hits &&
                  pa.lookup_or_allocate_tag_comparisons == pb.lookup_or_allocate_tag_comparisons);
        }
    }
}
static volatile u64 sink;
static double Bench(unsigned ways, bool simd, unsigned pattern, bool allocate) {
    auto cache = std::make_unique<BlockCache>(); cache->SetEightWay(ways == 8); cache->SetSimdTags(simd);
    auto pcs = PCs(ways); bool hit;
    for (auto pc : pcs) cache->LookupOrAllocate(pc, hit)->inst_count = 1;
    std::vector<u32> trace(1 << 16); std::mt19937 rng(813);
    for (auto& pc : trace) {
        pc = pcs[rng() % pcs.size()];
        if (pattern == 1 && rng() % 100 < 2) pc = rng() & ~3u;
        if (pattern == 2) pc = pcs[0];
        if (pattern == 3) pc = rng() & ~3u;
    }
    auto start = std::chrono::steady_clock::now(); u64 sum = 0;
    for (unsigned repeat = 0; repeat < 128; ++repeat) for (auto pc : trace) {
        auto* block = allocate ? cache->LookupOrAllocate(pc, hit) : cache->Lookup(pc);
        sum += block ? block->start_pc : 1;
    }
    sink = sum;
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now()-start).count() / (trace.size()*128);
}
int main(int argc, char** argv) {
    for (auto ways : {4u, 8u}) for (bool profiling : {false, true}) for (bool recency : {false, true})
        Differential(ways, profiling, recency);
    std::cout << "Differential 800000 operations passed.\n";
    if (argc == 2 && std::strcmp(argv[1], "--test-only") == 0) return 0;
    for (auto ways : {4u, 8u}) for (unsigned pattern = 0; pattern < 4; ++pattern) for (bool allocate : {false,true}) {
        std::vector<double> scalar, simd;
        for (unsigned run = 0; run < 5; ++run) {
            if (run % 2) { simd.push_back(Bench(ways, true, pattern, allocate)); scalar.push_back(Bench(ways, false, pattern, allocate)); }
            else { scalar.push_back(Bench(ways, false, pattern, allocate)); simd.push_back(Bench(ways, true, pattern, allocate)); }
        }
        std::sort(scalar.begin(), scalar.end()); std::sort(simd.begin(), simd.end());
        std::cout << ways << " way pattern " << pattern << (allocate ? " allocate" : " lookup")
                  << " scalar_ns=" << scalar[2] << " simd_ns=" << simd[2] << " ratio=" << simd[2]/scalar[2] << '\n';
    }
}
