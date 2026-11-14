// SPDX-License-Identifier: GPL-2.0-or-later
// Integer-only classification: these tests do not validate command-buffer backing
// memory, inspect payload words, or execute/reorder any guest register write.
#include "video_core/pica_shader_packet_profile.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace Cara::PicaShaderPackets;
std::uint64_t checks = 0;

void Require(bool condition, const char* message, const Packet& packet) {
    ++checks;
    if (condition) return;
    std::cerr << "packet classifier: " << message << " register=" << packet.register_id
              << " grouped=" << packet.grouped << " extras=" << packet.extra_words
              << " post_header_offset=" << packet.post_header_offset
              << " declared=" << packet.declared_words
              << " consumed=" << packet.consumed_words_after_header
              << " mask=" << packet.mask << " mirror=" << packet.mirror_gs << '\n';
    std::exit(1);
}

// The reference uses wide sums instead of the classifier's bounded subtraction.
// int64 nonnegative consumed + uint32 extras cannot overflow uint64.
void Check(const Packet& packet, Kind expected_kind, std::uint32_t capacity) {
    const auto result = Classify(packet);
    const bool known = expected_kind != Kind::Other;
    const bool complete = known && packet.consumed_words_after_header >= 0 &&
        std::uint64_t(packet.consumed_words_after_header) + packet.extra_words <=
            packet.declared_words;
    const bool fits = known && std::uint64_t(packet.post_header_offset) +
                                   packet.extra_words <= capacity;
    const bool valid_mask = packet.mask < 16;
    const bool valid_extras = packet.extra_words < 2048;
    const bool eligible = known && !packet.grouped && packet.extra_words >= 2 &&
                          valid_extras && valid_mask && complete && fits;
    constexpr std::array<std::uint32_t, 9> last_in_bucket{0, 1, 3, 7, 15, 31, 63, 127, 511};
    std::size_t bucket = 0;
    while (bucket < last_in_bucket.size() && packet.extra_words > last_in_bucket[bucket])
        ++bucket;
    Require(result.kind == expected_kind, "kind", packet);
    Require(result.length_bucket == bucket, "length bucket", packet);
    Require(result.complete_declared_span == complete, "declared span", packet);
    Require(result.capacity_fits == fits, "capacity after scalar header", packet);
    Require(result.mask_valid == valid_mask, "mask", packet);
    Require(result.extra_length_valid == valid_extras, "11-bit extra length", packet);
    Require(result.eligible == eligible, "eligibility", packet);
}

void CheckPorts(std::uint32_t first, Kind kind, std::uint32_t capacity) {
    constexpr auto max32 = std::numeric_limits<std::uint32_t>::max();
    constexpr auto max64 = std::numeric_limits<std::uint64_t>::max();
    const std::array<std::uint32_t, 19> lengths{
        0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 511, 512, 2047, 2048};
    const std::array<std::uint32_t, 9> offsets{
        0, 1, capacity - 3, capacity - 2, capacity - 1, capacity, capacity + 1,
        max32 - 1, max32};
    for (std::uint32_t port = 0; port < 8; ++port)
    for (std::uint32_t mask = 0; mask < 16; ++mask)
    for (bool grouped : {false, true})
    for (auto extras : lengths)
    for (auto offset : offsets) {
        Packet p{first + port, grouped, extras, offset, 2 + std::uint64_t(extras), 2,
                 mask, false};
        Check(p, kind, capacity); // Exactly complete declared packet.
        p.mirror_gs = true;
        Check(p, kind, capacity); // Mirroring must not alter packet eligibility.
        p.declared_words = extras ? 1 + std::uint64_t(extras) : 1;
        Check(p, kind, capacity); // One missing word, including incomplete header.
        p.declared_words = max64;
        Check(p, kind, capacity);
    }

    // Independent, explicit answers at the POST-header limit: the scalar first
    // word has already consumed one entry. No additional +1 belongs in capacity.
    Packet edge{first, false, 2, capacity - 2, 4, 2, 0, false};
    Require(Classify(edge).eligible, "two words exactly fill remaining capacity", edge);
    edge.post_header_offset = capacity - 1;
    Require(!Classify(edge).capacity_fits, "second extra would exceed capacity", edge);
    edge.post_header_offset = capacity;
    edge.extra_words = 0;
    Require(Classify(edge).capacity_fits, "empty tail at exact capacity", edge);
    Require(!Classify(edge).eligible, "empty tail is not a batched candidate", edge);
    edge.extra_words = 1;
    Require(!Classify(edge).capacity_fits, "nonempty tail at exact capacity", edge);
    edge.post_header_offset = max32;
    edge.extra_words = max32;
    Require(!Classify(edge).capacity_fits, "capacity arithmetic must not wrap", edge);
    Check(edge, kind, capacity);

    for (auto consumed : {std::numeric_limits<std::int64_t>::min(), std::int64_t(-1),
                          std::int64_t(0), std::int64_t(1), std::int64_t(2),
                          std::numeric_limits<std::int64_t>::max()})
    for (auto declared : {std::uint64_t(0), std::uint64_t(1), std::uint64_t(2),
                          std::uint64_t(4), std::uint64_t(max32), max64})
    for (auto extras : {0u, 1u, 2u, 2047u, 2048u, max32})
    for (auto mask : {0u, 15u, 16u, max32}) {
        Check(Packet{first + 7, false, extras, 0, declared, consumed, mask, false}, kind,
              capacity);
    }
}
void CheckCounters() {
    Counters counters;
    Packet p{0x2cc, false, 3, 509, 5, 2, 0, true};
    counters.Observe(p);
    p.mask = 15; p.mirror_gs = false;
    counters.Observe(p);
    p.grouped = true;
    counters.Observe(p);
    counters.Observe(Packet{0x2d6, false, 1, 4095, 3, 2, 15, false});
    counters.Observe(Packet{0, false, 100, 0, 102, 2, 15, false});
    counters.Observe(Packet{0x2cc, false, 4, 508, 5, 2, 7, false});
    const auto& repeated = counters.groups[0][0];
    Require(counters.all_packets == 6, "all headers counted once", p);
    Require(repeated.total.packets == 3 && repeated.total.extra_words == 10,
            "logical extras exclude scalar headers and mirrored destination stores", p);
    Require(repeated.total.complete_packets == 2 && repeated.total.capacity_packets == 3,
            "complete and capacity counters remain distinct", p);
    Require(repeated.total.eligible_packets == 2 && repeated.total.eligible_extra_words == 6,
            "eligible words count each input extra once", p);
    Require(repeated.lengths[2].packets == 2 && repeated.lengths[2].extra_words == 6 &&
            repeated.lengths[3].packets == 1 && repeated.lengths[3].extra_words == 4,
            "length histograms are partitions, not additional logical packets", p);
    Require(repeated.masks[0] == 1 && repeated.masks[7] == 1 && repeated.masks[15] == 1,
            "mask histogram counts packets including zero masks", p);
    Require(repeated.mirror_packets[0] == 2 && repeated.mirror_packets[1] == 1,
            "mirror histogram counts packets, not stores", p);
    Require(counters.groups[0][1].total.packets == 1 &&
            counters.groups[0][1].total.eligible_packets == 0,
            "grouped packet excluded from repeated eligibility", p);
    Require(counters.groups[1][0].total.packets == 1 &&
            counters.groups[1][0].total.eligible_packets == 0,
            "one-extra packet remains short", p);
    std::uint64_t grouped_packets = 0, grouped_extras = 0;
    for (const auto& kinds : counters.groups)
    for (const auto& group : kinds) {
        grouped_packets += group.total.packets;
        grouped_extras += group.total.extra_words;
    }
    Require(grouped_packets == 5 && grouped_extras == 14,
            "unknown header is all_packets only", p);
    Counters mirrors;
    for (std::uint32_t port = 0; port < 8; ++port)
    for (std::uint32_t mask = 0; mask < 16; ++mask)
    for (bool mirrored : {false, true})
        mirrors.Observe(Packet{0x2cc + port, false, 3, 0, 5, 2, mask, mirrored});
    Require(mirrors.all_packets == 256 && mirrors.groups[0][0].total.extra_words == 768 &&
            mirrors.groups[0][0].total.eligible_extra_words == 768,
            "all masks and mirror modes preserve logical word units", p);
    mirrors.Observe(Packet{0x2cc, false, 2, 0, 4, 2, 16, false});
    Require(mirrors.groups[0][0].total.eligible_packets == 256,
            "invalid mask neither indexes histogram nor becomes eligible", p);
}
} // namespace

int main() {
    // These literal port ranges/capacities are independent fixtures for VS only.
    // GS program ports have different scalar limits and are intentionally Other.
    CheckCounters();
    CheckPorts(0x2cc, Kind::Program, 512);
    CheckPorts(0x2d6, Kind::Swizzle, 4096);
    for (auto id : {0u, 0x29cu, 0x2a6u, 0x2cbu, 0x2d4u, 0x2d5u, 0x2deu,
                    std::numeric_limits<std::uint32_t>::max()})
    for (auto extras : {0u, 1u, 2u, 2047u, 2048u,
                        std::numeric_limits<std::uint32_t>::max()}) {
        Check(Packet{id, false, extras, 0, std::numeric_limits<std::uint64_t>::max(),
                     2, 15, false}, Kind::Other, 0);
    }
    std::cout << "PICA shader packet integer classifier: " << checks << " checks passed\n";
}
