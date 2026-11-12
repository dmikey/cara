// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Cara {
enum class PlayMode { Accuracy, Performance };
struct PlayPolicy {
    bool adaptive;
    unsigned guest_clock;
    unsigned maximum_speed;
};
constexpr PlayPolicy MakePlayPolicy(PlayMode mode,unsigned requestedMaximum,bool benchmark=false,int adaptiveOverride=-1) {
    const unsigned maximum=requestedMaximum<50?50:requestedMaximum>200?200:requestedMaximum;
    const bool adaptive=adaptiveOverride>=0?adaptiveOverride!=0:(!benchmark && mode==PlayMode::Performance);
    const bool accuracy=!benchmark && adaptiveOverride<0 && mode==PlayMode::Accuracy;
    return {adaptive,100,accuracy?105:maximum};
}
constexpr PlayMode DefaultPlayMode=PlayMode::Performance;
}
