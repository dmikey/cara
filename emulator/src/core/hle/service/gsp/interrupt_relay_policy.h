// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.
//
// Queue semantics adapted from Azahar PR #2095, merge
// 5ddbaeae237954d1f9c1843e6cf75d8b160c2601 (2026-05-06):
// src/core/hle/service/gsp/{gsp_interrupt.h,gsp_gpu.cpp}.
// Source: https://github.com/azahar-emu/azahar/pull/2095
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace Service::GSP::RelayQueuePolicy {
inline constexpr std::size_t Capacity = 0x34;
inline constexpr std::size_t PdcStopThreshold = 0x20;
inline constexpr std::uint8_t FullError = 1;
inline constexpr std::uint8_t IgnorePdcMask = 1;

enum class Outcome { Enqueued, Full, MissedPdc, IgnoredPdc, InvalidHeader, InvalidInterrupt };
struct Decision {
    Outcome outcome;
    bool signal_event;
    bool update_framebuffer;
    std::size_t slot = Capacity; // No slot was written unless outcome == Enqueued.
};

// This check does not reserve capacity or mutate the shared-memory error byte.
// Recheck at actual delivery on the emulation thread, before consuming a pending
// completion. A delayed completion must not be silently dropped or sent early.
template <typename Queue>
constexpr bool CanEnqueueCompletion(const Queue& queue) {
    return queue.index < Capacity && queue.number_interrupts < Capacity;
}

// Pure shared-memory mutation only: the caller owns event signaling, framebuffer
// updates and session lifetime checks. A successful write preserves prior error
// state; only guest code clears the sticky error byte.
// Pass the existing ABI byte at offset 3 (Cara's padding1) as config.
template <typename Queue, typename Interrupt>
constexpr Decision Enqueue(Queue& queue, Interrupt interrupt, std::uint8_t config) {
    static_assert(std::is_standard_layout_v<Queue> && sizeof(Queue) == 0x40);
    static_assert(offsetof(Queue, index) == 0 && offsetof(Queue, number_interrupts) == 1 &&
                  offsetof(Queue, error_code) == 2 && offsetof(Queue, missed_PDC0) == 4 &&
                  offsetof(Queue, missed_PDC1) == 8 && offsetof(Queue, slot) == 0xC);
    static_assert(sizeof(queue.slot) == Capacity && sizeof(queue.slot[0]) == 1);
    const auto id = static_cast<std::uint8_t>(interrupt);
    if (id > 6) return {Outcome::InvalidInterrupt, false, false};
    const bool pdc = id == 2 || id == 3;
    // Upstream deliberately applies PDC suppression before the full-queue test.
    // Even ignored/missed PDC must still update dirty framebuffer information.
    if (pdc) {
        if (config & IgnorePdcMask) return {Outcome::IgnoredPdc, false, true};
        if (queue.number_interrupts >= PdcStopThreshold) {
            if (id == 2) queue.missed_PDC0 = std::uint32_t(queue.missed_PDC0) + std::uint32_t{1};
            else queue.missed_PDC1 = std::uint32_t(queue.missed_PDC1) + std::uint32_t{1};
            return {Outcome::MissedPdc, false, true};
        }
    }
    if (queue.number_interrupts >= Capacity) {
        queue.error_code = FullError;
        return {Outcome::Full, false, pdc};
    }
    // Malformed guest heads are not normalized: that would invent a new FIFO
    // order. Fail closed without a slot write, count change or invented error.
    // This defensive result is additional hardening, not a hardware claim.
    if (queue.index >= Capacity) return {Outcome::InvalidHeader, false, pdc};
    const std::size_t next = (std::size_t(queue.index) + queue.number_interrupts) % Capacity;
    using Slot = std::remove_reference_t<decltype(queue.slot[0])>;
    queue.slot[next] = static_cast<Slot>(id);
    queue.number_interrupts = static_cast<std::uint8_t>(queue.number_interrupts + 1);
    return {Outcome::Enqueued, true, pdc, next};
}
} // namespace Service::GSP::RelayQueuePolicy
