#include "egl_android.h"

#include <android/native_window_jni.h>
#include <glad/glad.h>
#include <array>
#include <android/log.h>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef CARA_GL_CONTEXT_DISPATCH
#include "common/thread.h"
#include "video_core/renderer_opengl/gl_command_queue.h"
#endif
#include "jni_common.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

static constexpr std::array<EGLint, 15> egl_attribs{EGL_SURFACE_TYPE,
                                                    EGL_WINDOW_BIT,
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

static void SwapWithTiming(EGLDisplay display, EGLSurface surface, const char* route) {
    static const bool enabled = [] {
        for (const char* flag : {"CARA_TRACE_GLES_GPU_TIMING", "CARA_TRACE_GLES_SWAP_TIMING"}) {
            const char* value = std::getenv(flag);
            if (value && std::strcmp(value, "1") == 0)
                return true;
        }
        return false;
    }();
    if (!enabled) {
        eglSwapBuffers(display, surface);
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const EGLBoolean result = eglSwapBuffers(display, surface);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    __android_log_print(ANDROID_LOG_INFO, "CaraGPU", "swap route=%s us=%lld ok=%d", route,
                        static_cast<long long>(elapsed), static_cast<int>(result));
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
    host_window = surface;
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
    }

    CreateWindowSurface();

    return gladLoadGLES2Loader((GLADloadproc)eglGetProcAddress);
}

void EGLAndroid::UpdateSurface(ANativeWindow* surface) {
    new_window = surface;
    StopPresenting();
}

void EGLAndroid::UpdateWindow() {
    window_width = ANativeWindow_getWidth(host_window);
    window_height = ANativeWindow_getHeight(host_window);
    safe_inset_left = static_cast<u32>(NativeLibrary::GetSafeInsetLeft());
    safe_inset_top = static_cast<u32>(NativeLibrary::GetSafeInsetTop());
    safe_inset_right = static_cast<u32>(NativeLibrary::GetSafeInsetRight());
    safe_inset_bottom = static_cast<u32>(NativeLibrary::GetSafeInsetBottom());
    UpdateLayout();
}

void EGLAndroid::UpdateLayout() {
    UpdateFramebufferLayout(window_width, window_height);
}

void EGLAndroid::CreateWindowSurface() {
    if (!host_window) {
        return;
    }
    EGLint format;
    eglGetConfigAttrib(egl_display, egl_config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(host_window, 0, 0, format);
    egl_surface = eglCreateWindowSurface(egl_display, egl_config, host_window, nullptr);
    eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &window_width);
    eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &window_height);
    eglSurfaceAttrib(egl_display, egl_surface, EGL_SWAP_BEHAVIOR, EGL_BUFFER_DESTROYED);

    // screen
    safe_inset_left = static_cast<u32>(NativeLibrary::GetSafeInsetLeft());
    safe_inset_top = static_cast<u32>(NativeLibrary::GetSafeInsetTop());
    safe_inset_right = static_cast<u32>(NativeLibrary::GetSafeInsetRight());
    safe_inset_bottom = static_cast<u32>(NativeLibrary::GetSafeInsetBottom());
    scale_density = static_cast<float>(NativeLibrary::GetScaleDensity());

    MakeCurrent();
    UpdateLayout();
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
    if (!core_context || !core_context->DoneCurrent())
        return false;
    try {
        submission = std::make_unique<Common::HostRenderQueue>(128, 8 * 1024 * 1024,
            [this] {
                Common::SetCurrentThreadName("CaraGLES");
                if (!core_context->MakeCurrent())
                    throw std::runtime_error("EGL pbuffer ownership transfer failed");
            }, [this] { core_context->DoneCurrent(); });
        OpenGL::CommandQueue::Install(*submission);
        return true;
    } catch (const std::exception& error) {
        LOG_ERROR(Frontend, "Submission worker startup failed: {}", error.what());
        if (submission)
            StopSubmissionWorker();
        else if (!core_context->MakeCurrent())
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
    if (!core_context->MakeCurrent())
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
    core_context.reset();
    DestroyWindowSurface();
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
    if (presenting_state != PresentingState::Running) {
        if (presenting_state == PresentingState::Initial) {
            eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            presenting_state = PresentingState::Running;
        } else {
            return;
        }
    }
    if (VideoCore::Renderer()->TryPresent()) {
        SwapWithTiming(egl_display, egl_surface, "mailbox");
    }
}

void EGLAndroid::StopPresenting() {
    if (presenting_state == PresentingState::Running) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    presenting_state = PresentingState::Stopped;
}

void EGLAndroid::SwapBuffers() {
    SwapWithTiming(egl_display, egl_surface, "inline");
}

void EGLAndroid::PollEvents() {
    if (!new_window) {
        return;
    }

    host_window = new_window;
    new_window = nullptr;
    DestroyWindowSurface();
    CreateWindowSurface();
    VideoCore::Renderer()->ResetPresent();
    presentation_guard.Reset();
    if (use_shared_context) {
        presenting_state = PresentingState::Initial;
    }
}

void EGLAndroid::MakeCurrent() {
#ifdef CARA_GL_CONTEXT_DISPATCH
    // Window-surface recreation on the producer must not steal the worker pbuffer.
    if (submission) return;
#endif
    if (use_shared_context) {
        core_context->MakeCurrent();
    } else {
        eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    }
}

void EGLAndroid::DoneCurrent() {
    if (use_shared_context) {
        core_context->DoneCurrent();
    } else {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}
