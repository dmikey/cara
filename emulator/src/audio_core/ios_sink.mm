// SPDX-License-Identifier: GPL-2.0-or-later
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <UIKit/UIKit.h>
#import <dispatch/dispatch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include "audio_core/audio_types.h"
#include "audio_core/ios_sink.h"
#include "common/logging/log.h"

namespace AudioCore {
namespace {
constexpr unsigned buffer_frames = 512;
constexpr unsigned buffer_bytes = buffer_frames * 2 * sizeof(s16);
std::atomic<bool> muted{false};
std::atomic<bool> playback_paused{false};
struct Playback;
std::mutex active_mutex;
std::weak_ptr<Playback> active_playback;

struct Playback {
    std::mutex control;
    AudioQueueRef queue = nullptr;
    dispatch_queue_t callback_queue=dispatch_queue_create("com.dmikey.cara.audio",DISPATCH_QUEUE_SERIAL);
    std::array<AudioQueueBufferRef,3> buffers{};
    std::function<void(s16*,std::size_t)> callback;
    std::atomic<bool> stopping{false};
    bool background=false, interrupted=false, manually_paused=false, playing=false, primed=false;
    const bool diagnostics=std::getenv("CARA_AUDIO_DIAGNOSTICS")!=nullptr;
    std::atomic<int> error{0};
    std::atomic<uint64_t> callbacks{0}, frames_submitted{0}, nonzero_frames{0}, changing_frames{0};
    std::atomic<uint64_t> fill_nanoseconds{0}, max_fill_nanoseconds{0};
    s16 last_left=0,last_right=0;
    std::atomic<unsigned> peak{0};
    std::atomic<u64> queue_generations{0};

    static void Output(void* context,AudioQueueRef,AudioQueueBufferRef buffer) {
        auto* self=static_cast<Playback*>(context);
        if (self->stopping.load(std::memory_order_acquire)) return;
        if (self->diagnostics) self->callbacks.fetch_add(1,std::memory_order_relaxed);
        self->Fill(buffer);
    }
    void Fill(AudioQueueBufferRef buffer) noexcept {
        if (stopping.load(std::memory_order_acquire)) return;
        auto* output=static_cast<s16*>(buffer->mAudioData);
        const auto fill_started=diagnostics?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        try {
            if (callback) callback(output,buffer_frames);
            else std::fill_n(output,buffer_frames*2,0);
        } catch (...) {
            std::fill_n(output,buffer_frames*2,0); error.store(-1);
        }
        if (diagnostics) {
            const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-fill_started).count());
            fill_nanoseconds.fetch_add(ns,std::memory_order_relaxed);
            auto old=max_fill_nanoseconds.load();
            while(ns>old && !max_fill_nanoseconds.compare_exchange_weak(old,ns)) {}
        }
        if (muted.load(std::memory_order_relaxed)) std::fill_n(output,buffer_frames*2,0);
        if (diagnostics) {
            uint64_t nonzero=0,changing=0; unsigned maximum=0;
            for (unsigned i=0;i<buffer_frames;++i) {
                const int left=output[i*2],right=output[i*2+1];
                nonzero+=(left!=0 || right!=0);
                changing+=(left!=last_left || right!=last_right);
                last_left=s16(left); last_right=s16(right);
                maximum=std::max(maximum,unsigned(std::max(std::abs(left),std::abs(right))));
            }
            frames_submitted.fetch_add(buffer_frames,std::memory_order_relaxed);
            nonzero_frames.fetch_add(nonzero,std::memory_order_relaxed);
            changing_frames.fetch_add(changing,std::memory_order_relaxed);
            auto previous=peak.load(std::memory_order_relaxed);
            while (maximum>previous && !peak.compare_exchange_weak(previous,maximum,std::memory_order_relaxed)) {}
        }
        buffer->mAudioDataByteSize=buffer_bytes;
        if (!stopping.load(std::memory_order_acquire)) {
            const OSStatus status=AudioQueueEnqueueBuffer(queue,buffer,0,nullptr);
            if (status && !stopping.load(std::memory_order_acquire)) error.store(status);
        }
    }
    bool CreateLocked() {
        if (queue) return true;
        AudioStreamBasicDescription format{};
        // DSP samples stay at their native rate; Audio Queue converts to the device rate.
        format.mSampleRate=native_sample_rate;
        format.mFormatID=kAudioFormatLinearPCM;
        format.mFormatFlags=kLinearPCMFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked|kAudioFormatFlagsNativeEndian;
        format.mBytesPerPacket=format.mBytesPerFrame=2*sizeof(s16);
        format.mFramesPerPacket=1; format.mChannelsPerFrame=2; format.mBitsPerChannel=16;
        OSStatus status=AudioQueueNewOutputWithDispatchQueue(&queue,&format,0,callback_queue,
            ^(AudioQueueRef output,AudioQueueBufferRef buffer) { Playback::Output(this,output,buffer); });
        if (!status) for (auto& buffer:buffers) {
            status=AudioQueueAllocateBuffer(queue,buffer_bytes,&buffer);
            if (status) break;
        }
        if (status) {
            error.store(status);
            if (queue) AudioQueueDispose(queue,true);
            queue=nullptr; buffers.fill(nullptr);
            LOG_ERROR(Audio,"Cara iOS audio queue initialization failed: {}",status);
            return false;
        }
        AudioQueueSetParameter(queue,kAudioQueueParam_Volume,muted.load()?0.0f:1.0f);
        queue_generations.fetch_add(1,std::memory_order_relaxed);
        return true;
    }
    bool ActivateSessionLocked() {
        @autoreleasepool {
            AVAudioSession* session=AVAudioSession.sharedInstance;
            NSError* failure=nil;
            if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&failure] ||
                ![session setActive:YES error:&failure]) {
                error.store(failure?int(failure.code):-2);
                LOG_ERROR(Audio,"Cara iOS audio session: {}",failure?failure.localizedDescription.UTF8String:"activation failed");
                return false;
            }
        }
        return true;
    }
    void ResumeLocked() {
        if (stopping.load() || background || interrupted || manually_paused || !callback || playing) return;
        if (!ActivateSessionLocked() || !CreateLocked()) return;
        if (!primed) {
            dispatch_sync(callback_queue,^{ for (auto buffer:buffers) Fill(buffer); });
            primed=true;
        }
        const OSStatus status=AudioQueueStart(queue,nullptr);
        error.store(status); playing=(status==noErr);
        if (status) LOG_ERROR(Audio,"Cara iOS audio start failed: {}",status);
    }
    void PauseLocked() {
        if (queue && playing) AudioQueuePause(queue);
        playing=false;
    }
    void DiscardQueueLocked() {
        stopping.store(true,std::memory_order_release);
        if (queue) {
            AudioQueueStop(queue,true);
            dispatch_sync(callback_queue,^{});
            AudioQueueDispose(queue,true);
        }
        queue=nullptr; buffers.fill(nullptr); playing=false; primed=false;
        stopping.store(false,std::memory_order_release);
    }
    void ResetService() {
        std::lock_guard<std::mutex> lock(control);
        if (stopping.load()) return;
        DiscardQueueLocked();
        ResumeLocked();
    }
    void SetCallback(std::function<void(s16*,std::size_t)> next) {
        std::lock_guard<std::mutex> lock(control);
        DiscardQueueLocked(); callback=std::move(next);
        ResumeLocked();
    }
    void ApplyMute() {
        std::lock_guard<std::mutex> lock(control);
        if (queue) AudioQueueSetParameter(queue,kAudioQueueParam_Volume,muted.load()?0.0f:1.0f);
    }
    void SetPaused(bool pause) {
        std::lock_guard<std::mutex> lock(control); manually_paused=pause;
        if (pause) PauseLocked(); else ResumeLocked();
    }
    void SetBackground(bool value) {
        std::lock_guard<std::mutex> lock(control);
        if (stopping.load()) return;
        const bool returning=background && !value;
        background=value;
        if (value) DiscardQueueLocked();
        else {
            // A suspension can consume the interruption-ended notification. An
            // explicit foreground return should attempt a fresh audio session.
            if (returning) interrupted=false;
            ResumeLocked();
        }
    }
    void SetInterruption(bool began) {
        std::lock_guard<std::mutex> lock(control);
        if (stopping.load()) return;
        interrupted=began;
        if (began) DiscardQueueLocked();
        // Games may resume when an interruption ends without the optional
        // shouldResume hint. Background and manual-pause gates still apply.
        else ResumeLocked();
    }
    void Shutdown() {
        std::lock_guard<std::mutex> lock(control);
        if (stopping.exchange(true,std::memory_order_acq_rel)) return;
        if (queue) { AudioQueueStop(queue,true); AudioQueueDispose(queue,true); dispatch_sync(callback_queue,^{}); }
        queue=nullptr; buffers.fill(nullptr); callback={}; playing=false; primed=false;
        bool another_sink=false;
        { std::lock_guard<std::mutex> active_lock(active_mutex);
          if (auto other=active_playback.lock()) another_sink=(other.get()!=this); }
        if (!another_sink) @autoreleasepool {
            [AVAudioSession.sharedInstance setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:nil];
        }
    }
    ~Playback() { Shutdown(); }
};
std::shared_ptr<Playback> Active() {
    std::lock_guard<std::mutex> lock(active_mutex); return active_playback.lock();
}
} // namespace

class IOSAudioSink::Impl {
public:
    std::shared_ptr<Playback> state=std::make_shared<Playback>();
    id inactive=nil,active=nil,interruption=nil,reset=nil;
    Impl() {
        { std::lock_guard<std::mutex> lock(active_mutex); state->manually_paused=playback_paused.load(); active_playback=state; }
        std::weak_ptr<Playback> weak=state;
        NSNotificationCenter* center=NSNotificationCenter.defaultCenter;
        inactive=[center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
            if (auto owner=weak.lock()) owner->SetBackground(true);
        }];
        active=[center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
            if (auto owner=weak.lock()) owner->SetBackground(false);
        }];
        interruption=[center addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification* note) {
            NSNumber* type=note.userInfo[AVAudioSessionInterruptionTypeKey]; if (!type) return;
            const bool began=type.unsignedIntegerValue==AVAudioSessionInterruptionTypeBegan;
            if (auto owner=weak.lock()) owner->SetInterruption(began);
        }];
        reset=[center addObserverForName:AVAudioSessionMediaServicesWereResetNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
            if (auto owner=weak.lock()) owner->ResetService();
        }];
    }
    ~Impl() {
        NSNotificationCenter* center=NSNotificationCenter.defaultCenter;
        for (id token in @[inactive,active,interruption,reset]) [center removeObserver:token];
        { std::lock_guard<std::mutex> lock(active_mutex); if (active_playback.lock()==state) active_playback.reset(); }
        state->Shutdown();
    }
};
IOSAudioSink::IOSAudioSink(std::string_view) : impl(std::make_unique<Impl>()) {}
IOSAudioSink::~IOSAudioSink()=default;
unsigned int IOSAudioSink::GetNativeSampleRate() const { return native_sample_rate; }
void IOSAudioSink::SetCallback(std::function<void(s16*,std::size_t)> cb) { impl->state->SetCallback(std::move(cb)); }
void SetIOSAudioPaused(bool paused) { playback_paused.store(paused); if (auto state=Active()) state->SetPaused(paused); }
void SetIOSAudioMuted(bool value) { muted.store(value,std::memory_order_relaxed); if (auto state=Active()) state->ApplyMute(); }
bool IsIOSAudioMuted() { return muted.load(std::memory_order_relaxed); }
bool IOSAudioOutputFailed() { if (auto state=Active()) return state->error.load()!=0; return false; }
void WriteIOSAudioDiagnostics(const std::string& path, u64 requested, u64 supplied, u64 underruns) {
    auto state=Active(); if (!state) return;
    UInt32 running=0,size=sizeof(running);
    bool background=false,interrupted=false,manual=false;
    { std::lock_guard<std::mutex> lock(state->control);
      if (state->queue) AudioQueueGetProperty(state->queue,kAudioQueueProperty_IsRunning,&running,&size);
      background=state->background; interrupted=state->interrupted; manual=state->manually_paused; }
    @autoreleasepool {
        AVAudioSession* session=AVAudioSession.sharedInstance;
        NSString* route=session.currentRoute.outputs.firstObject.portType?:@"";
        NSDictionary* report=@{@"backend":@"ios_audio_queue",@"dsp_requested_frames":@(requested),
            @"dsp_supplied_frames":@(supplied),@"dsp_underrun_callbacks":@(underruns),
            @"dsp_fill_total_ms":@(double(state->fill_nanoseconds.load())/1e6),
            @"dsp_fill_max_ms":@(double(state->max_fill_nanoseconds.load())/1e6),@"sample_rate":@(native_sample_rate),
            @"channels":@2,@"frames_per_buffer":@(buffer_frames),@"buffers":@3,@"queue_running":@(running!=0),
            @"background":@(background),@"interrupted":@(interrupted),@"manually_paused":@(manual),
            @"queue_generations":@(state->queue_generations.load()),
            @"muted":@(IsIOSAudioMuted()),@"callbacks":@(state->callbacks.load()),
            @"frames_submitted":@(state->frames_submitted.load()),@"nonzero_frames":@(state->nonzero_frames.load()),
            @"changing_frames":@(state->changing_frames.load()),@"peak_pcm":@(state->peak.load()),
            @"last_error":@(state->error.load()),@"hardware_sample_rate":@(session.sampleRate),
            @"output_volume":@(session.outputVolume),@"output_port_type":route,
            @"uptime_seconds":@(NSProcessInfo.processInfo.systemUptime)};
        NSData* data=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
        [data writeToFile:[NSString stringWithUTF8String:path.c_str()] options:NSDataWritingAtomic error:nil];
    }
}
} // namespace AudioCore
