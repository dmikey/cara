// SPDX-License-Identifier: GPL-2.0-or-later
// Integer-only diagnostics for repeated VS shader upload packet candidates.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace Cara::PicaShaderPackets {
constexpr std::uint32_t VSProgramFirst = 0x2cc;
constexpr std::uint32_t VSSwizzleFirst = 0x2d6;
constexpr std::uint32_t PortCount = 8;
constexpr std::size_t LengthBucketCount = 10;
enum class Kind { Other, Program, Swizzle };
struct Packet {
    std::uint32_t register_id;
    bool grouped;
    std::uint32_t extra_words;
    std::uint32_t post_header_offset;
    std::uint64_t declared_words;
    std::int64_t consumed_words_after_header;
    std::uint32_t mask;
    bool mirror_gs;
};
struct Classification {
    Kind kind = Kind::Other;
    std::size_t length_bucket = 0;
    bool complete_declared_span = false;
    bool capacity_fits = false;
    bool mask_valid = false;
    bool extra_length_valid = false;
    bool eligible = false;
};
constexpr Kind PortKind(std::uint32_t id) {
    if (id >= VSProgramFirst && id - VSProgramFirst < PortCount)
        return Kind::Program;
    if (id >= VSSwizzleFirst && id - VSSwizzleFirst < PortCount)
        return Kind::Swizzle;
    return Kind::Other;
}
constexpr std::size_t LengthBucket(std::uint32_t count) {
    if (count < 2) return count;
    if (count < 4) return 2;
    if (count < 8) return 3;
    if (count < 16) return 4;
    if (count < 32) return 5;
    if (count < 64) return 6;
    if (count < 128) return 7;
    if (count < 512) return 8;
    return 9;
}
constexpr Classification Classify(const Packet& packet) {
    Classification result;
    result.kind = PortKind(packet.register_id);
    result.length_bucket = LengthBucket(packet.extra_words);
    result.mask_valid = packet.mask <= 15;
    result.extra_length_valid = packet.extra_words <= 2047;
    if (result.kind == Kind::Other)
        return result;
    if (packet.consumed_words_after_header >= 0) {
        const auto consumed = static_cast<std::uint64_t>(packet.consumed_words_after_header);
        result.complete_declared_span = consumed <= packet.declared_words &&
            packet.extra_words <= packet.declared_words - consumed;
    }
    const std::uint32_t capacity = result.kind == Kind::Program ? 512 : 4096;
    result.capacity_fits = packet.post_header_offset <= capacity &&
        packet.extra_words <= capacity - packet.post_header_offset;
    result.eligible = !packet.grouped && packet.extra_words >= 2 && result.mask_valid &&
        result.extra_length_valid && result.complete_declared_span && result.capacity_fits;
    return result;
}
struct Totals {
    std::uint64_t packets = 0, extra_words = 0;
    std::uint64_t complete_packets = 0, capacity_packets = 0;
    std::uint64_t eligible_packets = 0, eligible_extra_words = 0;
    constexpr void Observe(const Packet& packet, const Classification& classification) {
        ++packets;
        extra_words += packet.extra_words;
        complete_packets += classification.complete_declared_span;
        capacity_packets += classification.capacity_fits;
        eligible_packets += classification.eligible;
        if (classification.eligible) eligible_extra_words += packet.extra_words;
    }
};
struct Group {
    Totals total;
    std::array<Totals, LengthBucketCount> lengths{};
    std::array<std::uint64_t, 16> masks{};
    std::array<std::uint64_t, 2> mirror_packets{};
};
struct Counters {
    std::uint64_t all_packets = 0;
    // [program/swizzle][repeated/grouped]. Words count incoming extras once;
    // mirrored destination stores are deliberately not added to those counts.
    std::array<std::array<Group, 2>, 2> groups{};
    constexpr void Observe(const Packet& packet) {
        ++all_packets;
        const auto classification = Classify(packet);
        if (classification.kind == Kind::Other) return;
        auto& group = groups[classification.kind == Kind::Program ? 0 : 1][packet.grouped ? 1 : 0];
        group.total.Observe(packet, classification);
        group.lengths[classification.length_bucket].Observe(packet, classification);
        if (classification.mask_valid) ++group.masks[packet.mask];
        ++group.mirror_packets[packet.mirror_gs ? 1 : 0];
    }
};
} // namespace Cara::PicaShaderPackets
