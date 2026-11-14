// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
namespace Cara {
struct SessionOverlayPolicy {
    bool pause_guest;
    bool accept_game_input;
};
// Online menus keep hardware time running, while all overlays own player input.
constexpr SessionOverlayPolicy OverlayPolicy(bool running, bool library, bool menu,
                                            bool background, bool room_active) {
    const bool overlay = library || menu;
    return {background || (overlay && !room_active), running && !overlay && !background};
}
}
