// SPDX-License-Identifier: GPL-2.0-or-later
#include <iostream>
#include "ios/button_state.h"

int main() {
    Cara::ButtonState buttons;
    if (buttons.Poll(0)) return 1;
    buttons.Press(0);
    buttons.Release(0); // Both UIKit events occur before the next HID update.
    if (!buttons.Poll(0) || buttons.Poll(0)) return 2;
    buttons.Press(10); // Start remains down across updates.
    if (!buttons.Poll(10) || !buttons.Poll(10)) return 3;
    buttons.Press(0);
    buttons.Release(0);
    if (!buttons.Poll(0) || !buttons.Poll(10) || buttons.Poll(0)) return 4;
    buttons.Release(10);
    if (buttons.Poll(10)) return 5;
    buttons.Press(0);
    buttons.Press(10);
    buttons.Clear();
    if (buttons.Held() || buttons.Poll(0) || buttons.Poll(10)) return 6;
    std::cout << "iOS input regression passed: quick taps, holds, simultaneous buttons, reset\n";
}
