// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <limits>
#include <stdexcept>
#include "cara/presentation_guard.h"

static void Require(bool value,const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        Cara::PresentationGuard guard;
        std::uint64_t latest=0,shown=0,submissions=0;
        for (std::uint64_t ticket=0;ticket<100000;++ticket) {
            // A fast producer may finish many new guest frames in one display
            // interval. Their offscreen contents continue to replace latest.
            for (unsigned frame=0;frame<ticket%7+1;++frame) {
                ++latest;
                if (guard.Admit(ticket)) { shown=latest; ++submissions; }
                else Require(shown<latest,"Coalescing changed guest output state");
            }
            Require(!guard.Admit(ticket),"Duplicate host submission in one display interval");
        }
        Require(submissions==100000,"Lost display tickets");
        Require(shown<latest,"Fixture has no pending frame");
        if (guard.Admit(100000)) shown=latest;
        Require(shown==latest,"Latest pending framebuffer not presented on next ticket");
        guard.Reset(); Require(guard.Admit(100000),"Session reset retained old ticket");
        Require(guard.Admit(100000,false) && guard.Admit(100000,false),"Explicit bypass coalesced");
        Require(guard.Admit(100000),"Reenable retained stale ticket");
        Require(guard.Admit(std::numeric_limits<std::uint64_t>::max()),"Maximum ticket rejected");
        Require(guard.Admit(0),"Ticket wrap rejected");
        Require(!guard.Admit(0),"Wrapped duplicate admitted");
        std::cout<<"Presentation guard: 100000 display intervals, producer bursts, pending framebuffer, reset and wrap passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
