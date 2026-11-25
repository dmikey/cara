#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "cara/presentation_guard.h"
#include "cara/android_dual_display_plan.h"
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
    void UpdateExternalDisplay(ANativeWindow* surface,int mode);
    void PrimaryTouch(int x,int y,int action);
    void ExternalTouch(float x,float y,int action);
    int ExternalDisplayMode() const override { return external_mode.load(std::memory_order_acquire); }
    void DisableExternalDisplay() override { UpdateExternalDisplay(nullptr,0); }
    bool PresentDualFrame(unsigned framebuffer,unsigned width,unsigned height) override;
    bool IsPresentationContextCurrent() const override { return eglGetCurrentContext()==egl_context; }
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
    bool RecoverPresentationContext();
    bool UnbindCoreContext();
    void DestroyWindowSurface();
    void DestroyContext();
    void ApplyExternalRequest();
    void DestroyExternalSurface();
    Layout::FramebufferLayout PrimaryLayoutForMode(int mode);
    bool PresentAtlasOnOwner(unsigned framebuffer,unsigned width,unsigned height,
                             Layout::FramebufferLayout primary,int mode);
    struct ExternalRequest {std::shared_ptr<ANativeWindow> window;int mode;};
    // EGL surface creation, use and deletion are restricted to the same
    // owner as the primary presentation context. Producers retain requests.
    std::mutex external_request_mutex;
    std::optional<ExternalRequest> pending_external;
    std::shared_ptr<ANativeWindow> external_window;
    EGLSurface external_surface=EGL_NO_SURFACE;
    std::atomic<int> external_mode{0};
    std::mutex external_layout_mutex;
    Layout::FramebufferLayout primary_snapshot{},primary_combined{};
    Cara::AndroidDualDisplay::Insets primary_insets{};
    int primary_snapshot_mode=0;
    unsigned external_width=0,external_height=0;
    bool external_touch_down=false,primary_touch_down=false;
    std::atomic<bool> external_requested_touch{false};

    ANativeWindow* host_window = nullptr;
    // The EGL owner keeps the old window alive until its queued swaps have retired.
    std::shared_ptr<ANativeWindow> active_window;
    std::mutex pending_window_mutex;
    std::optional<std::shared_ptr<ANativeWindow>> pending_window;
    std::atomic<bool> pending_layout{false};

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
    std::atomic<PresentingState> presenting_state{PresentingState::Initial};
    bool use_shared_context;
    bool adaptive_frame_skip = false;
    std::atomic<std::uint64_t> display_ticket{0};
    Cara::PresentationGuard presentation_guard;
};
