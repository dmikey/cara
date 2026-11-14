// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once

#include <array>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace NativeMac {
struct CircleSegment {
    unsigned start;
    unsigned end;
    int x;
    int y;
};

// Fixed-size input description: no game-specific route, RAM inspection or hashes.
inline std::vector<CircleSegment> ParseCircleSegments(std::string_view text) {
    std::vector<CircleSegment> result;
    if (text.empty())
        return result;
    while (!text.empty()) {
        if (result.size() == 16)
            throw std::runtime_error("Native circle route exceeds 16 segments");
        const auto separator = text.find(';');
        auto item = text.substr(0, separator);
        std::array<int, 4> fields{};
        for (unsigned i = 0; i < fields.size(); ++i) {
            const auto colon = item.find(':');
            const auto word = item.substr(0, colon);
            const auto parsed = std::from_chars(word.data(), word.data() + word.size(), fields[i]);
            if (word.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != word.data() + word.size() ||
                (i < 3 ? colon == std::string_view::npos : colon != std::string_view::npos))
                throw std::runtime_error("Native circle route requires start:end:x:y segments");
            if (i < 3)
                item.remove_prefix(colon + 1);
        }
        if (fields[0] < 0 || fields[0] >= fields[1] || fields[1] > 100000 ||
            fields[2] < -1 || fields[2] > 1 || fields[3] < -1 || fields[3] > 1 ||
            (!result.empty() && unsigned(fields[0]) < result.back().end))
            throw std::runtime_error("Native circle segments must be ordered, bounded and disjoint");
        result.push_back({unsigned(fields[0]), unsigned(fields[1]), fields[2], fields[3]});
        if (separator == std::string_view::npos)
            break;
        text.remove_prefix(separator + 1);
        if (text.empty())
            throw std::runtime_error("Native circle route has a trailing separator");
    }
    return result;
}

// Keyboard circle-pad mapping handles diagonal normalization as for ordinary input.
inline std::array<bool, 4> CircleKeys(const std::vector<CircleSegment>& route, unsigned frame) {
    for (const auto& segment : route) {
        if (frame >= segment.start && frame < segment.end)
            return {segment.x < 0, segment.x > 0, segment.y > 0, segment.y < 0};
    }
    return {};
}
} // namespace NativeMac
