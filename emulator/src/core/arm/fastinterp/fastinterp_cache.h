// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <array>
#include <cstring>
#include "core/arm/fastinterp/fastinterp_types.h"

namespace Core::FastInterp {

// ============================================================================
// TLB-Style Block Cache
// ============================================================================
// Fixed-size set-associative block cache with no per-entry allocation.
// Four ways are the baseline; eight ways retain the same total block storage.

/// Maximum number of sets: 4096 at four ways, 2048 at eight ways.
constexpr size_t CACHE_SETS = 4096;

/// Baseline number of ways per set (associativity)
constexpr size_t CACHE_WAYS = 4;

/// Total number of cache entries
constexpr size_t CACHE_SIZE = CACHE_SETS * CACHE_WAYS;

/// Invalid tag marker (a guest PC of 0xFFFFFFFF can never be a real block start)
constexpr u32 INVALID_TAG = 0xFFFFFFFF;

/// Set-associative block cache with recently-used-bit replacement
class BlockCache {
public:
    BlockCache() {
        Clear();
    }

    struct Profile {
        u64 allocation_misses{};
        u64 empty_allocations{};
        u64 live_replacements{};
        u64 clears{};
        u64 invalidation_calls{};
        u64 invalidated_entries{};
    };

    void SetEightWay(bool enabled) {
        if (eight_way_ != enabled) {
            eight_way_ = enabled;
            Clear(); // Changing the set mapping always invalidates old entries.
        }
    }
    unsigned Ways() const { return eight_way_ ? 8u : 4u; }
    unsigned Sets() const { return CACHE_SIZE / Ways(); }

    void SetProfiling(bool enabled) { profiling_ = enabled; }
    bool ProfilingEnabled() const { return profiling_; }
    const Profile& GetProfile() const { return profile_; }

    /// Clear all cache entries
    void Clear() {
        if (profiling_) ++profile_.clears;
        tags_.fill(INVALID_TAG);
        for (auto& block : blocks_) {
            block.inst_count = 0;
            block.chain_target = nullptr; // Prevent dangling pointers
        }
        std::memset(lru_state_.data(), 0, lru_state_.size());
    }

    /// Invalidate cache entries whose decoded range intersects [start_addr, start_addr+length)
    void InvalidateRange(u32 start_addr, u32 length) {
        if (profiling_) ++profile_.invalidation_calls;
        // Set indices are hashed, so an address range does not map to a set
        // range: walk every entry and match on the block's own address range.
        // inst_count is zeroed so surviving pointers to the entry (chain
        // targets) fizzle instead of executing the stale decode.
        const u64 end_addr = static_cast<u64>(start_addr) + length;
        for (size_t i = 0; i < CACHE_SIZE; ++i) {
            if (tags_[i] == INVALID_TAG) {
                continue;
            }
            if (blocks_[i].start_pc < end_addr && blocks_[i].end_pc > start_addr) {
                if (profiling_) ++profile_.invalidated_entries;
                tags_[i] = INVALID_TAG;
                blocks_[i].inst_count = 0;
                blocks_[i].chain_target = nullptr;
            }
        }
    }

    BasicBlock* Lookup(u32 pc) {
        return eight_way_ ? LookupImpl<8>(pc) : LookupImpl<4>(pc);
    }
    BasicBlock* LookupOrAllocate(u32 pc, bool& was_hit) {
        return eight_way_ ? LookupOrAllocateImpl<8>(pc, was_hit)
                          : LookupOrAllocateImpl<4>(pc, was_hit);
    }

    template <typename Visitor>
    void VisitResidentBlocks(Visitor visit) const {
        for (size_t i = 0; i < CACHE_SIZE; ++i)
            if (tags_[i] != INVALID_TAG && blocks_[i].inst_count) visit(blocks_[i]);
    }

private:
    /// Look up a block by PC (returns nullptr if not found)
    template <unsigned Ways>
    BasicBlock* LookupImpl(u32 pc) {
        u32 set_idx = GetSetIndex<Ways>(pc);
        u32 base = set_idx * Ways;

        for (u32 way = 0; way < Ways; ++way) {
            if (tags_[base + way] == pc) {
                UpdateLRU<Ways>(set_idx, way);
                return &blocks_[base + way];
            }
        }
        return nullptr;
    }

    /// Look up a block, allocating an LRU victim on miss; sets was_hit
    template <unsigned Ways>
    BasicBlock* LookupOrAllocateImpl(u32 pc, bool& was_hit) {
        u32 set_idx = GetSetIndex<Ways>(pc);
        u32 base = set_idx * Ways;

        for (u32 way = 0; way < Ways; ++way) {
            if (tags_[base + way] == pc) {
                UpdateLRU<Ways>(set_idx, way);
                was_hit = true;
                return &blocks_[base + way];
            }
        }

        was_hit = false;
        u32 victim_way = GetLRUVictim<Ways>(set_idx);
        BasicBlock& block = blocks_[base + victim_way];
        // Diagnostics touch only an existing allocation slow path, never hits
        // or direct block chaining. Clear/invalidation retain lifetime totals.
        if (profiling_) {
            ++profile_.allocation_misses;
            if (tags_[base + victim_way] == INVALID_TAG) ++profile_.empty_allocations;
            else ++profile_.live_replacements;
        }

        tags_[base + victim_way] = pc;
        block.start_pc = pc;
        block.end_pc = pc;
        block.inst_count = 0;
        block.exec_count = 0;
        block.flags = 0;
        block.chain_target = nullptr;

        UpdateLRU<Ways>(set_idx, victim_way);
        return &block;
    }

    /// Hash a PC to a set index. Multiply-xor-shift spreads the aligned,
    /// clustered PCs of real code across sets better than bit extraction.
    template <unsigned Ways>
    static u32 GetSetIndex(u32 pc) {
        constexpr u32 HASH_MULT = 0x9E3779B9; // 2^32 / golden ratio
        u32 hash = pc * HASH_MULT;
        hash ^= (hash >> 16);
        return (hash >> 4) & ((CACHE_SIZE / Ways) - 1);
    }

    /// Update LRU state when way is accessed
    template <unsigned Ways>
    void UpdateLRU(u32 set_idx, u32 accessed_way) {
        // Pseudo-LRU with four or eight recently-used bits per active set
        // Bit i is set if way i was recently used
        u8& lru = lru_state_[set_idx];
        lru |= (1 << accessed_way);
        if (lru == ((1u << Ways) - 1)) {
            // All ways recently used, clear except accessed
            lru = (1 << accessed_way);
        }
    }

    /// Get LRU victim for eviction
    template <unsigned Ways>
    u32 GetLRUVictim(u32 set_idx) {
        u8 lru = lru_state_[set_idx];
        // Find first way that wasn't recently used
        for (u32 way = 0; way < Ways; ++way) {
            if (!(lru & (1 << way))) {
                return way;
            }
        }
        // All recently used, pick way 0
        return 0;
    }

    // Tags hold the full PC, so a match needs no start_pc re-check. Split from
    // the blocks (structure-of-arrays): a 4-way set probe touches one cache
    // line of tags instead of four/eight far-apart block headers.
    bool eight_way_{false};
    bool profiling_{false};
    Profile profile_{};
    std::array<u32, CACHE_SIZE> tags_;
    std::array<BasicBlock, CACHE_SIZE> blocks_;
    std::array<u8, CACHE_SETS> lru_state_;
};

} // namespace Core::FastInterp
