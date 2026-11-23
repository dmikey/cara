// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>

namespace Cara {

// Host-only scan results. This never owns guest authority or authorizes reuse
// of an uploaded payload: callers must read all current guest index bytes first.
class ExactMetalIndexScanMemo {
public:
    struct Key {
        std::uint64_t address;
        std::uint32_t count;
        unsigned width;
        bool operator==(const Key&) const = default;
    };
    struct Result {
        std::uint32_t first, last;
        std::size_t unique;
        bool operator==(const Result&) const = default;
    };
    struct Hash {
        static std::uint64_t Mix(std::uint64_t value) noexcept {
            value += 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31);
        }
        std::size_t operator()(const Key& key) const noexcept {
            return static_cast<std::size_t>(Mix(key.address) ^ Mix(key.count) ^ Mix(key.width));
        }
    };
    static constexpr std::size_t SmallBytes = 32 * 1024;
    static constexpr std::size_t LargeBytes = 128 * 1024;
    static constexpr std::size_t SmallEntries = 128, LargeEntries = 8;

    struct Stats {
        std::size_t entries{}, bytes{}, hits{}, misses{}, evictions{};
    };

    static bool Admits(const Key& key, std::span<const std::uint8_t> input) noexcept {
        const auto size = std::uint64_t{key.count} * key.width;
        return (key.width == 1 || key.width == 2) && key.count && key.count <= 65536 &&
            size == input.size() && size <= LargeBytes &&
            key.address <= std::numeric_limits<std::uint32_t>::max() - size;
    }

    std::optional<Result> Lookup(const Key& key, std::span<const std::uint8_t> input) {
        if (!Admits(key,input)) return std::nullopt;
        return input.size() <= SmallBytes ? small.Lookup(key,input) : large.Lookup(key,input);
    }
    void Store(const Key& key, std::span<const std::uint8_t> input, Result result) {
        if (!Admits(key,input)) return;
        if (input.size() <= SmallBytes) small.Store(key,input,result);
        else large.Store(key,input,result);
    }
    Stats SmallStats() const noexcept { return small.stats; }
    Stats LargeStats() const noexcept { return large.stats; }

private:
    template<std::size_t Capacity> struct Tier {
        struct Entry {
            std::unique_ptr<std::uint8_t[]> input;
            std::size_t size;
            Result result;
            std::list<Key>::iterator recent;
        };
        std::unordered_map<Key,Entry,Hash> entries;
        std::list<Key> recent;
        Stats stats;
        Tier() { entries.reserve(Capacity); }
        std::optional<Result> Lookup(const Key& key, std::span<const std::uint8_t> input) {
            const auto found = entries.find(key);
            if (found == entries.end() || found->second.size != input.size() ||
                std::memcmp(found->second.input.get(),input.data(),input.size()) != 0) {
                ++stats.misses;
                return std::nullopt;
            }
            ++stats.hits;
            recent.splice(recent.begin(),recent,found->second.recent);
            return found->second.result; // Never expose an entry across a caller's scan/read.
        }
        void Erase(typename decltype(entries)::iterator position) {
            stats.bytes -= position->second.size;
            recent.erase(position->second.recent);
            entries.erase(position);
            stats.entries = entries.size();
        }
        void Store(const Key& key, std::span<const std::uint8_t> input, Result result) {
            if (const auto old = entries.find(key); old != entries.end()) {
                // A full key fixes the byte length. Changed current payloads
                // can replace that snapshot without allocating another image.
                std::memcpy(old->second.input.get(),input.data(),input.size());
                old->second.result = result;
                recent.splice(recent.begin(),recent,old->second.recent);
                return;
            } else if (entries.size() == Capacity) {
                Erase(entries.find(recent.back()));
                ++stats.evictions;
            }
            // Evict before allocating: owned snapshots never exceed either
            // tier's byte bound, even during replacement. Failure loses a
            // cache entry only; it can never manufacture a successful lookup.
            auto copy = std::unique_ptr<std::uint8_t[]>(new std::uint8_t[input.size()]);
            std::memcpy(copy.get(),input.data(),input.size());
            recent.push_front(key);
            try {
                entries.emplace(key,Entry{std::move(copy),input.size(),result,recent.begin()});
            } catch (...) {
                recent.pop_front();
                throw;
            }
            stats.entries = entries.size();
            stats.bytes += input.size();
        }
    };
    Tier<SmallEntries> small;
    Tier<LargeEntries> large;
};

} // namespace Cara
