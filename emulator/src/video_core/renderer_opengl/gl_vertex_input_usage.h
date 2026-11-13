// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <tuple>
namespace OpenGL {
// Hashes are the shader setup's existing cached identities, refreshed by its
// program/swizzle dirty hooks. Uniform values intentionally do not prune branches.
struct IOSVertexInputReadKey {
    std::uint64_t program_hash{}, swizzle_hash{};
    std::uint32_t main_offset{}, num_outputs{};
    std::uint8_t sanitize_mul{};
    std::array<std::uint32_t, 16> output_map{};
    auto Fields() const {
        return std::tie(program_hash, swizzle_hash, main_offset, num_outputs, sanitize_mul, output_map);
    }
    bool operator<(const IOSVertexInputReadKey& rhs) const { return Fields() < rhs.Fields(); }
};
class IOSVertexInputUsage {
public:
    template<class Analyze> std::uint16_t Get(const IOSVertexInputReadKey& key, Analyze analyze) {
        if (const auto found = masks.find(key); found != masks.end()) return found->second;
        const auto mask = analyze().value_or(std::uint16_t{0xffff});
        if (masks.size() == 1024) masks.clear();
        masks.emplace(key, mask);
        return mask;
    }
private:
    std::map<IOSVertexInputReadKey, std::uint16_t> masks;
};
}
