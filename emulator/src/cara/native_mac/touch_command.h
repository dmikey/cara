// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
namespace NativeMac {
struct TouchCommand { unsigned x, y, frames; };
inline std::optional<TouchCommand> ParseTouchCommand(const std::string& line) {
    if (line.size()>95) return {};
    std::istringstream input(line);std::string name, extra;int x,y,frames;
    if (!(input>>name>>x>>y>>frames) || name!="touch" || (input>>extra) ||
        x<0 || x>319 || y<0 || y>239 || frames<1 || frames>600) return {};
    return TouchCommand{unsigned(x),unsigned(y),unsigned(frames)};
}
inline std::optional<std::pair<unsigned,unsigned>> TouchFramebufferPoint(
    const TouchCommand& command, unsigned left, unsigned top, unsigned right, unsigned bottom,
    bool enabled) {
    if (!enabled || right<=left || bottom<=top || command.x>319 || command.y>239) return {};
    const auto x=left+(std::uint64_t(2*command.x+1)*(right-left))/640;
    const auto y=top+(std::uint64_t(2*command.y+1)*(bottom-top))/480;
    return std::pair<unsigned,unsigned>{unsigned(x),unsigned(y)};
}
}
