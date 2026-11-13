// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted portions copyright Citra Emulator Project / Azahar Emulator Project.
// Approximate geometry/command constants adapted from Citra/Azahar DelayGenerator,
// GPLv2-or-later, commit 5ddbaeae237954d1f9c1843e6cf75d8b160c2601 (PR #2095).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>

namespace Cara {
inline bool ExperimentalP3DCompletionEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_EXPERIMENTAL_P3D_COMPLETION");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
inline bool ExperimentalPPFDependencyEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_EXPERIMENTAL_PPF_DEPENDENCY");
        return value && std::strcmp(value, "1") == 0;
    }();
    return ExperimentalP3DCompletionEnabled() && enabled;
}
// Pinned PR2095 measured TEX rates, not DMA rates. Integer rounding is upward.
inline std::uint64_t PPFTransferNanoseconds(std::uint64_t bytes, bool input_vram, bool output_vram) {
    const std::uint64_t mibps = input_vram ? 5400 : (output_vram ? 3100 : 450);
    const auto rate = mibps * 1024 * 1024;
    if (bytes > (UINT64_MAX - rate + 1) / 1000000000)
        throw std::overflow_error("Experimental PPF byte-duration overflow");
    return (bytes * 1000000000 + rate - 1) / rate + 300000;
}
inline std::uint64_t CheckedP3DAdd(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        throw std::overflow_error("Experimental P3D counter overflow");
    return a + b;
}
// A heuristic only: 22.4 ns/command and 400 ns/triangle, assuming 35% culled
// at 20% cost => 288 ns/triangle. Integer fifth-ns avoids floating drift.
struct P3DWorkEstimate {
    std::uint64_t fifth_ns = 0;
    void Command() { fifth_ns = CheckedP3DAdd(fifth_ns, 112); }
    void Draw(std::uint32_t original_vertices, unsigned topology) {
        const std::uint64_t triangles = (topology == 1 || topology == 2)
            ? (original_vertices >= 3 ? original_vertices - 2 : 1) : original_vertices / 3;
        fifth_ns = CheckedP3DAdd(fifth_ns, triangles * 1440);
    }
    std::uint64_t TakeNanoseconds() {
        // Pinned upstream's additional GSP service-cost estimate, not measured
        // universal hardware latency. DMA/PPF/fill behavior is not changed here.
        const auto result = CheckedP3DAdd(fifth_ns / 5, 300000);
        fifth_ns = 0;
        return result;
    }
    template <class Archive> void serialize(Archive& ar, unsigned) { ar & fifth_ns; }
};
inline std::int64_t P3DNanosecondsToTicks(std::uint64_t ns) {
    constexpr std::uint64_t hz = 268111856, billion = 1000000000;
    const auto seconds = ns / billion, remainder = ns % billion;
    if (seconds > std::uint64_t(std::numeric_limits<std::int64_t>::max()) / hz)
        throw std::overflow_error("Experimental P3D duration overflow");
    const auto fraction = (remainder * hz + billion - 1) / billion;
    const auto ticks = CheckedP3DAdd(seconds * hz, fraction);
    if (ticks > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("Experimental P3D duration overflow");
    return static_cast<std::int64_t>(std::max<std::uint64_t>(ticks, 1));
}
struct PendingP3D {
    std::uint64_t token = 0, generation = 0;
    std::int64_t deadline = 0;
    std::uint32_t recipient = 0;
    bool active = false;
    bool transfer = false;
    template <class Archive> void serialize(Archive& ar, unsigned) {
        ar & token & generation & deadline & recipient & active & transfer;
    }
};
struct P3DCompletionQueue {
    static constexpr std::size_t Capacity = 256;
    std::array<PendingP3D, Capacity> records{};
    std::array<std::uint64_t, 4> generations{};
    std::uint64_t next_token = 1;
    std::int64_t render_deadline = 0, transfer_deadline = 0;
    void InvalidateRecipient(unsigned recipient) {
        if (recipient >= generations.size()) throw std::invalid_argument("Invalid P3D recipient");
        generations[recipient] = CheckedP3DAdd(generations[recipient], 1);
    }
    PendingP3D Enqueue(std::int64_t now, std::int64_t duration, unsigned recipient, bool transfer = false) {
        if (now < 0 || duration <= 0 || recipient >= generations.size())
            throw std::invalid_argument("Invalid P3D completion request");
        auto slot = std::find_if(records.begin(), records.end(), [](const auto& r) { return !r.active; });
        if (slot == records.end()) throw std::runtime_error("Experimental P3D pending records exhausted");
        const auto begin = transfer ? std::max({now, render_deadline, transfer_deadline})
                                    : std::max(now, render_deadline);
        if (duration > std::numeric_limits<std::int64_t>::max() - begin ||
            next_token == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("Experimental P3D timeline/token overflow");
        *slot = {next_token++, generations[recipient], begin + duration, recipient, true, transfer};
        (transfer ? transfer_deadline : render_deadline) = slot->deadline;
        return *slot;
    }
    std::optional<PendingP3D> Peek(std::uint64_t token) const {
        for (const auto& r : records)
            if (r.active && r.token == token && r.recipient < generations.size() &&
                r.generation == generations[r.recipient]) return r;
        return std::nullopt;
    }
    std::optional<PendingP3D> Consume(std::uint64_t token, std::int64_t now) {
        auto slot = std::find_if(records.begin(), records.end(), [=](const auto& r) { return r.active && r.token == token; });
        if (slot == records.end()) return std::nullopt;
        if (now < slot->deadline) throw std::runtime_error("Experimental P3D completion fired early");
        // Do not allow a corrupted/reordered event to overtake earlier render work.
        for (const auto& r : records)
            if (r.active && r.token < token && (slot->transfer || !r.transfer)) throw std::runtime_error("Experimental P3D event order mismatch");
        const auto record = *slot;
        slot->active = false;
        if (record.recipient >= generations.size() || record.generation != generations[record.recipient])
            return std::nullopt;
        return record;
    }
    void Validate() const {
        if (!next_token || render_deadline < 0 || transfer_deadline < 0) throw std::runtime_error("Invalid saved P3D timeline");
        for (const auto& r : records) {
            if (!r.active) continue;
            if (!r.token || r.token >= next_token || r.recipient >= generations.size() ||
                r.generation > generations[r.recipient] || r.deadline < 0 || r.deadline > (r.transfer ? transfer_deadline : render_deadline))
                throw std::runtime_error("Invalid saved P3D record");
            for (const auto& other : records) {
                if (&r != &other && other.active && other.token == r.token)
                    throw std::runtime_error("Duplicate saved P3D token");
                else if (other.active && other.token < r.token && (r.transfer || !other.transfer) && other.deadline >= r.deadline)
                    throw std::runtime_error("Invalid saved P3D render ordering");
            }
        }
    }
    template <class Archive> void serialize(Archive& ar, unsigned) {
        ar & records & generations & next_token & render_deadline & transfer_deadline;
    }
};
} // namespace Cara
