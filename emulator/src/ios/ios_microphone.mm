// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/device_sensors.h"
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include "audio_core/ios_sink.h"
#include "common/logging/log.h"
#include "core/frontend/mic.h"

namespace {
using namespace Frontend::Mic;

std::atomic<bool> foreground{false};
std::mutex microphone_mutex;

struct Microphone : std::enable_shared_from_this<Microphone> {
    // MIC:U schedules one shared-memory write for every 15 guest samples.
    static constexpr size_t guest_chunk_frames=15;
    static constexpr size_t buffer_frames=128;
    static constexpr size_t sample_capacity=2048;
    dispatch_queue_t control=dispatch_queue_create("com.dmikey.cara.mic.control",DISPATCH_QUEUE_SERIAL);
    dispatch_queue_t callbacks=dispatch_queue_create("com.dmikey.cara.mic.samples",DISPATCH_QUEUE_SERIAL);
    AudioQueueRef queue=nullptr;
    std::atomic<u32> rate{0};
    std::atomic<bool> accepting{false};
    u32 queue_rate=0;
    u32 input_rate=0;
    u32 phase=0;
    s16 previous_input=0;
    bool have_previous=false;
    bool using_audio_session=false;
    bool requesting_permission=false;
    bool interrupted=false;

    std::mutex sample_mutex;
    std::array<s16,sample_capacity> samples{};
    size_t head=0,size=0;

    void Stop() {
        accepting=false;
        if (queue) {
            AudioQueueStop(queue,true);
            dispatch_sync(callbacks,^{});
            AudioQueueDispose(queue,true);
            queue=nullptr;
        }
        queue_rate=0;
        input_rate=0;
        phase=0;
        have_previous=false;
        if (using_audio_session) {
            AudioCore::SetIOSMicrophoneActive(false);
            using_audio_session=false;
        }
        std::lock_guard lock(sample_mutex);
        head=0;
        size=0;
    }

    void SamplesArrived(AudioQueueRef source, AudioQueueBufferRef buffer) {
        if (!accepting.load()) return;
        const auto* input=static_cast<const s16*>(buffer->mAudioData);
        const size_t count=buffer->mAudioDataByteSize/sizeof(s16);
        {
            std::lock_guard lock(sample_mutex);
            const auto append=[&](s16 value) {
                if (size==sample_capacity) {
                    head=(head+1)%sample_capacity;
                    --size;
                }
                samples[(head+size)%sample_capacity]=value;
                ++size;
            };
            for (size_t i=0;i<count;++i) {
                if (!have_previous) {
                    append(input[i]);
                    phase=input_rate;
                    have_previous=true;
                } else {
                    // CoreAudio records at the current route's native rate.
                    // Keep a continuous fractional guest sample position across buffers.
                    for (; phase<=queue_rate; phase+=input_rate) {
                        const s64 weighted=s64(previous_input)*(queue_rate-phase)+s64(input[i])*phase;
                        append(static_cast<s16>(weighted/queue_rate));
                    }
                    phase-=queue_rate;
                }
                previous_input=input[i];
            }
        }
        if (accepting.load()) AudioQueueEnqueueBuffer(source,buffer,0,nullptr);
    }
    void Read(std::array<s16,guest_chunk_frames>& values) {
        if (!foreground.load() || !accepting.load()) return;
        std::lock_guard lock(sample_mutex);
        if (size<values.size()) return;
        for (auto& value : values) {
            value=samples[head];
            head=(head+1)%sample_capacity;
        }
        size-=values.size();
    }

    void Start(u32 wanted_rate) {
        if (!AudioCore::SetIOSMicrophoneActive(true)) return;
        using_audio_session=true;
        if (wanted_rate!=rate.load() || !foreground.load()) { Stop(); return; }
        const auto device_rate=AVAudioSession.sharedInstance.sampleRate;
        if (!std::isfinite(device_rate) || device_rate<8000 || device_rate>192000) { Stop(); return; }
        input_rate=static_cast<u32>(std::lround(device_rate));
        queue_rate=wanted_rate;
        AudioStreamBasicDescription format{};
        format.mSampleRate=input_rate;
        format.mFormatID=kAudioFormatLinearPCM;
        format.mFormatFlags=kLinearPCMFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked;
        format.mBytesPerPacket=format.mBytesPerFrame=sizeof(s16);
        format.mFramesPerPacket=format.mChannelsPerFrame=1;
        format.mBitsPerChannel=16;
        const auto weak=weak_from_this();
        OSStatus status=AudioQueueNewInputWithDispatchQueue(&queue,&format,0,callbacks,
            ^(AudioQueueRef source,AudioQueueBufferRef buffer,const AudioTimeStamp*,UInt32,const AudioStreamPacketDescription*) {
                if (auto owner=weak.lock()) owner->SamplesArrived(source,buffer);
            });
        if (!status) {
            for (unsigned i=0;i<3 && !status;++i) {
                AudioQueueBufferRef buffer=nullptr;
                status=AudioQueueAllocateBuffer(queue,buffer_frames*sizeof(s16),&buffer);
                if (!status) status=AudioQueueEnqueueBuffer(queue,buffer,0,nullptr);
            }
        }
        if (!status) {
            accepting=true;
            status=AudioQueueStart(queue,nullptr);
        }
        if (status) {
            LOG_WARNING(Audio,"Cara microphone input queue: {}",status);
            Stop();
        }
    }

    void Refresh() {
        const auto owner=shared_from_this();
        dispatch_async(control,^{ owner->Reconcile(); });
    }
    void Reconcile() {
        const u32 wanted_rate=rate.load();
        if (!foreground.load() || !wanted_rate || interrupted) {
            if (queue || using_audio_session) Stop();
            return;
        }
        if (queue && queue_rate==wanted_rate &&
            std::abs(AVAudioSession.sharedInstance.sampleRate-input_rate)<.5) return;
        if (queue || using_audio_session) Stop();
        bool authorized=false,unknown=false;
        if (@available(iOS 17.0,*)) {
            const auto permission=AVAudioApplication.sharedInstance.recordPermission;
            authorized=permission==AVAudioApplicationRecordPermissionGranted;
            unknown=permission==AVAudioApplicationRecordPermissionUndetermined;
        } else {
            const auto permission=AVAudioSession.sharedInstance.recordPermission;
            authorized=permission==AVAudioSessionRecordPermissionGranted;
            unknown=permission==AVAudioSessionRecordPermissionUndetermined;
        }
        if (authorized) {
            Start(wanted_rate);
        } else if (unknown && !requesting_permission) {
            requesting_permission=true;
            const auto owner=shared_from_this();
            dispatch_async(dispatch_get_main_queue(),^{
                void(^reply)(BOOL)=^(BOOL) {
                    dispatch_async(owner->control,^{ owner->requesting_permission=false; owner->Refresh(); });
                };
                if (!foreground.load() || !owner->rate.load()) {
                    dispatch_async(owner->control,^{ owner->requesting_permission=false; });
                    return;
                }
                if (@available(iOS 17.0,*))
                    [AVAudioApplication requestRecordPermissionWithCompletionHandler:reply];
                else
                    [AVAudioSession.sharedInstance requestRecordPermission:reply];
            });
        }
    }
};
std::weak_ptr<Microphone> current_mic;

class DeviceMic final : public Interface {
public:
    DeviceMic() : capture(std::make_shared<Microphone>()) {}
    ~DeviceMic() override { StopSampling(); }
    void StartSampling(const Parameters& params) override {
        parameters=params;
        is_sampling=true;
        {
            std::lock_guard lock(microphone_mutex);
            current_mic=capture;
        }
        capture->rate=params.sample_rate;
        capture->Refresh();
    }
    void StopSampling() override {
        if (!is_sampling) return;
        is_sampling=false;
        capture->rate=0;
        capture->Refresh();
    }
    void AdjustSampleRate(u32 next) override {
        parameters.sample_rate=next;
        if (is_sampling) {
            capture->rate=next;
            capture->Refresh();
        }
    }
    Samples Read() override {
        std::array<s16,Microphone::guest_chunk_frames> pcm{};
        capture->Read(pcm);
        const bool unsigned_pcm=parameters.sign==Signedness::Unsigned;
        Samples result;
        result.reserve(pcm.size()*(parameters.sample_size==16?2:1));
        for (const s16 sample : pcm) {
            const u16 bits=static_cast<u16>(sample)^(unsigned_pcm?0x8000:0);
            if (parameters.sample_size==16) result.push_back(u8(bits));
            result.push_back(u8(bits>>8));
        }
        return result;
    }

private:
    std::shared_ptr<Microphone> capture;
};

class MicrophoneFactory final : public RealMicFactory {
public:
    std::unique_ptr<Interface> Create(std::string) override { return std::make_unique<DeviceMic>(); }
};
} // namespace

namespace Cara {
void RegisterIOSMicrophone() {
    RegisterRealMicFactory(std::make_unique<MicrophoneFactory>());
    for (NSNotificationName name in @[AVAudioSessionRouteChangeNotification,
                                      AVAudioSessionInterruptionNotification,
                                      AVAudioSessionMediaServicesWereResetNotification]) {
        [NSNotificationCenter.defaultCenter addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification* note) {
                std::shared_ptr<Microphone> current;
                {
                    std::lock_guard lock(microphone_mutex);
                    current=current_mic.lock();
                }
                if (!current) return;
                if ([note.name isEqualToString:AVAudioSessionInterruptionNotification]) {
                    NSNumber* type=note.userInfo[AVAudioSessionInterruptionTypeKey];
                    if (!type) return;
                    const bool began=type.unsignedIntegerValue==AVAudioSessionInterruptionTypeBegan;
                    dispatch_async(current->control,^{
                        current->interrupted=began;
                        current->Reconcile();
                    });
                } else if ([note.name isEqualToString:AVAudioSessionMediaServicesWereResetNotification]) {
                    dispatch_async(current->control,^{
                        current->Stop();
                        current->Reconcile();
                    });
                } else {
                    current->Refresh();
                }
            }];
    }
}
void SetIOSMicrophoneForeground(bool active) {
    foreground=active;
    std::shared_ptr<Microphone> current;
    {
        std::lock_guard lock(microphone_mutex);
        current=current_mic.lock();
    }
    if (current) dispatch_async(current->control,^{
        if (active) current->interrupted=false;
        current->Reconcile();
    });
}
} // namespace Cara
