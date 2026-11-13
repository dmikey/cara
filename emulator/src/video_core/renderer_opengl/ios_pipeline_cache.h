// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version. See license.txt.
#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <utility>

namespace OpenGL::IOSPipelineCache {
// Untrusted disk data is decoded transactionally, before any GL calls or large allocations.
constexpr std::size_t MaxBytes = 32 * 1024 * 1024;
constexpr std::size_t MaxEntries = 128;
constexpr std::size_t MaxSource = 256 * 1024;
constexpr std::size_t MaxBinary = 1024 * 1024;
struct Entry {
    std::string vertex, fragment;
    std::uint32_t format = 0;
    std::vector<std::uint8_t> binary;
};
inline std::uint64_t Digest(const void* bytes, std::size_t size) {
    auto p = static_cast<const std::uint8_t*>(bytes);
    std::uint64_t h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}
inline std::uint64_t KeyFromDigests(std::uint64_t vs, std::uint64_t fs) {
    const std::uint64_t hashes[]{vs, fs};
    return Digest(hashes, sizeof(hashes));
}
inline std::uint64_t Key(const std::string& vs, const std::string& fs) {
    return KeyFromDigests(Digest(vs.data(), vs.size()), Digest(fs.data(), fs.size()));
}
// Runtime-only, bounded memo. Stage object addresses are stable for their manager's
// lifetime. Slot collisions replace entries; both identities must match for a hit.
// GLSL is never examined on a hit, nor on a miss when stage digests are available.
template <typename Value, std::size_t Capacity = 256>
class StagePairMemo {
    static_assert(Capacity && !(Capacity & (Capacity - 1)), "capacity must be a power of two");
    struct Slot { const void* vertex = nullptr; const void* fragment = nullptr; Value value{}; };
    std::array<Slot, Capacity> slots{};
public:
    template <typename Factory>
    const Value& Get(const void* vertex, const void* fragment, Factory&& factory) {
        const auto v = reinterpret_cast<std::uintptr_t>(vertex);
        const auto f = reinterpret_cast<std::uintptr_t>(fragment);
        auto& slot = slots[((v >> 4) ^ (f >> 4) ^ (f >> 13)) & (Capacity - 1)];
        if (slot.vertex != vertex || slot.fragment != fragment) {
            slot.value = factory();
            slot.vertex = vertex;
            slot.fragment = fragment;
        }
        return slot.value;
    }
};
inline void Put(std::vector<std::uint8_t>& out, std::uint64_t n, unsigned width) {
    for (unsigned i = 0; i < width; ++i) out.push_back(static_cast<std::uint8_t>(n >> (i * 8)));
}
inline void Blob(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
    Put(out, size, 4);
    if (size) {
        auto p = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), p, p + size);
    }
}
inline std::vector<std::uint8_t> Encode(const std::string& identity,
                                      const std::vector<Entry>& entries) {
    if (identity.size() > 4096 || entries.size() > MaxEntries) return {};
    std::vector<std::uint8_t> out;
    Put(out, 0x4341524150534F01ULL, 8); // CARA pipeline store, schema 1
    Blob(out, identity.data(), identity.size());
    Put(out, entries.size(), 4);
    for (const auto& e : entries) {
        if (e.vertex.empty() || e.fragment.empty() || e.vertex.size() > MaxSource ||
            e.fragment.size() > MaxSource || e.binary.size() > MaxBinary) return {};
        Blob(out, e.vertex.data(), e.vertex.size());
        Blob(out, e.fragment.data(), e.fragment.size());
        Put(out, e.format, 4);
        Blob(out, e.binary.data(), e.binary.size());
        if (out.size() > MaxBytes - 8) return {};
    }
    Put(out, Digest(out.data(), out.size()), 8);
    return out;
}
inline bool Decode(const std::vector<std::uint8_t>& in, const std::string& identity,
                   std::vector<Entry>& entries) {
    if (in.size() < 28 || in.size() > MaxBytes) return false;
    std::size_t pos = 0;
    bool good = true;
    const auto number = [&](unsigned width) {
        std::uint64_t result = 0;
        if (pos > in.size() || width > in.size() - pos) { good = false; return result; }
        for (unsigned i = 0; i < width; ++i) result |= std::uint64_t(in[pos++]) << (i * 8);
        return result;
    };
    const auto blob = [&](std::size_t limit, auto& out) {
        auto size = number(4);
        if (!good || size > limit || pos > in.size() || size > in.size() - pos) {
            good = false; return;
        }
        out.assign(in.begin() + pos, in.begin() + pos + size);
        pos += size;
    };
    if (number(8) != 0x4341524150534F01ULL) return false;
    std::string found;
    blob(4096, found);
    if (!good || found != identity) return false;
    const auto count = number(4);
    if (!good || count > MaxEntries) return false;
    std::vector<Entry> decoded;
    for (std::size_t i = 0; i < count && good; ++i) {
        Entry e;
        blob(MaxSource, e.vertex); blob(MaxSource, e.fragment);
        e.format = static_cast<std::uint32_t>(number(4));
        blob(MaxBinary, e.binary);
        if (e.vertex.empty() || e.fragment.empty() || e.vertex.find('\0') != std::string::npos ||
            e.fragment.find('\0') != std::string::npos) good = false;
        decoded.push_back(std::move(e));
    }
    if (!good || pos != in.size() - 8) return false;
    const auto expected = Digest(in.data(), pos);
    if (number(8) != expected) return false;
    entries = std::move(decoded);
    return true;
}
} // namespace OpenGL::IOSPipelineCache
