// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version. Refer to license.txt.
#pragma once
#include "audio_core/hle/decoder.h"
#include "audio_core/hle/apple_aac_codec.h"
namespace AudioCore::HLE {
class AppleAACDecoder final : public DecoderBase {
public:
    explicit AppleAACDecoder(Memory::MemorySystem& memory);
    std::optional<BinaryResponse> ProcessRequest(const BinaryRequest& request) override;
    bool IsValid() const override { return true; }
    void Reset() override;
private:
    Memory::MemorySystem& memory;
    AppleAACCodec codec;
    bool diagnostics;
    u64 requests = 0, frames = 0, errors = 0, generation = 0;
};
}
