// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "core/arm/fastinterp/fastinterp_cache.h"
using namespace Core::FastInterp;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static std::vector<u32> Collisions(unsigned ways, unsigned count, unsigned capacity) {
    const auto index = [ways,capacity](u32 pc) {
        u32 hash = pc * 0x9e3779b9u; hash ^= hash >> 16;
        return (hash >> 4) & ((capacity / ways) - 1);
    };
    std::vector<u32> result;
    for (u32 pc = 0x1000; result.size() < count; pc += 4)
        if (index(pc) == index(0x1000)) result.push_back(pc);
    return result;
}
static BasicBlock* Allocate(BlockCache& cache, u32 pc, bool* was_hit = nullptr) {
    bool hit = false;
    auto* block = cache.LookupOrAllocate(pc, hit);
    if (!hit) { block->inst_count = 1; block->end_pc = pc + 4; }
    if (was_hit) *was_hit = hit;
    return block;
}
struct Result { unsigned hot_misses = 0, next_hot_misses = 0; u64 misses = 0; };
static Result Stream(unsigned capacity, unsigned ways, bool lazy, bool profiling) {
    constexpr unsigned rounds = 1000, second_rounds = 500;
    const auto pcs = Collisions(ways, ways + rounds + second_rounds + 2, capacity);
    auto cache = std::make_unique<BlockCache>(capacity);
    cache->SetEightWay(ways == 8); cache->SetChainRecency(lazy); cache->SetProfiling(profiling);
    std::array<BasicBlock*, 2> hot{};
    for (unsigned i = 0; i < ways; ++i) {
        auto* block = Allocate(*cache, pcs[i]); ++block->exec_count;
        if (i < hot.size()) hot[i] = block;
    }
    Result result;
    for (unsigned round = 0; round < rounds; ++round) {
        for (unsigned i = 0; i < hot.size(); ++i) {
            // Real direct chaining checks the target's PC and uses the pointer;
            // it increments exec_count without touching cache lookup recency.
            if (hot[i]->start_pc != pcs[i] || !hot[i]->inst_count) {
                bool hit = false; hot[i] = Allocate(*cache, pcs[i], &hit);
                result.hot_misses += !hit;
            }
            hot[i]->exec_count += 100;
        }
        ++Allocate(*cache, pcs[ways + round])->exec_count;
    }
    const std::array<u32, 2> next_pcs{pcs[ways + rounds], pcs[ways + rounds + 1]};
    for (unsigned i = 0; i < hot.size(); ++i) hot[i] = Allocate(*cache, next_pcs[i]);
    for (unsigned round = 0; round < second_rounds; ++round) {
        for (unsigned i = 0; i < hot.size(); ++i) {
            if (hot[i]->start_pc != next_pcs[i] || !hot[i]->inst_count) {
                bool hit = false; hot[i] = Allocate(*cache, next_pcs[i], &hit);
                result.next_hot_misses += !hit;
            }
            hot[i]->exec_count += 100;
        }
        ++Allocate(*cache, pcs[ways + rounds + 2 + round])->exec_count;
    }
    if (lazy) {
        Require(result.hot_misses == 0 && result.next_hot_misses == 0, "chained hot working set was evicted");
        Require(!cache->Lookup(pcs[0]) && !cache->Lookup(pcs[1]), "stopped hot phase remained pinned");
    }
    result.misses = cache->GetProfile().allocation_misses;
    std::ostringstream out; cache->WriteReplacementProfile(out);
    if (profiling) {
        Require(cache->GetProfile().live_replacements > 128, "fixture must exercise sample wrap");
        const auto text = out.str();
        std::size_t count = 0, pos = 0;
        while ((pos = text.find("\"victim_pc\"", pos)) != std::string::npos) { ++count; ++pos; }
        Require(count == 128, "replacement samples must remain fixed-capacity");
    } else Require(result.misses == 0, "disabled diagnostics changed profile totals");
    return result;
}
static void Coherence(unsigned capacity, unsigned ways) {
    auto cache = std::make_unique<BlockCache>(capacity); cache->SetEightWay(ways == 8);
    cache->SetChainRecency(true); cache->SetProfiling(true);
    const auto pcs = Collisions(ways, ways + 5, capacity);
    std::vector<BasicBlock*> blocks;
    for (unsigned i = 0; i < ways; ++i) {
        auto* block = Allocate(*cache, pcs[i]); block->exec_count = UINT32_MAX; blocks.push_back(block);
    }
    ++Allocate(*cache, pcs[ways])->exec_count;
    blocks[1]->exec_count = 3; // Counter wrap/reset must not become huge unsigned frequency.
    ++Allocate(*cache, pcs[ways + 1])->exec_count;
    blocks[ways - 1]->SetThumb(true); // Re-decode with retained count is likewise bounded activity.
    ++Allocate(*cache, pcs[ways + 2])->exec_count;
    Require(cache->GetProfile().counter_discontinuities >= 2, "counter/mode discontinuities not observed");
    auto* invalid = blocks[ways - 1];
    cache->InvalidateRange(invalid->start_pc, 4);
    Require(!invalid->inst_count && !invalid->chain_target, "invalidation did not poison decoded storage");
    auto* replacement = Allocate(*cache, pcs[ways + 3]);
    Require(replacement == invalid && replacement->exec_count == 0, "invalid slot was not preferred/reset");
    cache->Clear();
    unsigned residents = 0; cache->VisitResidentBlocks([&](const auto&) { ++residents; });
    Require(residents == 0, "clear retained a live block");
    auto* fresh = Allocate(*cache, pcs[0]);
    Require(fresh->exec_count == 0 && !fresh->IsThumb(), "clear/allocation retained stale activity or mode");
}
int main() {
    for (unsigned capacity : {DEFAULT_CACHE_SIZE,MAX_CACHE_SIZE}) for (unsigned ways : {4u, 8u}) {
        const auto baseline = Stream(capacity, ways, false, true);
        const auto candidate = Stream(capacity, ways, true, true);
        const auto quiet = Stream(capacity, ways, true, false);
        Require(baseline.hot_misses > candidate.hot_misses, "fixture did not expose chain recency gap");
        Require(quiet.hot_misses == candidate.hot_misses && quiet.next_hot_misses == candidate.next_hot_misses,
                "diagnostic toggle changed victim behavior");
        Coherence(capacity, ways);
        std::cout << "capacity=" << capacity << " ways=" << ways << " legacy_hot_redecodes=" << baseline.hot_misses
                  << " lazy_hot_redecodes=" << candidate.hot_misses
                  << " lazy_phase2_hot_redecodes=" << candidate.next_hot_misses << '\n';
    }
    std::cout << "Chain-recency cache: hot chain/cold stream, phase turnover, bounded samples, wrap/reset, invalidation and clear passed\n";
}
