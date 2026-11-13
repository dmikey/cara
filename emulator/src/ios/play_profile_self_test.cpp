// SPDX-License-Identifier: GPL-2.0-or-later
#include "ios/play_profile.h"
#include "ios/session_presentation.h"
#include <iostream>
int main() {
    static_assert(Cara::DefaultPlayMode==Cara::PlayMode::Performance);
    static_assert(!Cara::ShouldPaceFrames(false,false,false,false));
    static_assert(Cara::ShouldPaceFrames(false,false,false,true));
    static_assert(Cara::ShouldPaceFrames(false,false,true,false));
    static_assert(!Cara::ShouldPaceFrames(true,false,true,true));
    static_assert(Cara::ShouldPaceFrames(true,true,false,false));
    using State=Cara::SessionPresentation;
    static_assert(Cara::ResolveSessionPresentation(false,false,false,true,true,false)==State::Stopped);
    static_assert(Cara::ResolveSessionPresentation(true,true,true,true,true,false)==State::Loading);
    static_assert(Cara::ResolveSessionPresentation(false,true,false,false,true,false)==State::Loading);
    static_assert(Cara::ResolveSessionPresentation(false,true,true,false,false,false)==State::Running);
    static_assert(Cara::ResolveSessionPresentation(false,true,true,false,true,false)==State::Paused);
    static_assert(Cara::ResolveSessionPresentation(false,true,true,true,true,false)==State::Stopping);
    static_assert(Cara::ResolveSessionPresentation(false,false,false,true,false,true)==State::Failed);
    static_assert(!Cara::ClearSessionPresentation(State::Paused));
    static_assert(!Cara::AcceptSessionFrame(State::Paused,4,4));
    static_assert(Cara::AcceptSessionFrame(State::Running,4,4));
    static_assert(!Cara::AcceptSessionFrame(State::Running,3,4));
    for (const auto state:{State::Stopped,State::Loading,State::Stopping,State::Failed})
        if (!Cara::ClearSessionPresentation(state) || Cara::AcceptSessionFrame(state,4,4)) return 1;
    const auto manual=Cara::MakePlayPolicy(Cara::PlayMode::Performance,125,false,-1,true,75,false);
    if (manual.adaptive || manual.guest_clock!=75 || manual.maximum_speed!=125 || manual.limit_speed) return 1;
    for (const unsigned clock:{0u,25u,75u,100u,200u,999u}) {
        const auto locked=Cara::MakePlayPolicy(Cara::PlayMode::Accuracy,200,false,-1,true,clock,false);
        const auto diagnostic=Cara::MakePlayPolicy(Cara::PlayMode::Performance,125,true,-1,true,clock,false);
        const auto forced=Cara::MakePlayPolicy(Cara::PlayMode::Performance,125,false,0,true,clock,false);
        const auto automatic=Cara::MakePlayPolicy(Cara::PlayMode::Performance,105,false,-1,false,clock);
        if (locked.adaptive || locked.guest_clock!=100 || locked.maximum_speed!=105 || !locked.limit_speed ||
            diagnostic.adaptive || diagnostic.guest_clock!=100 || !diagnostic.limit_speed ||
            forced.adaptive || forced.guest_clock!=100 || !forced.limit_speed || !automatic.adaptive || automatic.guest_clock!=100) return 1;
    }
    if (Cara::MakePlayPolicy(Cara::PlayMode::Performance,105,false,-1,true,0).guest_clock!=25 ||
        Cara::MakePlayPolicy(Cara::PlayMode::Performance,105,false,-1,true,999).guest_clock!=200) return 1;
    for (unsigned maximum=0;maximum<=400;++maximum) {
        const auto accuracy=Cara::MakePlayPolicy(Cara::PlayMode::Accuracy,maximum);
        const auto performance=Cara::MakePlayPolicy(Cara::PlayMode::Performance,maximum);
        const auto benchmark=Cara::MakePlayPolicy(Cara::PlayMode::Performance,maximum,true);
        const auto overridden=Cara::MakePlayPolicy(Cara::PlayMode::Performance,maximum,false,0);
        if (accuracy.adaptive || accuracy.guest_clock!=100 || accuracy.maximum_speed!=105 ||
            !performance.adaptive || performance.guest_clock!=100 || performance.maximum_speed<50 || performance.maximum_speed>200 ||
            benchmark.adaptive || benchmark.guest_clock!=100 || overridden.adaptive || overridden.guest_clock!=100) return 1;
    }
    std::cout<<"PASS: Performance default; Accuracy105% speed/100% guest; bounded Performance cap; benchmark and explicit fixed-clock overrides\n";
    std::cout<<"PASS: session loading/pause/stop/failure transitions, retained paused display and rejection of queued/stale frames\n";
    std::cout<<"PASS: fixed-clock bounds, optional speed limit, locked Accuracy preset and benchmark isolation from saved tuning\n";
}
