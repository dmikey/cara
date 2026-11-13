// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include <limits>
#include "cara/presentation_guard.h"

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <typename Exception, typename Action> void Reject(Action action) {
    bool rejected = false;
    try { action(); } catch (const Exception&) { rejected = true; }
    Require(rejected, "Expected invalid operation to be rejected");
}
int main() {
    try {
        const double period = Cara::FramePacer{}.SecondsPerVBlank();
        Cara::PresentationGuard guard;
        double now = guard.ReadyAt(0, true, 100);
        guard.MarkSubmitted(now);
        for (unsigned i = 0; i < 100000; ++i) {
            const double previous = now;
            const unsigned speed = i % 11 == 0 ? 200 : 100;
            const double interval = Cara::FramePacer(speed).SecondsPerVBlank();
            // Vary CPU work, host scheduling oversleep, and blocking presentation.
            now += period * (i % 13) / 4;
            const double ready = guard.ReadyAt(now, true, speed);
            Require(ready >= now && ready >= previous + interval, "Early deadline");
            if (ready > now)
                Reject<std::logic_error>([&] { guard.MarkSubmitted(now); });
            now = ready + period * (i % 3) / 8;
            guard.MarkSubmitted(now);
            Require(now >= previous + interval, "Early actual submission");
        }
        guard.Reset(0);
        guard.ReadyAt(0, true, 100); guard.MarkSubmitted(0);
        Require(guard.ReadyAt(period * 2, true, 100) == period * 2,
                "Blocking present incurred another full interval");
        guard.MarkSubmitted(period * 2);
        Require(guard.ReadyAt(period * 2, true, 200) == period * 2.5, "Live 200% spacing");
        Require(guard.ReadyAt(period * 2, true, 100) == period * 3, "Live 100% spacing");
        Require(guard.ReadyAt(3600, true, 100) == 3600, "Pause retained timing debt");
        guard.MarkSubmitted(3600);
        Require(guard.ReadyAt(3600, true, 100) == 3600 + period, "Pause allowed catchup burst");
        Require(guard.ReadyAt(3600, false, 100) == 3600, "Disabled guard waited");
        Require(guard.ReadyAt(3600, true, 100) == 3600, "Reenable retained stale timeline");
        guard.MarkSubmitted(3600);
        Require(guard.ReadyAt(1, true, 100) == 1, "Backward clock retained deadline");
        guard.MarkSubmitted(1);
        guard.Reset(.5);
        Require(guard.ReadyAt(.5, true, 100) == .5, "Load reset retained deadline");
        guard.MarkSubmitted(.5);
        Require(guard.ReadyAt(.5, true, 0) == .5 + 2 * period, "Lower speed clamp");
        Require(guard.ReadyAt(.5, true, 999) == .5 + period / 2, "Upper speed clamp");
        for (double bad : {std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
            Reject<std::invalid_argument>([&] { guard.ReadyAt(bad, false, 100); });
            Reject<std::invalid_argument>([&] { guard.MarkSubmitted(bad); });
            Reject<std::invalid_argument>([&] { guard.Reset(bad); });
        }
        guard.Reset(std::numeric_limits<double>::max());
        guard.ReadyAt(std::numeric_limits<double>::max(), true, 100);
        guard.MarkSubmitted(std::numeric_limits<double>::max());
        Reject<std::overflow_error>([&] { guard.ReadyAt(std::numeric_limits<double>::max(), true, 100); });
        std::cout << "Presentation guard: 100000 variable-work submissions, blocking present, "
                     "speed changes, pauses, disable/reset, bounds and invalid clocks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
