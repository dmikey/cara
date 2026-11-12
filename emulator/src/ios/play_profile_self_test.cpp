// SPDX-License-Identifier: GPL-2.0-or-later
#include "ios/play_profile.h"
#include "ios/session_presentation.h"
#include <iostream>
int main() {
    static_assert(Cara::DefaultPlayMode==Cara::PlayMode::Performance);
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
}
