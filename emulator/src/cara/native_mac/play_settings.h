// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <charconv>
#include <string>
#include <string_view>

#include "cara/play_profile.h"

namespace Cara::MacPreferences {

enum class DisplayScale : unsigned { Fit = 0, Lossless2x = 2, Lossless4x = 4 };

struct PlaySettings {
    PlayMode mode = DefaultPlayMode;
    bool fixed_guest_clock = false;
    unsigned guest_clock_percent = 100;
    bool limit_speed = true;
    unsigned maximum_speed_percent = 100;
    bool adaptive_frame_skip = true;
    DisplayScale display_scale = DisplayScale::Fit;

    bool operator==(const PlaySettings&) const = default;

    PlayPolicy Policy() const {
        return MakePlayPolicy(mode, maximum_speed_percent, false, -1,
                              fixed_guest_clock, guest_clock_percent, limit_speed);
    }

    bool SkipHostFrames() const {
        return mode == PlayMode::Performance && adaptive_frame_skip;
    }

    template <typename Ini>
    static PlaySettings Read(const Ini& ini) {
        PlaySettings result;
        const auto flag = [&](const char* key, bool fallback) {
            const auto value = ini.Get("Play", key);
            if (value == "true" || value == "1")
                return true;
            if (value == "false" || value == "0")
                return false;
            return fallback;
        };
        const auto percent = [](std::string_view value, unsigned minimum) {
            unsigned number = 0;
            const auto end = value.data() + value.size();
            const auto parsed = std::from_chars(value.data(), end, number);
            return parsed.ec == std::errc{} && parsed.ptr == end && number >= minimum &&
                           number <= 200
                       ? number
                       : 100u;
        };
        if (ini.Get("Play", "mode") == "accuracy")
            result.mode = PlayMode::Accuracy;
        result.fixed_guest_clock = flag("fixed_guest_clock", false);
        result.guest_clock_percent = percent(ini.Get("Play", "guest_clock_percent"), 25);
        result.limit_speed = flag("limit_speed", true);
        result.maximum_speed_percent = percent(ini.Get("Play", "maximum_speed_percent"), 50);
        result.adaptive_frame_skip = flag("adaptive_frame_skip", true);
        const auto scale = ini.Get("Renderer", "output_scale");
        if (scale == "2")
            result.display_scale = DisplayScale::Lossless2x;
        else if (scale == "4")
            result.display_scale = DisplayScale::Lossless4x;
        return result;
    }

    template <typename Ini>
    void Write(Ini& ini) const {
        ini.Set("Play", "mode", mode == PlayMode::Accuracy ? "accuracy" : "performance");
        ini.Set("Play", "fixed_guest_clock", fixed_guest_clock ? "true" : "false");
        ini.Set("Play", "guest_clock_percent", std::to_string(guest_clock_percent));
        ini.Set("Play", "limit_speed", limit_speed ? "true" : "false");
        ini.Set("Play", "maximum_speed_percent", std::to_string(maximum_speed_percent));
        ini.Set("Play", "adaptive_frame_skip", adaptive_frame_skip ? "true" : "false");
        ini.Set("Renderer", "output_scale", std::to_string(static_cast<unsigned>(display_scale)));
    }
};

} // namespace Cara::MacPreferences
