#pragma once

#include <string>

namespace Cara::NativeGameMenu {
// Main-thread UI only. The core must be fully closed before using the request.
void Install();
enum class StateAction { None, Save, Load };
struct StateRequest { StateAction action = StateAction::None; unsigned slot = 1; };
bool RequestState(StateAction action, unsigned slot);
StateRequest TakeStateRequest();
void StateFailure(const std::string& message);
std::string TakeRequestedROM();
}
