// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#ifdef CITRA_IOS_GLES
#import <OpenGLES/EAGL.h>
#include <dlfcn.h>
#include <glad/glad.h>
#include "video_core/renderer_opengl/gl_state.h"
#endif
#include <atomic>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <thread>
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/input.h"
#include "core/settings.h"

static NSString* GraphicsLabel() {
#ifdef CITRA_IOS_GLES
    return @"GPU GLES3";
#else
    return @"software graphics";
#endif
}
static std::atomic<uint32_t> buttons{0};
static std::atomic<bool> paused{false};
class Button final : public Input::ButtonDevice {
    unsigned index;
public:
    explicit Button(unsigned i) : index(i) {}
    bool GetStatus() const override { return (buttons.load() & (1u << index)) != 0; }
};
class ButtonFactory final : public Input::Factory<Input::ButtonDevice> {
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& p) override {
        return std::make_unique<Button>(p.Get("code", 0));
    }
};
class Stick final : public Input::AnalogDevice {
    std::tuple<float, float> GetStatus() const override {
        auto b = buttons.load();
        return {float(bool(b & (1u << Settings::NativeButton::Right))) - float(bool(b & (1u << Settings::NativeButton::Left))),
                float(bool(b & (1u << Settings::NativeButton::Up))) - float(bool(b & (1u << Settings::NativeButton::Down)))};
    }
};
class StickFactory final : public Input::Factory<Input::AnalogDevice> {
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage&) override {
        return std::make_unique<Stick>();
    }
};
class IOSWindow final : public Frontend::EmuWindow {
public:
    std::function<void(unsigned, unsigned, unsigned, const std::vector<u8>&, unsigned)> present;
    unsigned frames = 0;
    std::string graphics_error;
#ifdef CITRA_IOS_GLES
    EAGLContext* __strong graphics_context = nil;
    GLuint framebuffer = 0;
    GLuint renderbuffer = 0;
#endif
    std::atomic<int> touch{-1};
    IOSWindow() { UpdateFramebufferLayout(400, 480); }
    void PollEvents() override {
        const int value = touch.load();
        if (value < 0) TouchReleased();
        else TouchPressed((value & 0xffff) + 40, (value >> 16) + 240);
    }
    void MakeCurrent() override {
#ifdef CITRA_IOS_GLES
        if (!graphics_context) graphics_context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
        if (!graphics_context || ![EAGLContext setCurrentContext:graphics_context]) {
            graphics_error = "Unable to create an iOS GLES3 context"; return;
        }
        if (!framebuffer) {
            static void* library = dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_LAZY | RTLD_LOCAL);
            if (!gladLoadGLES2Loader([](const char* name) { return dlsym(library, name); })) {
                graphics_error = "Unable to load iOS GLES3 functions"; return;
            }
            glGenFramebuffers(1, &framebuffer);
            glGenRenderbuffers(1, &renderbuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 400, 480);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                graphics_error = "Unable to create iOS GPU presentation framebuffer";
            std::ofstream capabilities(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "gpu-capabilities.txt");
            capabilities << "vendor=" << reinterpret_cast<const char*>(glGetString(GL_VENDOR))
                         << "\nrenderer=" << reinterpret_cast<const char*>(glGetString(GL_RENDERER))
                         << "\nversion=" << reinterpret_cast<const char*>(glGetString(GL_VERSION)) << "\n";
        }
#endif
    }
    void DoneCurrent() override {
#ifdef CITRA_IOS_GLES
        [EAGLContext setCurrentContext:nil];
#endif
    }
    unsigned GetDefaultFramebuffer() const override {
#ifdef CITRA_IOS_GLES
        return framebuffer;
#else
        return 0;
#endif
    }
    void SwapBuffers() override {
#ifdef CITRA_IOS_GLES
        const auto previous = OpenGL::OpenGLState::BindReadFramebuffer(framebuffer);
        std::vector<u8> image(400 * 480 * 4);
        glReadPixels(0, 0, 400, 480, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
        OpenGL::OpenGLState::BindReadFramebuffer(previous);
        for (unsigned screen = 0; screen < 2; ++screen) {
            const unsigned width = screen == 0 ? 400 : 320;
            const unsigned x = screen == 0 ? 0 : 40;
            const unsigned top = screen == 0 ? 0 : 240;
            std::vector<u8> pixels(width * 240 * 4);
            for (unsigned y = 0; y < 240; ++y)
                std::copy_n(image.data() + ((479 - top - y) * 400 + x) * 4, width * 4, pixels.data() + y * width * 4);
            PresentSoftwareFrame(screen, width, 240, pixels);
        }
#endif
    }
    void PresentSoftwareFrame(unsigned s, unsigned w, unsigned h, const std::vector<u8>& p) override {
        if (s == 0) ++frames;
        if (present) present(s, w, h, p, frames);
    }
};

@interface EmulatorController : UIViewController <UIDocumentPickerDelegate>
@end
@implementation EmulatorController {
    UIImageView* top;
    UIImageView* bottom;
    UILabel* status;
    UIButton* openButton;
    std::unique_ptr<IOSWindow> emuWindow;
    std::thread worker;
    std::atomic<bool> stop;
    std::atomic<bool> running;
    std::atomic<bool> framePending[2];
    unsigned frameCount;
    CFTimeInterval startedAt;
    bool autoStarted;
}
- (UIButton*)button:(NSString*)title code:(NSInteger)code {
    UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
    UIButtonConfiguration* c = [UIButtonConfiguration filledButtonConfiguration];
    c.title = title;
    c.baseBackgroundColor = [UIColor colorWithWhite:0.2 alpha:1];
    c.baseForegroundColor = UIColor.whiteColor;
    b.configuration = c;
    b.tag = code;
    b.multipleTouchEnabled = YES;
    [b addTarget:self action:@selector(press:) forControlEvents:UIControlEventTouchDown];
    [b addTarget:self action:@selector(releaseButton:) forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    return b;
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    NSString* rom = [NSUserDefaults.standardUserDefaults stringForKey:@"rom"];
    if (!autoStarted && rom.length && !running) {
        autoStarted = true;
        NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
        NSURL* url = [NSURL fileURLWithPath:[documents stringByAppendingPathComponent:rom.lastPathComponent]];
        if ([NSFileManager.defaultManager fileExistsAtPath:url.path]) [self startROM:url];
        else status.text = @"Test ROM is not present in Documents.";
    }
}
- (void)press:(UIButton*)b { buttons.fetch_or(1u << b.tag); }
- (void)releaseButton:(UIButton*)b { buttons.fetch_and(~(1u << b.tag)); }
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor colorWithWhite:0.06 alpha:1];
    stop = false; running = false; framePending[0] = false; framePending[1] = false;
    UILabel* title = [UILabel new]; title.text = @"Cara · No JIT";
    title.textColor = UIColor.whiteColor;
    title.font = [UIFont boldSystemFontOfSize:24];
    status = [UILabel new]; status.text = [NSString stringWithFormat:@"Interpreter test · %@ · audio off", GraphicsLabel()];
    status.font = [UIFont systemFontOfSize:13]; status.textColor = UIColor.lightGrayColor;
    status.numberOfLines = 2;
    top = [UIImageView new]; bottom = [UIImageView new];
    top.backgroundColor = bottom.backgroundColor = UIColor.blackColor;
    top.contentMode = bottom.contentMode = UIViewContentModeScaleAspectFit;
    top.translatesAutoresizingMaskIntoConstraints = NO;
    bottom.translatesAutoresizingMaskIntoConstraints = NO;
    [top.heightAnchor constraintEqualToAnchor:top.widthAnchor multiplier:240.0/400].active = YES;
    [bottom.heightAnchor constraintEqualToAnchor:bottom.widthAnchor multiplier:240.0/320].active = YES;
    bottom.userInteractionEnabled = YES;
    UIPanGestureRecognizer* pan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(touchScreen:)];
    pan.minimumNumberOfTouches = 1; pan.maximumNumberOfTouches = 1;
    [bottom addGestureRecognizer:pan];
    UITapGestureRecognizer* tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(touchScreen:)];
    [bottom addGestureRecognizer:tap];
    openButton = [UIButton buttonWithType:UIButtonTypeSystem];
    openButton.configuration = [UIButtonConfiguration filledButtonConfiguration];
    [openButton setTitle:@"Open .3ds / .cia / .3dsx" forState:UIControlStateNormal];
    [openButton addTarget:self action:@selector(openROM) forControlEvents:UIControlEventTouchUpInside];
    UIStackView* controls = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self button:@"L" code:Settings::NativeButton::L], [self button:@"↑" code:Settings::NativeButton::Up],
        [self button:@"X" code:Settings::NativeButton::X], [self button:@"Y" code:Settings::NativeButton::Y], [self button:@"R" code:Settings::NativeButton::R]]];
    UIStackView* controls2 = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self button:@"←" code:Settings::NativeButton::Left], [self button:@"↓" code:Settings::NativeButton::Down],
        [self button:@"→" code:Settings::NativeButton::Right], [self button:@"B" code:Settings::NativeButton::B], [self button:@"A" code:Settings::NativeButton::A]]];
    UIStackView* controls3 = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self button:@"Select" code:Settings::NativeButton::Select], [self button:@"Start" code:Settings::NativeButton::Start]]];
    for (UIStackView* row in @[controls, controls2, controls3]) { row.spacing = 8; row.distribution = UIStackViewDistributionFillEqually; }
    UIButton* stopButton = [UIButton buttonWithType:UIButtonTypeSystem];
    [stopButton setTitle:@"Stop game" forState:UIControlStateNormal];
    [stopButton addTarget:self action:@selector(stopGame) forControlEvents:UIControlEventTouchUpInside];
    UIStackView* stack = [[UIStackView alloc] initWithArrangedSubviews:@[title,status,openButton,top,bottom,controls,controls2,controls3,stopButton]];
    stack.axis = UILayoutConstraintAxisVertical; stack.spacing = 10;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    UIScrollView* scroll = [UIScrollView new]; scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll]; [scroll addSubview:stack];
    UILayoutGuide* safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint* preferredWidth = [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-24];
    preferredWidth.priority = UILayoutPriorityDefaultHigh;
    preferredWidth.active = YES;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor], [scroll.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:12],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-12],
        [stack.centerXAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.centerXAnchor],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-24],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:400]]];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(background) name:UIApplicationWillResignActiveNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(foreground) name:UIApplicationDidBecomeActiveNotification object:nil];
}
- (void)stopGame { stop = true; buttons = 0; status.text = @"Stopping…"; }
- (void)background { paused = true; buttons = 0; }
- (void)foreground { paused = false; }
- (void)touchScreen:(UIGestureRecognizer*)gesture {
    if (!running || !emuWindow) return;
    CGPoint p = [gesture locationInView:bottom];
    unsigned x = std::max(0.0, std::min(319.0, double(p.x / bottom.bounds.size.width * 320)));
    unsigned y = std::max(0.0, std::min(239.0, double(p.y / bottom.bounds.size.height * 240)));
    if ([gesture isKindOfClass:UITapGestureRecognizer.class]) {
        emuWindow->touch = int(x | (y << 16));
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            if (emuWindow) emuWindow->touch = -1;
        });
    } else if (gesture.state == UIGestureRecognizerStateEnded || gesture.state == UIGestureRecognizerStateCancelled) {
        emuWindow->touch = -1;
    } else {
        emuWindow->touch = int(x | (y << 16));
    }
}
- (void)openROM {
    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:YES];
    picker.delegate = self; [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController*)picker didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* url = urls.firstObject;
    if (!url || running) return;
    if (![ @[@"3ds",@"cci",@"cxi",@"cia",@"3dsx",@"elf"] containsObject:url.pathExtension.lowercaseString]) {
        status.text = @"Extract the ZIP first, then select the game file."; return;
    }
    if (worker.joinable()) worker.join();
    [self startROM:url];
}
- (void)showFrame:(unsigned)screen width:(unsigned)width height:(unsigned)height pixels:(const std::vector<u8>&)pixels frame:(unsigned)emulatedFrame {
    const bool captureCheckpoint = emulatedFrame % 600 == 0 &&
        [NSProcessInfo.processInfo.environment[@"CITRA_CAPTURE_FRAMES"] isEqualToString:@"1"];
    if (framePending[screen].exchange(true) && !captureCheckpoint) return;
    NSData* data = [NSData dataWithBytes:pixels.data() length:pixels.size()];
    __weak EmulatorController* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        EmulatorController* controller = weakSelf;
        if (!controller) return;
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGImageRef image = CGImageCreate(width,height,8,32,width*4,space,CGBitmapInfo(kCGImageAlphaLast) | kCGBitmapByteOrder32Big,provider,nullptr,false,kCGRenderingIntentDefault);
        UIImage* displayImage = [UIImage imageWithCGImage:image];
        (screen == 0 ? controller->top : controller->bottom).image = displayImage;
        if (screen == 0) ++controller->frameCount;
        if (captureCheckpoint) {
            NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
            NSString* filename = [NSString stringWithFormat:@"frame-%u-screen-%u.png",emulatedFrame,screen];
            [UIImagePNGRepresentation(displayImage) writeToFile:[documents stringByAppendingPathComponent:filename] atomically:YES];
        }
        if (controller->frameCount % 60 == 0) {
            double elapsed = CFAbsoluteTimeGetCurrent() - controller->startedAt;
            controller->status.text = [NSString stringWithFormat:@"No JIT · %@ · %.1f FPS average · audio off", GraphicsLabel(), controller->frameCount / std::max(elapsed, 0.001)];
            if ([NSProcessInfo.processInfo.environment[@"CITRA_CAPTURE_FRAMES"] isEqualToString:@"1"]) {
                NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
                [UIImagePNGRepresentation(displayImage) writeToFile:[documents stringByAppendingPathComponent:[NSString stringWithFormat:@"latest-screen-%u.png",screen]] atomically:YES];
                NSString* evidence = [NSString stringWithFormat:@"frames=%u elapsed=%.3f average_fps=%.3f cpu_jit=false shader_jit=false software_renderer=true",controller->frameCount,elapsed,controller->frameCount/std::max(elapsed,0.001)];
                [evidence writeToFile:[documents stringByAppendingPathComponent:@"run-status.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            }
        }
        CGImageRelease(image); CGColorSpaceRelease(space); CGDataProviderRelease(provider);
        controller->framePending[screen] = false;
    });
}
- (void)startROM:(NSURL*)url {
    frameCount = 0; startedAt = CFAbsoluteTimeGetCurrent();
    running = true; stop = false; openButton.enabled = NO;
    status.text = @"Loading with CPU interpreter…";
    std::string rom = url.path.UTF8String;
    NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    std::string user = std::string(documents.UTF8String) + "/citra/";
    __weak EmulatorController* weakSelf = self;
    emuWindow.reset();
    emuWindow = std::make_unique<IOSWindow>();
    emuWindow->present = [weakSelf](unsigned s,unsigned w,unsigned h,const std::vector<u8>& p,unsigned frame) {
        [weakSelf showFrame:s width:w height:h pixels:p frame:frame];
    };
    worker = std::thread([self,rom,user] {
        @autoreleasepool {
            Common::DetachedTasks tasks;
            FileUtil::SetUserPath(user);
            static std::once_flag logging;
            std::call_once(logging,[&] {
                Log::SetGlobalFilter(Log::Filter(Log::Level::Info));
                Log::AddBackend(std::make_unique<Log::FileBackend>(user + "emulator.log"));
                Input::RegisterFactory<Input::ButtonDevice>("ios",std::make_shared<ButtonFactory>());
                Input::RegisterFactory<Input::AnalogDevice>("ios",std::make_shared<StickFactory>());
            });
            auto& s = Settings::values;
            s.use_cpu_jit = false; s.use_shader_jit = false;
            s.use_hw_renderer = false; s.use_hw_shader = false;
#ifdef CITRA_IOS_GLES
            s.use_gles = true;
            s.use_hw_renderer = true;
            s.use_separable_shader = false;
            s.allow_shadow = false;
            s.shadow_rendering = false;
            s.use_shader_cache = false;
#endif
            s.use_virtual_sd = true; s.region_value = Settings::REGION_VALUE_AUTO_SELECT;
            s.resolution_factor = 1; s.frame_limit = 100;
            const char* target_env = std::getenv("CITRA_BENCHMARK_FRAMES");
            const unsigned target_frames = target_env ? static_cast<unsigned>(std::clamp(std::strtol(target_env, nullptr, 10), 1L, 18000L)) : 0;
            s.init_clock = target_frames ? Settings::InitClock::FixedTime : Settings::InitClock::SystemTime;
            s.init_time = 946684801;
            s.sink_id = "null"; s.audio_volume = 1; s.shared_font_type = Settings::SharedFontType::Auto;
            for (unsigned i=0; i<Settings::NativeButton::NumButtons; ++i)
                s.current_input_profile.buttons[i] = "engine:ios,code:" + std::to_string(i);
            s.current_input_profile.analogs[0] = "engine:ios";
            s.current_input_profile.touch_device = "engine:emu_window";
            Settings::Apply(); Frontend::RegisterDefaultApplets();
            auto& system = Core::System::GetInstance();
            FileUtil::CreateFullPath(FileUtil::GetUserPath(FileUtil::UserPath::LogDir));
            emuWindow->MakeCurrent();
            if (!emuWindow->graphics_error.empty()) {
                NSString* message = [NSString stringWithUTF8String:emuWindow->graphics_error.c_str()];
                running = false;
                dispatch_async(dispatch_get_main_queue(), ^{ self->status.text = message; self->openButton.enabled = YES; });
                return;
            }
            if (target_frames) FileUtil::Delete(user + "benchmark-status.json");
            auto result = system.Load(*emuWindow,rom);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (result == Core::System::ResultStatus::Success)
                    self->status.text = [NSString stringWithFormat:@"Running · no JIT · %@ · audio off", GraphicsLabel()];
                else self->status.text = [NSString stringWithFormat:@"Load failed (code %u). See Documents/citra/emulator.log",unsigned(result)];
            });
            if (result == Core::System::ResultStatus::Success) {
                const auto benchmark_start = std::chrono::steady_clock::now();
                auto next_frame_time = benchmark_start;
                unsigned paced_frame = 0;
                bool was_paused = false;
                while (!stop && (!target_frames || emuWindow->frames < target_frames)) {
                    if (paused) { was_paused = true; std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }
                    if (was_paused) { next_frame_time = std::chrono::steady_clock::now(); was_paused = false; }
                    result = system.RunLoop();
                    if (result != Core::System::ResultStatus::Success) break;
                    if (!target_frames && emuWindow->frames != paced_frame) {
                        paced_frame = emuWindow->frames;
                        next_frame_time += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / 59.833997376556916));
                        const auto now = std::chrono::steady_clock::now();
                        if (now - next_frame_time > std::chrono::milliseconds(100)) next_frame_time = now;
                        std::this_thread::sleep_until(next_frame_time);
                    }
                }
                if (target_frames) {
                    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - benchmark_start).count();
                    std::ofstream report(user + "benchmark-status.json");
                    report << "{\"frames\":" << emuWindow->frames << ",\"target_frames\":" << target_frames << ",\"result_code\":" << unsigned(result) << ",\"elapsed_seconds\":" << elapsed
                           << ",\"fps\":" << emuWindow->frames / elapsed
#ifdef CITRA_IOS_GLES
                           << ",\"jit\":false,\"software_renderer\":false,\"gpu_backend\":\"native_gles3\"}\n";
#else
                           << ",\"jit\":false,\"software_renderer\":true}\n";
#endif
                }
                system.Shutdown();
            }
            emuWindow->DoneCurrent();
            running = false;
            dispatch_async(dispatch_get_main_queue(), ^{
                self->openButton.enabled = YES;
                if (result != Core::System::ResultStatus::Success)
                    self->status.text = [NSString stringWithFormat:@"Emulation ended (code %u). See emulator.log",unsigned(result)];
                else self->status.text = @"Stopped · interpreter test · audio off";
            });
        }
    });
}
- (void)dealloc {
    stop = true;
    if (worker.joinable()) worker.join();
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}
@end

@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong,nonatomic) UIWindow* window;
@end
@implementation SceneDelegate
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    if (![scene isKindOfClass:UIWindowScene.class]) return;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = [EmulatorController new];
    [self.window makeKeyAndVisible];
}
@end
@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options { return YES; }
- (UISceneConfiguration*)application:(UIApplication*)app configurationForConnectingSceneSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* config = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    config.delegateClass = SceneDelegate.class;
    return config;
}
@end
int main(int argc,char** argv) { @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(AppDelegate.class)); } }
