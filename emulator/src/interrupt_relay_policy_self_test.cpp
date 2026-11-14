// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/gsp/interrupt_relay_policy.h"

namespace {
using Service::GSP::InterruptId;
using Service::GSP::InterruptRelayQueue;
namespace Policy = Service::GSP::RelayQueuePolicy;
using Policy::Outcome;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Guarded {
    std::uint64_t before = 0x123456789abcdef0ULL;
    InterruptRelayQueue queue{};
    std::uint64_t after = 0xfedcba9876543210ULL;
};
static_assert(sizeof(InterruptRelayQueue) == 0x40);
static_assert(offsetof(InterruptRelayQueue, padding1) == 3);
static_assert(static_cast<unsigned>(InterruptId::PDC0) == 2 &&
              static_cast<unsigned>(InterruptId::PDC1) == 3 &&
              static_cast<unsigned>(InterruptId::P3D) == 5 &&
              static_cast<unsigned>(InterruptId::DMA) == 6);
void Canary(const Guarded& guarded) {
    Require(guarded.before == 0x123456789abcdef0ULL && guarded.after == 0xfedcba9876543210ULL,
            "Relay mutation escaped ABI storage");
}
} // namespace

int main() {
    try {
        std::uint64_t cases = 0;
        // Exhaust all byte-sized guest heads/counts, interrupt classes and bit0
        // configurations. Expected mutations are isolated byte/counter effects;
        // no malformed header is silently repaired and no slot is overwritten.
        for (unsigned head = 0; head < 256; ++head)
            for (unsigned count = 0; count < 256; ++count)
                for (unsigned id = 0; id < 7; ++id)
                    for (unsigned config : {0u, 1u, 0xfeu, 0xffu}) {
                        Guarded guarded;
                        auto& queue = guarded.queue;
                        queue.index = head; queue.number_interrupts = count;
                        queue.error_code = 0xa5; queue.padding1 = config;
                        queue.missed_PDC0 = std::numeric_limits<std::uint32_t>::max();
                        queue.missed_PDC1 = 0x98765432;
                        for (unsigned slot = 0; slot < 52; ++slot)
                            queue.slot[slot] = static_cast<InterruptId>(slot % 7);
                        const auto original = queue;
                        auto expected = original;
                        const bool pdc = id == 2 || id == 3;
                        Outcome outcome;
                        std::size_t slot = 52;
                        if (pdc && (config & 1)) outcome = Outcome::IgnoredPdc;
                        else if (pdc && count >= 32) {
                            outcome = Outcome::MissedPdc;
                            if (id == 2) expected.missed_PDC0 = 0;
                            else ++expected.missed_PDC1;
                        } else if (count >= 52) {
                            outcome = Outcome::Full; expected.error_code = 1;
                        } else if (head >= 52) outcome = Outcome::InvalidHeader;
                        else {
                            outcome = Outcome::Enqueued;
                            slot = (head + count) % 52;
                            expected.slot[slot] = static_cast<InterruptId>(id);
                            expected.number_interrupts = count + 1;
                        }
                        Require(Policy::CanEnqueueCompletion(queue) == (head < 52 && count < 52),
                                "Wrong completion-capacity result");
                        const auto decision = Policy::Enqueue(queue, static_cast<InterruptId>(id),
                                                               queue.padding1);
                        Require(decision.outcome == outcome && decision.slot == slot &&
                                decision.signal_event == (outcome == Outcome::Enqueued) &&
                                decision.update_framebuffer == pdc, "Wrong relay decision");
                        Require(std::memcmp(&queue, &expected, sizeof(queue)) == 0,
                                "Unexpected ABI byte mutation or sticky error reset");
                        Canary(guarded); ++cases;
                    }
        // Unsupported IDs cannot turn into arbitrary slot bytes or signal a wake.
        for (unsigned id = 7; id < 256; ++id) {
            Guarded guarded; const auto original = guarded.queue;
            const auto decision = Policy::Enqueue(guarded.queue, static_cast<InterruptId>(id), 0);
            Require(decision.outcome == Outcome::InvalidInterrupt && !decision.signal_event &&
                    !decision.update_framebuffer && std::memcmp(&original, &guarded.queue, 64) == 0,
                    "Unsupported interrupt changed relay state");
            Canary(guarded); ++cases;
        }
        // Independent logical FIFO model with guest consumption, ring wraps,
        // repeated full queues, ignored/missed PDC and explicit guest error clear.
        Guarded guarded;
        auto& queue = guarded.queue;
        queue.index = 51;
        std::deque<InterruptId> model;
        std::uint32_t missed[2]{};
        std::uint8_t error = 0;
        std::uint32_t seed = 0x7be19a03;
        auto random = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
        std::uint64_t accepted = 0, full = 0, missed_count = 0, ignored = 0;
        for (unsigned iteration = 0; iteration < 1000000; ++iteration) {
            const auto value = random();
            if ((value & 3) == 0 && !model.empty()) {
                Require(queue.slot[queue.index] == model.front(), "FIFO consumer observed wrong ID");
                model.pop_front(); queue.index = (queue.index + 1) % 52; --queue.number_interrupts;
            } else {
                const auto id = static_cast<InterruptId>((value >> 8) % 7);
                const bool pdc = id == InterruptId::PDC0 || id == InterruptId::PDC1;
                queue.padding1 = (value >> 24) & 0xff;
                Outcome expected;
                if (pdc && (queue.padding1 & 1)) { expected = Outcome::IgnoredPdc; ++ignored; }
                else if (pdc && model.size() >= 32) {
                    expected = Outcome::MissedPdc;
                    ++missed[id == InterruptId::PDC0 ? 0 : 1]; ++missed_count;
                } else if (model.size() == 52) { expected = Outcome::Full; error = 1; ++full; }
                else { expected = Outcome::Enqueued; model.push_back(id); ++accepted; }
                const auto decision = Policy::Enqueue(queue, id, queue.padding1);
                Require(decision.outcome == expected &&
                        decision.signal_event == (expected == Outcome::Enqueued),
                        "FIFO model delivery/signal mismatch");
            }
            if (iteration % 997 == 0) { queue.error_code = 0; error = 0; }
            Require(queue.index < 52 && queue.number_interrupts == model.size() &&
                    queue.number_interrupts <= 52 && queue.error_code == error &&
                    queue.missed_PDC0 == missed[0] && queue.missed_PDC1 == missed[1],
                    "FIFO count/error/miss model mismatch");
            for (std::size_t i = 0; i < model.size(); ++i)
                Require(queue.slot[(queue.index + i) % 52] == model[i], "FIFO order mismatch");
            Canary(guarded);
        }
        Require(accepted && full && missed_count && ignored, "FIFO model missed boundary classes");
        std::cout << "Relay ABI/policy cases=" << cases << " FIFO operations=1000000 accepted="
                  << accepted << " full=" << full << " missed_pdc=" << missed_count
                  << " ignored_pdc=" << ignored << " passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
