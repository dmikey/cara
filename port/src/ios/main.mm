// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <atomic>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <functional>
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
    std::function<void(unsigned, unsigned, unsigned, const std::vector<u8>&)> present;
    std::atomic<int> touch{-1};
    IOSWindow() { UpdateFramebufferLayout(400, 480); }
    void PollEvents() override {
        const int value = touch.load();
        if (value < 0) TouchReleased();
        else TouchPressed((value & 0xffff) + 40, (value >> 16) + 240);
    }
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    void SwapBuffers() override {}
    void PresentSoftwareFrame(unsigned s, unsigned w, unsigned h, const std::vector<u8>& p) override {
        if (present) present(s, w, h, p);
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
    UILabel* title = [UILabel new]; title.text = @"Citra · No JIT";
    title.textColor = UIColor.whiteColor;
    title.font = [UIFont boldSystemFontOfSize:24];
    status = [UILabel new]; status.text = @"Interpreter test · software graphics · audio off";
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
- (void)showFrame:(unsigned)screen width:(unsigned)width height:(unsigned)height pixels:(const std::vector<u8>&)pixels {
    if (framePending[screen].exchange(true)) return;
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
        if (controller->frameCount % 60 == 0) {
            double elapsed = CFAbsoluteTimeGetCurrent() - controller->startedAt;
            controller->status.text = [NSString stringWithFormat:@"No JIT · software · %.1f FPS average · audio off", controller->frameCount / std::max(elapsed, 0.001)];
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
    emuWindow->present = [weakSelf](unsigned s,unsigned w,unsigned h,const std::vector<u8>& p) {
        [weakSelf showFrame:s width:w height:h pixels:p];
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
            s.use_virtual_sd = true; s.region_value = Settings::REGION_VALUE_AUTO_SELECT;
            s.resolution_factor = 1; s.frame_limit = 100;
            s.init_clock = Settings::InitClock::SystemTime;
            s.sink_id = "null"; s.audio_volume = 1; s.shared_font_type = Settings::SharedFontType::Auto;
            for (unsigned i=0; i<Settings::NativeButton::NumButtons; ++i)
                s.current_input_profile.buttons[i] = "engine:ios,code:" + std::to_string(i);
            s.current_input_profile.analogs[0] = "engine:ios";
            s.current_input_profile.touch_device = "engine:emu_window";
            Settings::Apply(); Frontend::RegisterDefaultApplets();
            auto& system = Core::System::GetInstance();
            auto result = system.Load(*emuWindow,rom);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (result == Core::System::ResultStatus::Success)
                    self->status.text = @"Running · no JIT · software graphics · audio off";
                else self->status.text = [NSString stringWithFormat:@"Load failed (code %u). See Documents/citra/emulator.log",unsigned(result)];
            });
            if (result == Core::System::ResultStatus::Success) {
                while (!stop) {
                    if (paused) { std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }
                    result = system.RunLoop();
                    if (result != Core::System::ResultStatus::Success) break;
                }
                system.Shutdown();
            }
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
