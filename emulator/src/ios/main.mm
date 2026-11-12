// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import "ios/skin_controls.h"
#include "audio_core/ios_sink.h"
#ifdef CARA_PGO_GENERATE
extern "C" void __llvm_profile_set_filename(const char*);
extern "C" int __llvm_profile_write_file(void);
#endif
#include "audio_core/dsp_interface.h"
#import "ios/save_sync.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#ifdef CARA_IOS_GLES
#import <OpenGLES/EAGL.h>
#import <QuartzCore/CAEAGLLayer.h>
#include <dlfcn.h>
#include <glad/glad.h>
#include "video_core/renderer_opengl/gl_state.h"
#include "ios/gpu_vertex_self_test.h"
#include "ios/gpu_depth_self_test.h"
#endif
#include <atomic>
#include <array>
#include "common/save_events.h"
#include "ios/button_state.h"
#include "cara/adaptive_clock.h"
#include <algorithm>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <thread>
#include <sstream>
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/frame_timing.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "core/core.h"
#include "core/savestate.h"
#include "core/arm/arm_interface.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/input.h"
#include "core/settings.h"

#ifdef CARA_IOS_GLES
@interface CaraGPUView : UIView
@end
@implementation CaraGPUView
+ (Class)layerClass { return CAEAGLLayer.class; }
- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.contentScaleFactor = UIScreen.mainScreen.scale;
        CAEAGLLayer* drawable = (CAEAGLLayer*)self.layer;
        drawable.opaque = YES;
        drawable.drawableProperties = @{kEAGLDrawablePropertyRetainedBacking: @NO,
                                         kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8};
    }
    return self;
}
@end
#endif

static bool CaptureBurst(unsigned frame) {
    const char* start_env = std::getenv("CARA_CAPTURE_BURST_START");
    if (!start_env) return false;
    const unsigned start = unsigned(std::max(0L, std::strtol(start_env, nullptr, 10)));
    const char* count_env = std::getenv("CARA_CAPTURE_BURST_COUNT");
    const unsigned count = count_env ? unsigned(std::clamp(std::strtol(count_env, nullptr, 10), 1L, 120L)) : 36;
    return frame >= start && frame - start < count;
}

static NSString* GraphicsLabel() {
#ifdef CARA_IOS_GLES
    return @"GPU GLES3";
#else
    return @"software graphics";
#endif
}
static Cara::ButtonState buttons;
static std::atomic<float> circleX{0},circleY{0},cstickX{0},cstickY{0};
static std::atomic<bool> digitalToCircle{false};
static std::atomic<bool> paused{false};
// Commands are consumed between guest slices, on the emulation/GL owner thread.
static std::atomic<int> stateCommand{0};
class Button final : public Input::ButtonDevice {
    unsigned index;
public:
    explicit Button(unsigned i) : index(i) {}
    bool GetStatus() const override {
        const bool pressed = buttons.Poll(index);
        if (pressed && std::getenv("CARA_INPUT_TRACE"))
            LOG_INFO(Frontend, "Guest HID read button {}", index);
        return pressed;
    }
};
class ButtonFactory final : public Input::Factory<Input::ButtonDevice> {
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& p) override {
        return std::make_unique<Button>(p.Get("code", 0));
    }
};
class Stick final : public Input::AnalogDevice {
    unsigned index;
public:
    explicit Stick(unsigned value) : index(value) {}
    std::tuple<float, float> GetStatus() const override {
        if (index==Settings::NativeAnalog::CStick) return {cstickX.load(),cstickY.load()};
        const float x=circleX.load(),y=circleY.load();
        if (x!=0 || y!=0) return {x,y};
        if (!digitalToCircle.load()) return {0,0};
        auto b = buttons.Held();
        return {float(bool(b & (1u << Settings::NativeButton::Right))) - float(bool(b & (1u << Settings::NativeButton::Left))),
                float(bool(b & (1u << Settings::NativeButton::Up))) - float(bool(b & (1u << Settings::NativeButton::Down)))};
    }
};
class StickFactory final : public Input::Factory<Input::AnalogDevice> {
    std::unique_ptr<Input::AnalogDevice> Create(const Common::ParamPackage& p) override {
        return std::make_unique<Stick>(unsigned(p.Get("index",0)));
    }
};
class IOSWindow final : public Frontend::EmuWindow {
public:
    std::function<void(unsigned, unsigned, unsigned, const std::vector<u8>&, unsigned)> present;
    std::function<void(unsigned)> frame_presented;
    unsigned frames = 0;
    std::string graphics_error;
#ifdef CARA_IOS_GLES
    EAGLContext* __strong graphics_context = nil;
    GLuint framebuffer = 0;
    GLuint renderbuffer = 0;
    CAEAGLLayer* __strong drawable_layer = nil;
    GLuint drawable_framebuffer = 0;
    GLuint drawable_renderbuffer = 0;
    GLint drawable_width = 0, drawable_height = 0;
    std::mutex display_mutex;
    CGRect display_rects[2]={CGRectMake(0,0,1,.5),CGRectMake(.1,.5,.8,.5)};
    int display_width=0,display_height=0;
    void SetDisplayLayout(CGRect upper,CGRect lower,CGSize size,CGFloat pixelScale) {
        if (size.width<=0 || size.height<=0) return;
        std::lock_guard<std::mutex> lock(display_mutex);
        CGRect rects[]={upper,lower};
        for (unsigned i=0;i<2;++i) display_rects[i]=CGRectMake(rects[i].origin.x/size.width,rects[i].origin.y/size.height,rects[i].size.width/size.width,rects[i].size.height/size.height);
        display_width=int(std::lround(size.width*pixelScale)); display_height=int(std::lround(size.height*pixelScale));
    }
#endif
    std::atomic<int> touch{-1};
    IOSWindow() { UpdateFramebufferLayout(400, 480); }
    ~IOSWindow() override {
#ifdef CARA_IOS_GLES
        if (graphics_context && framebuffer) {
            EAGLContext* previous = EAGLContext.currentContext;
            [EAGLContext setCurrentContext:graphics_context];
            glDeleteFramebuffers(1, &drawable_framebuffer);
            glDeleteRenderbuffers(1, &drawable_renderbuffer);
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteRenderbuffers(1, &renderbuffer);
            [EAGLContext setCurrentContext:previous];
        }
#endif
    }
    void PollEvents() override {
        const int value = touch.load();
        if (value < 0) TouchReleased();
        else TouchPressed((value & 0xffff) + 40, (value >> 16) + 240);
    }
    void MakeCurrent() override {
#ifdef CARA_IOS_GLES
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
            if (drawable_layer) {
                glGenRenderbuffers(1, &drawable_renderbuffer);
                glBindRenderbuffer(GL_RENDERBUFFER, drawable_renderbuffer);
                if (![graphics_context renderbufferStorage:GL_RENDERBUFFER fromDrawable:drawable_layer])
                    graphics_error = "Unable to allocate iOS screen drawable";
                glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &drawable_width);
                glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &drawable_height);
                glGenFramebuffers(1, &drawable_framebuffer);
                glBindFramebuffer(GL_FRAMEBUFFER, drawable_framebuffer);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, drawable_renderbuffer);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                    graphics_error = "Unable to create iOS screen framebuffer";
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
            }
            std::ofstream capabilities(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "gpu-capabilities.txt");
            capabilities << "vendor=" << reinterpret_cast<const char*>(glGetString(GL_VENDOR))
                         << "\nrenderer=" << reinterpret_cast<const char*>(glGetString(GL_RENDERER))
                         << "\nversion=" << reinterpret_cast<const char*>(glGetString(GL_VERSION)) << "\n";
        }
#endif
    }
    void DoneCurrent() override {
#ifdef CARA_IOS_GLES
        [EAGLContext setCurrentContext:nil];
#endif
    }
    unsigned GetDefaultFramebuffer() const override {
#ifdef CARA_IOS_GLES
        return framebuffer;
#else
        return 0;
#endif
    }
    void SwapBuffers() override {
#ifdef CARA_IOS_GLES
        if (drawable_framebuffer) {
            CGRect destinations[2]; int desired_width,desired_height;
            {
                std::lock_guard<std::mutex> lock(display_mutex);
                destinations[0]=display_rects[0]; destinations[1]=display_rects[1];
                desired_width=display_width; desired_height=display_height;
            }
            {
                Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::Present);
                const auto previous_read = OpenGL::OpenGLState::BindReadFramebuffer(framebuffer);
                const auto previous_draw = OpenGL::OpenGLState::BindDrawFramebuffer(drawable_framebuffer);
                const auto previous_renderbuffer = OpenGL::OpenGLState::BindRenderbuffer(drawable_renderbuffer);
                if (desired_width>0 && desired_height>0 && (drawable_width!=desired_width || drawable_height!=desired_height)) {
                    if (![graphics_context renderbufferStorage:GL_RENDERBUFFER fromDrawable:drawable_layer]) return;
                    glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_WIDTH,&drawable_width);
                    glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_HEIGHT,&drawable_height);
                }
                const bool scissor = glIsEnabled(GL_SCISSOR_TEST);
                glDisable(GL_SCISSOR_TEST);
                GLboolean mask[4]; glGetBooleanv(GL_COLOR_WRITEMASK,mask);
                glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
                glColorMask(mask[0],mask[1],mask[2],mask[3]);
                for (unsigned screen=0;screen<2;++screen) {
                    const CGRect r=destinations[screen];
                    const GLint x0=GLint(std::lround(r.origin.x*drawable_width));
                    const GLint x1=GLint(std::lround(CGRectGetMaxX(r)*drawable_width));
                    const GLint y0=GLint(std::lround((1-CGRectGetMaxY(r))*drawable_height));
                    const GLint y1=GLint(std::lround((1-r.origin.y)*drawable_height));
                    glBlitFramebuffer(screen?40:0,screen?0:240,screen?360:400,screen?240:480,x0,y0,x1,y1,GL_COLOR_BUFFER_BIT,GL_NEAREST);
                }
                if (scissor) glEnable(GL_SCISSOR_TEST);
                if (![graphics_context presentRenderbuffer:GL_RENDERBUFFER])
                    graphics_error = "iOS screen presentation failed";
                OpenGL::OpenGLState::BindRenderbuffer(previous_renderbuffer);
                OpenGL::OpenGLState::BindDrawFramebuffer(previous_draw);
                OpenGL::OpenGLState::BindReadFramebuffer(previous_read);
            }
            if (Common::FrameTiming::counters.enabled && std::getenv("CARA_GPU_WAIT_TIMING")) {
                Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::GPUWait);
                glFinish();
            }
            ++frames;
            if (frame_presented) frame_presented(frames);
            // Readback is only used for explicitly requested diagnostic screen captures.
            if (!std::getenv("CARA_CAPTURE_FRAMES") || (frames % 60 != 0 && !CaptureBurst(frames))) return;
        }
        const auto previous = OpenGL::OpenGLState::BindReadFramebuffer(framebuffer);
        std::vector<u8> image(400 * 480 * 4);
        {
            Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::Readback);
            glReadPixels(0, 0, 400, 480, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
        }
        OpenGL::OpenGLState::BindReadFramebuffer(previous);
        for (unsigned screen = 0; screen < 2; ++screen) {
            const unsigned width = screen == 0 ? 400 : 320;
            const unsigned x = screen == 0 ? 0 : 40;
            const unsigned top = screen == 0 ? 0 : 240;
            std::vector<u8> pixels(width * 240 * 4);
            for (unsigned y = 0; y < 240; ++y)
                std::copy_n(image.data() + ((479 - top - y) * 400 + x) * 4, width * 4, pixels.data() + y * width * 4);
            if (drawable_framebuffer) {
                if (present) present(screen, width, 240, pixels, frames);
            } else {
                PresentSoftwareFrame(screen, width, 240, pixels);
            }
        }
#endif
    }
    void PresentSoftwareFrame(unsigned s, unsigned w, unsigned h, const std::vector<u8>& p) override {
        if (s == 0) ++frames;
        if (present) present(s, w, h, p, frames);
    }
};

static NSString* CaraSpeedLabel(double refreshesPerSecond, bool adaptive, unsigned clock) {
    NSString* value=[NSString stringWithFormat:@"%.0f%% speed",refreshesPerSecond*100.0/59.833997];
    if (adaptive) value=[value stringByAppendingFormat:@" · CPU %u%% auto",clock];
    if (AudioCore::IOSAudioOutputFailed()) return [value stringByAppendingString:@" · Audio unavailable"];
    if (AudioCore::IsIOSAudioMuted()) return [value stringByAppendingString:@" · Muted"];
    return value;
}

@interface EmulatorController : UIViewController <UIDocumentPickerDelegate>
@end
@implementation EmulatorController {
    UIImageView* top;
    UIImageView* bottom;
#ifdef CARA_IOS_GLES
    CaraGPUView* gpuSurface;
#endif
    UILabel* status;
    UIImageView* skinArt;
    CaraSkinControls* skinControls;
    CaraSkin* activeSkin;
    NSDictionary* currentRepresentation;
    UIButton* menuFallback;
    BOOL pickingSkin;
    BOOL pickingSaveFolder;
    BOOL openingGame;
    CaraSaveSync* saveSync;
    NSURL* gameURL;
    BOOL gameScoped;
    BOOL modalPaused;
    uint32_t uiButtonMask;
    CGSize lastSkinSize;
    std::unique_ptr<IOSWindow> emuWindow;
    std::thread worker;
    std::atomic<bool> stop;
    std::atomic<bool> running;
    std::atomic<bool> stretchAudio;
    std::atomic<bool> stretchAudioDirty;
    std::atomic<bool> adaptiveCPU;
    std::atomic<bool> adaptiveCPUDirty;
    std::atomic<unsigned> guestClock;
    std::atomic<bool> framePending[2];
    unsigned frameCount;
    unsigned sessionGeneration;
    CFTimeInterval startedAt;
    CFTimeInterval backgroundAt;
    CFTimeInterval lastHUDTime;
    unsigned lastHUDFrame;
    bool autoStarted;
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    NSDictionary* devices=activeSkin.info[@"representations"];
    NSDictionary* styles=UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad ? (devices[@"ipad"]?:devices[@"iphone"]) : (devices[@"iphone"]?:devices[@"ipad"]);
    UIInterfaceOrientationMask mask=0;
    for (NSDictionary* orientations in styles.allValues) {
        if (orientations[@"landscape"]) mask|=UIInterfaceOrientationMaskLandscape;
        if (orientations[@"portrait"]) mask|=UIInterfaceOrientationMaskPortrait;
    }
    return mask?:UIInterfaceOrientationMaskLandscape;
}
- (NSURL*)defaultSkinURL {
    return [NSBundle.mainBundle URLForResource:@"CarbonDuo" withExtension:@"deltaskin"];
}
- (void)resetSkinInput {
    [skinControls reset]; uiButtonMask=0; buttons.Clear();
    circleX=0; circleY=0; cstickX=0; cstickY=0;
    if (emuWindow) emuWindow->touch=-1;
}
- (void)applySkin:(CaraSkin*)skin {
    activeSkin=skin; currentRepresentation=nil; skinControls.skin=skin;
    [self.view setNeedsLayout]; [self.view layoutIfNeeded];
    if (@available(iOS 16.0,*)) [self setNeedsUpdateOfSupportedInterfaceOrientations];
    else [UIViewController attemptRotationToDeviceOrientation];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    NSString* rom=[NSUserDefaults.standardUserDefaults stringForKey:@"rom"];
    if (!autoStarted && !running && std::getenv("CARA_OPEN_RECENT")) {
        autoStarted=true; [self openRecentGame]; return;
    }
    if (!autoStarted && rom.length && !running) {
        autoStarted=true;
        NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
        NSURL* url=[NSURL fileURLWithPath:[documents stringByAppendingPathComponent:rom.lastPathComponent]];
        if ([NSFileManager.defaultManager fileExistsAtPath:url.path]) [self openGameURL:url];
        else status.text=@"Game file not found.";
    }
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor=UIColor.blackColor;
    const bool benchmarkMute=std::getenv("CARA_BENCHMARK_FRAMES") && !std::getenv("CARA_BENCHMARK_AUDIO");
    AudioCore::SetIOSAudioMuted(benchmarkMute || [NSUserDefaults.standardUserDefaults boolForKey:@"audioMuted"]);
    stretchAudio=[NSUserDefaults.standardUserDefaults boolForKey:@"audioStretching"]; stretchAudioDirty=false;
    const char* adaptiveOverride=std::getenv("CARA_ADAPTIVE_GUEST_CPU");
    adaptiveCPU=adaptiveOverride?std::strcmp(adaptiveOverride,"1")==0:
        (!std::getenv("CARA_BENCHMARK_FRAMES") && [NSUserDefaults.standardUserDefaults boolForKey:@"adaptiveGuestCPU"]);
    adaptiveCPUDirty=false; guestClock=100;
    NSURL* saveDocuments=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    saveSync=[[CaraSaveSync alloc] initWithRoot:[saveDocuments URLByAppendingPathComponent:@"cara" isDirectory:YES]];
    stop=false; running=false; sessionGeneration=0; framePending[0]=false; framePending[1]=false;
    top=[UIImageView new]; bottom=[UIImageView new];
    top.backgroundColor=bottom.backgroundColor=UIColor.blackColor;
    top.contentMode=bottom.contentMode=UIViewContentModeScaleAspectFit;
    [self.view addSubview:top]; [self.view addSubview:bottom];
#ifdef CARA_IOS_GLES
    gpuSurface=[[CaraGPUView alloc] initWithFrame:self.view.bounds];
    [self.view addSubview:gpuSurface];
#endif
    skinArt=[UIImageView new]; skinArt.opaque=NO; skinArt.userInteractionEnabled=NO; [self.view addSubview:skinArt];
    skinControls=[[CaraSkinControls alloc] initWithFrame:CGRectZero]; [self.view addSubview:skinControls];
    __weak EmulatorController* weakSelf=self;
    skinControls.inputChanged=^(NSSet<NSString*>* pressed,CGPoint circle,CGPoint cstick,CGPoint touch) {
        EmulatorController* controller=weakSelf; if (!controller) return;
        static NSDictionary<NSString*,NSNumber*>* codes;
        static dispatch_once_t once;
        dispatch_once(&once,^{ codes=@{@"a":@(Settings::NativeButton::A),@"b":@(Settings::NativeButton::B),@"x":@(Settings::NativeButton::X),@"y":@(Settings::NativeButton::Y),@"up":@(Settings::NativeButton::Up),@"down":@(Settings::NativeButton::Down),@"left":@(Settings::NativeButton::Left),@"right":@(Settings::NativeButton::Right),@"l":@(Settings::NativeButton::L),@"r":@(Settings::NativeButton::R),@"zl":@(Settings::NativeButton::ZL),@"zr":@(Settings::NativeButton::ZR),@"start":@(Settings::NativeButton::Start),@"select":@(Settings::NativeButton::Select)}; });
        uint32_t mask=0;
        for (NSString* name in pressed) if (codes[name]) mask|=uint32_t(1)<<[codes[name] unsignedIntValue];
        const uint32_t before=controller->uiButtonMask;
        for (unsigned i=0;i<Settings::NativeButton::NumButtons;++i) {
            if ((mask & ~before) & (uint32_t(1)<<i)) buttons.Press(i);
            if ((before & ~mask) & (uint32_t(1)<<i)) buttons.Release(i);
        }
        controller->uiButtonMask=mask;
        circleX=float(circle.x); circleY=float(circle.y); cstickX=float(cstick.x); cstickY=float(cstick.y);
        if (controller->emuWindow) controller->emuWindow->touch=touch.x<0 ? -1 : int(unsigned(touch.x) | (unsigned(touch.y)<<16));
    };
    skinControls.menuPressed=^{ [weakSelf showMenu]; };
    status=[UILabel new]; status.text=@"Choose a game from Menu";
    status.font=[UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightMedium]; status.textColor=[UIColor colorWithRed:.7 green:.8 blue:.7 alpha:1];
    status.numberOfLines=2; status.userInteractionEnabled=NO; [self.view addSubview:status];
    menuFallback=[UIButton buttonWithType:UIButtonTypeSystem]; menuFallback.configuration=[UIButtonConfiguration filledButtonConfiguration];
    [menuFallback setTitle:@"Menu" forState:UIControlStateNormal]; [menuFallback addTarget:self action:@selector(showMenu) forControlEvents:UIControlEventTouchUpInside]; [self.view addSubview:menuFallback];
    NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSString* saved=[NSUserDefaults.standardUserDefaults stringForKey:@"controllerSkin"];
    const char* test=std::getenv("CARA_TEST_SKIN");
    NSURL* url=test?[NSURL fileURLWithPath:[documents stringByAppendingPathComponent:[NSString stringWithUTF8String:test].lastPathComponent]]:
        (saved?[NSURL fileURLWithPath:[documents stringByAppendingPathComponent:saved]]:[self defaultSkinURL]);
    NSError* error=nil;
    CaraSkin* skin=url?[CaraSkin loadURL:url error:&error]:nil;
    if (!skin) skin=[CaraSkin loadURL:[self defaultSkinURL] error:nil];
    [self applySkin:skin];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(background) name:UIApplicationWillResignActiveNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(foreground) name:UIApplicationDidBecomeActiveNotification object:nil];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
#ifdef CARA_IOS_GLES
    gpuSurface.frame=self.view.bounds;
#endif
    NSDictionary* rep=[activeSkin representationForPad:UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad landscape:self.view.bounds.size.width>self.view.bounds.size.height];
    menuFallback.hidden=rep!=nil;
    if (!rep) { menuFallback.frame=CGRectMake(24,48,120,44); return; }
    if (rep!=currentRepresentation) {
        currentRepresentation=rep; skinControls.representation=rep; skinArt.image=skinControls.artwork;
        BOOL circle=NO;
        for (NSDictionary* item in rep[@"items"]) if ([item[@"inputs"] isKindOfClass:NSDictionary.class] && [item[@"inputs"][@"x"] isEqual:@"circlePadX"]) circle=YES;
        digitalToCircle=!circle;
    }
    CGSize mapping=CGSizeMake([rep[@"mappingSize"][@"width"] doubleValue],[rep[@"mappingSize"][@"height"] doubleValue]);
    CGRect safe=UIEdgeInsetsInsetRect(self.view.bounds,self.view.safeAreaInsets);
    CGRect canvas=CaraAspectFit(mapping,safe);
    if (!CGSizeEqualToSize(lastSkinSize,canvas.size)) { [self resetSkinInput]; lastSkinSize=canvas.size; }
    skinArt.frame=canvas; skinControls.frame=canvas; [skinControls setNeedsDisplay];
    const auto convert=[&](CGRect r) { return CGRectMake(canvas.origin.x+r.origin.x/mapping.width*canvas.size.width,canvas.origin.y+r.origin.y/mapping.height*canvas.size.height,r.size.width/mapping.width*canvas.size.width,r.size.height/mapping.height*canvas.size.height); };
    top.frame=convert(skinControls.upperScreen); bottom.frame=convert(skinControls.lowerScreen);
    status.frame=convert(CGRectMake(80,215,420,64));
    status.adjustsFontSizeToFitWidth=YES; status.minimumScaleFactor=0.7;
#ifdef CARA_IOS_GLES
    if (emuWindow) emuWindow->SetDisplayLayout(top.frame,bottom.frame,self.view.bounds.size,gpuSurface.contentScaleFactor);
#endif
}
- (void)resumeFromMenu { modalPaused=NO; paused=false; AudioCore::SetIOSAudioPaused(stop || !running); }
- (void)showMenu {
    if (openingGame) return;
    modalPaused=YES; paused=true; AudioCore::SetIOSAudioPaused(true); [self resetSkinInput];
    UIAlertController* menu=[UIAlertController alertControllerWithTitle:@"Cara" message:saveSync.status preferredStyle:UIAlertControllerStyleActionSheet];
    [menu addAction:[UIAlertAction actionWithTitle:(AudioCore::IsIOSAudioMuted()?@"Unmute sound":@"Mute sound") style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
        const bool muted=!AudioCore::IsIOSAudioMuted(); AudioCore::SetIOSAudioMuted(muted);
        [NSUserDefaults.standardUserDefaults setBool:muted forKey:@"audioMuted"]; [self resumeFromMenu];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:(stretchAudio.load()?@"Disable audio stretching":@"Enable audio stretching") style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
        const bool enabled=!self->stretchAudio.load(); self->stretchAudio=enabled; self->stretchAudioDirty=true;
        [NSUserDefaults.standardUserDefaults setBool:enabled forKey:@"audioStretching"]; [self resumeFromMenu];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:(adaptiveCPU.load()?@"Disable adaptive guest CPU":@"Enable adaptive guest CPU (50–100%)") style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
        const bool enabled=!self->adaptiveCPU.load(); self->adaptiveCPU=enabled; self->adaptiveCPUDirty=true;
        [NSUserDefaults.standardUserDefaults setBool:enabled forKey:@"adaptiveGuestCPU"]; [self resumeFromMenu];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Open game…" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { self->pickingSkin=NO; [self openROM]; }]];
    NSData* recent=[NSUserDefaults.standardUserDefaults dataForKey:@"recentGameBookmark"];
    if (recent) [menu addAction:[UIAlertAction actionWithTitle:@"Open recent game" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
        [self openRecentGame];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Import skin…" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { self->pickingSkin=YES; [self openROM]; }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Use Carbon Duo" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
        [NSUserDefaults.standardUserDefaults removeObjectForKey:@"controllerSkin"];
        [self applySkin:[CaraSkin loadURL:[self defaultSkinURL] error:nil]]; [self resumeFromMenu];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Choose iCloud save folder…" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
        if (self->running) { self->status.text=@"Stop the game before changing the save folder."; [self resumeFromMenu]; return; }
        self->pickingSaveFolder=YES;
        UIDocumentPickerViewController* picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO];
        picker.delegate=self; [self presentViewController:picker animated:YES completion:nil];
    }]];
    if (saveSync.enabled) {
        [menu addAction:[UIAlertAction actionWithTitle:@"Sync saves now" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
            if (self->running) { self->status.text=@"Stop the game before restoring cloud saves."; [self resumeFromMenu]; return; }
            [self syncSaves:nil completion:nil];
        }]];
        [menu addAction:[UIAlertAction actionWithTitle:@"Disconnect save folder" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self->saveSync disconnect]; [self resumeFromMenu]; }]];
    }
    if (running) {
        [menu addAction:[UIAlertAction actionWithTitle:@"Save state…" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self showStateSlots:YES]; }]];
        [menu addAction:[UIAlertAction actionWithTitle:@"Load state…" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self showStateSlots:NO]; }]];
    }
    if (running) [menu addAction:[UIAlertAction actionWithTitle:@"Stop game" style:UIAlertActionStyleDestructive handler:^(UIAlertAction* action) { [self stopGame]; [self resumeFromMenu]; }]];
    [menu addAction:[UIAlertAction actionWithTitle:(running?@"Resume":@"Close") style:UIAlertActionStyleCancel handler:^(UIAlertAction* action) { [self resumeFromMenu]; }]];
    if (activeSkin && currentRepresentation) {
        menu.popoverPresentationController.sourceView=skinControls;
        CGRect r=[skinControls menuFrame]; NSDictionary* mapping=currentRepresentation[@"mappingSize"];
        menu.popoverPresentationController.sourceRect=CGRectMake(r.origin.x/[mapping[@"width"] doubleValue]*skinControls.bounds.size.width,r.origin.y/[mapping[@"height"] doubleValue]*skinControls.bounds.size.height,MAX(1,r.size.width/[mapping[@"width"] doubleValue]*skinControls.bounds.size.width),MAX(1,r.size.height/[mapping[@"height"] doubleValue]*skinControls.bounds.size.height));
    } else { menu.popoverPresentationController.sourceView=menuFallback; menu.popoverPresentationController.sourceRect=menuFallback.bounds; }
    [self presentViewController:menu animated:YES completion:nil];
}
- (void)showStateSlots:(BOOL)saving {
    UIAlertController* slots=[UIAlertController alertControllerWithTitle:(saving?@"Save state":@"Load state") message:(saving?@"Saving replaces the selected slot. Keep using in-game saves too.":@"Loading returns to this slot. Unsaved progress since then is lost.") preferredStyle:UIAlertControllerStyleAlert];
    for (unsigned slot=1; slot<=Core::SaveStateSlotCount; ++slot) {
        [slots addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Slot %u",slot] style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
            self->status.text=saving?@"Saving state…":@"Loading state…";
            stateCommand.store(saving?int(slot):-int(slot),std::memory_order_release);
        }]];
    }
    [slots addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction* action) { [self resumeFromMenu]; }]];
    [self presentViewController:slots animated:YES completion:nil];
}
- (void)stopGame { stop=true; AudioCore::SetIOSAudioPaused(true); [self resetSkinInput]; status.text=@"Stopping…"; }
- (void)background { backgroundAt = CFAbsoluteTimeGetCurrent(); paused = true; AudioCore::SetIOSAudioPaused(true); [self resetSkinInput]; }
- (void)foreground {
    if (backgroundAt > 0 && running) {
        const double duration = CFAbsoluteTimeGetCurrent() - backgroundAt;
        startedAt += duration;
        lastHUDTime += duration;
    }
    backgroundAt = 0; paused = modalPaused; AudioCore::SetIOSAudioPaused(paused);
}
- (void)openRecentGame {
    NSData* recent=[NSUserDefaults.standardUserDefaults dataForKey:@"recentGameBookmark"];
    BOOL stale=NO; NSError* error=nil;
    NSURL* url=recent?[NSURL URLByResolvingBookmarkData:recent options:NSURLBookmarkResolutionWithoutUI relativeToURL:nil bookmarkDataIsStale:&stale error:&error]:nil;
    if (url) [self openGameURL:url];
    else { status.text=@"Select the game again in Files."; [self resumeFromMenu]; }
}
- (void)openROM {
    UIDocumentPickerViewController* picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeData] asCopy:pickingSkin];
    picker.delegate=self; [self presentViewController:picker animated:YES completion:nil];
}
- (void)skinError:(NSString*)message {
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Could not import skin" message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self resumeFromMenu]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)importSkin:(NSURL*)url {
    const BOOL scoped=[url startAccessingSecurityScopedResource];
    NSError* error=nil; CaraSkin* skin=[CaraSkin loadURL:url error:&error];
    if (!skin) { if (scoped) [url stopAccessingSecurityScopedResource]; [self skinError:error.localizedDescription?:@"Unsupported skin."]; return; }
    NSDictionary* rep=[skin representationForPad:UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad landscape:self.view.bounds.size.width>self.view.bounds.size.height];
    CaraSkinControls* check=[[CaraSkinControls alloc] initWithFrame:CGRectZero]; check.skin=skin;
    if (rep) check.representation=rep;
    if (!rep || !check.artwork) { if (scoped) [url stopAccessingSecurityScopedResource]; [self skinError:@"The skin has no usable artwork or layout for this device."]; return; }
    NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSURL* destination=[NSURL fileURLWithPath:[documents stringByAppendingPathComponent:@"controller.deltaskin"]];
    NSData* data=[NSData dataWithContentsOfURL:url options:0 error:&error];
    BOOL saved=data && [data writeToURL:destination options:NSDataWritingAtomic error:&error];
    if (scoped) [url stopAccessingSecurityScopedResource];
    if (!saved) { [self skinError:error.localizedDescription]; return; }
    [NSUserDefaults.standardUserDefaults setObject:@"controller.deltaskin" forKey:@"controllerSkin"];
    [self applySkin:skin]; [self resumeFromMenu];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)picker { pickingSaveFolder=NO; pickingSkin=NO; [self resumeFromMenu]; }
- (void)documentPicker:(UIDocumentPickerViewController*)picker didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* url=urls.firstObject;
    if (!url) { [self resumeFromMenu]; return; }
    if (pickingSaveFolder) {
        pickingSaveFolder=NO; NSError* error=nil;
        if (![saveSync connectFolder:url error:&error]) { status.text=error.localizedDescription; [self resumeFromMenu]; return; }
        [self syncSaves:nil completion:nil]; return;
    }
    if (pickingSkin || [@[@"deltaskin",@"caraskin"] containsObject:url.pathExtension.lowercaseString]) { [self importSkin:url]; return; }
    [self openGameURL:url];
}
- (void)syncSaves:(NSString*)resolution completion:(void(^)(BOOL))completion {
    status.text=@"Syncing saves…";
    [saveSync requestSync:YES resolution:resolution completion:^(NSDictionary* result,NSError* error) {
        [self resumeFromMenu];
        if (error) { self->status.text=error.localizedDescription; if (completion) completion(NO); return; }
        if ([result[@"conflict"] boolValue]) {
            UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Choose saves" message:@"Both versions are preserved. Choose which complete save set to use." preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"Keep this iPad’s saves" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self syncSaves:@"local" completion:completion]; }]];
            for (NSDictionary* version in result[@"versions"]) {
                NSString* title=[@"Use cloud version " stringByAppendingString:[version[@"id"] substringToIndex:MIN((NSUInteger)8,[version[@"id"] length])]];
                [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self syncSaves:version[@"id"] completion:completion]; }]];
            }
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction* action) { if (completion) completion(NO); }]];
            [self presentViewController:alert animated:YES completion:nil]; return;
        }
        self->status.text=self->saveSync.status; if (completion) completion(YES);
    }];
}
- (void)openGameURL:(NSURL*)url {
    if (openingGame) return;
    if (![@[@"3ds",@"cci",@"cxi",@"3dsx",@"elf"] containsObject:url.pathExtension.lowercaseString]) { status.text=@"Choose an extracted .3ds, .cci, .cxi, .3dsx or .elf game."; [self resumeFromMenu]; return; }
    openingGame=YES; stop=true; [self resumeFromMenu];
    if (worker.joinable()) worker.join();
    if (gameScoped) [gameURL stopAccessingSecurityScopedResource];
    gameURL=url; gameScoped=[url startAccessingSecurityScopedResource];
    NSData* bookmark=[url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if (bookmark) [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:@"recentGameBookmark"];
    [self syncSaves:nil completion:^(BOOL ok) {
        if (!ok) { self->openingGame=NO; return; }
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            __block NSError* error=nil; __block NSURL* readable=nil;
            [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:url options:0 error:&error byAccessor:^(NSURL* actual) {
                if ([actual checkResourceIsReachableAndReturnError:&error]) readable=actual;
            }];
            dispatch_async(dispatch_get_main_queue(), ^{ self->openingGame=NO; if (readable) [self startROM:readable]; else self->status.text=error.localizedDescription?:@"Game unavailable. Select it again in Files."; });
        });
    }];
}
- (void)openExternalURL:(NSURL*)url {
    if ([@[@"deltaskin",@"caraskin"] containsObject:url.pathExtension.lowercaseString]) [self importSkin:url];
    else [self openGameURL:url];
}
- (void)showFrame:(unsigned)screen width:(unsigned)width height:(unsigned)height pixels:(const std::vector<u8>&)pixels frame:(unsigned)emulatedFrame session:(unsigned)session {
    const bool captureCheckpoint = (emulatedFrame % 600 == 0 || CaptureBurst(emulatedFrame)) &&
        [NSProcessInfo.processInfo.environment[@"CARA_CAPTURE_FRAMES"] isEqualToString:@"1"];
    if (framePending[screen].exchange(true) && !captureCheckpoint) return;
    NSData* data = [NSData dataWithBytes:pixels.data() length:pixels.size()];
    __weak EmulatorController* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        EmulatorController* controller = weakSelf;
        if (!controller || controller->sessionGeneration!=session) return;
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGImageRef image = CGImageCreate(width,height,8,32,width*4,space,CGBitmapInfo(kCGImageAlphaLast) | kCGBitmapByteOrder32Big,provider,nullptr,false,kCGRenderingIntentDefault);
        UIImage* displayImage = [UIImage imageWithCGImage:image];
        (screen == 0 ? controller->top : controller->bottom).image = displayImage;
        if (screen == 0) ++controller->frameCount;
        if (captureCheckpoint) {
            NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
            NSString* folder = CaptureBurst(emulatedFrame)
                ? [documents stringByAppendingPathComponent:@"capture-burst"] : documents;
            [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
            NSString* filename = [NSString stringWithFormat:@"frame-%u-screen-%u.png",emulatedFrame,screen];
            [UIImagePNGRepresentation(displayImage) writeToFile:[folder stringByAppendingPathComponent:filename] atomically:YES];
        }
        const unsigned reportedFrame =
#ifdef CARA_IOS_GLES
            emulatedFrame;
#else
            controller->frameCount;
#endif
        if (reportedFrame % 60 == 0) {
            double elapsed = CFAbsoluteTimeGetCurrent() - controller->startedAt;
#ifndef CARA_IOS_GLES
            if (screen==0 && reportedFrame>controller->lastHUDFrame) {
                const double now=CFAbsoluteTimeGetCurrent();
                const double refresh=(reportedFrame-controller->lastHUDFrame)/std::max(now-controller->lastHUDTime,0.001);
                controller->lastHUDTime=now; controller->lastHUDFrame=reportedFrame;
                controller->status.text=CaraSpeedLabel(refresh,controller->adaptiveCPU.load(),controller->guestClock.load());
            }
#endif
            if ([NSProcessInfo.processInfo.environment[@"CARA_CAPTURE_FRAMES"] isEqualToString:@"1"]) {
                NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
                [UIImagePNGRepresentation(displayImage) writeToFile:[documents stringByAppendingPathComponent:[NSString stringWithFormat:@"latest-screen-%u.png",screen]] atomically:YES];
                NSString* evidence = [NSString stringWithFormat:@"frames=%u elapsed=%.3f average_fps=%.3f cpu_jit=false shader_jit=false backend=%@",reportedFrame,elapsed,reportedFrame/std::max(elapsed,0.001),GraphicsLabel()];
                [evidence writeToFile:[documents stringByAppendingPathComponent:@"run-status.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            }
        }
        if (screen==1 && captureCheckpoint && std::getenv("CARA_CAPTURE_SKIN_UI")) {
            NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
#ifdef CARA_IOS_GLES
            const BOOL hidden=controller->gpuSurface.hidden; controller->gpuSurface.hidden=YES;
#endif
            UIGraphicsImageRendererFormat* format=[UIGraphicsImageRendererFormat defaultFormat]; format.scale=1;
            UIImage* composite=[[[UIGraphicsImageRenderer alloc] initWithSize:controller.view.bounds.size format:format] imageWithActions:^(UIGraphicsImageRendererContext* context) { [controller.view.layer renderInContext:context.CGContext]; }];
#ifdef CARA_IOS_GLES
            controller->gpuSurface.hidden=hidden;
#endif
            [UIImagePNGRepresentation(composite) writeToFile:[documents stringByAppendingPathComponent:@"skin-preview.png"] atomically:YES];
            const auto frameDict=[](CGRect r) { return @{@"x":@(r.origin.x),@"y":@(r.origin.y),@"width":@(r.size.width),@"height":@(r.size.height)}; };
            NSDictionary* layout=@{@"skin":controller->activeSkin.info[@"name"]?:@"none",@"view":frameDict(controller.view.bounds),@"upper":frameDict(controller->top.frame),@"lower":frameDict(controller->bottom.frame),@"capture":@"UIKit composite with actual diagnostic game frames"};
            [[NSJSONSerialization dataWithJSONObject:layout options:NSJSONWritingPrettyPrinted error:nil] writeToFile:[documents stringByAppendingPathComponent:@"skin-layout.json"] atomically:YES];
        }
        CGImageRelease(image); CGColorSpaceRelease(space); CGDataProviderRelease(provider);
        controller->framePending[screen] = false;
    });
}
- (void)runSkinInputCheck {
    if (!std::getenv("CARA_SKIN_SELF_TEST") || !currentRepresentation) return;
    NSMutableArray* failures=[NSMutableArray new]; unsigned checks=0;
    const auto require=[&](BOOL ok,NSString* name) { ++checks; if (!ok) [failures addObject:name]; };
    NSDictionary* mapping=currentRepresentation[@"mappingSize"];
    const auto local=[&](CGPoint p) { return CGPointMake(p.x/[mapping[@"width"] doubleValue]*skinControls.bounds.size.width,p.y/[mapping[@"height"] doubleValue]*skinControls.bounds.size.height); };
    NSValue* one=[NSValue valueWithPointer:(void*)1]; NSValue* two=[NSValue valueWithPointer:(void*)2];
    for (NSDictionary* item in currentRepresentation[@"items"]) {
        if (![item[@"inputs"] isKindOfClass:NSArray.class]) continue;
        NSString* name=[item[@"inputs"] firstObject];
        unsigned code;
        if ([name isEqual:@"a"]) code=Settings::NativeButton::A;
        else if ([name isEqual:@"start"]) code=Settings::NativeButton::Start;
        else if ([name isEqual:@"zl"]) code=Settings::NativeButton::ZL;
        else if ([name isEqual:@"zr"]) code=Settings::NativeButton::ZR;
        else continue;
        [self resetSkinInput]; CGRect r=CaraSkinRect(item[@"frame"]);
        CGPoint point=local(CGPointMake(CGRectGetMidX(r),CGRectGetMidY(r)));
        require([self.view hitTest:[skinControls convertPoint:point toView:self.view] withEvent:nil]==skinControls,[name stringByAppendingString:@" UIKit hit target"]);
        [skinControls updateContact:one atPoint:point];
        [skinControls endContact:one];
        Button device(code); require(device.GetStatus(),[name stringByAppendingString:@" short tap"]);
        require(!device.GetStatus(),[name stringByAppendingString:@" release"]);
    }
    [self resetSkinInput];
    for (NSDictionary* item in currentRepresentation[@"items"]) {
        if (![item[@"inputs"] isKindOfClass:NSDictionary.class]) continue;
        NSString* axis=item[@"inputs"][@"x"]; CGRect r=CaraSkinRect(item[@"frame"]);
        if ([axis isEqual:@"circlePadX"]) {
            [skinControls updateContact:one atPoint:local(CGPointMake(CGRectGetMaxX(r)-1,CGRectGetMidY(r)))];
            Stick device(Settings::NativeAnalog::CirclePad); auto [x,y]=device.GetStatus(); require(x>.95 && std::abs(y)<.01,@"circle pad right");
        } else if ([axis isEqual:@"cStickX"]) {
            [skinControls updateContact:two atPoint:local(CGPointMake(CGRectGetMidX(r),r.origin.y+1))];
            Stick device(Settings::NativeAnalog::CStick); auto [x,y]=device.GetStatus(); require(y>.95 && std::abs(x)<.01,@"C-stick up");
        }
    }
    [self resetSkinInput];
    CGRect screen=skinControls.lowerScreen;
    [skinControls updateContact:one atPoint:local(CGPointMake(CGRectGetMidX(screen),CGRectGetMidY(screen)))];
    require(emuWindow && (emuWindow->touch.load()&65535)==160 && (emuWindow->touch.load()>>16)==120,@"touch center");
    [skinControls endContact:one]; require(emuWindow && emuWindow->touch.load()<0,@"touch release");
    [self resetSkinInput];
    NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSDictionary* result=@{@"passed":@(failures.count==0),@"checks":@(checks),@"failures":failures,@"skin":activeSkin.info[@"name"]?:@"none",@"scope":@"Same contact dispatch and HID/analog adapters used by UIKit; not physical finger input"};
    [[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil] writeToFile:[documents stringByAppendingPathComponent:@"skin-input-check.json"] atomically:YES];
}
- (void)startROM:(NSURL*)url {
    const unsigned session=++sessionGeneration;
    framePending[0]=false; framePending[1]=false;
    frameCount = 0; startedAt = CFAbsoluteTimeGetCurrent();
    lastHUDTime = startedAt; lastHUDFrame = 0;
    stateCommand=0; running=true; stop=false; [self resumeFromMenu]; [self resetSkinInput];
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    status.text = @"Loading game…";
    std::string rom = url.path.UTF8String;
    NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    std::string user = std::string(documents.UTF8String) + "/cara/";
    __weak EmulatorController* weakSelf = self;
    emuWindow.reset();
    emuWindow = std::make_unique<IOSWindow>();
    [self viewDidLayoutSubviews];
    [self runSkinInputCheck];
#ifdef CARA_IOS_GLES
    [self.view layoutIfNeeded];
    emuWindow->drawable_layer = (CAEAGLLayer*)gpuSurface.layer;
    emuWindow->frame_presented = [weakSelf,user,session](unsigned frame) {
        if (frame % 60 != 0) return;
        const auto timing = Common::FrameTiming::counters;
        const auto title=Core::System::GetInstance().GetTitleID();
        dispatch_async(dispatch_get_main_queue(), ^{
            EmulatorController* controller = weakSelf;
            if (!controller || !controller->running || controller->sessionGeneration!=session) return;
            const double now = CFAbsoluteTimeGetCurrent();
            const double elapsed = now - controller->lastHUDTime;
            const double fps = (frame - controller->lastHUDFrame) / std::max(elapsed, 0.001);
            controller->lastHUDTime = now; controller->lastHUDFrame = frame;
            controller->status.text = CaraSpeedLabel(fps,controller->adaptiveCPU.load(),controller->guestClock.load());
            if (timing.enabled) {
                std::ofstream report(user + "runtime-status.json");
                report << "{\"title_id\":" << title << ",\"frame\":" << frame << ",\"display_fps\":" << fps
                       << ",\"guest_cpu_percent\":" << controller->guestClock.load()
                       << ",\"adaptive_guest_cpu\":" << (controller->adaptiveCPU.load()?"true":"false")
                       << ",\"thermal_state\":" << long(NSProcessInfo.processInfo.thermalState)
                       << ",\"gpu_vertex_draws\":" << timing.gpu_vertex_draws
                       << ",\"cpu_vertex_draws\":" << timing.cpu_vertex_draws << "}\n";
            }
        });
    };
#endif
    emuWindow->present = [weakSelf,session](unsigned s,unsigned w,unsigned h,const std::vector<u8>& p,unsigned frame) {
        [weakSelf showFrame:s width:w height:h pixels:p frame:frame session:session];
    };
    worker = std::thread([self,rom,user,session] {
        @autoreleasepool {
            Common::DetachedTasks tasks;
            FileUtil::SetUserPath(user);
#ifdef CARA_PGO_GENERATE
            const std::string pgo_path=user+"cpu-training.profraw";
            __llvm_profile_set_filename(pgo_path.c_str());
#endif
            static std::once_flag logging;
            std::call_once(logging,[&] {
                Log::SetGlobalFilter(Log::Filter(Log::Level::Info));
                Log::AddBackend(std::make_unique<Log::FileBackend>(user + "emulator.log"));
                Input::RegisterFactory<Input::ButtonDevice>("ios",std::make_shared<ButtonFactory>());
                Input::RegisterFactory<Input::AnalogDevice>("ios",std::make_shared<StickFactory>());
            });
            auto& s = Settings::values;
            s.is_new_3ds = false; // Core selects New 3DS mode for recognized titles on load.
            s.use_cpu_jit = false; s.use_shader_jit = false;
            s.use_hw_renderer = false; s.use_hw_shader = false;
#ifdef CARA_IOS_GLES
            s.use_gles = true;
            s.use_hw_renderer = true;
            s.use_hw_shader = std::getenv("CARA_CPU_VERTEX") == nullptr;
            s.use_separable_shader = false;
            s.allow_shadow = false;
            s.shadow_rendering = false;
            s.use_shader_cache = false;
#endif
            s.use_virtual_sd = true; s.region_value = Settings::REGION_VALUE_AUTO_SELECT;
            s.resolution_factor = 1; s.frame_limit = 100;
            const char* target_env = std::getenv("CARA_BENCHMARK_FRAMES");
            const unsigned target_frames = target_env ? static_cast<unsigned>(std::clamp(std::strtol(target_env, nullptr, 10), 1L, 18000L)) : 0;
            const bool pace_frames = !target_frames || std::getenv("CARA_PACED_BENCHMARK") != nullptr;
            Common::FrameTiming::counters = {};
            Common::FrameTiming::counters.enabled = std::getenv("CARA_FRAME_TIMING") != nullptr;
            s.init_clock = target_frames ? Settings::InitClock::FixedTime : Settings::InitClock::SystemTime;
            s.init_time = 946684801;
            s.sink_id = std::getenv("CARA_AUDIO_DISABLED") ? "null" : "ios"; s.audio_volume = 1; s.enable_audio_stretching = self->stretchAudio.load() && std::getenv("CARA_AUDIO_NO_STRETCH")==nullptr; s.shared_font_type = Settings::SharedFontType::Auto;
            for (unsigned i=0; i<Settings::NativeButton::NumButtons; ++i)
                s.current_input_profile.buttons[i] = "engine:ios,code:" + std::to_string(i);
            s.current_input_profile.analogs[0] = "engine:ios,index:0";
            s.current_input_profile.analogs[1] = "engine:ios,index:1";
            s.current_input_profile.touch_device = "engine:emu_window";
            Settings::Apply(); Frontend::RegisterDefaultApplets();
            auto& system = Core::System::GetInstance();
            FileUtil::CreateFullPath(FileUtil::GetUserPath(FileUtil::UserPath::LogDir));
            emuWindow->MakeCurrent();
#ifdef CARA_IOS_GLES
            if (std::getenv("CARA_VERTEX_SELF_TEST") && emuWindow->graphics_error.empty()) {
                std::string error;
                const bool passed = RunGPUVertexSelfTest(error);
                std::ofstream report(user + "gpu-vertex-self-test.json");
                report << "{\"passed\":" << (passed ? "true" : "false")
                       << ",\"fixtures\":\"uniform_branch_loop_quaternion_color_packed_sparse_zero_clipping\"}\n";
                LOG_INFO(Frontend, "GPU vertex fixture passed={} detail={}", passed, error);
                if (!passed) emuWindow->graphics_error = error;
            }
#endif
#ifdef CARA_IOS_GLES
            if (std::getenv("CARA_DEPTH_SELF_TEST") && emuWindow->graphics_error.empty()) {
                std::string raw_error, quantized_error;
                std::array<bool, 2> raw_formats{}, quantized_formats{};
                const bool raw = RunGPUDepthSelfTest(false, raw_error, &raw_formats);
                const bool quantized = RunGPUDepthSelfTest(true, quantized_error, &quantized_formats);
                std::ofstream report(user + "gpu-depth-self-test.json");
                report << "{\"raw_matches_integer_pica\":" << (raw ? "true" : "false")
                       << ",\"quantized_matches_integer_pica\":" << (quantized ? "true" : "false")
                       << ",\"raw_d16\":" << (raw_formats[0] ? "true" : "false")
                       << ",\"raw_d24\":" << (raw_formats[1] ? "true" : "false")
                       << ",\"quantized_d16\":" << (quantized_formats[0] ? "true" : "false")
                       << ",\"quantized_d24\":" << (quantized_formats[1] ? "true" : "false") << "}\n";
                LOG_INFO(Frontend, "Depth fixture raw={} {} quantized={} {}", raw, raw_error, quantized, quantized_error);
                if (!quantized) emuWindow->graphics_error = quantized_error;
            }
#endif
            if (!emuWindow->graphics_error.empty()) {
                NSString* message = [NSString stringWithUTF8String:emuWindow->graphics_error.c_str()];
                running = false;
                dispatch_async(dispatch_get_main_queue(), ^{ self->status.text = message;  UIApplication.sharedApplication.idleTimerDisabled = NO; });
                return;
            }
            // Diagnostic files describe one session, never a previously opened game.
            for (const char* name : {"benchmark-status.json","frame-timing.json","runtime-status.json",
                                     "audio-status.json","save-state-test-status.json"})
                FileUtil::Delete(user+name);
            auto result = system.Load(*emuWindow,rom);
            if (result==Core::System::ResultStatus::Success)
                LOG_INFO(Frontend,"Cara session title {:016X}",system.GetTitleID());
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self->sessionGeneration!=session) return;
                if (result == Core::System::ResultStatus::Success)
                    self->status.text = @"Running";
                else self->status.text = [NSString stringWithFormat:@"Load failed (code %u). See Documents/cara/emulator.log",unsigned(result)];
            });
            if (result == Core::System::ResultStatus::Success) {
                const auto benchmark_start = std::chrono::steady_clock::now();
                auto next_frame_time = benchmark_start;
                unsigned paced_frame = 0;
                unsigned late_paced_frames = 0;
                bool was_paused = false;
                auto pause_started = benchmark_start;
                std::chrono::steady_clock::duration paused_duration{};
                auto last_frame_time = benchmark_start;
                unsigned timed_frame = 0;
                std::vector<double> frame_times;
                std::vector<std::string> timing_windows;
                auto previous_counters = Common::FrameTiming::counters;
                std::vector<unsigned> test_press_frames;
                const char* press_frames_env = std::getenv("CARA_TEST_PRESS_A_FRAMES");
                if (target_frames && press_frames_env) {
                    std::istringstream stream(press_frames_env);
                    std::string item;
                    while (std::getline(stream, item, ',')) {
                        const long value = std::strtol(item.c_str(), nullptr, 10);
                        if (value > 0 && value < target_frames) test_press_frames.push_back(unsigned(value));
                    }
                    std::sort(test_press_frames.begin(), test_press_frames.end());
                    test_press_frames.erase(std::unique(test_press_frames.begin(), test_press_frames.end()), test_press_frames.end());
                }
                size_t next_test_press = 0;
                unsigned active_test_press = 0;
                const char* window_env = std::getenv("CARA_TIMING_WINDOW_FRAMES");
                const unsigned window_frames = window_env ? unsigned(std::clamp(std::strtol(window_env, nullptr, 10), 60L, 600L)) : 600;
                const bool audio_diagnostics=std::getenv("CARA_AUDIO_DIAGNOSTICS")!=nullptr;
                unsigned audio_report_frame=0;
                Cara::AdaptiveClock clock_control;
                clock_control.SetEnabled(self->adaptiveCPU.load());
                self->guestClock=100;
                unsigned clock_frame=emuWindow->frames;
                auto clock_frame_time=std::chrono::steady_clock::now();
                double clock_work_seconds=0;
                const auto reset_clock_samples=[&]() {
                    clock_control.ResetSamples(); clock_work_seconds=0;
                    clock_frame=emuWindow->frames; clock_frame_time=std::chrono::steady_clock::now();
                    if (system.IsPoweredOn()) system.CoreTiming().UpdateClockSpeed(clock_control.Percent());
                    self->guestClock=clock_control.Percent();
                };
                const auto state_test_frame=[](const char* name) -> unsigned {
                    const char* value=std::getenv(name);
                    return value?unsigned(std::clamp(std::strtol(value,nullptr,10),0L,100000L)):0;
                };
                const unsigned state_save_frame=state_test_frame("CARA_STATE_TEST_SAVE_FRAME");
                const unsigned state_load_frame=state_test_frame("CARA_STATE_TEST_LOAD_FRAME");
                bool state_saved=false,state_loaded=false;
                std::array<u64,69> saved_execution{};
                const auto execution_snapshot=[&]() {
                    std::array<u64,69> snapshot{};
                    for (unsigned cpu=0;cpu<4;++cpu) {
                        for (unsigned reg=0;reg<16;++reg) snapshot[cpu*17+reg]=system.GetCore(cpu).GetReg(reg);
                        snapshot[cpu*17+16]=system.GetCore(cpu).GetCPSR();
                    }
                    snapshot.back()=system.CoreTiming().GetGlobalTicks(); return snapshot;
                };
                const auto state_test_report=[&](const std::string& error) {
                    std::ofstream report(user+"save-state-test-status.json",std::ios::trunc);
                    report << "{\"saved\":" << state_saved << ",\"loaded\":" << state_loaded << ",\"frame\":" << emuWindow->frames << ",\"failed\":" << !error.empty() << "}";
                    if (!error.empty()) LOG_ERROR(Frontend,"Save-state device test: {}",error);
                };
                while (!stop && (!target_frames || emuWindow->frames < target_frames)) {
                    if (self->adaptiveCPUDirty.exchange(false)) {
                        clock_control.SetEnabled(self->adaptiveCPU.load()); reset_clock_samples();
                    }
                    if (self->stretchAudioDirty.exchange(false)) {
                        s.enable_audio_stretching=self->stretchAudio.load();
                        system.DSP().EnableStretching(s.enable_audio_stretching);
                    }
                    try {
                        if (state_save_frame && !state_saved && emuWindow->frames>=state_save_frame) {
                            saved_execution=execution_snapshot();
                            system.SaveStateToPath(user+"device-test.carastate"); state_saved=true; state_test_report("");
                        }
                        if (state_saved && state_load_frame && !state_loaded && emuWindow->frames>=state_load_frame) {
                            AudioCore::SetIOSAudioPaused(true);
                            system.LoadStateFromPath(user+"device-test.carastate");
                            if (execution_snapshot()!=saved_execution) throw std::runtime_error("CPU/timing did not match saved checkpoint");
                            state_loaded=true; state_test_report(""); reset_clock_samples(); AudioCore::SetIOSAudioPaused(paused);
                        }
                    } catch (const std::exception& error) {
                        state_test_report(error.what()); result=Core::System::ResultStatus::ErrorUnknown; break;
                    }
                    if (const int command=stateCommand.exchange(0,std::memory_order_acq_rel)) {
                        std::string failure;
                        try {
                            if (command>0) system.SaveState(unsigned(command));
                            else system.LoadState(unsigned(-command));
                        } catch (const std::exception& error) { failure=error.what(); }
                        reset_clock_samples();
                        const bool powered=system.IsPoweredOn();
                        NSString* message=failure.empty()?[NSString stringWithFormat:@"State %@ in slot %d",command>0?@"saved":@"loaded",std::abs(command)]:[NSString stringWithUTF8String:failure.c_str()];
                        dispatch_async(dispatch_get_main_queue(), ^{
                            if (self->sessionGeneration!=session) return;
                            self->status.text=message;
                            if (!failure.empty()) {
                                UIAlertController* alert=[UIAlertController alertControllerWithTitle:@"Save state" message:message preferredStyle:UIAlertControllerStyleAlert];
                                [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self resumeFromMenu]; }]];
                                [self presentViewController:alert animated:YES completion:nil];
                            } else [self resumeFromMenu];
                        });
                        if (!powered) { result=Core::System::ResultStatus::ErrorUnknown; break; }
                    }
                    if (paused) {
                        if (!was_paused) pause_started = std::chrono::steady_clock::now();
                        was_paused = true; std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue;
                    }
                    if (was_paused) {
                        const auto resumed = std::chrono::steady_clock::now();
                        paused_duration += resumed - pause_started;
                        last_frame_time += resumed - pause_started;
                        next_frame_time = resumed; was_paused = false; reset_clock_samples();
                    }
                    if (active_test_press && emuWindow->frames >= active_test_press + 6) {
                        buttons.Release(Settings::NativeButton::A); active_test_press = 0;
                    }
                    if (!active_test_press && next_test_press < test_press_frames.size() &&
                        emuWindow->frames >= test_press_frames[next_test_press]) {
                        buttons.Press(Settings::NativeButton::A);
                        active_test_press = emuWindow->frames; ++next_test_press;
                    }
                    static uint64_t observedSave=0;
                    static unsigned saveFrame=0;
                    const uint64_t generation=Cara::save_commit_generation.load(std::memory_order_relaxed);
                    if (generation!=observedSave) { observedSave=generation; saveFrame=emuWindow->frames+120; }
                    if (saveFrame && emuWindow->frames>=saveFrame) { saveFrame=0; std::fflush(nullptr); [saveSync captureCommittedSave]; }
                    const auto work_started=clock_control.Enabled()?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
                    result = system.RunLoop();
                    if (clock_control.Enabled()) clock_work_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-work_started).count();
                    if (audio_diagnostics && emuWindow->frames >= audio_report_frame+60) {
                        const auto audio=system.DSP().GetOutputCounters();
                        AudioCore::WriteIOSAudioDiagnostics(user+"audio-status.json",audio.requested,audio.supplied,audio.underruns); audio_report_frame=emuWindow->frames;
                    }
                    if (!emuWindow->graphics_error.empty()) result = Core::System::ResultStatus::ErrorVideoCore;
                    if (result != Core::System::ResultStatus::Success) break;
                    if (Common::FrameTiming::counters.enabled && emuWindow->frames != timed_frame) {
                        const auto now = std::chrono::steady_clock::now();
                        frame_times.push_back(std::chrono::duration<double, std::milli>(now - last_frame_time).count());
                        last_frame_time = now;
                        timed_frame = emuWindow->frames;
                        if (frame_times.size() == window_frames || (target_frames && timed_frame == target_frames)) {
                            double total_ms = 0;
                            unsigned late = 0;
                            for (double ms : frame_times) {
                                total_ms += ms;
                                if (ms > 1000.0 / 59.833997376556916) ++late;
                            }
                            std::sort(frame_times.begin(), frame_times.end());
                            const auto current = Common::FrameTiming::counters;
                            const double count = double(frame_times.size());
                            timing_windows.push_back("{\"end_frame\":" + std::to_string(timed_frame) +
                                ",\"frames\":" + std::to_string(frame_times.size()) +
                                ",\"guest_cpu_percent\":" + std::to_string(clock_control.Percent()) +
                                ",\"mean_ms\":" + std::to_string(total_ms / count) +
                                ",\"p50_ms\":" + std::to_string(frame_times[frame_times.size() / 2]) +
                                ",\"p95_ms\":" + std::to_string(frame_times[(frame_times.size() - 1) * 95 / 100]) +
                                ",\"max_ms\":" + std::to_string(frame_times.back()) +
                                ",\"over_budget_frames\":" + std::to_string(late) +
                                ",\"cpu_shader_ms_per_frame\":" + std::to_string((current.shader_seconds - previous_counters.shader_seconds) * 1000 / count) +
                                ",\"guest_cpu_ms_per_frame\":" + std::to_string((current.guest_cpu_seconds - previous_counters.guest_cpu_seconds) * 1000 / count) +
                                ",\"draw_ms_per_frame\":" + std::to_string((current.draw_seconds - previous_counters.draw_seconds) * 1000 / count) +
                                ",\"vertex_prepare_ms_per_frame\":" + std::to_string((current.vertex_prepare_seconds - previous_counters.vertex_prepare_seconds) * 1000 / count) +
                                ",\"vertex_upload_ms_per_frame\":" + std::to_string((current.vertex_upload_seconds - previous_counters.vertex_upload_seconds) * 1000 / count) +
                                ",\"draw_submit_ms_per_frame\":" + std::to_string((current.draw_submit_seconds - previous_counters.draw_submit_seconds) * 1000 / count) +
                                ",\"surface_download_ms_per_frame\":" + std::to_string((current.surface_download_seconds - previous_counters.surface_download_seconds) * 1000 / count) +
                                ",\"shader_compile_ms_per_frame\":" + std::to_string((current.shader_compile_seconds - previous_counters.shader_compile_seconds) * 1000 / count) +
                                ",\"gpu_wait_ms_per_frame\":" + std::to_string((current.gpu_wait_seconds - previous_counters.gpu_wait_seconds) * 1000 / count) +
                                ",\"surface_download_calls\":" + std::to_string(current.surface_download_calls - previous_counters.surface_download_calls) +
                                ",\"surface_download_bytes\":" + std::to_string(current.surface_download_bytes - previous_counters.surface_download_bytes) +
                                ",\"cpu_surface_invalidations\":" + std::to_string(current.cpu_surface_invalidations - previous_counters.cpu_surface_invalidations) +
                                ",\"shader_compile_calls\":" + std::to_string(current.shader_compile_calls - previous_counters.shader_compile_calls) +
                                ",\"lit_draws\":" + std::to_string(current.lit_draws - previous_counters.lit_draws) +
                                ",\"active_light_sum\":" + std::to_string(current.active_light_sum - previous_counters.active_light_sum) +
                                ",\"vertex_upload_fallbacks\":" + std::to_string(current.vertex_upload_fallbacks - previous_counters.vertex_upload_fallbacks) +
                                ",\"vertex_storage_allocations\":" + std::to_string(current.vertex_storage_allocations - previous_counters.vertex_storage_allocations) +
                                ",\"vertex_pool_peak_bytes\":" + std::to_string(current.vertex_pool_peak_bytes) +
                                ",\"guest_pc\":[" + std::to_string(current.last_guest_pc[0]) + "," + std::to_string(current.last_guest_pc[1]) + "," + std::to_string(current.last_guest_pc[2]) + "," + std::to_string(current.last_guest_pc[3]) + "]" +
                                ",\"guest_core_ticks\":[" + std::to_string(current.guest_core_ticks[0] - previous_counters.guest_core_ticks[0]) + "," + std::to_string(current.guest_core_ticks[1] - previous_counters.guest_core_ticks[1]) + "," + std::to_string(current.guest_core_ticks[2] - previous_counters.guest_core_ticks[2]) + "," + std::to_string(current.guest_core_ticks[3] - previous_counters.guest_core_ticks[3]) + "]" +
                                ",\"guest_ticks\":" + std::to_string(current.guest_ticks - previous_counters.guest_ticks) +
                                ",\"readback_ms_per_frame\":" + std::to_string((current.readback_seconds - previous_counters.readback_seconds) * 1000 / count) +
                                ",\"present_ms_per_frame\":" + std::to_string((current.present_seconds - previous_counters.present_seconds) * 1000 / count) +
                                ",\"shader_calls\":" + std::to_string(current.shader_calls - previous_counters.shader_calls) +
                                ",\"gpu_vertex_draws\":" + std::to_string(current.gpu_vertex_draws - previous_counters.gpu_vertex_draws) +
                                ",\"gpu_vertices\":" + std::to_string(current.gpu_vertices - previous_counters.gpu_vertices) +
                                ",\"cpu_vertex_draws\":" + std::to_string(current.cpu_vertex_draws - previous_counters.cpu_vertex_draws) + "}");
                            previous_counters = current;
                            frame_times.clear();
                            std::ofstream timing_report(user + "frame-timing.json");
                            timing_report << "{\"title_id\":" << system.GetTitleID() << ",\"instrumented\":true,\"uncapped\":" << (pace_frames ? "false" : "true") << ",\"windows\":[";
                            for (size_t i = 0; i < timing_windows.size(); ++i)
                                timing_report << (i ? "," : "") << timing_windows[i];
                            timing_report << "]}\n";
                        }
                    }
                    if (clock_control.Enabled() && emuWindow->frames!=clock_frame) {
                        const auto now=std::chrono::steady_clock::now();
                        const unsigned advanced=emuWindow->frames-clock_frame;
                        const double elapsed=std::chrono::duration<double>(now-clock_frame_time).count();
                        const unsigned old_percent=clock_control.Percent();
                        if (advanced<=4) {
                            for (unsigned frame=0;frame<advanced;++frame)
                                clock_control.ObserveFrame(clock_work_seconds/advanced,elapsed/advanced);
                        } else clock_control.ResetSamples();
                        if (clock_control.Percent()!=old_percent) {
                            system.CoreTiming().UpdateClockSpeed(clock_control.Percent());
                            self->guestClock=clock_control.Percent();
                            LOG_INFO(Frontend,"Adaptive guest CPU: {}%",clock_control.Percent());
                        }
                        clock_frame=emuWindow->frames; clock_frame_time=now; clock_work_seconds=0;
                    }
                    if (pace_frames && emuWindow->frames != paced_frame) {
                        paced_frame = emuWindow->frames;
                        next_frame_time += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / 59.833997376556916));
                        const auto now = std::chrono::steady_clock::now();
                        if (now > next_frame_time + std::chrono::milliseconds(1)) ++late_paced_frames;
                        if (now - next_frame_time > std::chrono::milliseconds(100)) next_frame_time = now;
                        std::this_thread::sleep_until(next_frame_time);
                    }
                }
                if (target_frames) {
                    const double wall_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - benchmark_start).count();
                    const double pause_elapsed = std::chrono::duration<double>(paused_duration).count();
                    const double elapsed = std::max(0.001, wall_elapsed - pause_elapsed);
                    std::ofstream report(user + "benchmark-status.json");
                    report << "{\"title_id\":" << system.GetTitleID() << ",\"frames\":" << emuWindow->frames << ",\"target_frames\":" << target_frames << ",\"result_code\":" << unsigned(result) << ",\"wall_seconds\":" << wall_elapsed << ",\"paused_seconds\":" << pause_elapsed << ",\"elapsed_seconds\":" << elapsed
                           << ",\"fps\":" << emuWindow->frames / elapsed
                           << ",\"paced\":" << (pace_frames ? "true" : "false")
                           << ",\"late_paced_frames\":" << late_paced_frames
                           << ",\"cpu_backend\":\"" << ((std::getenv("CARA_CPU_BACKEND") && std::strcmp(std::getenv("CARA_CPU_BACKEND"), "dyncom") == 0) ? "dyncom" : "fastinterp") << "\""
                           << ",\"adaptive_guest_cpu\":" << (clock_control.Enabled()?"true":"false")
                           << ",\"guest_cpu_percent\":" << clock_control.Percent()
                           << ",\"new_3ds\":" << (s.is_new_3ds ? "true" : "false")
#ifdef CARA_IOS_GLES
                           << ",\"gpu_vertex_draws\":" << Common::FrameTiming::counters.gpu_vertex_draws
                           << ",\"gpu_vertices\":" << Common::FrameTiming::counters.gpu_vertices
                           << ",\"cpu_vertex_draws\":" << Common::FrameTiming::counters.cpu_vertex_draws
                           << ",\"jit\":false,\"software_renderer\":false,\"gpu_backend\":\"native_gles3\"}\n";
#else
                           << ",\"jit\":false,\"software_renderer\":true}\n";
#endif
                }
                system.Shutdown();
                [saveSync captureCommittedSave];
            }
#ifdef CARA_PGO_GENERATE
            const int pgo_status=__llvm_profile_write_file();
            std::ofstream(user+"cpu-training-status.json") << "{\"write_status\":" << pgo_status << "}\n";
            __llvm_profile_set_filename("/dev/null");
#endif
            AudioCore::SetIOSAudioPaused(true);
            emuWindow->DoneCurrent();
            running = false;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self->running || self->sessionGeneration!=session) return;
                const bool keepAwake = target_frames && std::getenv("CARA_TEST_KEEP_AWAKE");
                UIApplication.sharedApplication.idleTimerDisabled = keepAwake;
                if (keepAwake) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 900 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                    if (!self->running) UIApplication.sharedApplication.idleTimerDisabled = NO;
                });
                if (result != Core::System::ResultStatus::Success)
                    self->status.text = [NSString stringWithFormat:@"Emulation ended (code %u). See emulator.log",unsigned(result)];
                else self->status.text = @"Stopped";
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
    for (UIOpenURLContext* context in options.URLContexts) [(EmulatorController*)self.window.rootViewController openExternalURL:context.URL];
}
- (void)scene:(UIScene*)scene openURLContexts:(NSSet<UIOpenURLContext*>*)contexts {
    for (UIOpenURLContext* context in contexts) [(EmulatorController*)self.window.rootViewController openExternalURL:context.URL];
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
