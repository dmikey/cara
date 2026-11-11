// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <string>
#include <string_view>
#include "audio_core/sink.h"

namespace AudioCore {
class IOSAudioSink final : public Sink {
public:
    explicit IOSAudioSink(std::string_view device);
    ~IOSAudioSink() override;
    unsigned int GetNativeSampleRate() const override;
    void SetCallback(std::function<void(s16*, std::size_t)> cb) override;
private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
void SetIOSAudioPaused(bool paused);
void SetIOSAudioMuted(bool muted);
bool IsIOSAudioMuted();
bool IOSAudioOutputFailed();
// Diagnostic snapshots are written by the emulation thread, never the playback callback.
void WriteIOSAudioDiagnostics(const std::string& path);
} // namespace AudioCore
