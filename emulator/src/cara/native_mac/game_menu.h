#pragma once

#include <string>

namespace Cara::NativeGameMenu {
// Main-thread UI only. The core must be fully closed before using the request.
void Install();
std::string TakeRequestedROM();
}
