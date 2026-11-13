// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version. Refer to license.txt.
#include "apple_aac_codec.h"
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <algorithm>
#include <cstring>
namespace AudioCore::HLE {
namespace {
constexpr OSStatus NeedInput = 'caNI';
constexpr unsigned Rates[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
struct Packet { const std::uint8_t* data; unsigned size; };
struct Input {
    const std::vector<Packet>& packets;
    unsigned channels;
    std::size_t next = 0;
    AudioStreamPacketDescription description{};
};
OSStatus Supply(AudioConverterRef, UInt32* count, AudioBufferList* data,
                AudioStreamPacketDescription** description, void* opaque) {
    auto& in = *static_cast<Input*>(opaque);
    if (in.next == in.packets.size()) { *count = 0; return NeedInput; }
    const auto& packet = in.packets[in.next++];
    *count = 1;
    data->mNumberBuffers = 1;
    data->mBuffers[0] = {in.channels, packet.size, const_cast<std::uint8_t*>(packet.data)};
    in.description = {0, 1024, packet.size};
    if (description) *description = &in.description;
    return noErr;
}
}
struct AppleAACCodec::Impl {
    AudioConverterRef converter = nullptr;
    std::vector<std::uint8_t> pending;
    unsigned rate = 48000, channels = 2, profile = 0, frequency = 0, observed_profile = 0;
    OSStatus error = 0;
    void Clear() {
        if (converter) AudioConverterDispose(converter);
        converter = nullptr;
        pending.clear();
        profile = 0;
    }
    ~Impl() { Clear(); }
    bool Configure(unsigned p, unsigned f, unsigned c) {
        Clear();
        rate = Rates[f]; channels = c; profile = p; frequency = f;
        AudioStreamBasicDescription source{};
        source.mSampleRate = rate; source.mFormatID = kAudioFormatMPEG4AAC;
        source.mFramesPerPacket = 1024; source.mChannelsPerFrame = channels;
        UInt32 info_size = sizeof(source);
        error = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, nullptr, &info_size, &source);
        if (error) return false;
        AudioStreamBasicDescription dest{};
        dest.mSampleRate = rate; dest.mFormatID = kAudioFormatLinearPCM;
        dest.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
        dest.mBytesPerPacket = dest.mBytesPerFrame = 2 * channels;
        dest.mFramesPerPacket = 1; dest.mChannelsPerFrame = channels; dest.mBitsPerChannel = 16;
        AudioClassDescription codec{kAudioDecoderComponentType, kAudioFormatMPEG4AAC,
                                    'appl'}; // Apple software codec manufacturer on macOS and iOS
        error = AudioConverterNewSpecific(&source, &dest, 1, &codec, &converter);
        if (error) return false;
        AudioStreamBasicDescription actual{};
        UInt32 actual_size = sizeof(actual);
        error = AudioConverterGetProperty(converter, kAudioConverterCurrentOutputStreamDescription,
                                          &actual_size, &actual);
        if (error) return false;
        if (actual.mSampleRate != rate || actual.mChannelsPerFrame != channels ||
            actual.mBytesPerFrame != 2 * channels) error = 'caOF';
        return error == noErr;
    }
};
AppleAACCodec::AppleAACCodec() : impl(std::make_unique<Impl>()) {}
AppleAACCodec::~AppleAACCodec() = default;
void AppleAACCodec::Reset() { impl->Clear(); impl->error = 0; }
std::int32_t AppleAACCodec::LastError() const { return impl->error; }
unsigned AppleAACCodec::LastProfile() const { return impl->observed_profile; }
bool AppleAACCodec::Decode(const std::uint8_t* bytes, std::size_t size, AppleAACResult& output) {
    output = {};
    auto fail = [&](OSStatus error) { impl->Clear(); impl->error = error; return false; };
    if (size > MaxInput || (!bytes && size)) return fail('caSZ');
    std::vector<std::uint8_t> input = impl->pending;
    if (size) input.insert(input.end(), bytes, bytes + size);
    impl->pending.clear();
    std::vector<Packet> packets;
    unsigned profile = 0, frequency = 0, channels = 0;
    std::size_t pos = 0;
    while (input.size() - pos >= 7) {
        const auto* h = input.data() + pos;
        if (h[0] != 255 || (h[1] & 0xf6) != 0xf0) return fail('caAD');
        const unsigned header = (h[1] & 1) ? 7 : 9;
        const unsigned length = ((h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
        const unsigned p = (h[2] >> 6) + 1, f = (h[2] >> 2) & 15;
        const unsigned c = ((h[2] & 1) << 2) | (h[3] >> 6);
        impl->observed_profile = p;
        // ADTS LC mono/stereo configuration only; no PCE or multiple raw blocks.
        if (length <= header || p != 2 || f < 3 || f > 11 || c < 1 || c > 2 || (h[6] & 3))
            return fail('caFM');
        if (input.size() - pos < length) break;
        if (!packets.empty() && (p != profile || f != frequency || c != channels))
            return fail('caMX');
        profile = p; frequency = f; channels = c;
        packets.push_back({h + header, length - header});
        if (packets.size() > 128) return fail('caSZ');
        pos += length;
    }
    if (input.size() - pos >= 8192) return fail('caSZ');
    // Configure before retaining the incomplete tail: Configure resets stream state.
    if (!packets.empty() && (!impl->converter || profile != impl->profile ||
                            frequency != impl->frequency || channels != impl->channels)) {
        if (!impl->Configure(profile, frequency, channels)) return fail(impl->error);
    }
    impl->pending.assign(input.begin() + pos, input.end());
    output.sample_rate = impl->rate; output.channels = impl->channels;
    if (packets.empty()) return true;
    Input context{packets, channels};
    std::vector<std::int16_t> interleaved(4096 * channels);
    for (unsigned iteration = 0; iteration < 272; ++iteration) {
        UInt32 frames = 4096;
        AudioBufferList buffer{1, {{channels, static_cast<UInt32>(interleaved.size() * 2), interleaved.data()}}};
        const auto status = AudioConverterFillComplexBuffer(impl->converter, Supply, &context,
                                                            &frames, &buffer, nullptr);
        if (frames > 4096 || output.pcm[0].size() + frames > MaxOutputFrames) return fail('caSZ');
        if (status != noErr && status != NeedInput) return fail(status);
        for (UInt32 i = 0; i < frames; ++i)
            for (unsigned channel = 0; channel < channels; ++channel)
                output.pcm[channel].push_back(interleaved[i * channels + channel]);
        if (status == NeedInput) { impl->error = 0; return true; }
        if (!frames && context.next == packets.size()) return fail('caNP');
    }
    return fail('caLP');
}
}
