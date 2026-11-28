// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include "pica_metal_index_snapshot_lease.h"
namespace Cara {
struct MetalIndexImageKey {
    MetalIndexSnapshotLease lease;
    std::array<std::uint16_t, 2> header{};
    std::size_t image_bytes = 0;
    bool operator==(const MetalIndexImageKey&) const = default;
};
// The capture owner exclusively owns the immutable Prepared->Upload phase.
// It must not modify this image until the draw has copied/uploaded its inputs.
// No capture-owner reference is retained by the GPU cache or deferred packet.
class MetalIndexUploadProof {
public:
    static std::optional<MetalIndexUploadProof> Bind(
        const MetalVerifiedIndexRead& verified,
        std::shared_ptr<const std::vector<std::uint16_t>> owner) {
        if (!owner) return std::nullopt;
        auto lease = verified.Identity();
        const auto image = std::as_bytes(std::span(*owner));
        constexpr std::size_t prefix = 2 * sizeof(std::uint16_t);
        if (!lease || image.size() < prefix || lease->Bytes() > image.size()-prefix ||
            image.size()%512 || image.size()-prefix-lease->Bytes() >= 512)
            return std::nullopt;
        if (!verified.MatchesPayload(image.data()+prefix,lease->Bytes()))
            return std::nullopt;
        // The existing capture initializes every padding byte, not just bytes
        // sampled by the shader. Include all of them in the equality proof.
        if (!std::all_of(image.begin()+prefix+lease->Bytes(),image.end(),
                        [](auto b) { return b == std::byte{}; }))
            return std::nullopt;
        MetalIndexImageKey key{std::move(lease),{},image.size()};
        std::memcpy(key.header.data(),image.data(),prefix);
        return MetalIndexUploadProof(std::move(key),std::move(owner),image.data());
    }
    std::optional<MetalIndexImageKey> KeyFor(std::span<const std::byte> image) const {
        std::array<std::uint16_t,2> header{};
        if (!owner || !key.lease || image.data()!=data || image.size()!=key.image_bytes ||
            std::as_bytes(std::span(*owner)).data()!=data ||
            std::as_bytes(std::span(*owner)).size()!=key.image_bytes)
            return std::nullopt;
        std::memcpy(header.data(),image.data(),sizeof(header));
        if (header!=key.header) return std::nullopt;
        return key;
    }
private:
    MetalIndexUploadProof(MetalIndexImageKey key_,
                          std::shared_ptr<const std::vector<std::uint16_t>> owner_,
                          const std::byte* data_)
        : key(std::move(key_)),owner(std::move(owner_)),data(data_) {}
    MetalIndexImageKey key;
    std::shared_ptr<const std::vector<std::uint16_t>> owner;
    const std::byte* data;
};
} // namespace Cara
