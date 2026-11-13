// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version. Refer to license.txt.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
namespace AudioCore::HLE {
struct AppleAACResult {
    unsigned sample_rate = 48000, channels = 2;
    std::array<std::vector<std::int16_t>, 2> pcm;
};
// Owns bounded ADTS framing and the software decoder's overlap history. Not real-time safe.
class AppleAACCodec {
public:
    static constexpr std::size_t MaxInput = 65536;
    static constexpr std::size_t MaxOutputFrames = 131072;
    AppleAACCodec();
    ~AppleAACCodec();
    void Reset();
    bool Decode(const std::uint8_t* bytes, std::size_t size, AppleAACResult& output);
    std::int32_t LastError() const;
    unsigned LastProfile() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
