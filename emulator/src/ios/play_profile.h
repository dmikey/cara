// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Cara {
enum class PlayMode { Accuracy, Performance };
struct PlayPolicy {
    bool adaptive;
    unsigned guest_clock;
    unsigned maximum_speed;
    bool limit_speed;
};
constexpr PlayPolicy MakePlayPolicy(PlayMode mode,unsigned requestedMaximum,bool benchmark=false,int adaptiveOverride=-1,
                                  bool fixedGuestRequested=false,unsigned requestedGuest=100,bool limitRequested=true) {
    const unsigned maximum=requestedMaximum<50?50:requestedMaximum>200?200:requestedMaximum;
    const bool diagnostic=benchmark || adaptiveOverride>=0;
    const bool adaptive=adaptiveOverride>=0?adaptiveOverride!=0:(!benchmark && mode==PlayMode::Performance && !fixedGuestRequested);
    const bool accuracy=!benchmark && adaptiveOverride<0 && mode==PlayMode::Accuracy;
    const unsigned guest=requestedGuest<25?25:requestedGuest>200?200:requestedGuest;
    return {adaptive,!diagnostic && !accuracy && fixedGuestRequested?guest:100,accuracy?100:maximum,
            accuracy || diagnostic || limitRequested};
}
// Clock experiments must be explicit, fixed-policy and isolated from regular play.
constexpr PlayPolicy DiagnosticGuestClock(PlayPolicy policy,bool benchmark,unsigned clock) {
    if (benchmark && !policy.adaptive && (clock==50 || clock==100)) policy.guest_clock=clock;
    return policy;
}
constexpr PlayMode DefaultPlayMode=PlayMode::Performance;
constexpr bool ShouldPaceFrames(bool benchmark,bool pacedBenchmark,bool speedOverride,bool userLimit) {
    return benchmark?pacedBenchmark:(speedOverride || userLimit);
}
}
