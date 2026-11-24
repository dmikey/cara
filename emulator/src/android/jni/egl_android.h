#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "cara/presentation_guard.h"
#include "core/frontend/emu_window.h"

struct ANativeWindow;
class SharedContext_Android;
#ifdef CARA_GL_CONTEXT_DISPATCH
namespace Common { class HostRenderQueue; }
#endif

class EGLAndroid : public Frontend::EmuWindow {
public:
    explicit EGLAndroid(bool use_shared_context);
    ~EGLAndroid() override;

    bool Initialize(ANativeWindow* surface);
#ifdef CARA_GL_CONTEXT_DISPATCH
    bool StartSubmissionWorker();
    bool StopSubmissionWorker() noexcept;
    bool HasCoreContext() const;
#endif
    void UpdateSurface(ANativeWindow* surface);
    void UpdateWindow();
    void UpdateLayout();

    void MakeCurrent() override;
    void DoneCurrent() override;
    void PollEvents() override;
    void SwapBuffers() override;
    bool ShouldPresentFrame() override;
    void SetAdaptiveFrameSkip(bool enabled);

    void TryPresenting();
    void StopPresenting();

private:
    void CreateWindowSurface();
    void CreateEGLWindowSurface();
    void RefreshWindowLayout();
    bool BindCoreContext();
    bool UnbindCoreContext();
    void PollInlineSurface();
    void DestroyWindowSurface();
    void DestroyContext();

    ANativeWindow* new_window = nullptr;
    ANativeWindow* host_window = nullptr;
    // The EGL owner keeps the old window alive until its queued swaps have retired.
    std::shared_ptr<ANativeWindow> inline_window;
    std::mutex pending_window_mutex;
    std::optional<std::shared_ptr<ANativeWindow>> pending_inline_window;
    std::atomic<bool> pending_inline_layout{false};

    EGLint window_width = 1;
    EGLint window_height = 1;

    EGLConfig egl_config = nullptr;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLSurface inline_pbuffer = EGL_NO_SURFACE;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLDisplay egl_display = EGL_NO_DISPLAY;

    std::unique_ptr<SharedContext_Android> core_context;
#ifdef CARA_GL_CONTEXT_DISPATCH
    std::unique_ptr<Common::HostRenderQueue> submission;
#endif

    enum class PresentingState {
        Initial,
        Running,
        Stopped,
    };
    PresentingState presenting_state{};
    bool use_shared_context;
    bool adaptive_frame_skip = false;
    std::atomic<std::uint64_t> display_ticket{0};
    Cara::PresentationGuard presentation_guard;
};
