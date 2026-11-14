// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
// Arithmetic-only oracle: no ROM, GL context, or game/user data.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
#include "video_core/renderer_opengl/gl_lcd_upload.h"

namespace {
unsigned checks{};
void Check(bool value) {
    ++checks;
    if (!value) {
        std::cerr << "LCD upload check " << checks << " failed\n";
        std::abort();
    }
}
using Color = std::array<std::uint8_t, 4>;
Color Pattern(unsigned screen, unsigned x, unsigned y) {
    return {std::uint8_t(x * 13 + y * 3 + screen * 19),
            std::uint8_t(x * 7 + y * 29 + screen * 41),
            std::uint8_t(x * 31 + y * 5 + screen * 67),
            std::uint8_t(x * 17 + y * 11 + screen * 97)};
}
void EncodeGuest(unsigned format, const Color& color, std::uint8_t* pixel) {
    if (format == 0) {
        pixel[0] = color[3]; pixel[1] = color[2]; pixel[2] = color[1]; pixel[3] = color[0];
    } else if (format == 1) {
        pixel[0] = color[2]; pixel[1] = color[1]; pixel[2] = color[0];
    } else {
        const unsigned word = format == 2 ? ((color[0] >> 3) << 11) | ((color[1] >> 2) << 5) | (color[2] >> 3) :
                              format == 3 ? ((color[0] >> 3) << 11) | ((color[1] >> 3) << 6) | ((color[2] >> 3) << 1) | (color[3] >> 7) :
                                            ((color[0] >> 4) << 12) | ((color[1] >> 4) << 8) | ((color[2] >> 4) << 4) | (color[3] >> 4);
        pixel[0] = word & 255; pixel[1] = word >> 8;
    }
}
void CheckUploaded(unsigned format, const std::uint8_t* uploaded, const Color& color) {
    if (format < 2) {
        for (unsigned c = 0; c < (format == 0 ? 4 : 3); ++c) Check(uploaded[c] == color[c]);
    } else {
        std::uint16_t word{};
        std::memcpy(&word, uploaded, sizeof(word)); // Matching GL packed types read native words.
        if (format == 2) {
            Check((word >> 11) == (color[0] >> 3));
            Check(((word >> 5) & 63) == (color[1] >> 2));
            Check((word & 31) == (color[2] >> 3));
        } else if (format == 3) {
            Check((word >> 11) == (color[0] >> 3));
            Check(((word >> 6) & 31) == (color[1] >> 3));
            Check(((word >> 1) & 31) == (color[2] >> 3));
            Check((word & 1) == (color[3] >> 7));
        } else {
            for (unsigned c = 0; c < 4; ++c) Check(((word >> (12 - c * 4)) & 15) == (color[c] >> 4));
        }
    }
}
} // namespace

int main() {
    using OpenGL::LcdUpload::Stage;
    std::vector<std::uint8_t> output;
    // Every packed code, odd upload width, and padding poisoned independently.
    for (unsigned format = 2; format <= 4; ++format) {
        std::vector<std::uint8_t> source(65536 * 4, 0xcd);
        for (unsigned word = 0; word < 65536; ++word) {
            source[word * 4] = word & 255; source[word * 4 + 1] = word >> 8;
        }
        Check(Stage(source.data(), source.size(), format, 1, 65536, 4, output));
        Check(output.size() == 65536 * 2);
        for (unsigned word = 0; word < 65536; ++word) {
            std::uint16_t actual{}; std::memcpy(&actual, output.data() + word * 2, 2);
            Check(actual == word);
        }
    }
    // Both LCDs use sideways buffers. DrawScreens rotates (x,y) to upload (239-y,x).
    for (unsigned format = 0; format < 5; ++format) {
        const unsigned bpp = format == 0 ? 4 : format == 1 ? 3 : 2;
        for (unsigned screen = 0; screen < 2; ++screen) {
            const unsigned screen_width = screen == 0 ? 400 : 320;
            for (bool padded : {false, true}) {
                const unsigned stride = (padded ? 256 : 240) * bpp;
                std::vector<std::uint8_t> source(stride * screen_width, 0xa5);
                for (unsigned y = 0; y < 240; ++y)
                    for (unsigned x = 0; x < screen_width; ++x)
                        EncodeGuest(format, Pattern(screen, x, y), source.data() + x * stride + (239 - y) * bpp);
                const auto before = source;
                Check(Stage(source.data(), source.size(), format, 240, screen_width, stride, output));
                Check(output.size() == 240 * screen_width * bpp);
                Check(source == before);
                for (unsigned y = 0; y < 240; ++y)
                    for (unsigned x = 0; x < screen_width; ++x)
                        CheckUploaded(format, output.data() + (x * 240 + 239 - y) * bpp, Pattern(screen, x, y));
            }
        }
        // Tight rows not divisible by GL's default alignment4, with padded guest rows.
        std::vector<std::uint8_t> source(4 * bpp * 3, 0x9b);
        for (unsigned y = 0; y < 3; ++y)
            for (unsigned x = 0; x < 3; ++x)
                EncodeGuest(format, Pattern(2, x, y), source.data() + (y * 4 + x) * bpp);
        Check(Stage(source.data(), source.size(), format, 3, 3, 4 * bpp, output));
        for (unsigned y = 0; y < 3; ++y)
            for (unsigned x = 0; x < 3; ++x)
                CheckUploaded(format, output.data() + (y * 3 + x) * bpp, Pattern(2, x, y));
        const auto old_output = output;
        Check(!Stage(source.data(), 4 * bpp * 2 + 3 * bpp - 1, format, 3, 3, 4 * bpp, output));
        Check(output == old_output);
        Check(!Stage(source.data(), source.size(), format, 3, 3, 3 * bpp - 1, output));
        Check(output == old_output);
    }
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const std::uint8_t byte{};
    Check(!Stage(nullptr, maximum, 0, 1, 1, 4, output));
    Check(!Stage(&byte, maximum, 5, 1, 1, 4, output));
    Check(!Stage(&byte, maximum, 0, 0, 1, 4, output));
    Check(!Stage(&byte, maximum, 0, 1, 0, 4, output));
    Check(!Stage(&byte, maximum, 0, maximum, 1, maximum, output));
    Check(!Stage(&byte, maximum, 0, 1, maximum, 4, output));
    Check(!Stage(&byte, maximum, 0, 1, 3, maximum, output));
    std::cout << "PASS " << checks << " LCD byte assertions; all packed16 codes; both LCD rotations; tight/padded rows\n";
}
