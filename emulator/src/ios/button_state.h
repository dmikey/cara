// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace Cara {
// UIKit may deliver a quick press and release between guest HID updates.
class ButtonState {
    std::atomic<std::uint32_t> held{0};
    std::atomic<std::uint32_t> pending{0};
public:
    void Press(unsigned index) {
        const auto mask = std::uint32_t{1} << index;
        held.fetch_or(mask);
        pending.fetch_or(mask);
    }
    void Release(unsigned index) { held.fetch_and(~(std::uint32_t{1} << index)); }
    bool Poll(unsigned index) {
        const auto mask = std::uint32_t{1} << index;
        const auto pressed = pending.fetch_and(~mask);
        return ((held.load() | pressed) & mask) != 0;
    }
    std::uint32_t Held() const { return held.load(); }
    void Clear() { held = 0; pending = 0; }
};
} // namespace Cara
