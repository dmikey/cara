// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version. Refer to license.txt.
#include "audio_core/hle/apple_aac_decoder.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include "common/logging/log.h"
#include "core/memory.h"
namespace AudioCore::HLE {
namespace {
bool InFCRAM(u32 address, std::size_t bytes) {
    return address >= Memory::FCRAM_PADDR &&
           static_cast<u64>(address) - Memory::FCRAM_PADDR <= Memory::FCRAM_SIZE &&
           bytes <= Memory::FCRAM_SIZE - (static_cast<u64>(address) - Memory::FCRAM_PADDR);
}
}
AppleAACDecoder::AppleAACDecoder(Memory::MemorySystem& memory_) : memory(memory_) {
    const char* value = std::getenv("CARA_AUDIO_DIAGNOSTICS");
    diagnostics = value && std::strcmp(value, "0") != 0;
    Reset();
}
void AppleAACDecoder::Reset() {
    codec.Reset();
    ++generation;
}
std::optional<BinaryResponse> AppleAACDecoder::ProcessRequest(const BinaryRequest& request) {
    BinaryResponse response{};
    if (request.cmd == DecoderCommand::Init || request.cmd == DecoderCommand::Unknown) {
        if (request.cmd == DecoderCommand::Init) Reset();
        std::memcpy(&response, &request, sizeof(response));
        response.unknown1 = 0;
        return response;
    }
    if (request.cmd != DecoderCommand::Decode) return {};
    ++requests;
    response.codec = request.codec;
    response.cmd = request.cmd;
    response.size = request.size;
    response.sample_rate = DecoderSampleRate::Rate48000;
    AppleAACResult output;
    bool ok = request.codec == DecoderCodec::AAC && request.size <= AppleAACCodec::MaxInput && InFCRAM(request.src_addr, request.size);
    if (ok) {
        // Decode fully before writes; input/output aliases cannot invalidate callback storage.
        ok = codec.Decode(memory.GetFCRAMPointer(request.src_addr - Memory::FCRAM_PADDR),
                          request.size, output);
    }
    const std::size_t bytes = output.pcm[0].size() * sizeof(std::int16_t);
    if (ok && bytes) {
        ok = InFCRAM(request.dst_addr_ch0, bytes) &&
             (output.channels == 1 || InFCRAM(request.dst_addr_ch1, bytes));
    }
    int peak = 0;
    if (ok) {
        response.sample_rate = GetSampleRateEnum(output.sample_rate);
        response.num_channels = output.channels;
        response.num_samples = static_cast<u32>(output.pcm[0].size());
        for (unsigned channel = 0; channel < output.channels; ++channel) {
            if (bytes) std::memcpy(memory.GetFCRAMPointer(
                (channel ? request.dst_addr_ch1 : request.dst_addr_ch0) - Memory::FCRAM_PADDR),
                output.pcm[channel].data(), bytes);
            if (diagnostics) for (auto sample : output.pcm[channel])
                peak = std::max(peak, std::abs(static_cast<int>(sample)));
        }
        frames += output.pcm[0].size();
    } else {
        ++errors;
        // A bounded zero-sample reply avoids wedging the guest's binary pipe on bad input.
        if (errors <= 8 || (errors & (errors - 1)) == 0)
            LOG_WARNING(Audio_DSP, "Apple AAC request rejected: error={} bytes={} profile={} total_errors={}",
                        codec.LastError(), static_cast<u32>(request.size), codec.LastProfile(), errors);
        Reset();
    }
    if (diagnostics && (requests <= 8 || requests % 256 == 0 || (!ok && (errors <= 8 || (errors & (errors - 1)) == 0)))) {
        LOG_INFO(Audio_DSP, "Cara AAC requests={} pcm_frames={} peak={} errors={} rate={} channels={} profile={} generation={}",
                 requests, frames, peak, errors, output.sample_rate, output.channels,
                 codec.LastProfile(), generation);
    }
    return response;
}
}
