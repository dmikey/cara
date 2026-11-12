// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <iostream>
#include <stdexcept>
#include "cara/adaptive_clock.h"
#include "core/core_timing.h"

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        for (unsigned percent : {25u, 50u, 70u, 75u, 100u}) {
            Core::Timing timing;
            timing.UpdateClockSpeed(percent);
            for (unsigned core = 0; core < 4; ++core) {
                auto timer = timing.GetTimer(core);
                timer->Advance(10000);
                const auto budget = timer->GetDowncount();
                Require(std::abs(budget - 100 * s64(percent)) <= 1, "Wrong CPU budget");
                for (unsigned cycle = 0; cycle < percent * 100; ++cycle) timer->AddTicks(1);
                Require(std::abs(timer->GetTicks() - 10000) <= 1, "Fractional clock drift");
            }
        }
        for (unsigned percent : {25u, 50u, 75u, 100u}) {
            Core::Timing timing;
            unsigned calls = 0;
            const auto* event = timing.RegisterEvent("clock-test", [&](u64, s64) { ++calls; });
            timing.ScheduleEvent(1000, event);
            timing.UpdateClockSpeed(percent);
            auto timer = timing.GetTimer(0);
            timer->Advance(1000);
            timer->AddTicks(timer->GetDowncount());
            timer->Advance(1000);
            Require(calls == 1, "Peripheral event rate changed with CPU clock");
            const auto before = timer->GetTicks();
            timing.UpdateClockSpeed(100);
            Require(timer->GetTicks() == before, "Clock transition moved guest time");
        }
        Core::Timing timing;
        bool rejected = false;
        try { timing.UpdateClockSpeed(0); } catch (const std::invalid_argument&) { rejected = true; }
        Require(rejected, "Zero clock accepted");

        Cara::AdaptiveClock control;
        for (unsigned frame = 0; frame < 1000; ++frame) control.ObserveFrame(0.04, 0.04);
        Require(control.Percent() == 100, "Disabled adaptive mode changed the clock");
        control.SetEnabled(true);
        for (unsigned frame = 0; frame < 3000; ++frame) control.ObserveFrame(0.03, 0.03);
        Require(control.Percent() == 50, "Sustained overload did not reach the safe floor");
        for (unsigned frame = 0; frame < 3000; ++frame) control.ObserveFrame(0.005, 1.0 / 60);
        Require(control.Percent() == 100, "Adaptive clock did not recover");
        control.SetEnabled(true);
        for (unsigned frame = 0; frame < 180; ++frame) control.ObserveFrame(0.005, 1.0 / 60);
        control.ObserveFrame(0.4, 0.4);
        for (unsigned frame = 0; frame < 300; ++frame) control.ObserveFrame(0.005, 1.0 / 60);
        Require(control.Percent() == 100, "One transient stall lowered the clock");
        for (unsigned frame = 0; frame < 300; ++frame) control.ObserveFrame(0.005, 1.0 / 60);
        Require(control.Percent() == 100, "Frame pacing was mistaken for CPU load");
        control.SetEnabled(false);
        Require(control.Percent() == 100, "Disabling adaptive mode did not restore full clock");
        std::cout << "Four-core clock budgets, fractional cycles, peripheral timing, transitions, "
                     "adaptive bounds, recovery, transient stalls and pacing passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
