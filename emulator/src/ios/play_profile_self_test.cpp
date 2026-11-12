// SPDX-License-Identifier: GPL-2.0-or-later
#include "ios/play_profile.h"
#include <iostream>
int main() {
    static_assert(Cara::DefaultPlayMode==Cara::PlayMode::Performance);
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
}
