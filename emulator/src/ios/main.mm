// SPDX-License-Identifier: GPL-2.0-or-later
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#include <sys/qos.h>
#include <sys/resource.h>
#include <time.h>
#import "ios/skin_controls.h"
#include "audio_core/ios_sink.h"
#ifdef CARA_PGO_GENERATE
extern "C" void __llvm_profile_set_filename(const char*);
extern "C" int __llvm_profile_write_file(void);
#endif
#include "audio_core/dsp_interface.h"
#import "ios/save_sync.h"
#import "ios/shell_controllers.h"
#import "ios/state_catalog.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#ifdef CARA_IOS_GLES
#import <OpenGLES/EAGL.h>
#import <QuartzCore/CAEAGLLayer.h>
#include <dlfcn.h>
#include <glad/glad.h>
#include "video_core/renderer_opengl/gl_state.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/video_core.h"
#include "ios/gpu_vertex_self_test.h"
#include "ios/gpu_state_self_test.h"
#include "ios/gpu_depth_self_test.h"
#endif
#include <atomic>
#include <array>
#include "common/save_events.h"
#include "ios/button_state.h"
#include "cara/adaptive_clock.h"
#include "cara/frame_pacer.h"
#include "ios/play_profile.h"
#include "ios/session_presentation.h"
#include <algorithm>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "common/scm_rev.h"
#include <functional>
#include <fstream>
#include <iomanip>
#include <thread>
#include <sstream>
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/frame_timing.h"
#include "common/cpu_category_timing.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "core/core.h"
#include "core/arm/fastinterp/fastinterp.h"
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
    // Owner-thread only. Call after diagnostic readback, while the context is current.
    void ClearDisplay() {
#ifdef CARA_IOS_GLES
        if (!framebuffer || [EAGLContext currentContext] != graphics_context) return;
        const auto previous_draw=OpenGL::OpenGLState::BindDrawFramebuffer(framebuffer);
        const auto previous_renderbuffer=OpenGL::OpenGLState::BindRenderbuffer(renderbuffer);
        const bool scissor=glIsEnabled(GL_SCISSOR_TEST);
        const bool discard=glIsEnabled(GL_RASTERIZER_DISCARD);
        GLboolean mask[4]; GLfloat clear[4];
        glGetBooleanv(GL_COLOR_WRITEMASK,mask); glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);
        glDisable(GL_SCISSOR_TEST); glDisable(GL_RASTERIZER_DISCARD);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
        if (drawable_framebuffer) {
            OpenGL::OpenGLState::BindDrawFramebuffer(drawable_framebuffer);
            OpenGL::OpenGLState::BindRenderbuffer(drawable_renderbuffer);
            glClear(GL_COLOR_BUFFER_BIT);
            [graphics_context presentRenderbuffer:GL_RENDERBUFFER];
        }
        glClearColor(clear[0],clear[1],clear[2],clear[3]);
        glColorMask(mask[0],mask[1],mask[2],mask[3]);
        if (scissor) glEnable(GL_SCISSOR_TEST);
        if (discard) glEnable(GL_RASTERIZER_DISCARD);
        OpenGL::OpenGLState::BindRenderbuffer(previous_renderbuffer);
        OpenGL::OpenGLState::BindDrawFramebuffer(previous_draw);
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

static const char* CaraSelectedCPUBackend() {
    const char* backend=std::getenv("CARA_CPU_BACKEND");
    return backend && std::strcmp(backend,"dyncom")==0 ? "dyncom" : "fastinterp";
}

static BOOL CaraCanTuneEmulation() {
    return !std::getenv("CARA_BENCHMARK_FRAMES") && !std::getenv("CARA_ADAPTIVE_GUEST_CPU") && !std::getenv("CARA_MAX_SPEED_PERCENT");
}
static NSDictionary* CaraGameOverrides;
static id CaraEmulationValue(NSString* key) { return CaraGameOverrides[key]?:[NSUserDefaults.standardUserDefaults objectForKey:key]; }
static Cara::PlayPolicy CaraSavedPlayPolicy() {
    const NSInteger saved=[CaraEmulationValue(@"maximumSpeedPercent") integerValue];
    const unsigned maximum=saved>=50 && saved<=200?unsigned(saved):100;
    const char* override=std::getenv("CARA_ADAPTIVE_GUEST_CPU");
    return Cara::MakePlayPolicy([CaraEmulationValue(@"playMode") isEqual:@"accuracy"]?Cara::PlayMode::Accuracy:Cara::PlayMode::Performance,
        maximum,std::getenv("CARA_BENCHMARK_FRAMES")!=nullptr,override?(std::strcmp(override,"1")==0?1:0):-1,
        [CaraEmulationValue(@"fixedGuestClockEnabled") boolValue],unsigned(std::clamp<NSInteger>([CaraEmulationValue(@"fixedGuestClockPercent") integerValue],25,200)),
        [CaraEmulationValue(@"speedLimitEnabled") boolValue]);
}

@interface EmulatorController : UIViewController <UIDocumentPickerDelegate>
- (NSUInteger)maximumSpeedPercent;
- (void)setMaximumSpeedPercent:(NSUInteger)percent;
- (void)showMaximumSpeed;
@end
@implementation EmulatorController {
    NSString* pendingLibraryState;
    UIImageView* top;
    UIImageView* bottom;
#ifdef CARA_IOS_GLES
    CaraGPUView* gpuSurface;
#endif
    UILabel* status;
    UIImageView* skinArt;
    CaraSkinControls* skinControls;
    CaraSkin* activeSkin;
    CaraSkin* portraitSkin;
    CaraSkin* landscapeSkin;
    CaraStatsView* performanceHUD;
    CaraSessionIndicator* statusIndicator;
    NSString* pickingSkinOrientation;
    NSDictionary* currentRepresentation;
    UIButton* menuFallback;
    BOOL pickingSkin;
    BOOL pickingSaveFolder;
    BOOL openingGame;
    BOOL launchingOffline;
    CaraSaveSync* saveSync;
    CaraSaveSync* stateSync;
    CaraLibraryStore* libraryStore;
    CaraLibraryController* libraryController;
    UINavigationController* libraryNavigation;
    __weak CaraSettingsController* settingsController;
    NSTimer* cloudFeedbackTimer;
    BOOL libraryVisible;
    BOOL pickingROMsFolder;
    NSURL* gameURL;
    BOOL gameScoped;
    BOOL modalPaused;
    uint32_t uiButtonMask;
    CGSize lastSkinSize;
    std::unique_ptr<IOSWindow> emuWindow;
    std::thread worker;
    std::atomic<bool> stop;
    std::atomic<bool> running;
    std::atomic<bool> gameLoaded;
    NSString* sessionFailure;
    NSString* sessionNotice;
    std::atomic<bool> stretchAudio;
    std::atomic<bool> stretchAudioDirty;
    std::atomic<bool> adaptiveCPU;
    std::atomic<bool> adaptiveCPUDirty;
    std::atomic<unsigned> guestClock;
    std::atomic<unsigned> maximumSpeedLimit;
    std::atomic<bool> speedLimitEnabled;
    std::atomic<unsigned> fixedGuestClock;
    NSString* settingsCategory;
    std::atomic<bool> framePending[2];
    unsigned frameCount;
    unsigned sessionGeneration;
    CFTimeInterval startedAt;
    CFTimeInterval backgroundAt;
    CFTimeInterval lastHUDTime;
    unsigned lastHUDFrame;
    bool autoStarted;
}
- (BOOL)prefersStatusBarHidden { return !libraryVisible; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    return UIInterfaceOrientationMaskAllButUpsideDown;
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
- (void)reloadSelectedSkins {
    NSUserDefaults* defaults=NSUserDefaults.standardUserDefaults;
    NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    CaraSkin* builtIn=[CaraSkin loadURL:[self defaultSkinURL] error:nil];
    const BOOL pad=UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad;
    NSString* legacy=[defaults stringForKey:@"controllerSkin"];
    if (![defaults boolForKey:@"orientationSkinPreferencesMigrated"]) {
        CaraSkin* old=legacy?[CaraSkin loadURL:[NSURL fileURLWithPath:[documents stringByAppendingPathComponent:legacy]] error:nil]:nil;
        for (NSString* orientation in @[@"Portrait",@"Landscape"]) {
            if ([old strictRepresentationForPad:pad landscape:[orientation isEqual:@"Landscape"]]) [defaults setObject:legacy forKey:[@"controllerSkin" stringByAppendingString:orientation]];
        }
        [defaults setBool:YES forKey:@"orientationSkinPreferencesMigrated"];
    }
    for (NSString* orientation in @[@"Portrait",@"Landscape"]) {
        NSString* file=[defaults stringForKey:[@"controllerSkin" stringByAppendingString:orientation]];
        const char* test=std::getenv("CARA_TEST_SKIN"); if (test) file=[NSString stringWithUTF8String:test].lastPathComponent;
        CaraSkin* skin=file?[CaraSkin loadURL:[NSURL fileURLWithPath:[documents stringByAppendingPathComponent:file]] error:nil]:nil;
        const BOOL landscape=[orientation isEqual:@"Landscape"];
        if (![skin strictRepresentationForPad:pad landscape:landscape]) skin=builtIn;
        if (landscape) landscapeSkin=skin; else portraitSkin=skin;
    }
    [self applySkin:self.view.bounds.size.width>self.view.bounds.size.height?landscapeSkin:portraitSkin];
}
- (void)resetSkinForOrientation:(NSString*)orientation {
    [NSUserDefaults.standardUserDefaults removeObjectForKey:[@"controllerSkin" stringByAppendingString:orientation]];
    [self reloadSelectedSkins]; [self resumeFromMenu];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    NSString* rom=[[NSUserDefaults.standardUserDefaults volatileDomainForName:NSArgumentDomain] objectForKey:@"rom"];
    if (!autoStarted && !running && std::getenv("CARA_OPEN_RECENT")) {
        autoStarted=true; [self openRecentGame]; return;
    }
    if (!autoStarted && rom.length && !running) {
        autoStarted=true;
        [self ensureLibrary];
        NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
        NSURL* url=[NSURL fileURLWithPath:[documents stringByAppendingPathComponent:rom.lastPathComponent]];
        if (![NSFileManager.defaultManager fileExistsAtPath:url.path])
            url=[libraryStore.defaultFolder URLByAppendingPathComponent:rom.lastPathComponent];
        if ([NSFileManager.defaultManager fileExistsAtPath:url.path]) [self openGameURL:url];
        else status.text=@"Game file not found.";
    }
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor=UIColor.blackColor;
#ifdef CARA_IOS_GLES
    // Explicit launch overrides retain reproducible renderer comparisons.
    if (!std::getenv("CARA_GPU_PACKED_INPUTS")) setenv("CARA_GPU_PACKED_INPUTS","1",0);
    // Keep eligible tiled/linear RGBA8 aliases on the GPU; other layouts fall back.
    if (!std::getenv("CARA_GPU_SURFACE_REMAP")) setenv("CARA_GPU_SURFACE_REMAP","1",0);
    if (!std::getenv("CARA_VS_UNIFORM_REUSE")) setenv("CARA_VS_UNIFORM_REUSE","1",0);
    if (!std::getenv("CARA_FASTINTERP_ALU_OPERANDS")) setenv("CARA_FASTINTERP_ALU_OPERANDS","1",0);
    if (!std::getenv("CARA_FASTINTERP_TICK_BUCKETS")) setenv("CARA_FASTINTERP_TICK_BUCKETS","1",0);
    if (!std::getenv("CARA_FASTINTERP_CHAIN_BUDGET")) setenv("CARA_FASTINTERP_CHAIN_BUDGET","1",0);
    if (!std::getenv("CARA_VERTEX_UPLOAD_BANK_MIB") &&
        std::strcmp(std::getenv("CARA_GPU_PACKED_INPUTS"),"1")==0 &&
        NSProcessInfo.processInfo.physicalMemory>=6ULL*1024*1024*1024)
        setenv("CARA_VERTEX_UPLOAD_BANK_MIB","80",0);
    // Exact upload shadows trade bounded host memory for fewer driver writes.
    if (!std::getenv("CARA_VERTEX_UPLOAD_REUSE") &&
        std::strcmp(std::getenv("CARA_GPU_PACKED_INPUTS"),"1")==0 &&
        NSProcessInfo.processInfo.physicalMemory>=6ULL*1024*1024*1024)
        setenv("CARA_VERTEX_UPLOAD_REUSE","1",0);
    if (!std::getenv("CARA_VERTEX_INDEX_CACHE") &&
        std::strcmp(std::getenv("CARA_GPU_PACKED_INPUTS"),"1")==0 &&
        NSProcessInfo.processInfo.physicalMemory>=6ULL*1024*1024*1024)
        setenv("CARA_VERTEX_INDEX_CACHE","1",0);
#endif
    const bool benchmarkMute=std::getenv("CARA_BENCHMARK_FRAMES") && !std::getenv("CARA_BENCHMARK_AUDIO");
    AudioCore::SetIOSAudioMuted(benchmarkMute || [NSUserDefaults.standardUserDefaults boolForKey:@"audioMuted"]);
    stretchAudio=[NSUserDefaults.standardUserDefaults boolForKey:@"audioStretching"]; stretchAudioDirty=false;
    NSString* mode=[NSUserDefaults.standardUserDefaults stringForKey:@"playMode"];
    if (![@[@"accuracy",@"performance"] containsObject:mode]) mode=@"performance";
    [NSUserDefaults.standardUserDefaults setObject:mode forKey:@"playMode"];
    [NSUserDefaults.standardUserDefaults registerDefaults:@{@"maximumSpeedPercent":@100,@"audioStretching":@NO,@"fixedGuestClockEnabled":@NO,@"fixedGuestClockPercent":@100,@"speedLimitEnabled":@YES}];
    const auto policy=CaraSavedPlayPolicy();
    adaptiveCPU=policy.adaptive; fixedGuestClock=policy.guest_clock; maximumSpeedLimit=policy.maximum_speed; speedLimitEnabled=policy.limit_speed;
    adaptiveCPUDirty=false; guestClock=policy.guest_clock;
    NSURL* saveDocuments=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    saveSync=[[CaraSaveSync alloc] initWithRoot:[saveDocuments URLByAppendingPathComponent:@"cara" isDirectory:YES]];
    stateSync=[[CaraSaveSync alloc] initWithRoot:[saveDocuments URLByAppendingPathComponent:@"cara" isDirectory:YES] statesOnly:YES];
    stop=false; running=false; gameLoaded=false; sessionGeneration=0; framePending[0]=false; framePending[1]=false;
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
    const char* inputLock=std::getenv("CARA_BENCHMARK_INPUT_LOCK");
    const bool benchmarkInputLock=std::getenv("CARA_BENCHMARK_FRAMES") && inputLock && std::strcmp(inputLock,"1")==0;
    skinControls.inputChanged=^(NSSet<NSString*>* pressed,CGPoint circle,CGPoint cstick,CGPoint touch) {
        EmulatorController* controller=weakSelf; if (!controller) return;
        // Scripted benchmarks own game inputs; the menu remains available.
        if (benchmarkInputLock && controller->running) return;
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
    performanceHUD=[[CaraStatsView alloc] initWithFrame:CGRectZero]; [self.view addSubview:performanceHUD];
    status=[UILabel new]; status.text=@"Stopped";
    status.font=[UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightMedium]; status.textColor=[UIColor colorWithRed:.7 green:.8 blue:.7 alpha:1];
    status.numberOfLines=2; status.userInteractionEnabled=NO; status.hidden=YES;
    statusIndicator=[CaraSessionIndicator new]; [self.view addSubview:statusIndicator];
    menuFallback=[UIButton buttonWithType:UIButtonTypeSystem]; menuFallback.configuration=[UIButtonConfiguration filledButtonConfiguration];
    [menuFallback setTitle:@"Menu" forState:UIControlStateNormal]; [menuFallback addTarget:self action:@selector(showMenu) forControlEvents:UIControlEventTouchUpInside]; [self.view addSubview:menuFallback];
    [self reloadSelectedSkins];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(background) name:UIApplicationWillResignActiveNotification object:nil];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(foreground) name:UIApplicationDidBecomeActiveNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(syncFeedbackChanged:) name:CaraSaveSyncDidChangeNotification object:nil];
    NSString* launchROM=[[NSUserDefaults.standardUserDefaults volatileDomainForName:NSArgumentDomain] objectForKey:@"rom"];
    if (!launchROM.length && !std::getenv("CARA_OPEN_RECENT") && !std::getenv("CARA_BENCHMARK_FRAMES")) [self showLibrary];
    if (!std::getenv("CARA_BENCHMARK_FRAMES")) {
        __weak EmulatorController* owner=self;
        cloudFeedbackTimer=[NSTimer scheduledTimerWithTimeInterval:15 repeats:YES block:^(NSTimer* timer) {
            EmulatorController* controller=owner; if (!controller) { [timer invalidate]; return; }
            [controller->saveSync refreshCloudStatus]; [controller->stateSync refreshCloudStatus];
        }];
    }
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    libraryNavigation.view.frame=self.view.bounds;
#ifdef CARA_IOS_GLES
    gpuSurface.frame=self.view.bounds;
#endif
    const BOOL landscape=self.view.bounds.size.width>self.view.bounds.size.height;
    CaraSkin* selected=landscape?landscapeSkin:portraitSkin;
    if (selected && selected!=activeSkin) { activeSkin=selected; skinControls.skin=selected; currentRepresentation=nil; }
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
    CGRect statsFrame=CaraSkinRect(rep[@"caraStatsFrame"]);
    if (statsFrame.size.width<=0 || statsFrame.size.height<=0) statsFrame=CGRectMake(52,50,430,102);
    const CaraHUDLayout hud=CaraSkinHUDLayout(rep,canvas);
    performanceHUD.frame=hud.metrics;
    NSString* statsKey=landscape?@"landscapeStatsVisible":@"portraitStatsVisible";
    performanceHUD.hidden=[NSUserDefaults.standardUserDefaults objectForKey:statsKey] && ![NSUserDefaults.standardUserDefaults boolForKey:statsKey];
    status.frame=landscape?convert(CGRectMake(statsFrame.origin.x,CGRectGetMaxY(statsFrame)+8,statsFrame.size.width,54)):convert(CGRectMake((mapping.width-600)/2,mapping.height-170,600,48));
    status.numberOfLines=landscape?2:1;
    statusIndicator.frame=hud.indicator;
    status.adjustsFontSizeToFitWidth=YES; status.minimumScaleFactor=0.7;
#ifdef CARA_IOS_GLES
    if (emuWindow) emuWindow->SetDisplayLayout(top.frame,bottom.frame,self.view.bounds.size,gpuSurface.contentScaleFactor);
#endif
}
- (NSUInteger)maximumSpeedPercent { return maximumSpeedLimit.load(); }
- (void)setMaximumSpeedPercent:(NSUInteger)percent {
    if (!CaraCanTuneEmulation() || [[NSUserDefaults.standardUserDefaults stringForKey:@"playMode"] isEqual:@"accuracy"]) return;
    const unsigned value=unsigned(std::clamp<NSUInteger>(percent,Cara::FramePacer::MinSpeedPercent,Cara::FramePacer::MaxSpeedPercent));
    [NSUserDefaults.standardUserDefaults setInteger:value forKey:@"maximumSpeedPercent"];
    [self applySavedPlayPolicy];
}
- (void)showMaximumSpeed {
    UIAlertController* limits=[UIAlertController alertControllerWithTitle:@"Maximum speed" message:@"Limits game speed. 100% is the default." preferredStyle:UIAlertControllerStyleAlert];
    for (NSUInteger percent : {100u,105u,125u,150u,200u}) {
        NSString* title=[NSString stringWithFormat:@"%@%lu%%%@",maximumSpeedLimit.load()==percent?@"✓ ":@"",(unsigned long)percent,percent==100?@" (Default)":@""];
        [limits addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) {
            [self setMaximumSpeedPercent:percent]; [self resumeFromMenu];
        }]];
    }
    [limits addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction* action) { [self resumeFromMenu]; }]];
    [self presentViewController:limits animated:YES completion:nil];
}
- (Cara::SessionPresentation)sessionPresentation {
    return Cara::ResolveSessionPresentation(openingGame,running.load(),gameLoaded.load(),stop.load(),paused.load(),sessionFailure!=nil);
}
- (void)updateSessionPresentation {
    const auto state=[self sessionPresentation];
    NSString* label=[NSString stringWithUTF8String:Cara::SessionPresentationLabel(state)];
    if (state==Cara::SessionPresentation::Failed) label=sessionFailure;
    else if (sessionNotice && !Cara::ClearSessionPresentation(state)) label=[label stringByAppendingFormat:@" · %@",sessionNotice];
    status.text=label;
    const auto light=state==Cara::SessionPresentation::Running?CaraStatusLightGreen:
        state==Cara::SessionPresentation::Failed?CaraStatusLightRed:
        state==Cara::SessionPresentation::Stopped?CaraStatusLightDim:CaraStatusLightAmber;
    [statusIndicator updateLight:light accessibilityStatus:label];
    if (Cara::ClearSessionPresentation(state)) {
        top.image=nil; bottom.image=nil;
        [performanceHUD showInactiveGuestClock:fixedGuestClock.load() adaptive:adaptiveCPU.load()];
    }
#ifdef CARA_IOS_GLES
    gpuSurface.hidden=Cara::ClearSessionPresentation(state);
#endif
}
- (void)resumeFromMenu {
    modalPaused=libraryVisible; paused=modalPaused;
    AudioCore::SetIOSAudioPaused(modalPaused || stop || !running);
    [self updateSessionPresentation];
}
- (void)ensureLibrary {
    if (libraryStore) return;
    NSURL* documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    libraryStore=[[CaraLibraryStore alloc] initWithDocuments:documents defaults:NSUserDefaults.standardUserDefaults];
}
- (void)showLibrary {
    if (openingGame) return;
    [self ensureLibrary];
    if (!libraryController) {
        libraryController=[CaraLibraryController new]; libraryController.store=libraryStore;
        __weak EmulatorController* owner=self;
        libraryController.openGame=^(NSURL* url) { EmulatorController* c=owner; if(c) c->pendingLibraryState=nil; [owner openGameURL:url]; };
        libraryController.addGame=^{ EmulatorController* controller=owner; if (!controller) return; controller->pickingSkin=NO; [controller openROM]; };
        libraryController.chooseFolder=^{ [owner chooseROMsFolder]; };
        libraryController.showSettings=^{ [owner showFullSettings]; };
        libraryController.statesForGame=^NSArray*(NSURL* url) { return [owner libraryStatesForGame:url]; };
        libraryController.startFromState=^(NSURL* url,NSString* filename) { EmulatorController* c=owner; if(!c) return; c->pendingLibraryState=filename; [c openGameURL:url]; };
        libraryController.gameSettings=^(NSURL* url) { [owner showGameSettings:url]; };
        libraryController.clearGameSettings=^(NSURL* url) { EmulatorController* c=owner; if(!c) return; [c->libraryStore setMetadata:@{@"overrides":@{}} forGame:url]; if([c->gameURL.URLByStandardizingPath isEqual:url.URLByStandardizingPath]) { CaraGameOverrides=nil; [c applySavedPlayPolicy]; c->stretchAudio=[CaraEmulationValue(@"audioStretching") boolValue]; c->stretchAudioDirty=true; } };
        libraryController.resumeGame=^{ [owner hideLibrary]; };
        libraryNavigation=[[UINavigationController alloc] initWithRootViewController:libraryController];
        [self addChildViewController:libraryNavigation]; libraryNavigation.view.frame=self.view.bounds;
        libraryNavigation.view.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
        [self.view addSubview:libraryNavigation.view]; [libraryNavigation didMoveToParentViewController:self];
    }
    libraryVisible=YES; modalPaused=YES; paused=true; AudioCore::SetIOSAudioPaused(true); [self resetSkinInput];
    [self updateSessionPresentation];
    libraryNavigation.view.hidden=NO; [self.view bringSubviewToFront:libraryNavigation.view]; libraryController.canResume=running && !stop; libraryController.activeGameURL=gameURL;
    [libraryController reloadLibrary]; [self updateSyncFeedback];
    [self setNeedsStatusBarAppearanceUpdate];
    if (@available(iOS 16.0,*)) [self setNeedsUpdateOfSupportedInterfaceOrientations];
}
- (void)hideLibrary {
    libraryVisible=NO; libraryNavigation.view.hidden=YES; [libraryController suspendArtwork];
    [self resumeFromMenu]; [self setNeedsStatusBarAppearanceUpdate];
    if (@available(iOS 16.0,*)) [self setNeedsUpdateOfSupportedInterfaceOrientations];
}
- (void)shellError:(NSString*)title message:(NSString*)message {
    status.text=message;
    UIAlertController* alert=[UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self resumeFromMenu]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)chooseROMsFolder {
    if (running || saveSync.busy || stateSync.busy) { [self shellError:@"ROMs folder" message:@"Stop the game and wait for syncing to finish before changing folders."]; return; }
    pickingROMsFolder=YES;
    UIDocumentPickerViewController* picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO]; picker.delegate=self;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)syncFeedbackChanged:(NSNotification*)notification {
    if (notification.object==saveSync || notification.object==stateSync) [self updateSyncFeedback];
}
- (void)updateSyncFeedback {
    if (!NSThread.isMainThread) { dispatch_async(dispatch_get_main_queue(),^{ [self updateSyncFeedback]; }); return; }
    BOOL busy=saveSync.busy || stateSync.busy;
    CaraSaveSync* attention=nil;
    for (CaraSaveSync* channel in @[saveSync,stateSync]) if ([channel.phase isEqual:@"error"] || [channel.phase isEqual:@"conflict"]) { attention=channel; break; }
    libraryController.syncing=busy;
    if (attention) {
        libraryController.syncTitle=[NSString stringWithFormat:@"%@: %@",attention.statesOnly?@"States":@"Saves",attention.status]; libraryController.syncDetail=attention.detail;
    } else if (busy) {
        libraryController.syncTitle=@"Syncing your progress…";
        libraryController.syncDetail=[NSString stringWithFormat:@"Saves: %@ · States: %@",saveSync.status,stateSync.status];
    } else if (!saveSync.enabled && !stateSync.enabled) {
        libraryController.syncTitle=@"Your progress, kept close";
        libraryController.syncDetail=@"Connect iCloud Drive in Settings to sync saves and states.";
    } else {
        libraryController.syncTitle=(saveSync.cloudUploadVerified && stateSync.cloudUploadVerified)?@"Uploaded to iCloud":@"Saves & states";
        libraryController.syncDetail=[NSString stringWithFormat:@"Saves: %@ · States: %@",saveSync.status,stateSync.status];
    }
    if (settingsController) { settingsController.sections=[self settingsForCategory:settingsCategory]; [settingsController refresh]; }
}
- (NSArray*)settingsSections {
    __weak EmulatorController* owner=self;
    const BOOL stopped=!running && !openingGame;
    const BOOL available=stopped && !saveSync.busy && !stateSync.busy;
    const auto action=[](NSString* title,NSString* symbol,BOOL enabled,void(^block)(void)) { return @{@"title":title,@"symbol":symbol,@"enabled":@(enabled),@"action":block}; };
    NSMutableArray* sections=[NSMutableArray new];
    if (running) {
        NSMutableArray* game=[NSMutableArray arrayWithArray:@[
            action(@"Game library",@"square.grid.2x2",YES,^{ [owner showLibrary]; }),
            action(@"Save state…",@"square.and.arrow.down",YES,^{ [owner showStateSlots:YES]; }),
            action(@"Load state…",@"arrow.counterclockwise",YES,^{ [owner showStateSlots:NO]; }),
        ]];
        if (std::getenv("CARA_PROFILE_CHECKPOINT")) [game addObject:action(@"Capture comparison checkpoint",@"camera.viewfinder",YES,^{ EmulatorController* controller=owner; if (controller) stateCommand.store(101,std::memory_order_release); })];
        NSMutableDictionary* stopRow=[action(@"Stop game",@"stop.circle",YES,^{ [owner stopGame]; [owner showLibrary]; }) mutableCopy]; stopRow[@"destructive"]=@YES; [game addObject:stopRow];
        [sections addObject:@{@"title":@"Current game",@"rows":game,@"footer":@"Keep using in-game saves. Save states require a matching Cara engine build, game revision, and system mode."}];
    }
    NSMutableArray* locations=[NSMutableArray arrayWithObject:@{@"title":@"ROMs folder",@"subtitle":libraryStore.folderName?:@"Roms",@"symbol":@"folder",@"enabled":@(available),@"action":^{ [owner chooseROMsFolder]; }}];
    [locations addObject:action(@"Open game from Files…",@"doc.badge.plus",!openingGame,^{ EmulatorController* c=owner; if (c) { c->pickingSkin=NO; [c openROM]; } })];
    if (libraryStore.hasCustomFolder) [locations addObject:action(@"Use Cara’s Roms folder",@"folder.fill",available,^{ EmulatorController* c=owner; if (c) { [c->libraryStore useDefaultFolder]; [c->libraryController reloadLibrary]; [c resumeFromMenu]; } })];
    [sections addObject:@{@"title":@"Game library",@"rows":locations,@"footer":@"Cara creates Roms in its own Files folder. You can also choose a folder elsewhere; games stay in their original locations. Stop the game before changing folders."}];
    NSMutableArray* cloud=[NSMutableArray new];
    for (CaraSaveSync* channel in @[saveSync,stateSync]) {
        NSString* label=channel.statesOnly?@"Save states":@"In-game saves";
        NSString* last=channel.lastFolderWrite?[NSDateFormatter localizedStringFromDate:channel.lastFolderWrite dateStyle:NSDateFormatterShortStyle timeStyle:NSDateFormatterShortStyle]:nil;
        NSString* detail=[NSString stringWithFormat:@"%@\n%@%@",channel.status,channel.detail,last?[@"\nFolder updated " stringByAppendingString:last]:@""];
        [cloud addObject:@{@"title":label,@"subtitle":detail,@"symbol":[channel.phase isEqual:@"error"]?@"exclamationmark.icloud":@"icloud"}];
    }
    [cloud addObject:action(@"Choose iCloud Drive folder…",@"folder.badge.gearshape",available,^{ EmulatorController* c=owner; if (!c) return; c->pickingSaveFolder=YES; UIDocumentPickerViewController* picker=[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeFolder] asCopy:NO]; picker.delegate=c; [c presentViewController:picker animated:YES completion:nil]; })];
    [cloud addObject:action(@"Sync now / resolve conflicts",@"arrow.triangle.2.circlepath.icloud",available && (saveSync.enabled||stateSync.enabled),^{ [owner syncSaves:nil completion:nil]; })];
    if (saveSync.enabled || stateSync.enabled) [cloud addObject:action(@"Disconnect sync folder",@"icloud.slash",!saveSync.busy && !stateSync.busy,^{ EmulatorController* c=owner; if (!c) return; [c->saveSync disconnect]; [c->stateSync disconnect]; [c resumeFromMenu]; })];
    [sections addObject:@{@"title":@"iCloud & Files",@"rows":cloud,@"footer":@"Choose the same iCloud Drive folder on each device. Stop the game to restore data or change folders. Save states need a matching Cara build."}];
    [sections addObject:@{@"title":@"Audio",@"rows":@[
        @{@"title":@"Sound",@"symbol":@"speaker.wave.2",@"value":@(!AudioCore::IsIOSAudioMuted()),@"change":^(BOOL on) { AudioCore::SetIOSAudioMuted(!on); [NSUserDefaults.standardUserDefaults setBool:!on forKey:@"audioMuted"]; }},
        @{@"title":@"Audio stretching",@"subtitle":@"Adjust audio when emulation speed varies.",@"symbol":@"waveform",@"value":@(stretchAudio.load()),@"change":^(BOOL on) { EmulatorController* c=owner; if (!c) return; c->stretchAudio=on; c->stretchAudioDirty=true; [NSUserDefaults.standardUserDefaults setBool:on forKey:@"audioStretching"]; }},
    ]}];
    [sections addObject:@{@"title":@"Play mode",@"rows":@[@{@"title":@"Accuracy mode",@"subtitle":@"Locks game speed and guest clock at 100%.",@"symbol":@"scope",@"value":@([[NSUserDefaults.standardUserDefaults stringForKey:@"playMode"] isEqual:@"accuracy"]),@"change":^(BOOL on) { [owner setPlayMode:on?@"accuracy":@"performance"]; [owner updateSyncFeedback]; }}]}];
    const BOOL accuracy=[[NSUserDefaults.standardUserDefaults stringForKey:@"playMode"] isEqual:@"accuracy"];
    const BOOL tuning=CaraCanTuneEmulation() && !accuracy;
    const BOOL fixed=accuracy || [NSUserDefaults.standardUserDefaults boolForKey:@"fixedGuestClockEnabled"];
    [sections addObject:@{@"title":@"Emulation",@"rows":@[
        @{@"title":@"Play mode",@"subtitle":accuracy?@"Accuracy · 100% guest CPU, 100% speed":@"Performance",@"symbol":@"speedometer"},
        @{@"title":@"Use fixed guest clock",@"subtitle":@"Turn off to let Cara adjust the guest clock automatically.",@"symbol":@"cpu",@"value":@(fixed),@"enabled":@(tuning),@"change":^(BOOL on) { [owner setEmulationPreference:@"fixedGuestClockEnabled" value:@(on) refresh:YES]; }},
        @{@"title":@"Guest clock",@"subtitle":@"25–200% · Normal guest clock: 100%.",@"enabled":@(tuning && fixed),@"slider":@{@"min":@25,@"max":@200,@"step":@5,@"value":@(accuracy?100:std::clamp<NSInteger>([NSUserDefaults.standardUserDefaults integerForKey:@"fixedGuestClockPercent"],25,200))},@"sliderChange":^(double value) { [owner setEmulationPreference:@"fixedGuestClockPercent" value:@(std::lround(value)) refresh:NO]; }},
        @{@"title":@"Limit game speed",@"subtitle":@"Turn off to run as fast as the device allows.",@"symbol":@"gauge.with.dots.needle.67percent",@"value":@(speedLimitEnabled.load()),@"enabled":@(tuning),@"change":^(BOOL on) { [owner setEmulationPreference:@"speedLimitEnabled" value:@(on) refresh:YES]; }},
        @{@"title":@"Maximum speed",@"subtitle":@"50–200% · Default limit: 100%.",@"enabled":@(tuning && speedLimitEnabled.load()),@"slider":@{@"min":@50,@"max":@200,@"step":@5,@"value":@(maximumSpeedLimit.load())},@"sliderChange":^(double value) { [owner setMaximumSpeedPercent:NSUInteger(std::lround(value))]; }},
        action(@"Reset emulation tuning",@"arrow.counterclockwise",tuning,^{ EmulatorController* c=owner; if (!c) return; for (NSString* key in @[@"fixedGuestClockEnabled",@"fixedGuestClockPercent",@"speedLimitEnabled",@"maximumSpeedPercent"]) [NSUserDefaults.standardUserDefaults removeObjectForKey:key]; [c applySavedPlayPolicy]; [c updateSyncFeedback]; [c resumeFromMenu]; }),
    ],@"footer":accuracy?@"Accuracy locks the guest clock at 100% and speed at 100%. Choose Performance to adjust these controls.":@"Guest clock and game speed are separate controls. Changes take effect when you resume."}];
    for (NSString* orientation in @[@"Landscape",@"Portrait"]) {
        NSString* key=[@"controllerSkin" stringByAppendingString:orientation];
        NSString* selected=[NSUserDefaults.standardUserDefaults stringForKey:key];
        NSString* statsKey=[orientation isEqual:@"Landscape"]?@"landscapeStatsVisible":@"portraitStatsVisible";
        BOOL showStats=![NSUserDefaults.standardUserDefaults objectForKey:statsKey] || [NSUserDefaults.standardUserDefaults boolForKey:statsKey];
        NSString* title=[orientation stringByAppendingString:@" skin"];
        [sections addObject:@{@"title":title,@"rows":@[
            @{@"title":@"Import skin…",@"subtitle":selected?:@"Built-in",@"symbol":[orientation isEqual:@"Portrait"]?@"rectangle.portrait":@"rectangle",@"action":^{ EmulatorController* c=owner; if (c) { c->pickingSkin=YES; c->pickingSkinOrientation=orientation; [c openROM]; } }},
            action(@"Use built-in skin",@"sun.max",YES,^{ [owner resetSkinForOrientation:orientation]; }),
            @{@"title":@"Performance bar",@"subtitle":@"Game FPS, emulation speed, and guest CPU clock.",@"symbol":@"chart.bar",@"value":@(showStats),@"change":^(BOOL on) { [NSUserDefaults.standardUserDefaults setBool:on forKey:statsKey]; [owner.view setNeedsLayout]; }},
        ],@"footer":@"Portrait and landscape keep their own skin and performance-bar settings."}];
    }
    return sections;
}
- (NSArray*)settingsForCategory:(NSString*)category {
    NSArray* all=[self settingsSections];
    if ([category isEqual:@"Library & iCloud"]) return [all filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSDictionary* section,NSDictionary* bindings) { return [@[@"Game library",@"iCloud & Files"] containsObject:section[@"title"]]; }]];
    if ([category isEqual:@"Skins"]) return [all filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:^BOOL(NSDictionary* section,NSDictionary* bindings) { return [section[@"title"] hasSuffix:@"skin"]; }]];
    return all;
}
- (void)showSettingsCategory:(NSString*)category {
    settingsCategory=category;
    CaraSettingsController* settings=[CaraSettingsController new]; settings.title=category; settings.sections=[self settingsForCategory:category]; settingsController=settings;
    __weak EmulatorController* owner=self; settings.finished=^{ [owner resumeFromMenu]; };
    UINavigationController* navigation=(UINavigationController*)self.presentedViewController;
    if ([navigation isKindOfClass:UINavigationController.class]) [navigation pushViewController:settings animated:YES];
}
- (void)setPlayMode:(NSString*)mode {
    if (!CaraCanTuneEmulation()) return;
    [NSUserDefaults.standardUserDefaults setObject:mode forKey:@"playMode"];
    [self applySavedPlayPolicy];
}
- (void)setEmulationPreference:(NSString*)key value:(NSNumber*)value refresh:(BOOL)refresh {
    if (!CaraCanTuneEmulation() || [[NSUserDefaults.standardUserDefaults stringForKey:@"playMode"] isEqual:@"accuracy"]) return;
    if (![@[@"fixedGuestClockEnabled",@"fixedGuestClockPercent",@"speedLimitEnabled"] containsObject:key]) return;
    [NSUserDefaults.standardUserDefaults setObject:value forKey:key]; [self applySavedPlayPolicy];
    if (refresh) [self updateSyncFeedback];
}
- (void)applySavedPlayPolicy {
    const auto policy=CaraSavedPlayPolicy();
    adaptiveCPU=policy.adaptive; fixedGuestClock=policy.guest_clock; maximumSpeedLimit=policy.maximum_speed; speedLimitEnabled=policy.limit_speed; adaptiveCPUDirty=true;
    [NSUserDefaults.standardUserDefaults setBool:policy.adaptive forKey:@"adaptiveGuestCPU"];
    UINavigationController* navigation=(UINavigationController*)self.presentedViewController;
    if ([navigation isKindOfClass:UINavigationController.class] && [navigation.viewControllers.firstObject isKindOfClass:CaraQuickSettingsController.class])
        ((CaraQuickSettingsController*)navigation.viewControllers.firstObject).performanceDetail=[self performanceModeDetail];
    [self updateSessionPresentation];
}
- (NSString*)performanceModeDetail {
    return [NSUserDefaults.standardUserDefaults boolForKey:@"fixedGuestClockEnabled"]?[NSString stringWithFormat:@"Fixed guest clock · %ld%%",(long)[NSUserDefaults.standardUserDefaults integerForKey:@"fixedGuestClockPercent"]]:@"Adaptive CPU clock";
}
- (NSArray*)libraryStatesForGame:(NSURL*)url {
    NSDictionary* info=[libraryStore metadataForGame:url]; if(!info[@"stateID"] || !info[@"gameHash"]) return @[@{@"title":@"Open this game to check its states",@"subtitle":@"Cara needs the title identity and game revision before it can list compatible states.",@"compatible":@NO}];
    NSURL* documents=[NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    CaraStateCatalog* catalog=[[CaraStateCatalog alloc] initWithDirectory:[documents URLByAppendingPathComponent:@"cara/states"] gameID:info[@"stateID"] fingerprint:[NSString stringWithUTF8String:Core::GetSaveStateBuildFingerprint()] gameHash:info[@"gameHash"] new3DS:[info[@"new3DS"] boolValue] backend:strcmp(CaraSelectedCPUBackend(),"dyncom")==0?1:0];
    BOOL access=[url startAccessingSecurityScopedResource]; NSDictionary* fileInfo=[url resourceValuesForKeys:@[NSURLContentModificationDateKey,NSURLFileSizeKey] error:nil]; if(access) [url stopAccessingSecurityScopedResource];
    BOOL sameRevision=info[@"romModified"] && [info[@"romModified"] isEqual:fileInfo[NSURLContentModificationDateKey]] && [info[@"romSize"] isEqual:fileInfo[NSURLFileSizeKey]];
    NSMutableArray* records=[NSMutableArray new];
    for(CaraStateRecord* state in catalog.records) [records addObject:@{@"title":state.name,@"filename":state.filename,@"compatible":@(state.compatible && sameRevision),@"subtitle":[NSString stringWithFormat:@"%@ · %@",[NSDateFormatter localizedStringFromDate:state.date dateStyle:NSDateFormatterMediumStyle timeStyle:NSDateFormatterShortStyle],!sameRevision?@"Open the game again to verify its revision":state.compatible?@"Ready to load":@"Incompatible with this build or game"]}];
    return records;
}
- (void)showFullSettings {
    if(self.presentedViewController) return;
    CaraSettingsController* settings=[CaraSettingsController new]; settings.categorized=YES; settings.sections=[self settingsSections]; settingsController=settings; settingsCategory=@"All";
    __weak EmulatorController* owner=self; settings.finished=^{ EmulatorController* c=owner; if(c) [c->libraryController reloadLibrary]; [owner resumeFromMenu]; };
    UINavigationController* nav=[[UINavigationController alloc] initWithRootViewController:settings]; nav.modalPresentationStyle=UIModalPresentationFullScreen; [self presentViewController:nav animated:YES completion:nil];
}
- (void)showGameSettings:(NSURL*)url {
    CaraSettingsController* settings=[CaraSettingsController new]; settings.title=@"Game settings";
    NSMutableDictionary* values=[[[libraryStore metadataForGame:url] objectForKey:@"overrides"] mutableCopy]?:[NSMutableDictionary new];
    __weak EmulatorController* owner=self;
    void(^save)(NSString*,id)=^(NSString* key,id value) { values[key]=value; EmulatorController* c=owner; if(!c) return; [c->libraryStore setMetadata:@{@"overrides":values} forGame:url]; if([c->gameURL.URLByStandardizingPath isEqual:url.URLByStandardizingPath]) { CaraGameOverrides=[values copy]; [c applySavedPlayPolicy]; c->stretchAudio=[CaraEmulationValue(@"audioStretching") boolValue]; c->stretchAudioDirty=true; } };
    id(^value)(NSString*)=^id(NSString* key) { return values[key]?:[NSUserDefaults.standardUserDefaults objectForKey:key]; };
    settings.sectionsProvider=^NSArray* {
    BOOL accuracy=[value(@"playMode") isEqual:@"accuracy"];
    return @[@{@"title":@"Emulation",@"rows":@[
        @{@"title":@"Accuracy mode",@"subtitle":@"Locks guest clock and speed to 100%. Uses the same 100% guest clock and speed as global Accuracy mode.",@"value":@(accuracy),@"change":^(BOOL on) { save(@"playMode",on?@"accuracy":@"performance"); }},
        @{@"title":@"Fixed guest clock",@"value":@(accuracy || [value(@"fixedGuestClockEnabled") boolValue]),@"enabled":@(!accuracy),@"change":^(BOOL on) { save(@"fixedGuestClockEnabled",@(on)); }},
        @{@"title":@"Guest clock",@"enabled":@(!accuracy && [value(@"fixedGuestClockEnabled") boolValue]),@"slider":@{@"min":@25,@"max":@200,@"step":@5,@"value":@(accuracy?100:([value(@"fixedGuestClockPercent") integerValue]?:100))},@"sliderChange":^(double v) { save(@"fixedGuestClockPercent",@(lround(v))); }},
        @{@"title":@"Limit game speed",@"value":@(accuracy || [value(@"speedLimitEnabled") boolValue]),@"enabled":@(!accuracy),@"change":^(BOOL on) { save(@"speedLimitEnabled",@(on)); }},
        @{@"title":@"Maximum speed",@"enabled":@(!accuracy && [value(@"speedLimitEnabled") boolValue]),@"slider":@{@"min":@50,@"max":@200,@"step":@5,@"value":@(accuracy?100:([value(@"maximumSpeedPercent") integerValue]?:100))},@"sliderChange":^(double v) { save(@"maximumSpeedPercent",@(lround(v))); }},
    ],@"footer":@"Only this game uses these overrides. Clear game settings in Game details to use global preferences again."},@{@"title":@"Audio",@"rows":@[@{@"title":@"Audio stretching",@"value":@([value(@"audioStretching") boolValue]),@"change":^(BOOL on) { save(@"audioStretching",@(on)); }}]}];
    };
    UINavigationController* nav=[[UINavigationController alloc] initWithRootViewController:settings]; nav.modalPresentationStyle=UIModalPresentationPageSheet; nav.preferredContentSize=CGSizeMake(620,800); [self presentViewController:nav animated:YES completion:nil];
}
- (void)showMenu {
    if (openingGame || self.presentedViewController) return;
    [self ensureLibrary]; modalPaused=YES; paused=true; AudioCore::SetIOSAudioPaused(true); [self resetSkinInput];
    [self updateSessionPresentation];
    CaraQuickSettingsController* quick=[CaraQuickSettingsController new]; quick.playMode=[NSUserDefaults.standardUserDefaults stringForKey:@"playMode"]?:@"performance";
    quick.modeSelectionEnabled=CaraCanTuneEmulation(); quick.performanceDetail=[self performanceModeDetail];
    __weak EmulatorController* owner=self;
    quick.chooseMode=^(NSString* mode) { [owner setPlayMode:mode]; };
    BOOL landscape=self.view.bounds.size.width>self.view.bounds.size.height;
    NSString* meterKey=landscape?@"landscapeStatsVisible":@"portraitStatsVisible";
    quick.showFPS=![NSUserDefaults.standardUserDefaults objectForKey:meterKey] || [NSUserDefaults.standardUserDefaults boolForKey:meterKey];
    quick.toggleFPS=^(BOOL visible) { [NSUserDefaults.standardUserDefaults setBool:visible forKey:meterKey]; [owner.view setNeedsLayout]; };
    NSMutableArray* actions=[NSMutableArray new];
    if (running) {
        [actions addObject:@{@"title":@"Save state",@"symbol":@"square.and.arrow.down",@"action":^{ [owner showStateSlots:YES]; }}];
        [actions addObject:@{@"title":@"Load state",@"symbol":@"arrow.counterclockwise",@"action":^{ [owner showStateSlots:NO]; }}];
    }
    [actions addObject:@{@"title":@"Library & iCloud",@"symbol":@"folder",@"keepOpen":@YES,@"action":^{ [owner showSettingsCategory:@"Library & iCloud"]; }}];
    [actions addObject:@{@"title":@"Skins",@"symbol":@"gamecontroller",@"keepOpen":@YES,@"action":^{ [owner showSettingsCategory:@"Skins"]; }}];
    if (running) [actions addObject:@{@"title":@"Game library",@"symbol":@"square.grid.2x2",@"action":^{ [owner showLibrary]; }}];
    [actions addObject:@{@"title":@"Advanced",@"symbol":@"slider.horizontal.3",@"keepOpen":@YES,@"action":^{ [owner showSettingsCategory:@"Advanced"]; }}];
    quick.actions=actions; quick.finished=^{ [owner resumeFromMenu]; };
    UINavigationController* navigation=[[UINavigationController alloc] initWithRootViewController:quick]; navigation.view.tintColor=[UIColor colorWithRed:1 green:.48 blue:.24 alpha:1];
    navigation.modalPresentationStyle=UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad?UIModalPresentationFormSheet:UIModalPresentationPageSheet;
    navigation.preferredContentSize=CGSizeMake(620,850);
    if (@available(iOS 15.0,*)) { navigation.sheetPresentationController.detents=@[UISheetPresentationControllerDetent.largeDetent]; navigation.sheetPresentationController.prefersGrabberVisible=YES; navigation.sheetPresentationController.preferredCornerRadius=28; }
    navigation.presentationController.delegate=quick;
    [self presentViewController:navigation animated:YES completion:nil];
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
- (void)stopGame { stop=true; sessionNotice=nil; AudioCore::SetIOSAudioPaused(true); [self resetSkinInput]; [self updateSessionPresentation]; }
- (void)background { backgroundAt = CFAbsoluteTimeGetCurrent(); paused = true; AudioCore::SetIOSAudioPaused(true); [self resetSkinInput]; [self updateSessionPresentation]; }
- (void)foreground {
    if (backgroundAt > 0 && running) {
        const double duration = CFAbsoluteTimeGetCurrent() - backgroundAt;
        startedAt += duration;
        lastHUDTime += duration;
    }
    backgroundAt = 0; paused = modalPaused; AudioCore::SetIOSAudioPaused(paused || stop || !running);
    [self updateSessionPresentation];
    [saveSync refreshCloudStatus]; [stateSync refreshCloudStatus];
    if (libraryVisible) [libraryController reloadLibrary];
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
    NSString* orientation=pickingSkinOrientation?:((self.view.bounds.size.width>self.view.bounds.size.height)?@"Landscape":@"Portrait");
    pickingSkin=NO; pickingSkinOrientation=nil;
    const BOOL scoped=[url startAccessingSecurityScopedResource];
    NSError* error=nil; CaraSkin* skin=[CaraSkin loadURL:url error:&error];
    if (!skin) { if (scoped) [url stopAccessingSecurityScopedResource]; [self skinError:error.localizedDescription?:@"Unsupported skin."]; return; }
    NSDictionary* rep=[skin strictRepresentationForPad:UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad landscape:[orientation isEqual:@"Landscape"]];
    CaraSkinControls* check=[[CaraSkinControls alloc] initWithFrame:CGRectZero]; check.skin=skin;
    if (rep) check.representation=rep;
    if (!rep || !check.artwork) { if (scoped) [url stopAccessingSecurityScopedResource]; [self skinError:[NSString stringWithFormat:@"This skin has no usable %@ layout. Your other skin is unchanged.",orientation.lowercaseString]]; return; }
    NSString* documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
    NSString* name=[NSString stringWithFormat:@"controller-%@.deltaskin",orientation.lowercaseString];
    NSURL* destination=[NSURL fileURLWithPath:[documents stringByAppendingPathComponent:name]];
    NSData* data=[NSData dataWithContentsOfURL:url options:0 error:&error];
    BOOL saved=data && [data writeToURL:destination options:NSDataWritingAtomic error:&error];
    if (scoped) [url stopAccessingSecurityScopedResource];
    if (!saved) { [self skinError:error.localizedDescription]; return; }
    [NSUserDefaults.standardUserDefaults setObject:name forKey:[@"controllerSkin" stringByAppendingString:orientation]];
    [self reloadSelectedSkins]; [self resumeFromMenu];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)picker { pickingSaveFolder=NO; pickingSkin=NO; pickingROMsFolder=NO; pickingSkinOrientation=nil; [self resumeFromMenu]; }
- (void)documentPicker:(UIDocumentPickerViewController*)picker didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    NSURL* url=urls.firstObject;
    if (!url) { [self resumeFromMenu]; return; }
    if (pickingROMsFolder) {
        pickingROMsFolder=NO; [self ensureLibrary]; NSError* error=nil;
        if (![libraryStore chooseFolder:url error:&error]) { [self shellError:@"ROMs folder unavailable" message:error.localizedDescription]; return; }
        [libraryController reloadLibrary]; [self resumeFromMenu]; return;
    }
    if (pickingSaveFolder) {
        pickingSaveFolder=NO; NSError* error=nil;
        if (![saveSync connectFolder:url error:&error] || ![stateSync connectFolder:url error:&error]) { [self shellError:@"Sync folder unavailable" message:error.localizedDescription]; return; }
        [self syncSaves:nil completion:nil]; return;
    }
    if (pickingSkin || [@[@"deltaskin",@"caraskin"] containsObject:url.pathExtension.lowercaseString]) { [self importSkin:url]; return; }
    [self openGameURL:url];
}
- (void)syncSaves:(NSString*)resolution completion:(void(^)(BOOL))completion {
    [self syncChannel:saveSync resolution:resolution completion:^(BOOL ok) {
        if (!ok) { if (completion) completion(NO); return; }
        if (self->launchingOffline) { if (completion) completion(YES); return; }
        [self syncChannel:self->stateSync resolution:nil completion:completion];
    }];
}
- (void)syncChannel:(CaraSaveSync*)channel resolution:(NSString*)resolution completion:(void(^)(BOOL))completion {
    [channel requestSync:YES resolution:resolution completion:^(NSDictionary* result,NSError* error) {
        [self updateSyncFeedback];
        if (error) {
            if (self->openingGame) {
                UIAlertController* offline=[UIAlertController alertControllerWithTitle:@"Sync folder unavailable" message:[error.localizedDescription stringByAppendingString:@"\n\nYou can continue with the saves and states already on this device."] preferredStyle:UIAlertControllerStyleAlert];
                [offline addAction:[UIAlertAction actionWithTitle:@"Use local data" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { self->launchingOffline=YES; if (completion) completion(YES); }]];
                [offline addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction* action) { [self resumeFromMenu]; if (completion) completion(NO); }]];
                [self presentViewController:offline animated:YES completion:nil];
            } else { [self shellError:(channel.statesOnly?@"Save states couldn’t sync":@"Saves couldn’t sync") message:error.localizedDescription]; if (completion) completion(NO); }
            return;
        }
        if ([result[@"conflict"] boolValue]) {
            UIAlertController* alert=[UIAlertController alertControllerWithTitle:(channel.statesOnly?@"Choose save states":@"Choose saves") message:@"Both versions are preserved. Choose which complete set to use on this device." preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"Keep this device’s version" style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self syncChannel:channel resolution:@"local" completion:completion]; }]];
            for (NSDictionary* version in result[@"versions"]) {
                NSDate* date=[NSDate dateWithTimeIntervalSince1970:[version[@"created"] doubleValue]];
                NSString* title=[@"Use folder version · " stringByAppendingString:[NSDateFormatter localizedStringFromDate:date dateStyle:NSDateFormatterShortStyle timeStyle:NSDateFormatterShortStyle]];
                [alert addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault handler:^(UIAlertAction* action) { [self syncChannel:channel resolution:version[@"id"] completion:completion]; }]];
            }
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction* action) { [self resumeFromMenu]; if (completion) completion(NO); }]];
            [self presentViewController:alert animated:YES completion:nil]; return;
        }
        [self resumeFromMenu]; if (completion) completion(YES);
    }];
}
- (void)openGameURL:(NSURL*)url {
    if (openingGame) return;
    if (![@[@"3ds",@"cci",@"cxi",@"3dsx",@"elf"] containsObject:url.pathExtension.lowercaseString]) { status.text=@"Choose an extracted .3ds, .cci, .cxi, .3dsx or .elf game."; [self resumeFromMenu]; return; }
    [self ensureLibrary];
    openingGame=YES; launchingOffline=NO; sessionFailure=nil; sessionNotice=nil; stop=true; [self resumeFromMenu];
    if (worker.joinable()) worker.join();
    if (gameScoped) [gameURL stopAccessingSecurityScopedResource];
    CaraGameOverrides=[libraryStore metadataForGame:url][@"overrides"];
    [self applySavedPlayPolicy]; stretchAudio=[CaraEmulationValue(@"audioStretching") boolValue];
    gameURL=url; gameScoped=[url startAccessingSecurityScopedResource];
    NSData* bookmark=[url bookmarkDataWithOptions:NSURLBookmarkCreationMinimalBookmark includingResourceValuesForKeys:nil relativeToURL:nil error:nil];
    if (bookmark) [NSUserDefaults.standardUserDefaults setObject:bookmark forKey:@"recentGameBookmark"];
    if (!std::getenv("CARA_BENCHMARK_FRAMES")) [libraryStore rememberGame:url error:nil];
    [self syncSaves:nil completion:^(BOOL ok) {
        if (!ok) { self->openingGame=NO; self->pendingLibraryState=nil; [self updateSessionPresentation]; return; }
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
            __block NSError* error=nil; __block NSURL* readable=nil;
            [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:url options:0 error:&error byAccessor:^(NSURL* actual) {
                if ([actual checkResourceIsReachableAndReturnError:&error]) readable=actual;
            }];
            dispatch_async(dispatch_get_main_queue(), ^{ self->openingGame=NO; if (readable) [self startROM:readable]; else [self shellError:@"Game unavailable" message:error.localizedDescription?:@"Select this game again in Files."]; });
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
#ifndef CARA_IOS_GLES
    const double gameFPS=screen==0 && emulatedFrame%60==0 && !std::getenv("CARA_BENCHMARK_FRAMES")?Core::System::GetInstance().GetAndResetPerfStats().game_fps:-1.0;
#endif
    NSData* data = [NSData dataWithBytes:pixels.data() length:pixels.size()];
    __weak EmulatorController* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        EmulatorController* controller = weakSelf;
        if (!controller || controller->sessionGeneration!=session) return;
        CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGImageRef image = CGImageCreate(width,height,8,32,width*4,space,CGBitmapInfo(kCGImageAlphaLast) | kCGBitmapByteOrder32Big,provider,nullptr,false,kCGRenderingIntentDefault);
        UIImage* displayImage = [UIImage imageWithCGImage:image];
        const BOOL display=Cara::AcceptSessionFrame([controller sessionPresentation],session,controller->sessionGeneration);
        if (display) (screen == 0 ? controller->top : controller->bottom).image = displayImage;
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
                if (display) [controller->performanceHUD updateGameFPS:gameFPS speed:refresh*100.0/59.833997 guestClock:controller->guestClock.load() adaptive:controller->adaptiveCPU.load()];
                [controller updateSessionPresentation];
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
    [self hideLibrary];
    NSString* requestedState=pendingLibraryState; pendingLibraryState=nil;
    const unsigned session=++sessionGeneration;
    framePending[0]=false; framePending[1]=false;
    frameCount = 0; startedAt = CFAbsoluteTimeGetCurrent();
    lastHUDTime = startedAt; lastHUDFrame = 0;
    stateCommand=0; gameLoaded=false; sessionFailure=nil; sessionNotice=nil; running=true; stop=false; [self resumeFromMenu]; [self resetSkinInput];
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    [self updateSessionPresentation];
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
        const double gameFPS=std::getenv("CARA_BENCHMARK_FRAMES")?-1.0:Core::System::GetInstance().GetAndResetPerfStats().game_fps;
        dispatch_async(dispatch_get_main_queue(), ^{
            EmulatorController* controller = weakSelf;
            if (!controller || !controller->running || controller->sessionGeneration!=session) return;
            const double now = CFAbsoluteTimeGetCurrent();
            const double elapsed = now - controller->lastHUDTime;
            const double fps = (frame - controller->lastHUDFrame) / std::max(elapsed, 0.001);
            controller->lastHUDTime = now; controller->lastHUDFrame = frame;
            if (Cara::AcceptSessionFrame([controller sessionPresentation],session,controller->sessionGeneration))
                [controller->performanceHUD updateGameFPS:gameFPS speed:fps*100.0/59.833997 guestClock:controller->guestClock.load() adaptive:controller->adaptiveCPU.load()];
            [controller updateSessionPresentation];
            if (timing.enabled) {
                std::ofstream report(user + "runtime-status.json");
                report << "{\"title_id\":" << title << ",\"frame\":" << frame << ",\"display_fps\":" << fps
                       << ",\"guest_cpu_percent\":" << controller->guestClock.load()
                       << ",\"maximum_speed_percent\":" << controller->maximumSpeedLimit.load()
                       << ",\"adaptive_guest_cpu\":" << (controller->adaptiveCPU.load()?"true":"false")
                       << ",\"cpu_backend\":" << std::quoted(CaraSelectedCPUBackend())
                       << ",\"engine_revision\":" << std::quoted(Common::g_scm_rev)
                       << ",\"engine_build_fingerprint\":" << std::quoted(Core::GetSaveStateBuildFingerprint())
                       << ",\"host_physical_memory_bytes\":" << NSProcessInfo.processInfo.physicalMemory
                       << ",\"vertex_upload_bank_budget_bytes\":" << timing.vertex_upload_bank_budget_bytes
                       << ",\"thermal_state\":" << long(NSProcessInfo.processInfo.thermalState)
                       << ",\"gpu_vertex_draws\":" << timing.gpu_vertex_draws
                       << ",\"cpu_vertex_draws\":" << timing.cpu_vertex_draws;
                task_vm_info_data_t memory{}; mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
                if (task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&memory),&count)==KERN_SUCCESS)
                    report << ",\"physical_footprint_bytes\":" << memory.phys_footprint;
                report << "}\n";
            }
        });
    };
#endif
    emuWindow->present = [weakSelf,session](unsigned s,unsigned w,unsigned h,const std::vector<u8>& p,unsigned frame) {
        [weakSelf showFrame:s width:w height:h pixels:p frame:frame session:session];
    };
    worker = std::thread([self,rom,user,session,url,requestedState] {
        // Diagnostic only: requested QoS is not evidence of P/E-core placement.
        const char* interactive_qos_env = std::getenv("CARA_WORKER_INTERACTIVE_QOS");
        const bool worker_interactive_qos = interactive_qos_env &&
                                            std::strcmp(interactive_qos_env, "1") == 0;
        const auto worker_qos_initial = qos_class_self();
        const int worker_qos_set_result = worker_interactive_qos
            ? pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) : 0;
        @autoreleasepool {
            Common::DetachedTasks tasks;
            FileUtil::SetUserPath(user);
            // Clear reports before any self-test can fail, so stale results are
            // never mistaken for evidence from this launch.
            for (const char* name : {"benchmark-status.json","frame-timing.json","runtime-status.json",
                                     "audio-status.json","save-state-test-status.json","fastinterp-blocks.json",
                                     "surface-flushes.json","gpu-vertex-self-test.json","surface-coherence-self-test.json",
                                     "benchmark-frame.json","benchmark-frame.rgba","profile-checkpoint-status.json","uniform-reuse-self-test.json"})
                FileUtil::Delete(user+name);

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
            s.resolution_factor = 1; s.use_frame_limit = false; // Frontend counts every presented vblank.
            s.frame_limit = u16(self->maximumSpeedLimit.load());
            const char* target_env = std::getenv("CARA_BENCHMARK_FRAMES");
            const unsigned target_frames = target_env ? static_cast<unsigned>(std::clamp(std::strtol(target_env, nullptr, 10), 1L, 18000L)) : 0;
            const bool benchmark_paced=std::getenv("CARA_PACED_BENCHMARK")!=nullptr;
            bool pace_frames = !target_frames || benchmark_paced;
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
                bool passed = false;
                try {
                    passed = RunGPUStateSelfTest(error) && RunGPUVertexSelfTest(error);
                } catch (const std::exception& exception) {
                    error = std::string("GPU fixture exception: ") + exception.what();
                } catch (const char* exception) {
                    error = std::string("GPU fixture exception: ") + exception;
                } catch (...) {
                    error = "Unknown GPU fixture exception";
                }
                std::ofstream report(user + "gpu-vertex-self-test.json");
                GLint max_texture_size=0; glGetIntegerv(GL_MAX_TEXTURE_SIZE,&max_texture_size);
                GLint max_draw_buffers=0,max_color_attachments=0;
                glGetIntegerv(GL_MAX_DRAW_BUFFERS,&max_draw_buffers);
                glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS,&max_color_attachments);

                report << "{\"max_texture_size\":" << max_texture_size << ",\"max_draw_buffers\":" << max_draw_buffers << ",\"max_color_attachments\":" << max_color_attachments << ",\"passed\":" << (passed ? "true" : "false") << ",\"detail\":" << std::quoted(error)
                       << ",\"fixtures\":\"uniform_branch_loop_quaternion_color_packed_sparse_zero_clipping_large_batches_indexed_u16_packed_bytes\"}\n";
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
                emuWindow->ClearDisplay(); emuWindow->DoneCurrent();
                dispatch_async(dispatch_get_main_queue(), ^{ if (self->sessionGeneration!=session) return; self->sessionFailure=[@"Graphics error · " stringByAppendingString:message]; [self updateSessionPresentation]; UIApplication.sharedApplication.idleTimerDisabled = NO; });
                return;
            }
            auto result = system.Load(*emuWindow,rom);
            if(result==Core::System::ResultStatus::Success) {
                NSString* titleID=[NSString stringWithFormat:@"%016llX",(unsigned long long)system.GetTitleID()];
                NSData* hash=[NSData dataWithBytes:system.StateGameHash().data() length:32];
                BOOL new3DS=Settings::values.is_new_3ds;
                NSMutableString* identity=[NSMutableString stringWithString:@"homebrew-"]; for(auto byte:system.StateGameHash()) [identity appendFormat:@"%02X",byte];
                NSString* stateID=system.GetTitleID()?titleID:identity;
                dispatch_async(dispatch_get_main_queue(),^{ if(self->sessionGeneration==session) { NSMutableDictionary* metadata=[@{@"titleID":titleID,@"stateID":stateID,@"gameHash":hash,@"new3DS":@(new3DS)} mutableCopy]; NSDictionary* fileInfo=[url resourceValuesForKeys:@[NSURLContentModificationDateKey,NSURLFileSizeKey] error:nil]; if(fileInfo[NSURLContentModificationDateKey]) metadata[@"romModified"]=fileInfo[NSURLContentModificationDateKey]; if(fileInfo[NSURLFileSizeKey]) metadata[@"romSize"]=fileInfo[NSURLFileSizeKey]; [self->libraryStore setMetadata:metadata forGame:url]; [self->libraryStore recordPlayed:url]; } });
                if(requestedState.length) {
                    try {
                        if(![requestedState.lastPathComponent isEqual:requestedState] || ![requestedState hasPrefix:[stateID stringByAppendingString:@"."]] || ![requestedState hasSuffix:@".carastate"]) throw std::runtime_error("Invalid state location");
                        system.LoadStateFromPath(FileUtil::GetUserPath(FileUtil::UserPath::StatesDir)+requestedState.UTF8String);
                    } catch(const std::exception& error) {
                        NSString* message=[NSString stringWithUTF8String:error.what()];
                        dispatch_async(dispatch_get_main_queue(),^{ [self shellError:@"State couldn’t load" message:message]; });
                        paused=true;
                    }
                }
            }
#ifdef CARA_IOS_GLES
            if (result==Core::System::ResultStatus::Success && std::getenv("CARA_UNIFORM_SELF_TEST")) {
                std::string error;
                auto* rasterizer=dynamic_cast<OpenGL::RasterizerOpenGL*>(VideoCore::Rasterizer());
                bool passed=false;
                try { passed=rasterizer && rasterizer->RunUniformReuseSelfTest(error); }
                catch (const std::exception& exception) { error=exception.what(); }
                catch (...) { error="Unknown uniform fixture exception"; }
                std::ofstream(user+"uniform-reuse-self-test.json")
                    << "{\"passed\":" << (passed?"true":"false") << ",\"detail\":" << std::quoted(error) << "}\n";
                if (!passed) { emuWindow->graphics_error=error; result=Core::System::ResultStatus::ErrorVideoCore; }
            }
            if (result==Core::System::ResultStatus::Success && std::getenv("CARA_SURFACE_SELF_TEST")) {
                std::string error;
                auto* rasterizer=dynamic_cast<OpenGL::RasterizerOpenGL*>(VideoCore::Rasterizer());
                bool passed=false;
                try {
                    passed=rasterizer && rasterizer->RunSurfaceCoherenceSelfTest(error);
                } catch (const std::exception& exception) {
                    error=std::string("Surface fixture exception: ")+exception.what();
                } catch (const char* exception) {
                    error=std::string("Surface fixture exception: ")+exception;
                } catch (...) {
                    error="Unknown surface fixture exception";
                }
                std::ofstream report(user+"surface-coherence-self-test.json");
                report << "{\"passed\":" << (passed?"true":"false") << ",\"detail\":" << std::quoted(error) << "}\n";
                if (!passed) { LOG_ERROR(Frontend,"Surface coherence fixture: {}",error); result=Core::System::ResultStatus::ErrorUnknown; }
            }
#endif
            if (result==Core::System::ResultStatus::Success && std::getenv("CARA_PROFILE_LOAD")) {
                try { system.LoadStateFromPath(user+"profile-comparison.carastate"); }
                catch (const std::exception& error) {
                    LOG_ERROR(Frontend,"Comparison checkpoint: {}",error.what());
                    result=Core::System::ResultStatus::ErrorUnknown;
                }
            }
            if (result==Core::System::ResultStatus::Success)
                LOG_INFO(Frontend,"Cara session title {:016X}",system.GetTitleID());
            gameLoaded=result==Core::System::ResultStatus::Success;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self->sessionGeneration!=session) return;
                if (result != Core::System::ResultStatus::Success)
                    self->sessionFailure = [NSString stringWithFormat:@"Load failed (code %u)",unsigned(result)];
                [self updateSessionPresentation];
            });
            if (result == Core::System::ResultStatus::Success) {
                const auto benchmark_start = std::chrono::steady_clock::now();
                const auto host_seconds=[](std::chrono::steady_clock::time_point value) { return std::chrono::duration<double>(value.time_since_epoch()).count(); };
                unsigned paced_speed=self->maximumSpeedLimit.load();
                bool has_speed_override=false;
                if (const char* speed_override=std::getenv("CARA_MAX_SPEED_PERCENT")) {
                    char* end=nullptr;
                    const long value=std::strtol(speed_override,&end,10);
                    if (end!=speed_override && *end=='\0' && value>=50 && value<=200) {
                        paced_speed=unsigned(value); has_speed_override=true;
                    }
                }
                Cara::FramePacer frame_pacer(paced_speed);
                pace_frames=Cara::ShouldPaceFrames(target_frames!=0,benchmark_paced,has_speed_override,self->speedLimitEnabled.load());
                frame_pacer.Reset(host_seconds(benchmark_start));
                unsigned paced_frame = emuWindow->frames;
                unsigned late_paced_frames = 0;
                bool was_paused = false;
                auto pause_started = benchmark_start;
                std::chrono::steady_clock::duration paused_duration{};
                auto last_frame_time = benchmark_start;
                unsigned timed_frame = 0;
                std::vector<double> frame_times;
                std::vector<std::string> timing_windows;
                auto previous_counters = Common::FrameTiming::counters;
                const bool category_timing=Common::FrameTiming::counters.enabled && Common::CPUCategoryTiming::DiagnosticEnabled();
                Common::CPUCategoryTiming::Counters previous_categories{};
                double category_clock_ns_per_read=0;
                bool category_clock_calibrated=false;
                if (category_timing) {
                    Common::CPUCategoryTiming::Reset();
                    std::uint64_t begin=0,end=0,value=0;
                    unsigned good=0;
                    if (Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr,begin)) {
                        for (unsigned i=0;i<4096;++i) good+=Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr,value);
                        category_clock_calibrated=Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr,end) && good==4096 && end>=begin;
                        if (category_clock_calibrated) category_clock_ns_per_read=double(end-begin)/4097;
                    }
                    previous_categories=Common::CPUCategoryTiming::Snapshot();
                }
                const bool host_cpu_timing=std::getenv("CARA_HOST_CPU_TIMING")!=nullptr;
                const auto read_host_cpu=[]() -> std::pair<double,double> {
                    timespec thread{}; rusage process{};
                    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID,&thread)!=0 || getrusage(RUSAGE_SELF,&process)!=0)
                        return {-1.0,-1.0};
                    return {double(thread.tv_sec)+double(thread.tv_nsec)*1e-9,
                            double(process.ru_utime.tv_sec+process.ru_stime.tv_sec)+
                            double(process.ru_utime.tv_usec+process.ru_stime.tv_usec)*1e-6};
                };
                auto previous_host_cpu=host_cpu_timing?read_host_cpu():std::pair<double,double>{0,0};

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
                struct TestCircleSegment { unsigned begin,end; float x,y; };
                std::vector<TestCircleSegment> test_circle_segments;
                if (target_frames) {
                    if (const char* movement=std::getenv("CARA_TEST_CIRCLE_SEGMENTS")) {
                        std::istringstream stream(movement); std::string item;
                        while (std::getline(stream,item,',')) {
                            TestCircleSegment segment{}; char trailing{};
                            if (std::sscanf(item.c_str(),"%u:%u:%f:%f%c",&segment.begin,&segment.end,&segment.x,&segment.y,&trailing)==4 &&
                                segment.begin<segment.end && segment.end<=target_frames &&
                                std::isfinite(segment.x) && std::isfinite(segment.y) &&
                                std::abs(segment.x)<=1 && std::abs(segment.y)<=1)
                                test_circle_segments.push_back(segment);
                        }
                    }
                }
                unsigned test_circle_frame=~0u;
                size_t next_test_press = 0;
                unsigned active_test_press = 0;
                const char* window_env = std::getenv("CARA_TIMING_WINDOW_FRAMES");
                const unsigned window_frames = window_env ? unsigned(std::clamp(std::strtol(window_env, nullptr, 10), 60L, 600L)) : 600;
                const bool audio_diagnostics=std::getenv("CARA_AUDIO_DIAGNOSTICS")!=nullptr;
                unsigned audio_report_frame=0;
                Cara::AdaptiveClock clock_control;
                clock_control.SetEnabled(self->adaptiveCPU.load());
                const auto policy_clock=[&]() { return self->adaptiveCPU.load()?clock_control.Percent():self->fixedGuestClock.load(); };
                self->guestClock=policy_clock();
                unsigned clock_frame=emuWindow->frames;
                auto clock_frame_time=std::chrono::steady_clock::now();
                double clock_work_seconds=0;
                const auto reset_clock_samples=[&]() {
                    clock_control.ResetSamples(); clock_work_seconds=0;
                    clock_frame=emuWindow->frames; clock_frame_time=std::chrono::steady_clock::now();
                    if (system.IsPoweredOn()) system.CoreTiming().UpdateClockSpeed(policy_clock());
                    self->guestClock=policy_clock();
                };
                // A restored state may carry an adaptive underclock. Apply the current
                // frontend policy before the first slice, including fixed-clock tests.
                reset_clock_samples();
                LOG_INFO(Frontend,"Session guest CPU policy: {}%",policy_clock());
                const auto state_test_frame=[](const char* name) -> unsigned {
                    const char* value=std::getenv(name);
                    return value?unsigned(std::clamp(std::strtol(value,nullptr,10),0L,100000L)):0;
                };
                const unsigned state_save_frame=state_test_frame("CARA_STATE_TEST_SAVE_FRAME");
                const unsigned state_load_frame=state_test_frame("CARA_STATE_TEST_LOAD_FRAME");
                bool state_saved=false,state_loaded=false;
                const unsigned profile_save_frame=state_test_frame("CARA_PROFILE_SAVE_FRAME");
                const unsigned test_start_frame=state_test_frame("CARA_TEST_HOLD_START_FRAME");
                bool profile_saved=false,test_start_pressed=false,test_start_released=false;

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
                        if (profile_save_frame && !profile_saved && emuWindow->frames>=profile_save_frame) {
                            profile_saved=true; // One attempt; an unsupported checkpoint must not end play.
                            std::string failure;
                            try { system.SaveStateToPath(user+"profile-comparison.carastate"); }
                            catch (const std::exception& error) { failure=error.what(); }
                            std::ofstream(user+"profile-checkpoint-status.json")
                                << "{\"saved\":" << (failure.empty()?"true":"false") << ",\"frame\":" << emuWindow->frames
                                << ",\"title_id\":" << system.GetTitleID()
                                << ",\"engine_build_fingerprint\":" << std::quoted(Core::GetSaveStateBuildFingerprint())
                                << ",\"rom_filename\":" << std::quoted(std::string(FileUtil::GetFilename(rom)))
                                << ",\"detail\":" << std::quoted(failure) << "}\n";
                            if (failure.empty()) LOG_INFO(Frontend,"Comparison checkpoint saved at frame {}",emuWindow->frames);
                            else LOG_ERROR(Frontend,"Comparison checkpoint failed: {}",failure);
                        }
                        if (state_save_frame && !state_saved && emuWindow->frames>=state_save_frame) {
                            saved_execution=execution_snapshot();
                            system.SaveStateToPath(user+"device-test.carastate"); state_saved=true; state_test_report("");
                        }
                        if (state_saved && state_load_frame && !state_loaded && emuWindow->frames>=state_load_frame) {
                            AudioCore::SetIOSAudioPaused(true);
                            system.LoadStateFromPath(user+"device-test.carastate");
                            if (execution_snapshot()!=saved_execution) throw std::runtime_error("CPU/timing did not match saved checkpoint");
                            state_loaded=true; state_test_report(""); reset_clock_samples();
                            frame_pacer.Reset(host_seconds(std::chrono::steady_clock::now())); paced_frame=emuWindow->frames;
                            AudioCore::SetIOSAudioPaused(paused);
                        }
                    } catch (const std::exception& error) {
                        state_test_report(error.what()); result=Core::System::ResultStatus::ErrorUnknown; break;
                    }
                    if (const int command=stateCommand.exchange(0,std::memory_order_acq_rel)) {
                        std::string failure;
                        try {
                            if (command==101) system.SaveStateToPath(user+"profile-comparison.carastate");
                            else if (command>0) { system.SaveState(unsigned(command)); [stateSync captureCommittedSave]; }
                            else system.LoadState(unsigned(-command));
                        } catch (const std::exception& error) { failure=error.what(); }
                        reset_clock_samples();
                        frame_pacer.Reset(host_seconds(std::chrono::steady_clock::now())); paced_frame=emuWindow->frames;
                        const bool powered=system.IsPoweredOn();
                        NSString* message=failure.empty()?[NSString stringWithFormat:@"State %@ in slot %d",command>0?@"saved":@"loaded",std::abs(command)]:[NSString stringWithUTF8String:failure.c_str()];
                        if (command==101 && failure.empty()) message=@"Comparison checkpoint saved";
                        dispatch_async(dispatch_get_main_queue(), ^{
                            if (self->sessionGeneration!=session) return;
                            self->sessionNotice=message;
                            [self updateSessionPresentation];
                            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{
                                if (self->sessionGeneration==session && self->sessionNotice==message) { self->sessionNotice=nil; [self updateSessionPresentation]; }
                            });
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
                        frame_pacer.Reset(host_seconds(resumed)); paced_frame=emuWindow->frames;
                        was_paused = false; reset_clock_samples();
                    }
                    if (test_start_frame && !test_start_pressed && emuWindow->frames>=test_start_frame) {
                        buttons.Press(Settings::NativeButton::Start); test_start_pressed=true;
                    }
                    if (test_start_pressed && !test_start_released && emuWindow->frames>=test_start_frame+120) {
                        buttons.Release(Settings::NativeButton::Start); test_start_released=true;
                    }
                    if (active_test_press && emuWindow->frames >= active_test_press + 6) {
                        buttons.Release(Settings::NativeButton::A); active_test_press = 0;
                    }
                    if (!active_test_press && next_test_press < test_press_frames.size() &&
                        emuWindow->frames >= test_press_frames[next_test_press]) {
                        buttons.Press(Settings::NativeButton::A);
                        active_test_press = emuWindow->frames; ++next_test_press;
                    }
                    if (!test_circle_segments.empty() && test_circle_frame!=emuWindow->frames) {
                        test_circle_frame=emuWindow->frames;
                        float x=0,y=0;
                        for (const auto& segment:test_circle_segments) {
                            if (test_circle_frame>=segment.begin && test_circle_frame<segment.end) {
                                x=segment.x; y=segment.y; break;
                            }
                        }
                        circleX=x; circleY=y;
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
                            const auto host_cpu=host_cpu_timing?read_host_cpu():std::pair<double,double>{0,0};
                            const bool host_cpu_valid=host_cpu_timing && previous_host_cpu.first>=0 &&
                                host_cpu.first>=previous_host_cpu.first && host_cpu.second>=previous_host_cpu.second;

                            std::string category_window;
                            if (category_timing) {
                                const auto categories=Common::CPUCategoryTiming::Snapshot();
                                std::uint64_t entries=0;
                                category_window=",\"cpu_category_ns\":[";
                                for (size_t i=0;i<Common::CPUCategoryTiming::CategoryCount;++i)
                                    category_window+=(i?",":"")+std::to_string(categories.cpu_ns[i]-previous_categories.cpu_ns[i]);
                                category_window+="],\"cpu_category_entries\":[";
                                for (size_t i=0;i<Common::CPUCategoryTiming::CategoryCount;++i) {
                                    const auto delta=categories.entries[i]-previous_categories.entries[i]; entries+=delta;
                                    category_window+=(i?",":"")+std::to_string(delta);
                                }
                                category_window+="],\"cpu_category_clock_failures\":"+std::to_string(categories.clock_failures-previous_categories.clock_failures)+
                                    ",\"cpu_category_overflows\":"+std::to_string(categories.counter_overflows-previous_categories.counter_overflows)+
                                    ",\"cpu_category_clock_reads\":"+std::to_string(entries*2);
                                previous_categories=categories;
                            }
                            timing_windows.push_back("{\"end_frame\":" + std::to_string(timed_frame) +
                                ",\"frames\":" + std::to_string(frame_times.size()) +
                                ",\"guest_cpu_percent\":" + std::to_string(policy_clock()) +
                                ",\"mean_ms\":" + std::to_string(total_ms / count) +
                                ",\"host_cpu_timing\":" + std::string(host_cpu_valid?"true":"false") +
                                ",\"emulation_thread_cpu_ms_per_frame\":" + std::to_string(host_cpu_valid?(host_cpu.first-previous_host_cpu.first)*1000/count:0) +
                                ",\"process_cpu_ms_per_frame\":" + std::to_string(host_cpu_valid?(host_cpu.second-previous_host_cpu.second)*1000/count:0) +
                                ",\"p50_ms\":" + std::to_string(frame_times[frame_times.size() / 2]) +
                                ",\"p95_ms\":" + std::to_string(frame_times[(frame_times.size() - 1) * 95 / 100]) +
                                ",\"max_ms\":" + std::to_string(frame_times.back()) +
                                ",\"over_budget_frames\":" + std::to_string(late) +
                                ",\"cpu_shader_ms_per_frame\":" + std::to_string((current.shader_seconds - previous_counters.shader_seconds) * 1000 / count) +
                                ",\"guest_cpu_ms_per_frame\":" + std::to_string((current.guest_cpu_seconds - previous_counters.guest_cpu_seconds) * 1000 / count) +
                                ",\"draw_ms_per_frame\":" + std::to_string((current.draw_seconds - previous_counters.draw_seconds) * 1000 / count) +
                                ",\"texture_lookup_checks\":" + std::to_string(current.texture_lookup_checks - previous_counters.texture_lookup_checks) +
                                ",\"texture_lookup_hits\":" + std::to_string(current.texture_lookup_hits - previous_counters.texture_lookup_hits) +
                                ",\"texture_lookup_misses\":" + std::to_string(current.texture_lookup_misses - previous_counters.texture_lookup_misses) +
                                ",\"texture_lookup_stores\":" + std::to_string(current.texture_lookup_stores - previous_counters.texture_lookup_stores) +
                                ",\"texture_lookup_ambiguous_rejects\":" + std::to_string(current.texture_lookup_ambiguous_rejects - previous_counters.texture_lookup_ambiguous_rejects) +
                                ",\"texture_lookup_epoch_rejects\":" + std::to_string(current.texture_lookup_epoch_rejects - previous_counters.texture_lookup_epoch_rejects) +
                                ",\"texture_lookup_verified_hits\":" + std::to_string(current.texture_lookup_verified_hits - previous_counters.texture_lookup_verified_hits) +
                                ",\"texture_lookup_verify_mismatches\":" + std::to_string(current.texture_lookup_verify_mismatches - previous_counters.texture_lookup_verify_mismatches) +
                                ",\"draw_framebuffer_acquire_ms_per_frame\":" + std::to_string((current.draw_framebuffer_acquire_seconds - previous_counters.draw_framebuffer_acquire_seconds) * 1000 / count) +
                                ",\"draw_framebuffer_acquire_calls\":" + std::to_string((current.draw_framebuffer_acquire_calls - previous_counters.draw_framebuffer_acquire_calls)) +
                                ",\"draw_texture_acquire_ms_per_frame\":" + std::to_string((current.draw_texture_acquire_seconds - previous_counters.draw_texture_acquire_seconds) * 1000 / count) +
                                ",\"draw_texture_acquire_calls\":" + std::to_string((current.draw_texture_acquire_calls - previous_counters.draw_texture_acquire_calls)) +
                                ",\"draw_shader_setup_ms_per_frame\":" + std::to_string((current.draw_shader_setup_seconds - previous_counters.draw_shader_setup_seconds) * 1000 / count) +
                                ",\"draw_shader_setup_calls\":" + std::to_string((current.draw_shader_setup_calls - previous_counters.draw_shader_setup_calls)) +
                                ",\"draw_dirty_invalidate_ms_per_frame\":" + std::to_string((current.draw_dirty_invalidate_seconds - previous_counters.draw_dirty_invalidate_seconds) * 1000 / count) +
                                ",\"draw_dirty_invalidate_calls\":" + std::to_string((current.draw_dirty_invalidate_calls - previous_counters.draw_dirty_invalidate_calls)) +
                                ",\"render_reinterpret_check_ms_per_frame\":" + std::to_string((current.render_reinterpret_check_seconds - previous_counters.render_reinterpret_check_seconds) * 1000 / count) +
                                ",\"render_reinterpret_check_calls\":" + std::to_string((current.render_reinterpret_check_calls - previous_counters.render_reinterpret_check_calls)) +
                                ",\"render_reinterpret_invocations\":" + std::to_string((current.render_reinterpret_invocations - previous_counters.render_reinterpret_invocations)) +
                                ",\"render_reinterpret_find_calls\":" + std::to_string((current.render_reinterpret_find_calls - previous_counters.render_reinterpret_find_calls)) +
                                ",\"render_reinterpret_avoided_calls\":" + std::to_string((current.render_reinterpret_avoided_calls - previous_counters.render_reinterpret_avoided_calls)) +
                                ",\"render_reinterpret_repeat_opportunities\":" + std::to_string((current.render_reinterpret_repeat_opportunities - previous_counters.render_reinterpret_repeat_opportunities)) +
                                ",\"contiguous_transfer_loads\":" + std::to_string((current.contiguous_transfer_loads - previous_counters.contiguous_transfer_loads)) +
                                ",\"contiguous_transfer_stores\":" + std::to_string((current.contiguous_transfer_stores - previous_counters.contiguous_transfer_stores)) +
                                ",\"contiguous_transfer_bytes\":" + std::to_string((current.contiguous_transfer_bytes - previous_counters.contiguous_transfer_bytes)) +
                                ",\"vertex_prepare_ms_per_frame\":" + std::to_string((current.vertex_prepare_seconds - previous_counters.vertex_prepare_seconds) * 1000 / count) +
                                ",\"vertex_upload_ms_per_frame\":" + std::to_string((current.vertex_upload_seconds - previous_counters.vertex_upload_seconds) * 1000 / count) +
                                ",\"vertex_orphan_upload_ms_per_frame\":" + std::to_string((current.vertex_orphan_upload_seconds - previous_counters.vertex_orphan_upload_seconds) * 1000 / count) +
                                ",\"vertex_storage_allocate_ms_per_frame\":" + std::to_string((current.vertex_storage_allocate_seconds - previous_counters.vertex_storage_allocate_seconds) * 1000 / count) +
                                ",\"vertex_subimage_upload_ms_per_frame\":" + std::to_string((current.vertex_subimage_upload_seconds - previous_counters.vertex_subimage_upload_seconds) * 1000 / count) +
                                ",\"stream_map_ms_per_frame\":" + std::to_string((current.stream_map_seconds - previous_counters.stream_map_seconds)*1000.0/count) +
                                ",\"stream_unmap_ms_per_frame\":" + std::to_string((current.stream_unmap_seconds - previous_counters.stream_unmap_seconds)*1000.0/count) +
                                ",\"stream_map_calls\":" + std::to_string(current.stream_map_calls - previous_counters.stream_map_calls) +
                                ",\"stream_unmap_calls\":" + std::to_string(current.stream_unmap_calls - previous_counters.stream_unmap_calls) +
                                ",\"stream_mapped_bytes\":" + std::to_string(current.stream_mapped_bytes - previous_counters.stream_mapped_bytes) +
                                ",\"draw_submit_sample_wall_ms\":" + std::to_string((current.draw_submit_cpu_samples.wall_seconds - previous_counters.draw_submit_cpu_samples.wall_seconds)*1000.0) +
                                ",\"draw_submit_sample_cpu_ms\":" + std::to_string((current.draw_submit_cpu_samples.cpu_seconds - previous_counters.draw_submit_cpu_samples.cpu_seconds)*1000.0) +
                                ",\"draw_submit_samples\":" + std::to_string((current.draw_submit_cpu_samples.samples - previous_counters.draw_submit_cpu_samples.samples)) +
                                ",\"draw_submit_sample_clock_failures\":" + std::to_string((current.draw_submit_cpu_samples.clock_failures - previous_counters.draw_submit_cpu_samples.clock_failures)) +
                                ",\"vertex_orphan_sample_wall_ms\":" + std::to_string((current.vertex_orphan_cpu_samples.wall_seconds - previous_counters.vertex_orphan_cpu_samples.wall_seconds)*1000.0) +
                                ",\"vertex_orphan_sample_cpu_ms\":" + std::to_string((current.vertex_orphan_cpu_samples.cpu_seconds - previous_counters.vertex_orphan_cpu_samples.cpu_seconds)*1000.0) +
                                ",\"vertex_orphan_samples\":" + std::to_string((current.vertex_orphan_cpu_samples.samples - previous_counters.vertex_orphan_cpu_samples.samples)) +
                                ",\"vertex_orphan_sample_clock_failures\":" + std::to_string((current.vertex_orphan_cpu_samples.clock_failures - previous_counters.vertex_orphan_cpu_samples.clock_failures)) +
                                ",\"stream_map_sample_wall_ms\":" + std::to_string((current.stream_map_cpu_samples.wall_seconds - previous_counters.stream_map_cpu_samples.wall_seconds)*1000.0) +
                                ",\"stream_map_sample_cpu_ms\":" + std::to_string((current.stream_map_cpu_samples.cpu_seconds - previous_counters.stream_map_cpu_samples.cpu_seconds)*1000.0) +
                                ",\"stream_map_samples\":" + std::to_string((current.stream_map_cpu_samples.samples - previous_counters.stream_map_cpu_samples.samples)) +
                                ",\"stream_map_sample_clock_failures\":" + std::to_string((current.stream_map_cpu_samples.clock_failures - previous_counters.stream_map_cpu_samples.clock_failures)) +
                                ",\"stream_unmap_sample_wall_ms\":" + std::to_string((current.stream_unmap_cpu_samples.wall_seconds - previous_counters.stream_unmap_cpu_samples.wall_seconds)*1000.0) +
                                ",\"stream_unmap_sample_cpu_ms\":" + std::to_string((current.stream_unmap_cpu_samples.cpu_seconds - previous_counters.stream_unmap_cpu_samples.cpu_seconds)*1000.0) +
                                ",\"stream_unmap_samples\":" + std::to_string((current.stream_unmap_cpu_samples.samples - previous_counters.stream_unmap_cpu_samples.samples)) +
                                ",\"stream_unmap_sample_clock_failures\":" + std::to_string((current.stream_unmap_cpu_samples.clock_failures - previous_counters.stream_unmap_cpu_samples.clock_failures)) +
                                ",\"uniform_upload_sample_wall_ms\":" + std::to_string((current.uniform_upload_cpu_samples.wall_seconds - previous_counters.uniform_upload_cpu_samples.wall_seconds)*1000.0) +
                                ",\"uniform_upload_sample_cpu_ms\":" + std::to_string((current.uniform_upload_cpu_samples.cpu_seconds - previous_counters.uniform_upload_cpu_samples.cpu_seconds)*1000.0) +
                                ",\"uniform_upload_samples\":" + std::to_string((current.uniform_upload_cpu_samples.samples - previous_counters.uniform_upload_cpu_samples.samples)) +
                                ",\"uniform_upload_sample_clock_failures\":" + std::to_string((current.uniform_upload_cpu_samples.clock_failures - previous_counters.uniform_upload_cpu_samples.clock_failures)) +
                                ",\"vertex_subimage_sample_wall_ms\":" + std::to_string((current.vertex_subimage_cpu_samples.wall_seconds - previous_counters.vertex_subimage_cpu_samples.wall_seconds)*1000.0) +
                                ",\"vertex_subimage_sample_cpu_ms\":" + std::to_string((current.vertex_subimage_cpu_samples.cpu_seconds - previous_counters.vertex_subimage_cpu_samples.cpu_seconds)*1000.0) +
                                ",\"vertex_subimage_samples\":" + std::to_string((current.vertex_subimage_cpu_samples.samples - previous_counters.vertex_subimage_cpu_samples.samples)) +
                                ",\"vertex_subimage_sample_clock_failures\":" + std::to_string((current.vertex_subimage_cpu_samples.clock_failures - previous_counters.vertex_subimage_cpu_samples.clock_failures)) +
                                ",\"uniform_upload_ms_per_frame\":" + std::to_string((current.uniform_upload_seconds - previous_counters.uniform_upload_seconds)*1000.0/count) +
                                ",\"surface_remap_ms_per_frame\":" + std::to_string((current.surface_remap_seconds - previous_counters.surface_remap_seconds)*1000.0/count) +
                                ",\"surface_remap_calls\":" + std::to_string(current.surface_remap_calls - previous_counters.surface_remap_calls) +
                                ",\"surface_remap_bytes\":" + std::to_string(current.surface_remap_bytes - previous_counters.surface_remap_bytes) +
                                ",\"surface_pack_ms_per_frame\":" + std::to_string((current.surface_pack_seconds - previous_counters.surface_pack_seconds)*1000.0/count) +
                                ",\"state_apply_ms_per_frame\":" + std::to_string((current.state_apply_seconds - previous_counters.state_apply_seconds)*1000.0/count) +
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
                                ",\"vertex_upload_bytes\":" + std::to_string(current.vertex_upload_bytes - previous_counters.vertex_upload_bytes) +
                                ",\"vertex_index_cache_hits\":" + std::to_string(current.vertex_index_cache_hits - previous_counters.vertex_index_cache_hits) +
                                ",\"vertex_index_cache_misses\":" + std::to_string(current.vertex_index_cache_misses - previous_counters.vertex_index_cache_misses) +
                                ",\"vertex_index_cache_bytes\":" + std::to_string(current.vertex_index_cache_bytes) +
                                ",\"lit_pair_probe_draws\":" + std::to_string(current.lit_pair_probe_draws - previous_counters.lit_pair_probe_draws) +
                                ",\"lit_pair_probe_unavailable_draws\":" + std::to_string(current.lit_pair_probe_unavailable_draws - previous_counters.lit_pair_probe_unavailable_draws) +
                                ",\"lit_pair_probe_vertices\":" + std::to_string(current.lit_pair_probe_vertices - previous_counters.lit_pair_probe_vertices) +
                                ",\"lit_pair_probe_unique_pairs\":" + std::to_string(current.lit_pair_probe_unique_pairs - previous_counters.lit_pair_probe_unique_pairs) +
                                ",\"lit_pair_probe_baseline_vs_runs\":" + std::to_string(current.lit_pair_probe_baseline_vs_runs - previous_counters.lit_pair_probe_baseline_vs_runs) +
                                ",\"lit_pair_probe_lru16_misses\":" + std::to_string(current.lit_pair_probe_lru16_misses - previous_counters.lit_pair_probe_lru16_misses) +
                                ",\"lit_pair_probe_lru32_misses\":" + std::to_string(current.lit_pair_probe_lru32_misses - previous_counters.lit_pair_probe_lru32_misses) +
                                ",\"lit_pair_probe_lru16_vs_runs\":" + std::to_string(current.lit_pair_probe_lru16_vs_runs - previous_counters.lit_pair_probe_lru16_vs_runs) +
                                ",\"lit_pair_probe_lru32_vs_runs\":" + std::to_string(current.lit_pair_probe_lru32_vs_runs - previous_counters.lit_pair_probe_lru32_vs_runs) +
                                ",\"lit_pair_probe_pair_table_bytes\":" + std::to_string(current.lit_pair_probe_pair_table_bytes - previous_counters.lit_pair_probe_pair_table_bytes) +
                                ",\"lit_pair_probe_selected_draws\":" + std::to_string(current.lit_pair_probe_selected_draws - previous_counters.lit_pair_probe_selected_draws) +
                                ",\"lit_pair_probe_selected_baseline_vs_runs\":" + std::to_string(current.lit_pair_probe_selected_baseline_vs_runs - previous_counters.lit_pair_probe_selected_baseline_vs_runs) +
                                ",\"lit_pair_probe_selected_lru32_vs_runs\":" + std::to_string(current.lit_pair_probe_selected_lru32_vs_runs - previous_counters.lit_pair_probe_selected_lru32_vs_runs) +
                                ",\"vertex_upload_reused_bytes\":" + std::to_string(current.vertex_upload_reused_bytes - previous_counters.vertex_upload_reused_bytes) +
                                ",\"vertex_upload_reused_textures\":" + std::to_string(current.vertex_upload_reused_textures - previous_counters.vertex_upload_reused_textures) +
                                ",\"vertex_reuse_check_ms_per_frame\":" + std::to_string((current.vertex_reuse_check_seconds - previous_counters.vertex_reuse_check_seconds)*1000/count) +
                                ",\"vertex_index_upload_bytes\":" + std::to_string(current.vertex_index_upload_bytes - previous_counters.vertex_index_upload_bytes) +
                                ",\"gpu_input_records\":" + std::to_string(current.gpu_input_records - previous_counters.gpu_input_records) +
                                ",\"gpu_indexed_input_draws\":" + std::to_string(current.gpu_indexed_input_draws - previous_counters.gpu_indexed_input_draws) +
                                ",\"gpu_packed_input_draws\":" + std::to_string(current.gpu_packed_input_draws - previous_counters.gpu_packed_input_draws) +
                                ",\"packed_validation_failures\":" + std::to_string(current.packed_validation_failures) +
                                ",\"packed_verified_draws\":" + std::to_string(current.packed_verified_draws) +
                                ",\"vertex_orphan_upload_bytes\":" + std::to_string(current.vertex_orphan_upload_bytes - previous_counters.vertex_orphan_upload_bytes) +
                                ",\"vertex_gl_image_calls\":" + std::to_string(current.vertex_gl_image_calls - previous_counters.vertex_gl_image_calls) +
                                ",\"vertex_gl_subimage_calls\":" + std::to_string(current.vertex_gl_subimage_calls - previous_counters.vertex_gl_subimage_calls) +
                                ",\"vertex_reuse_expanded\":" + std::to_string(current.vertex_reuse_expanded - previous_counters.vertex_reuse_expanded) +
                                ",\"vertex_reuse_unique\":" + std::to_string(current.vertex_reuse_unique - previous_counters.vertex_reuse_unique) +
                                ",\"vertex_reuse_attribute_bytes\":" + std::to_string(current.vertex_reuse_attribute_bytes - previous_counters.vertex_reuse_attribute_bytes) +
                                ",\"vertex_reuse_unique_attribute_bytes\":" + std::to_string(current.vertex_reuse_unique_attribute_bytes - previous_counters.vertex_reuse_unique_attribute_bytes) +
                                ",\"vertex_reuse_invalid_draws\":" + std::to_string(current.vertex_reuse_invalid_draws - previous_counters.vertex_reuse_invalid_draws) +
                                ",\"vertex_upload_bank_budget_bytes\":" + std::to_string(current.vertex_upload_bank_budget_bytes) +
                                ",\"guest_pc\":[" + std::to_string(current.last_guest_pc[0]) + "," + std::to_string(current.last_guest_pc[1]) + "," + std::to_string(current.last_guest_pc[2]) + "," + std::to_string(current.last_guest_pc[3]) + "]" +
                                ",\"guest_core_ticks\":[" + std::to_string(current.guest_core_ticks[0] - previous_counters.guest_core_ticks[0]) + "," + std::to_string(current.guest_core_ticks[1] - previous_counters.guest_core_ticks[1]) + "," + std::to_string(current.guest_core_ticks[2] - previous_counters.guest_core_ticks[2]) + "," + std::to_string(current.guest_core_ticks[3] - previous_counters.guest_core_ticks[3]) + "]" +
                                ",\"guest_ticks\":" + std::to_string(current.guest_ticks - previous_counters.guest_ticks) +
                                ",\"readback_ms_per_frame\":" + std::to_string((current.readback_seconds - previous_counters.readback_seconds) * 1000 / count) +
                                ",\"present_ms_per_frame\":" + std::to_string((current.present_seconds - previous_counters.present_seconds) * 1000 / count) +
                                ",\"shader_calls\":" + std::to_string(current.shader_calls - previous_counters.shader_calls) +
                                ",\"gpu_vertex_draws\":" + std::to_string(current.gpu_vertex_draws - previous_counters.gpu_vertex_draws) +
                                ",\"gpu_lit_pair_draws\":" + std::to_string(current.gpu_lit_pair_draws - previous_counters.gpu_lit_pair_draws) +
                                ",\"gpu_lit_pair_vertices\":" + std::to_string(current.gpu_lit_pair_vertices - previous_counters.gpu_lit_pair_vertices) +
                                ",\"gpu_lit_pair_ids\":" + std::to_string(current.gpu_lit_pair_ids - previous_counters.gpu_lit_pair_ids) +
                                ",\"gpu_direct_vertex_draws\":" + std::to_string(current.gpu_direct_vertex_draws - previous_counters.gpu_direct_vertex_draws) +
                                ",\"gpu_direct_vertices\":" + std::to_string(current.gpu_direct_vertices - previous_counters.gpu_direct_vertices) +
                                ",\"gpu_direct_unique_inputs\":" + std::to_string(current.gpu_direct_unique_inputs - previous_counters.gpu_direct_unique_inputs) +
                                ",\"gpu_feedback_vertex_draws\":" + std::to_string(current.gpu_feedback_vertex_draws - previous_counters.gpu_feedback_vertex_draws) +
                                ",\"gpu_feedback_source_vertices\":" + std::to_string(current.gpu_feedback_source_vertices - previous_counters.gpu_feedback_source_vertices) +
                                ",\"gpu_vertices\":" + std::to_string(current.gpu_vertices - previous_counters.gpu_vertices) +
                                ",\"cpu_vertex_draws\":" + std::to_string(current.cpu_vertex_draws - previous_counters.cpu_vertex_draws) + category_window + "}");
                            previous_counters = current;
                            previous_host_cpu = host_cpu;
                            frame_times.clear();
                            std::ofstream timing_report(user + "frame-timing.json");
                            timing_report << "{\"title_id\":" << system.GetTitleID()
                                          << ",\"rom_filename\":" << std::quoted(std::string(FileUtil::GetFilename(rom)))
                                          << ",\"cpu_backend\":" << std::quoted(CaraSelectedCPUBackend())
                                          << ",\"engine_revision\":" << std::quoted(Common::g_scm_rev)
                                          << ",\"engine_build_fingerprint\":" << std::quoted(Core::GetSaveStateBuildFingerprint())
                                          << ",\"cpu_category_timing\":" << (category_timing?"true":"false")
                                          << ",\"cpu_category_clock_calibrated\":" << (category_clock_calibrated?"true":"false")
                                          << ",\"cpu_category_clock_ns_per_read\":" << category_clock_ns_per_read
                                          << ",\"cpu_category_names\":[\"others\",\"cpu_execution_including_memory\",\"hle_svc\",\"gpu_command\",\"dsp_hle_tick\"]"
                                          << ",\"worker_interactive_qos_requested\":" << (worker_interactive_qos?"true":"false")
                                          << ",\"worker_qos_set_result\":" << (worker_interactive_qos?std::to_string(worker_qos_set_result):"null")
                                          << ",\"worker_qos_initial_class_self\":" << static_cast<unsigned>(worker_qos_initial)
                                          << ",\"worker_qos_class_self\":" << static_cast<unsigned>(qos_class_self())
                                          << ",\"vertex_reuse_probe\":" << (std::getenv("CARA_VERTEX_REUSE_TRACE")?"true":"false")
                                          << ",\"instrumented\":true,\"uncapped\":" << (pace_frames ? "false" : "true") << ",\"windows\":[";
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
                    if (emuWindow->frames != paced_frame) {
                        const auto now=std::chrono::steady_clock::now();
                        const bool should_pace=Cara::ShouldPaceFrames(target_frames!=0,benchmark_paced,has_speed_override,self->speedLimitEnabled.load());
                        const bool pacing_changed=should_pace!=pace_frames; pace_frames=should_pace;
                        if (!has_speed_override) paced_speed=self->maximumSpeedLimit.load();
                        if (!pace_frames || pacing_changed || emuWindow->frames<paced_frame) {
                            // Uncapped play, policy changes and frame resets discard old deadlines.
                            frame_pacer.Reset(host_seconds(now));
                        } else {
                            frame_pacer.SetSpeedPercent(paced_speed,host_seconds(now));
                            const double deadline=frame_pacer.Advance(emuWindow->frames-paced_frame,host_seconds(now));
                            if (deadline<=host_seconds(now)) ++late_paced_frames;
                            const auto wake=std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(deadline)));
                            std::this_thread::sleep_until(wake);
                        }
                        paced_frame=emuWindow->frames;
                    }
                }
                if (target_frames) {
                    const double wall_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - benchmark_start).count();
                    const double pause_elapsed = std::chrono::duration<double>(paused_duration).count();
                    const double elapsed = std::max(0.001, wall_elapsed - pause_elapsed);
                    std::ofstream report(user + "benchmark-status.json");
                    report << "{\"title_id\":" << system.GetTitleID() << ",\"rom_filename\":" << std::quoted(std::string(FileUtil::GetFilename(rom))) << ",\"frames\":" << emuWindow->frames << ",\"target_frames\":" << target_frames << ",\"result_code\":" << unsigned(result) << ",\"wall_seconds\":" << wall_elapsed << ",\"paused_seconds\":" << pause_elapsed << ",\"elapsed_seconds\":" << elapsed
                           << ",\"fps\":" << emuWindow->frames / elapsed
                           << ",\"paced\":" << (pace_frames ? "true" : "false")
                           << ",\"maximum_speed_percent\":" << paced_speed
                           << ",\"late_paced_frames\":" << late_paced_frames
                           << ",\"cpu_backend\":\"" << ((std::getenv("CARA_CPU_BACKEND") && std::strcmp(std::getenv("CARA_CPU_BACKEND"), "dyncom") == 0) ? "dyncom" : "fastinterp") << "\""
                           << ",\"adaptive_guest_cpu\":" << (clock_control.Enabled()?"true":"false")
                           << ",\"guest_cpu_percent\":" << policy_clock()
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
#ifdef CARA_IOS_GLES
                // Capture only after measured work ends, before renderer destruction.
                // Raw pixels avoid UIKit/display color conversion in replay comparisons.
                if (target_frames && std::getenv("CARA_BENCHMARK_CAPTURE")) {
                    const auto previous = OpenGL::OpenGLState::BindReadFramebuffer(emuWindow->framebuffer);
                    GLint pack_buffer=0, pack_row_length=0, pack_alignment=0;
                    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
                    glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
                    glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
                    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
                    glPixelStorei(GL_PACK_ALIGNMENT, 4);
                    std::vector<u8> pixels(400 * 480 * 4);
                    glReadPixels(0, 0, 400, 480, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                    const GLenum capture_error = glGetError();
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
                    glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
                    glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
                    OpenGL::OpenGLState::BindReadFramebuffer(previous);
                    if (capture_error == GL_NO_ERROR) {
                        std::ofstream raw(user+"benchmark-frame.rgba", std::ios::binary);
                        raw.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
                    }
                    std::ofstream(user+"benchmark-frame.json")
                        << "{\"width\":400,\"height\":480,\"origin\":\"bottom_left\",\"format\":\"RGBA8\",\"gl_error\":"
                        << capture_error << ",\"frame\":" << emuWindow->frames << "}\n";
                }
#endif
                if (std::getenv("CARA_FASTINTERP_PROFILE")) {
                    std::ofstream profile(user+"fastinterp-blocks.json");
                    profile << "{\"frame\":" << emuWindow->frames << ",\"kind\":\"resident_block_census_not_time_samples\",\"cores\":[";
                    bool comma=false;
                    for (u32 id=0;id<system.GetNumCores();++id) {
                        if (auto* cpu=dynamic_cast<Core::FastInterp::ARM_FastInterp*>(&system.GetCore(id))) {
                            if (comma) profile << ','; cpu->WriteBlockProfile(profile); comma=true;
                        }
                    }
                    profile << "]}\n";
                }
                system.Shutdown();
                [saveSync captureCommittedSave];
                [stateSync captureCommittedSave];
            }
#ifdef CARA_PGO_GENERATE
            const int pgo_status=__llvm_profile_write_file();
            std::ofstream(user+"cpu-training-status.json") << "{\"write_status\":" << pgo_status << "}\n";
            __llvm_profile_set_filename("/dev/null");
#endif
            AudioCore::SetIOSAudioPaused(true);
            // Raw benchmark capture above must retain the final game pixels.
            emuWindow->ClearDisplay();
            emuWindow->DoneCurrent();
            running = false; gameLoaded=false;
            dispatch_async(dispatch_get_main_queue(), ^{
                if (self->running || self->sessionGeneration!=session) return;
                const bool keepAwake = target_frames && std::getenv("CARA_TEST_KEEP_AWAKE");
                UIApplication.sharedApplication.idleTimerDisabled = keepAwake;
                if (keepAwake) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 900 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                    if (!self->running) UIApplication.sharedApplication.idleTimerDisabled = NO;
                });
                if (result != Core::System::ResultStatus::Success && !self->sessionFailure)
                    self->sessionFailure = [NSString stringWithFormat:@"Stopped · Error %u",unsigned(result)];
                [self updateSessionPresentation];
                self->libraryController.canResume=NO;
                if (!target_frames && !self->openingGame) {
                    [self showLibrary];
                    if (result != Core::System::ResultStatus::Success && !self.presentedViewController) [self shellError:@"Game stopped" message:self->status.text];
                }
            });
        }
    });
}
- (void)dealloc {
    stop = true;
    if (worker.joinable()) worker.join();
    [cloudFeedbackTimer invalidate];
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
