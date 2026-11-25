#include "egl_android.h"

#include <android/native_window_jni.h>
#include <glad/glad.h>
#include <array>
#include <android/log.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef CARA_GL_CONTEXT_DISPATCH
#include "common/thread.h"
#include "video_core/renderer_opengl/gl_command_queue.h"
#endif
#include "jni_common.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

static constexpr std::array<EGLint, 15> egl_attribs{EGL_SURFACE_TYPE,
                                                    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                                    EGL_RENDERABLE_TYPE,
                                                    EGL_OPENGL_ES3_BIT_KHR,
                                                    EGL_BLUE_SIZE,
                                                    8,
                                                    EGL_GREEN_SIZE,
                                                    8,
                                                    EGL_RED_SIZE,
                                                    8,
                                                    EGL_DEPTH_SIZE,
                                                    0,
                                                    EGL_STENCIL_SIZE,
                                                    0,
                                                    EGL_NONE};
static constexpr std::array<EGLint, 5> egl_empty_attribs{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
static constexpr std::array<EGLint, 4> egl_context_attribs{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

static EGLBoolean SwapWithTiming(EGLDisplay display, EGLSurface surface, const char* route) {
    static const bool enabled = [] {
        for (const char* flag : {"CARA_TRACE_GLES_GPU_TIMING", "CARA_TRACE_GLES_SWAP_TIMING"}) {
            const char* value = std::getenv(flag);
            if (value && std::strcmp(value, "1") == 0)
                return true;
        }
        return false;
    }();
    if (!enabled) {
        return eglSwapBuffers(display, surface);
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const EGLBoolean result = eglSwapBuffers(display, surface);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    __android_log_print(ANDROID_LOG_INFO, "CaraGPU", "swap route=%s us=%lld ok=%d", route,
                        static_cast<long long>(elapsed), static_cast<int>(result));
    return result;
}

class SharedContext_Android {
public:
    SharedContext_Android(EGLDisplay egl_display, EGLConfig egl_config,
                          EGLContext egl_share_context)
        : egl_display{egl_display}, egl_surface{eglCreatePbufferSurface(egl_display, egl_config,
                                                                        egl_empty_attribs.data())},
          egl_context{eglCreateContext(egl_display, egl_config, egl_share_context,
                                       egl_context_attribs.data())} {}

    ~SharedContext_Android() {
        if (!eglDestroySurface(egl_display, egl_surface)) {
            LOG_CRITICAL(Frontend, "eglDestroySurface() failed");
        }

        if (!eglDestroyContext(egl_display, egl_context)) {
            LOG_CRITICAL(Frontend, "eglDestroySurface() failed");
        }
    }

    bool IsCurrent() const { return eglGetCurrentContext() == egl_context; }

    bool MakeCurrent() {
        return eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    }

    bool DoneCurrent() {
        return eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

private:
    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLContext egl_context;
};

static std::shared_ptr<ANativeWindow> RetainWindow(ANativeWindow* window) {
    if (!window)
        return {};
    ANativeWindow_acquire(window);
    return {window, ANativeWindow_release};
}

EGLAndroid::EGLAndroid(bool use_shared_context) : use_shared_context(use_shared_context) {}

bool EGLAndroid::Initialize(ANativeWindow* surface) {
    setenv("CARA_VS_UNIFORM_REUSE", "1", 0);
#ifdef CARA_ANDROID_GLES_ATLAS
    setenv("CARA_GPU_PACKED_INPUTS", "1", 0);
    setenv("CARA_VERTEX_UPLOAD_REUSE", "1", 0);
    setenv("CARA_VERTEX_INDEX_CACHE", "1", 0);
    setenv("CARA_VERTEX_UPLOAD_BANK_MIB", "80", 0);
    setenv("CARA_GPU_DEPTH_REINTERPRET", "1", 0);
    setenv("CARA_VERTEX_UPLOAD_ORDER_REUSE", "1", 0);
#endif
    active_window = RetainWindow(surface);
    host_window = active_window.get();
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!egl_display) {
        // Error: eglGetDisplay() failed
        return false;
    }

    EGLint egl_major, egl_minor;
    if (!eglInitialize(egl_display, &egl_major, &egl_minor)) {
        // Error: eglInitialize() failed
        return false;
    }

    EGLint num_configs;
    if (!eglChooseConfig(egl_display, egl_attribs.data(), &egl_config, 1, &num_configs)) {
        // Error: couldn't get an EGL visual config
        return false;
    }

    egl_context = eglCreateContext(egl_display, egl_config, 0, egl_context_attribs.data());
    if (egl_context == EGL_NO_CONTEXT) {
        return false;
    }

    if (use_shared_context) {
        core_context =
            std::make_unique<SharedContext_Android>(egl_display, egl_config, egl_context);
    } else {
        presenting_state = PresentingState::Stopped;
        inline_pbuffer = eglCreatePbufferSurface(egl_display, egl_config, egl_empty_attribs.data());
        if (inline_pbuffer == EGL_NO_SURFACE)
            return false;
    }

    CreateWindowSurface();
    if (!BindCoreContext())
        return false;

    return gladLoadGLES2Loader((GLADloadproc)eglGetProcAddress);
}

void EGLAndroid::UpdateSurface(ANativeWindow* surface) {
    auto window = RetainWindow(surface);
    std::lock_guard lock(pending_window_mutex);
    if (use_shared_context)
        StopPresenting();
    pending_window = std::move(window);
}

void EGLAndroid::UpdateWindow() {
    pending_layout.store(true, std::memory_order_release);
}

void EGLAndroid::RefreshWindowLayout() {
    if (!host_window)
        return;
    window_width = ANativeWindow_getWidth(host_window);
    window_height = ANativeWindow_getHeight(host_window);
    safe_inset_left = static_cast<u32>(NativeLibrary::GetSafeInsetLeft());
    safe_inset_top = static_cast<u32>(NativeLibrary::GetSafeInsetTop());
    safe_inset_right = static_cast<u32>(NativeLibrary::GetSafeInsetRight());
    safe_inset_bottom = static_cast<u32>(NativeLibrary::GetSafeInsetBottom());
    scale_density = static_cast<float>(NativeLibrary::GetScaleDensity());
    UpdateLayout();
}

void EGLAndroid::UpdateLayout() {
    std::lock_guard lock(external_layout_mutex);
    UpdateFramebufferLayout(std::max(1,window_width),std::max(1,window_height));
    primary_combined=GetFramebufferLayout();
    primary_insets={safe_inset_left,safe_inset_top,safe_inset_right,safe_inset_bottom};
    const int mode=ExternalDisplayMode();
    primary_snapshot=PrimaryLayoutForMode(mode);
    primary_snapshot_mode=mode;
    // Keep the core framebuffer layout combined: its top rectangle also drives
    // automatic internal resolution and screenshot dimensions. Only the native
    // presentation packet and touch route use the split physical layout.
}

void EGLAndroid::CreateWindowSurface() {
    CreateEGLWindowSurface();
    MakeCurrent();
    RefreshWindowLayout();
}

void EGLAndroid::CreateEGLWindowSurface() {
    if (!host_window)
        return;
    EGLint format;
    eglGetConfigAttrib(egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(host_window, 0, 0, format);
    egl_surface = eglCreateWindowSurface(egl_display, egl_config, host_window, nullptr);
    if (egl_surface != EGL_NO_SURFACE)
        eglSurfaceAttrib(egl_display, egl_surface, EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED);
}

void EGLAndroid::DestroyWindowSurface() {
    if (!egl_surface) {
        return;
    }
    if (eglGetCurrentSurface(EGL_DRAW) == egl_surface) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (!eglDestroySurface(egl_display, egl_surface)) {
        // Could not destroy window surface
    }
    egl_surface = EGL_NO_SURFACE;
}

void EGLAndroid::DestroyContext() {
    if (!egl_context) {
        return;
    }
    if (eglGetCurrentContext() == egl_context) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (!eglDestroyContext(egl_display, egl_context)) {
        // Could not destroy drawing context
    }
    if (!eglTerminate(egl_display)) {
        // Could not destroy display connection
    }
    egl_context = EGL_NO_CONTEXT;
    egl_display = EGL_NO_DISPLAY;
}

#ifdef CARA_GL_CONTEXT_DISPATCH
bool EGLAndroid::StartSubmissionWorker() {
    if (!UnbindCoreContext())
        return false;
    try {
        submission = std::make_unique<Common::HostRenderQueue>(128, 8 * 1024 * 1024,
            [this] {
                Common::SetCurrentThreadName("CaraGLES");
                if (!BindCoreContext())
                    throw std::runtime_error("EGL render context ownership transfer failed");
            }, [this] { UnbindCoreContext(); });
        OpenGL::CommandQueue::Install(*submission);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR(Frontend, "Submission worker startup failed: {}", error.what());
        if (submission)
            StopSubmissionWorker();
        else if (!BindCoreContext())
            LOG_ERROR(Frontend, "Core EGL context unavailable after failed worker startup");
        return false;
    }
}

bool EGLAndroid::HasCoreContext() const {
    return core_context ? core_context->IsCurrent() : eglGetCurrentContext() == egl_context;
}

bool EGLAndroid::StopSubmissionWorker() noexcept {
    if (!submission)
        return true;
    bool clean = true;
    try {
        if (OpenGL::CommandQueue::Queue() == submission.get())
            OpenGL::CommandQueue::Uninstall();
    } catch (...) {
        clean = false;
    }
    // Stop joins even if the queue is poisoned. Uninstall restores dispatch on failure.
    try {
        submission->Stop();
    } catch (...) {
        clean = false;
    }
    submission.reset();
    if (!BindCoreContext())
        clean = false;
    if (!clean)
        LOG_ERROR(Frontend, "GLES worker stopped after queue or EGL context loss");
    return clean;
}
#endif

EGLAndroid::~EGLAndroid() {
#ifdef CARA_GL_CONTEXT_DISPATCH
    StopSubmissionWorker();
#endif
    DestroyExternalSurface();
    core_context.reset();
    DestroyWindowSurface();
    if (inline_pbuffer != EGL_NO_SURFACE) {
        if (eglGetCurrentSurface(EGL_DRAW) == inline_pbuffer)
            eglMakeCurrent(egl_display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        eglDestroySurface(egl_display, inline_pbuffer);
    }
    DestroyContext();
}

bool EGLAndroid::ShouldPresentFrame() {
    return presentation_guard.Admit(display_ticket.load(std::memory_order_relaxed),
                                    adaptive_frame_skip);
}

void EGLAndroid::SetAdaptiveFrameSkip(bool enabled) {
    adaptive_frame_skip = enabled;
    presentation_guard.Reset();
}

void EGLAndroid::TryPresenting() {
    display_ticket.fetch_add(1, std::memory_order_relaxed);
    if (!use_shared_context)
        return;
    std::unique_lock lock(pending_window_mutex, std::try_to_lock);
    if (!lock || pending_window)
        return;
    if (presenting_state != PresentingState::Running) {
        if (presenting_state == PresentingState::Initial) {
            if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context))
                return;
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            presenting_state = PresentingState::Running;
        } else {
            return;
        }
    }
    ApplyExternalRequest();
    if (presenting_state != PresentingState::Running || eglGetCurrentContext() != egl_context)
        return;
    if (VideoCore::Renderer()->TryPresent()) {
        SwapWithTiming(egl_display, egl_surface, "mailbox");
    }
}

void EGLAndroid::StopPresenting() {
    if (eglGetCurrentContext()==egl_context) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    presenting_state = PresentingState::Stopped;
}

void EGLAndroid::SwapBuffers() {
    if (egl_surface == EGL_NO_SURFACE)
        return;
#ifdef CARA_GL_CONTEXT_DISPATCH
    if (submission && !use_shared_context) {
        // Submit after the composed framebuffer draws and before any later GL work.
        OpenGL::CommandQueue::SubmitHost([display = egl_display, surface = egl_surface] {
            SwapWithTiming(display, surface, "render-worker");
        });
        return;
    }
#endif
    SwapWithTiming(egl_display, egl_surface, "inline");
}

void EGLAndroid::PollEvents() {
    std::optional<std::shared_ptr<ANativeWindow>> replacement;
    {
        std::lock_guard lock(pending_window_mutex);
        replacement.swap(pending_window);
    }
    const bool layout_dirty = pending_layout.exchange(false, std::memory_order_acq_rel);
    if (replacement) {
        // Keep the old native window alive until its queued EGL swaps retire.
        const auto replace_surface = [this, window = replacement->get()] {
            DestroyWindowSurface();
            host_window = window;
            CreateEGLWindowSurface();
            if (!use_shared_context && !BindCoreContext()) {
                DestroyWindowSurface();
                if (!BindCoreContext())
                    throw std::runtime_error("Could not rebind the Android render surface");
            }
        };
#ifdef CARA_GL_CONTEXT_DISPATCH
        if (submission && !use_shared_context) {
            OpenGL::CommandQueue::Flush();
            submission->Call(replace_surface);
        } else
#endif
        {
            replace_surface();
        }
        active_window = std::move(*replacement);
        presentation_guard.Reset();
    }
    if (!use_shared_context) {
        bool has_request;
        { std::lock_guard lock(external_request_mutex);has_request=pending_external.has_value(); }
        if(has_request){
#ifdef CARA_GL_CONTEXT_DISPATCH
            if(submission){
                OpenGL::CommandQueue::Flush();
                submission->Call([this]{ApplyExternalRequest();});
            } else
#endif
                ApplyExternalRequest();
        }
    }
    if (replacement || layout_dirty || pending_layout.exchange(false,std::memory_order_acq_rel))
        RefreshWindowLayout();
    if (replacement && use_shared_context) {
        VideoCore::Renderer()->ResetPresent();
        presenting_state = egl_surface == EGL_NO_SURFACE ? PresentingState::Stopped
                                                        : PresentingState::Initial;
    }
}


namespace {
using Cara::AndroidDualDisplay::Rect;
using Cara::AndroidDualDisplay::Fit;
Common::Rectangle<u32> ToCommon(Rect r){return {r.left,r.top,r.right,r.bottom};}
Layout::FramebufferLayout SinglePanel(unsigned width,unsigned height,bool bottom,
                                      Cara::AndroidDualDisplay::Insets insets={}) {
    Layout::FramebufferLayout layout{width,height,!bottom,bottom,{},{}};
    auto rect=ToCommon(Fit(width,height,bottom,insets));
    if(bottom)layout.bottom_screen=rect;else layout.top_screen=rect;
    return layout;
}

// GLAD can be wrapped by the sole producer's render queue; the EGL owner must
// call real driver entrypoints for this serialized host presentation packet.
// No guest command is dispatched here. Preserve all touched GL state and check
// the driver error result rather than dropping any error.
struct DualAtlasGL {
    PFNGLGETINTEGERVPROC get_int{};
    PFNGLGETBOOLEANVPROC get_bool{};
    PFNGLISENABLEDPROC is_enabled{};
    PFNGLENABLEPROC enable{};
    PFNGLDISABLEPROC disable{};
    PFNGLCOLORMASKPROC mask{};
    PFNGLBINDFRAMEBUFFERPROC bind{};
    PFNGLCLEARBUFFERFVPROC clear{};
    PFNGLBLITFRAMEBUFFERPROC blit{};
    PFNGLGETERRORPROC error{};
    template<class T> static T Load(const char* name){return reinterpret_cast<T>(eglGetProcAddress(name));}
    DualAtlasGL():
      get_int(Load<PFNGLGETINTEGERVPROC>("glGetIntegerv")),
      get_bool(Load<PFNGLGETBOOLEANVPROC>("glGetBooleanv")),
      is_enabled(Load<PFNGLISENABLEDPROC>("glIsEnabled")),
      enable(Load<PFNGLENABLEPROC>("glEnable")),disable(Load<PFNGLDISABLEPROC>("glDisable")),
      mask(Load<PFNGLCOLORMASKPROC>("glColorMask")),bind(Load<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer")),
      clear(Load<PFNGLCLEARBUFFERFVPROC>("glClearBufferfv")),blit(Load<PFNGLBLITFRAMEBUFFERPROC>("glBlitFramebuffer")),
      error(Load<PFNGLGETERRORPROC>("glGetError")){}
    bool Valid() const {return get_int&&get_bool&&is_enabled&&enable&&disable&&mask&&bind&&clear&&blit&&error;}
};
}

void EGLAndroid::UpdateExternalDisplay(ANativeWindow* surface,int mode) {
    if(!surface || Cara::AndroidDualDisplay::Decode(mode)==Cara::AndroidDualDisplay::Mode::Off){
        surface=nullptr;mode=0;
    }
    auto window=RetainWindow(surface);
    {
        std::lock_guard lock(external_request_mutex);
        pending_external=ExternalRequest{std::move(window),mode};
    }
    external_requested_touch.store(mode==2,std::memory_order_release);
    {
        std::lock_guard lock(external_layout_mutex);
        external_touch_down=primary_touch_down=false;
        TouchReleased();
    }
}

void EGLAndroid::DestroyExternalSurface() {
    if(external_surface!=EGL_NO_SURFACE){
        // Every presentation packet restores its original EGL surface.
        if(eglGetCurrentSurface(EGL_DRAW)==external_surface)
            eglMakeCurrent(egl_display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        eglDestroySurface(egl_display,external_surface);
        external_surface=EGL_NO_SURFACE;
    }
    external_window.reset();
    {
        std::lock_guard lock(external_layout_mutex);
        external_width=external_height=0;external_touch_down=primary_touch_down=false;
        TouchReleased();
        external_mode.store(0,std::memory_order_release);
    }
    pending_layout.store(true,std::memory_order_release);
}

void EGLAndroid::ApplyExternalRequest() {
    std::optional<ExternalRequest> request;
    {
        std::lock_guard lock(external_request_mutex);
        request.swap(pending_external);
    }
    if(!request)return;
    if(!request->window || !request->mode){DestroyExternalSurface();return;}
    if(external_window.get()!=request->window.get() || external_surface==EGL_NO_SURFACE){
        DestroyExternalSurface();
        EGLint format=0;
        if(!eglGetConfigAttrib(egl_display,egl_config,EGL_NATIVE_VISUAL_ID,&format) ||
           ANativeWindow_setBuffersGeometry(request->window.get(),0,0,format)!=0){
            __android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","Could not configure external display window");return;
        }
        external_surface=eglCreateWindowSurface(egl_display,egl_config,request->window.get(),nullptr);
        if(external_surface==EGL_NO_SURFACE){
            __android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","Could not create external EGL surface: 0x%x",eglGetError());return;
        }
        external_window=request->window;
        eglSurfaceAttrib(egl_display,external_surface,EGL_SWAP_BEHAVIOR,EGL_BUFFER_DESTROYED);
    }
    EGLint width=0,height=0;
    // Prove the surface is usable on this context before splitting the primary.
    const auto previous_draw=eglGetCurrentSurface(EGL_DRAW);
    const auto previous_read=eglGetCurrentSurface(EGL_READ);
    const auto context=eglGetCurrentContext();
    if(context!=egl_context || !eglQuerySurface(egl_display,external_surface,EGL_WIDTH,&width) ||
       !eglQuerySurface(egl_display,external_surface,EGL_HEIGHT,&height) || width<=0 || height<=0 ||
       !eglMakeCurrent(egl_display,external_surface,external_surface,egl_context)){
        __android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","External display not ready; preserving combined handheld layout");
        DestroyExternalSurface();return;
    }
    if(!eglMakeCurrent(egl_display,previous_draw,previous_read,context)){
        __android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","Could not restore primary EGL surface following attach");
        DestroyExternalSurface();
        if(!RecoverPresentationContext() && !use_shared_context)
            throw std::runtime_error("Could not restore EGL owner after external display attach");
        return;
    }
    {
        std::lock_guard lock(external_layout_mutex);
        external_width=static_cast<unsigned>(width);external_height=static_cast<unsigned>(height);
        external_touch_down=primary_touch_down=false;TouchReleased();
        external_mode.store(request->mode,std::memory_order_release);
    }
    pending_layout.store(true,std::memory_order_release);
}

Layout::FramebufferLayout EGLAndroid::PrimaryLayoutForMode(int mode) {
    if(mode==0)return primary_combined;
    return SinglePanel(primary_combined.width,primary_combined.height,mode==1,primary_insets);
}

void EGLAndroid::PrimaryTouch(int x,int y,int action) {
    std::lock_guard lock(external_layout_mutex);
    const int mode=ExternalDisplayMode();
    if(mode==2)return;
    if(mode==0){
        if(action&1)TouchPressed(x,y);
        else if(action&2)TouchMoved(x,y);
        else if(action&4)TouchReleased();
        return;
    }
    if(action&4){primary_touch_down=false;TouchReleased();return;}
    if(!(action&1)&&(!(action&2)||!primary_touch_down))return;
    const auto layout=PrimaryLayoutForMode(mode);
    if(!layout.width||!layout.height)return;
    const auto r=layout.bottom_screen;
    const auto mapped=Cara::AndroidDualDisplay::Touch({r.left,r.top,r.right,r.bottom},
        layout.width,layout.height,float(x)/layout.width,float(y)/layout.height,(action&2)!=0);
    if(mapped){TouchNormalized(mapped->first,mapped->second);primary_touch_down=true;}
}

void EGLAndroid::ExternalTouch(float x,float y,int action) {
    std::lock_guard lock(external_layout_mutex);
    if(action==1 || action==3){
        if(external_touch_down){TouchReleased();external_touch_down=false;}
        return;
    }
    if(ExternalDisplayMode()!=2 || !external_requested_touch.load(std::memory_order_acquire))return;
    if(action!=0 && (action!=2 || !external_touch_down))return;
    const auto rect=Fit(external_width,external_height,true);
    const auto mapped=Cara::AndroidDualDisplay::Touch(rect,external_width,external_height,x,y,action==2);
    if(mapped){TouchNormalized(mapped->first,mapped->second);external_touch_down=true;}
}

bool EGLAndroid::PresentDualFrame(unsigned framebuffer,unsigned width,unsigned height) {
    // The source is always a canonical combined atlas. Even a queued atlas
    // from before detach can be presented into the restored combined layout.
    const int mode=ExternalDisplayMode();
    Layout::FramebufferLayout primary;
    {
        std::lock_guard lock(external_layout_mutex);
        primary=primary_snapshot_mode==mode?primary_snapshot:PrimaryLayoutForMode(mode);
    }
    const auto present=[this,framebuffer,width,height,primary,mode]{
        PresentAtlasOnOwner(framebuffer,width,height,primary,mode);
    };
#ifdef CARA_GL_CONTEXT_DISPATCH
    if(submission && !use_shared_context){
        OpenGL::CommandQueue::SubmitHost(present);
        return true;
    }
#endif
    present();
    return true;
}

bool EGLAndroid::PresentAtlasOnOwner(unsigned framebuffer,unsigned width,unsigned height,
                                     Layout::FramebufferLayout primary,int mode) {
    if(!framebuffer || !width || !height || eglGetCurrentContext()!=egl_context)return false;
    const auto previous_draw=eglGetCurrentSurface(EGL_DRAW),previous_read=eglGetCurrentSurface(EGL_READ);
    static const DualAtlasGL gl;
    if(!gl.Valid()){__android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","Missing external display GL entrypoints");DestroyExternalSurface();return false;}
    GLint read=0,draw=0;GLboolean mask[4]{};
    gl.get_int(GL_READ_FRAMEBUFFER_BINDING,&read);gl.get_int(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
    gl.get_bool(GL_COLOR_WRITEMASK,mask);
    const bool scissor=gl.is_enabled(GL_SCISSOR_TEST)==GL_TRUE;
    const auto old_error=gl.error();
    if(old_error!=GL_NO_ERROR)
        __android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","Preexisting GL error before dual presentation: 0x%x",old_error);
    const auto source=Layout::DefaultFrameLayout(width,height,false);
    auto blit=[&](const Common::Rectangle<u32>& from,const Common::Rectangle<u32>& to,unsigned dst_h){
        if(!from.GetWidth()||!from.GetHeight()||!to.GetWidth()||!to.GetHeight())return;
        const bool integer=to.GetWidth()%from.GetWidth()==0 && to.GetHeight()%from.GetHeight()==0;
        gl.blit(from.left,height-from.bottom,from.right,height-from.top,
                to.left,dst_h-to.bottom,to.right,dst_h-to.top,GL_COLOR_BUFFER_BIT,integer?GL_NEAREST:GL_LINEAR);
    };
    const auto output=[&](EGLSurface surface,const Layout::FramebufferLayout& layout,const char* route){
        if(surface==EGL_NO_SURFACE || !layout.width || !layout.height)return false;
        if(eglGetCurrentSurface(EGL_DRAW)!=surface||eglGetCurrentSurface(EGL_READ)!=surface)
            if(!eglMakeCurrent(egl_display,surface,surface,egl_context))return false;
        gl.bind(GL_READ_FRAMEBUFFER,framebuffer);
        gl.bind(GL_DRAW_FRAMEBUFFER,0);
        gl.disable(GL_SCISSOR_TEST);
        gl.mask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        const GLfloat black[4]{0,0,0,1};
        gl.clear(GL_COLOR,0,black);
        if(layout.top_screen_enabled)blit(source.top_screen,layout.top_screen,layout.height);
        if(layout.bottom_screen_enabled)blit(source.bottom_screen,layout.bottom_screen,layout.height);
        const GLenum error=gl.error();
        if(error!=GL_NO_ERROR){__android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","Dual presentation GL error 0x%x route=%s",error,route);return false;}
        return SwapWithTiming(egl_display,surface,route)==EGL_TRUE;
    };
    const bool main_ok=output(egl_surface,primary,"dual-handheld");
    bool external_ok=true;
    if(mode && ExternalDisplayMode()==mode){
        unsigned ew,eh;
        {std::lock_guard lock(external_layout_mutex);ew=external_width;eh=external_height;}
        external_ok=output(external_surface,SinglePanel(ew,eh,mode==2),"dual-external");
    }
    const bool restored=eglGetCurrentSurface(EGL_DRAW)==previous_draw&&eglGetCurrentSurface(EGL_READ)==previous_read
        ? true:eglMakeCurrent(egl_display,previous_draw,previous_read,egl_context)==EGL_TRUE;
    if(!restored){
        __android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","Dual presentation could not restore original EGL surface");
        DestroyExternalSurface();
        if(!RecoverPresentationContext()){
            if(!use_shared_context)throw std::runtime_error("Dual presentation lost EGL owner");
            return false;
        }
    }
    gl.bind(GL_READ_FRAMEBUFFER,static_cast<GLuint>(read));
    gl.bind(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(draw));
    gl.mask(mask[0],mask[1],mask[2],mask[3]);
    if(scissor)gl.enable(GL_SCISSOR_TEST);else gl.disable(GL_SCISSOR_TEST);
    const auto restore_error=gl.error();
    if(restore_error!=GL_NO_ERROR)__android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","Dual presentation GL state restore error 0x%x",restore_error);
    if(!external_ok){__android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","External display lost; preserving handheld layout");DestroyExternalSurface();}
    return main_ok&&external_ok&&restored&&restore_error==GL_NO_ERROR&&old_error==GL_NO_ERROR;
}

// Surface loss is not context loss: recover onto a tiny same-context pbuffer
// so the existing mailbox fence can still retire its source reads safely.
bool EGLAndroid::RecoverPresentationContext() {
    if(egl_surface!=EGL_NO_SURFACE && eglMakeCurrent(egl_display,egl_surface,egl_surface,egl_context))
        return true;
    if(use_shared_context)presenting_state=PresentingState::Initial;
    if(inline_pbuffer==EGL_NO_SURFACE)
        inline_pbuffer=eglCreatePbufferSurface(egl_display,egl_config,egl_empty_attribs.data());
    const bool recovered=inline_pbuffer!=EGL_NO_SURFACE &&
        eglMakeCurrent(egl_display,inline_pbuffer,inline_pbuffer,egl_context)==EGL_TRUE;
    if(!recovered)__android_log_print(ANDROID_LOG_ERROR,"CaraDisplay","External display recovery could not rebind presentation context");
    return recovered;
}

bool EGLAndroid::BindCoreContext() {
    if (use_shared_context)
        return core_context && core_context->MakeCurrent();
    const EGLSurface surface = egl_surface != EGL_NO_SURFACE ? egl_surface : inline_pbuffer;
    return eglMakeCurrent(egl_display, surface, surface, egl_context);
}

bool EGLAndroid::UnbindCoreContext() {
    if (use_shared_context)
        return core_context && core_context->DoneCurrent();
    return eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void EGLAndroid::MakeCurrent() {
#ifdef CARA_GL_CONTEXT_DISPATCH
    if (submission)
        return;
#endif
    BindCoreContext();
}

void EGLAndroid::DoneCurrent() {
#ifdef CARA_GL_CONTEXT_DISPATCH
    if (submission)
        return;
#endif
    UnbindCoreContext();
}
