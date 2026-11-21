// SPDX-License-Identifier: GPL-2.0-or-later
#import "ios/device_sensors.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMotion/CoreMotion.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <numbers>
#include "core/frontend/camera/factory.h"
#include "core/frontend/input.h"

namespace {
using namespace Service::CAM;
using Common::Vec3;

std::atomic<bool> foreground{false};
std::atomic<UIInterfaceOrientation> orientation{UIInterfaceOrientationPortrait};
std::atomic<unsigned> motion_clients{0};

struct MotionReading {
    Vec3<float> acceleration{0, 0, -1};
    Vec3<float> gyro{};
};
std::mutex motion_mutex;
MotionReading reading;
CMMotionManager* motion_manager;
NSOperationQueue* motion_queue;
std::atomic<unsigned> motion_generation{0};

Vec3<float> GuestAxes(Vec3<float> value, UIInterfaceOrientation direction) {
    switch (direction) {
    case UIInterfaceOrientationLandscapeLeft: return {value.y, value.z, value.x};
    case UIInterfaceOrientationLandscapeRight: return {-value.y, value.z, -value.x};
    case UIInterfaceOrientationPortraitUpsideDown: return {value.x, value.z, -value.y};
    default: return {-value.x, value.z, value.y};
    }
}

void RefreshMotion() {
    if (!foreground.load() || !motion_clients.load()) {
        if (motion_manager.accelerometerActive || motion_manager.gyroActive) {
            ++motion_generation;
            [motion_manager stopAccelerometerUpdates];
            [motion_manager stopGyroUpdates];
            std::lock_guard lock(motion_mutex);
            reading = {};
        }
        return;
    }
    if (!motion_manager) {
        motion_manager=[CMMotionManager new];
        motion_queue=[NSOperationQueue new];
        motion_queue.maxConcurrentOperationCount=1;
        motion_manager.accelerometerUpdateInterval=1.0/100;
        motion_manager.gyroUpdateInterval=1.0/100;
    }
    const auto generation=motion_generation.load();
    if (motion_manager.accelerometerAvailable && !motion_manager.accelerometerActive) {
        [motion_manager startAccelerometerUpdatesToQueue:motion_queue withHandler:^(CMAccelerometerData* data, NSError*) {
            if (!data || generation!=motion_generation.load() || !foreground.load()) return;
            const auto xyz=data.acceleration;
            std::lock_guard lock(motion_mutex);
            if (generation==motion_generation.load()) reading.acceleration={float(xyz.x),float(xyz.y),float(xyz.z)};
        }];
    }
    if (motion_manager.gyroAvailable && !motion_manager.gyroActive) {
        [motion_manager startGyroUpdatesToQueue:motion_queue withHandler:^(CMGyroData* data, NSError*) {
            if (!data || generation!=motion_generation.load() || !foreground.load()) return;
            const auto xyz=data.rotationRate;
            constexpr auto degrees=180/std::numbers::pi_v<float>;
            std::lock_guard lock(motion_mutex);
            if (generation==motion_generation.load())
                reading.gyro=Vec3<float>{float(xyz.x),float(xyz.y),float(xyz.z)}*degrees;
        }];
    }
}

class DeviceMotion final : public Input::MotionDevice {
public:
    ~DeviceMotion() override {
        if (requested.exchange(false) && --motion_clients==0)
            dispatch_async(dispatch_get_main_queue(), ^{ RefreshMotion(); });
    }
    std::tuple<Vec3<float>,Vec3<float>> GetStatus() const override {
        if (!requested.exchange(true) && ++motion_clients==1)
            dispatch_async(dispatch_get_main_queue(), ^{ RefreshMotion(); });
        if (!foreground.load()) return {{0,-1,0},{}};
        std::lock_guard lock(motion_mutex);
        const auto direction=orientation.load();
        // The default is flat with the touchscreen facing up.
        const auto acceleration=reading.acceleration;
        return {GuestAxes(acceleration,direction),GuestAxes(reading.gyro,direction)};
    }
private:
    mutable std::atomic<bool> requested{false};
};
class MotionFactory final : public Input::Factory<Input::MotionDevice> {
public:
    std::unique_ptr<Input::MotionDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<DeviceMotion>();
    }
};
} // namespace

@interface CaraCameraFeed : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
- (instancetype)initWithPosition:(AVCaptureDevicePosition)position;
- (void)addClient;
- (void)removeClient;
- (void)refresh;
- (CVPixelBufferRef)copyFrame CF_RETURNS_RETAINED;
@end

@implementation CaraCameraFeed {
    AVCaptureDevicePosition position;
    AVCaptureSession* capture;
    AVCaptureConnection* captureConnection;
    dispatch_queue_t sessionQueue;
    dispatch_queue_t framesQueue;
    std::atomic<unsigned> subscribers;
    BOOL requestingPermission;
    std::mutex frameLock;
    CVPixelBufferRef latest;
}
- (instancetype)initWithPosition:(AVCaptureDevicePosition)value {
    if ((self=[super init])) {
        position=value;
        sessionQueue=dispatch_queue_create("com.dmikey.cara.camera.session",DISPATCH_QUEUE_SERIAL);
        framesQueue=dispatch_queue_create("com.dmikey.cara.camera.frames",DISPATCH_QUEUE_SERIAL);
    }
    return self;
}
- (void)addClient { ++subscribers; [self refresh]; }
- (void)removeClient { --subscribers; [self refresh]; }
- (void)clearFrame {
    std::lock_guard lock(frameLock);
    if (latest) CVPixelBufferRelease(latest);
    latest=nullptr;
}
- (CVPixelBufferRef)copyFrame {
    std::lock_guard lock(frameLock);
    return latest && foreground.load() ? CVPixelBufferRetain(latest) : nullptr;
}
- (void)refresh {
    dispatch_async(sessionQueue,^{
        if (!foreground.load() || !subscribers.load()) {
            [self->capture stopRunning];
            dispatch_sync(self->framesQueue,^{ [self clearFrame]; });
            return;
        }
        const auto permission=[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
        if (permission==AVAuthorizationStatusNotDetermined && !self->requestingPermission) {
            self->requestingPermission=YES;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!foreground.load() || !self->subscribers.load()) {
                    dispatch_async(self->sessionQueue,^{ self->requestingPermission=NO; });
                    return;
                }
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL) {
                    dispatch_async(self->sessionQueue,^{ self->requestingPermission=NO; [self refresh]; });
                }];
            });
        }
        if (permission!=AVAuthorizationStatusAuthorized) return;
        if (!self->capture) {
            AVCaptureDevice* device=[AVCaptureDevice defaultDeviceWithDeviceType:AVCaptureDeviceTypeBuiltInWideAngleCamera
                mediaType:AVMediaTypeVideo position:self->position];
            if (!device) return;
            AVCaptureDeviceInput* input=[AVCaptureDeviceInput deviceInputWithDevice:device error:nil];
            if (!input) return;
            AVCaptureSession* session=[AVCaptureSession new];
            if ([session canSetSessionPreset:AVCaptureSessionPreset640x480])
                session.sessionPreset=AVCaptureSessionPreset640x480;
            AVCaptureVideoDataOutput* output=[AVCaptureVideoDataOutput new];
            output.videoSettings=@{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)};
            output.alwaysDiscardsLateVideoFrames=YES;
            if (![session canAddInput:input] || ![session canAddOutput:output]) return;
            [session addInput:input];
            [session addOutput:output];
            [output setSampleBufferDelegate:self queue:self->framesQueue];
            self->capture=session;
            self->captureConnection=[output connectionWithMediaType:AVMediaTypeVideo];
            if ([self->captureConnection isVideoMirroringSupported]) {
                self->captureConnection.automaticallyAdjustsVideoMirroring=NO;
                self->captureConnection.videoMirrored=NO;
            }
        }
        if ([self->captureConnection isVideoOrientationSupported]) {
            AVCaptureVideoOrientation direction=AVCaptureVideoOrientationPortrait;
            switch (orientation.load()) {
            case UIInterfaceOrientationLandscapeLeft: direction=AVCaptureVideoOrientationLandscapeLeft; break;
            case UIInterfaceOrientationLandscapeRight: direction=AVCaptureVideoOrientationLandscapeRight; break;
            case UIInterfaceOrientationPortraitUpsideDown: direction=AVCaptureVideoOrientationPortraitUpsideDown; break;
            default: break;
            }
            if (self->captureConnection.videoOrientation!=direction)
                self->captureConnection.videoOrientation=direction;
        }
        if (!self->capture.running) [self->capture startRunning];
    });
}
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sample
       fromConnection:(AVCaptureConnection*)connection {
    if (!foreground.load() || !subscribers.load()) return;
    CVPixelBufferRef frame=CMSampleBufferGetImageBuffer(sample);
    if (!frame) return;
    {
        std::lock_guard lock(frameLock);
        if (latest) CVPixelBufferRelease(latest);
        latest=CVPixelBufferRetain(frame);
    }
}
- (void)dealloc { if (latest) CVPixelBufferRelease(latest); }
@end

namespace {
CaraCameraFeed* CameraFeed(bool front) {
    static CaraCameraFeed* inner=[[CaraCameraFeed alloc] initWithPosition:AVCaptureDevicePositionFront];
    static CaraCameraFeed* outer=[[CaraCameraFeed alloc] initWithPosition:AVCaptureDevicePositionBack];
    return front?inner:outer;
}

class DeviceCamera final : public Camera::CameraInterface {
public:
    DeviceCamera(bool front, Flip initial_flip) : feed(CameraFeed(front)), flip(initial_flip) {}
    ~DeviceCamera() override { StopCapture(); }
    void StartCapture() override {
        if (!capturing.exchange(true)) [feed addClient];
    }
    void StopCapture() override {
        if (capturing.exchange(false)) [feed removeClient];
    }
    void SetResolution(const Resolution& size) override {
        std::lock_guard lock(settings);
        width=size.width;
        height=size.height;
    }
    void SetFlip(Flip value) override {
        std::lock_guard lock(settings);
        flip=value;
    }
    void SetEffect(Effect value) override {
        std::lock_guard lock(settings);
        effect=value;
    }
    void SetFormat(OutputFormat value) override {
        std::lock_guard lock(settings);
        format=value;
    }
    void SetFrameRate(FrameRate) override {}
    bool IsPreviewAvailable() override {
        auto frame=[feed copyFrame];
        if (!frame) return false;
        CVPixelBufferRelease(frame);
        return true;
    }

    std::vector<u16> ReceiveFrame() override {
        unsigned w,h;
        Flip image_flip;
        Effect image_effect;
        bool rgb;
        {
            std::lock_guard lock(settings);
            w=width;
            h=height;
            image_flip=flip;
            image_effect=effect;
            rgb=format==OutputFormat::RGB565;
        }
        if (!w || !h || w>640 || h>480) return {};
        std::vector<u16> output(w*h,rgb?0:0x8000);
        auto frame=capturing.load()?[feed copyFrame]:nullptr;
        if (!frame) return output;
        if (CVPixelBufferLockBaseAddress(frame,kCVPixelBufferLock_ReadOnly)==kCVReturnSuccess) {
            struct LockedFrame {
                CVPixelBufferRef buffer;
                ~LockedFrame() { CVPixelBufferUnlockBaseAddress(buffer,kCVPixelBufferLock_ReadOnly); }
            } lock{frame};
            const auto* pixels=static_cast<const u8*>(CVPixelBufferGetBaseAddress(frame));
            const size_t stride=CVPixelBufferGetBytesPerRow(frame);
            const size_t input_w=CVPixelBufferGetWidth(frame),input_h=CVPixelBufferGetHeight(frame);
            if (pixels && input_w && input_h && stride>=input_w*4) {
                const size_t crop_w=std::max<size_t>(1,std::min(input_w,input_h*w/h));
                const size_t crop_h=std::max<size_t>(1,std::min(input_h,input_w*h/w));
                const bool mirror_x=(static_cast<u8>(image_flip)&1)!=0;
                const bool mirror_y=(static_cast<u8>(image_flip)&2)!=0;
                const auto pixel=[&](unsigned x,unsigned y) {
                    const size_t in_x=(mirror_x?w-1-x:x)*crop_w/w+(input_w-crop_w)/2;
                    const size_t in_y=(mirror_y?h-1-y:y)*crop_h/h+(input_h-crop_h)/2;
                    const u8* bgra=pixels+in_y*stride+in_x*4;
                    std::array color{int(bgra[2]),int(bgra[1]),int(bgra[0])};
                    const int gray=(77*color[0]+150*color[1]+29*color[2])>>8;
                    if (image_effect==Effect::Mono) color.fill(gray);
                    else if (image_effect==Effect::Negative)
                        for (auto& channel : color) channel=255-channel;
                    else if (image_effect==Effect::Negafilm) color.fill(255-gray);
                    else if (image_effect==Effect::Sepia || image_effect==Effect::Sepia01)
                        color={std::min(gray+32,255),gray,std::max(gray-24,0)};
                    return color;
                };
                const auto yuv=[&](unsigned x,unsigned y) {
                    const auto [r,g,b]=pixel(x,y);
                    return std::array{(66*r+129*g+25*b+128)/256+16,
                        std::clamp((-38*r-74*g+112*b+32768+128)/256,0,255),
                        std::clamp((112*r-94*g-18*b+32768+128)/256,0,255)};
                };
                for (unsigned y=0;y<h;++y) {
                    for (unsigned x=0;x<w;++x) {
                        const auto offset=y*w+x;
                        if (rgb) {
                            const auto [r,g,b]=pixel(x,y);
                            output[offset]=u16(((r>>3)<<11)|((g>>2)<<5)|(b>>3));
                        } else if (x+1<w) {
                            const auto [y0,u0,v0]=yuv(x,y);
                            if ((x&1)==0) {
                                const auto [y1,u1,v1]=yuv(x+1,y);
                                output[offset]=u16(y0|(((u0+u1)/2)<<8));
                                output[offset+1]=u16(y1|(((v0+v1)/2)<<8));
                                ++x;
                            }
                        }
                    }
                }
            }
        }
        CVPixelBufferRelease(frame);
        return output;
    }

private:
    CaraCameraFeed* feed;
    std::atomic<bool> capturing{false};
    std::mutex settings;
    unsigned width=640,height=480;
    Flip flip;
    Effect effect=Effect::None;
    OutputFormat format=OutputFormat::YUV422;
};

class CameraFactory final : public Camera::CameraFactory {
public:
    std::unique_ptr<Camera::CameraInterface> Create(const std::string& config,
                                                    const Flip& flip) override {
        return std::make_unique<DeviceCamera>(config=="front",flip);
    }
};
} // namespace

namespace Cara {
void RegisterIOSDeviceSensors() {
    Input::RegisterFactory<Input::MotionDevice>("ios",std::make_shared<MotionFactory>());
    Camera::RegisterFactory("ios",std::make_unique<CameraFactory>());
    RegisterIOSMicrophone();
}

void SetIOSDeviceSensorContext(bool active, UIInterfaceOrientation direction) {
    const auto old_direction=direction==UIInterfaceOrientationUnknown?orientation.load():orientation.exchange(direction);
    const bool changed=foreground.exchange(active)!=active;
    if (!changed && (direction==old_direction || direction==UIInterfaceOrientationUnknown)) return;
    RefreshMotion();
    [CameraFeed(true) refresh];
    [CameraFeed(false) refresh];
    if (changed) SetIOSMicrophoneForeground(active);
}
} // namespace Cara
