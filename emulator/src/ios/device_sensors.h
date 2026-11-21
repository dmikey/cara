// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <UIKit/UIKit.h>

namespace Cara {
void RegisterIOSDeviceSensors();
void SetIOSDeviceSensorContext(bool active, UIInterfaceOrientation orientation);

// Shared with the microphone bridge. Each backend suspends host input while
// the game is paused or the scene is inactive.
void RegisterIOSMicrophone();
void SetIOSMicrophoneForeground(bool active);
} // namespace Cara
