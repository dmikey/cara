// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Cara {
// One host submission per display callback. Guest rendering and emulated time
// continue normally; the next ticket presents the latest composed framebuffer.
class PresentationGuard {
public:
    bool Admit(std::uint64_t ticket, bool enabled=true) {
        if (!enabled) { Reset(); return true; }
        if (seen_ && ticket==last_ticket_) return false;
        last_ticket_=ticket; seen_=true; return true;
    }
    void Reset() { seen_=false; }
private:
    std::uint64_t last_ticket_=0;
    bool seen_=false;
};
}
