// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <ostream>
#include <utility>
#include <vector>

namespace OpenGL::IndexPayloadReuse {
struct Identity {
    std::uint64_t owner{}, serial{};
    bool Valid() const { return owner && serial; }
    bool operator==(const Identity&) const = default;
};
inline bool Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("CARA_VERTEX_INDEX_IDENTITY_REUSE");
        return v && std::strcmp(v, "1") == 0;
    }();
    return enabled;
}
// A saturated source permanently returns zero; it never wraps to an old owner.
inline std::uint64_t NextOwner(std::atomic<std::uint64_t>& source) {
    auto current = source.load(std::memory_order_relaxed);
    while (current) {
        const auto next = current == std::numeric_limits<std::uint64_t>::max() ? 0 : current + 1;
        if (source.compare_exchange_weak(current, next, std::memory_order_relaxed)) return current;
    }
    return 0;
}
inline std::atomic<std::uint64_t> owner_source{1};
class Issuer {
public:
    // Optional seeds/source permit bounded overflow/reset fixtures, not RAM epochs.
    explicit Issuer(bool enabled = false, std::uint64_t first_serial = 1,
                    std::atomic<std::uint64_t>& owners = owner_source)
        : enabled(enabled), owners(owners), serial(first_serial) {
        if (enabled) owner = NextOwner(owners);
    }
    Issuer(const Issuer&) = delete;
    Issuer& operator=(const Issuer&) = delete;
    Identity Issue() {
        if (!enabled || !owner || !serial) return {};
        const Identity id{owner, serial};
        serial = serial == std::numeric_limits<std::uint64_t>::max() ? 0 : serial + 1;
        return id;
    }
    void Reset() {
        if (!enabled) return;
        owner = NextOwner(owners); serial = 1;
    }
private:
    const bool enabled;
    std::atomic<std::uint64_t>& owners;
    std::uint64_t owner{}, serial{};
};

// The token cannot survive a byte replacement or resize by accident. Published
// data is const; only an exact equality/upload commit may attach an identity.
class Shadow {
public:
    Shadow() = default;
    Shadow(const Shadow&) = delete;
    Shadow& operator=(const Shadow&) = delete;
    Shadow(Shadow&& other) noexcept
        : bytes(std::move(other.bytes)), identity(std::exchange(other.identity, {})) {}
    Shadow& operator=(Shadow&& other) noexcept {
        if (this != &other) {
            identity = {}; bytes = std::move(other.bytes);
            identity = std::exchange(other.identity, {});
        }
        return *this;
    }
    const std::uint8_t* data() const { return bytes.data(); }
    std::size_t size() const { return bytes.size(); }
    bool empty() const { return bytes.empty(); }
    template<class It> void assign(It first, It last) {
        identity = {}; // Also fail closed if vector::assign throws.
        bytes.assign(first, last);
    }
    void clear() { identity = {}; bytes.clear(); }
    Identity GetIdentity() const { return identity; }
    void InvalidateIdentity() { identity = {}; }
    void CommitIdentity(Identity id) { identity = bytes.empty() ? Identity{} : id; }
private:
    std::vector<std::uint8_t> bytes;
    Identity identity;
};

struct Stats {
    std::uint64_t token_hits{}, fallbacks{}, ineligible_fallbacks{};
    std::uint64_t full_index_bytes_avoided{}, resets{};
};
inline Stats stats; // Renderer producer only; never accessed by GL worker jobs.
inline void ResetStats() { stats = {}; }
inline void RecordReset() { ++stats.resets; }
inline bool TryIdentity(const Shadow& shadow, Identity current, std::size_t size) {
    const auto stored = shadow.GetIdentity();
    if (size && shadow.size() == size && current.Valid() && stored.Valid() && stored == current) {
        ++stats.token_hits; stats.full_index_bytes_avoided += size;
        return true;
    }
    ++stats.fallbacks;
    if (!size || shadow.size() != size || !current.Valid() || !stored.Valid())
        ++stats.ineligible_fallbacks;
    return false;
}
inline void WriteJSON(std::ostream& out) {
    out << "{\"enabled\":" << (Enabled() ? "true" : "false")
        << ",\"token_hits\":" << stats.token_hits << ",\"fallbacks\":" << stats.fallbacks
        << ",\"ineligible_fallbacks\":" << stats.ineligible_fallbacks
        << ",\"full_index_bytes_avoided\":" << stats.full_index_bytes_avoided
        << ",\"resets\":" << stats.resets << '}';
}
} // namespace OpenGL::IndexPayloadReuse
