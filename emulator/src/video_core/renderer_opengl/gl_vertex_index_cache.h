// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <list>
#include <unordered_map>
#include <vector>
#include <utility>

namespace OpenGL {

// Only input topology is cached. Vertex attributes and shader outputs remain live.
class IOSVertexIndexCache {
public:
    struct Key {
        std::uint32_t address{}, count{}, offset{}, topology{}, format{};
        bool operator==(const Key& other) const {
            return address == other.address && count == other.count && offset == other.offset &&
                   topology == other.topology && format == other.format;
        }
    };
    struct Data {
        std::vector<std::uint8_t> source;
        std::vector<std::uint32_t> triangles;
        std::vector<std::uint16_t> packed;
        std::uint32_t first{}, last{}, unique{};
        bool packable{};
        std::size_t Bytes() const {
            return source.capacity() + triangles.capacity() * sizeof(std::uint32_t) +
                   packed.capacity() * sizeof(std::uint16_t);
        }
    };

    explicit IOSVertexIndexCache(std::size_t budget = 32 * 1024 * 1024,
                                 std::size_t maximum_entries = 2048)
        : budget_bytes(budget), entry_limit(maximum_entries) {}

    IOSVertexIndexCache(const IOSVertexIndexCache&) = delete;
    IOSVertexIndexCache& operator=(const IOSVertexIndexCache&) = delete;

    // The result remains valid until the next cache mutation. The renderer uses
    // it for one draw on its owning thread, then performs the next lookup.
    const Data* Find(const Key& key, const std::uint8_t* source, std::size_t size) {
        const auto found = entries.find(key);
        if (found == entries.end()) return nullptr;
        auto& entry = found->second;
        if (entry.data.source.size() != size ||
            (size && (!source || std::memcmp(entry.data.source.data(), source, size)))) {
            Erase(found);
            return nullptr;
        }
        recent.splice(recent.begin(), recent, entry.position);
        return &entry.data;
    }

    const Data* Store(const Key& key, const std::uint8_t* source, std::size_t size,
                      std::vector<std::uint32_t>& triangles) {
        if (triangles.empty() || (size && !source) || !entry_limit) return nullptr;
        Data data;
        if (size) data.source.assign(source, source + size);
        data.triangles = std::move(triangles);
        const auto bounds = std::minmax_element(data.triangles.begin(), data.triangles.end());
        data.first = *bounds.first;
        data.last = *bounds.second;
        data.packable = std::uint64_t(data.last) - data.first < 65536;
        if (data.packable) {
            std::bitset<65536> seen;
            // Matches the R16UI texture's 128-texel row alignment.
            data.packed.resize((data.triangles.size() + 127) / 128 * 128, 0);
            for (std::size_t i = 0; i < data.triangles.size(); ++i) {
                const auto index = std::uint16_t(data.triangles[i] - data.first);
                data.packed[i] = index;
                seen.set(index);
            }
            data.unique = static_cast<std::uint32_t>(seen.count());
        }
        const auto bytes = data.Bytes();
        if (bytes > budget_bytes) {
            triangles = std::move(data.triangles);
            return nullptr;
        }
        const auto previous = entries.find(key);
        if (previous != entries.end()) Erase(previous);
        while (!recent.empty() &&
               (used_bytes + bytes > budget_bytes || entries.size() >= entry_limit))
            Erase(entries.find(recent.back()));
        recent.push_front(key);
        auto inserted = entries.emplace(key, Entry{std::move(data), recent.begin()}).first;
        used_bytes += bytes;
        return &inserted->second.data;
    }

    std::size_t Bytes() const { return used_bytes; }
    std::size_t Size() const { return entries.size(); }

private:
    struct Hash {
        std::size_t operator()(const Key& key) const {
            std::size_t hash = key.address;
            for (auto value : {key.count, key.offset, key.topology, key.format})
                hash ^= std::size_t(value) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    struct Entry {
        Data data;
        std::list<Key>::iterator position;
    };
    using Entries = std::unordered_map<Key, Entry, Hash>;
    void Erase(Entries::iterator entry) {
        used_bytes -= entry->second.data.Bytes();
        recent.erase(entry->second.position);
        entries.erase(entry);
    }
    const std::size_t budget_bytes, entry_limit;
    std::size_t used_bytes{};
    std::list<Key> recent;
    Entries entries;
};

} // namespace OpenGL
