// Copyright 2026 Cara contributors. GPL-2.0-or-later.
#pragma once
#include <array>
#include <SDL_scancode.h>
#include "input_common/keyboard.h"

namespace Cara {
// Track only real window key presses. Reconciliation releases a missed key-up;
// it never invents presses or alters diagnostic/script-owned input.
class KeyboardLatch {
public:
    void Event(InputCommon::Keyboard& keyboard, int code, bool down) {
        if (code < 0 || code >= SDL_NUM_SCANCODES) return;
        held[code] = down;
        if (down) keyboard.PressKey(code); else keyboard.ReleaseKey(code);
    }
    void Reconcile(InputCommon::Keyboard& keyboard, const unsigned char* physical, int count) {
        for (int code = 0; code < SDL_NUM_SCANCODES; ++code)
            if (held[code] && (code >= count || !physical || !physical[code]))
                Event(keyboard, code, false);
    }
    void Release(InputCommon::Keyboard& keyboard) {
        held.fill(false);
        keyboard.ReleaseAllKeys();
    }
private:
    std::array<bool, SDL_NUM_SCANCODES> held{};
};
}
