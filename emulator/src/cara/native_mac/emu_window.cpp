#include "cara/native_mac/native_command_policy.h"
#include "cara/native_mac/diagnostic_clock.h"
// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "cara/lodepng_image_interface.h"
#include "common/color.h"
#include "common/host_render_queue.h"
#include "video_core/renderer_opengl/gl_command_queue.h"
#include "common/file_util.h"
#include "common/frame_timing.h"
#include "core/arm/arm_interface.h"
#include "core/core_timing.h"
#include "core/hw/gpu.h"
#include "core/hw/lcd.h"
#include "core/memory.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <fmt/format.h>
#include <glad/glad.h>
#include "cara/native_mac/emu_window.h"
#include "cara/native_mac/touch_command.h"
#include "cara/native_mac/profile_snapshot.h"
#include "cara/native_mac/benchmark_controls.h"
#include "cara/native_mac/script_only_input.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/settings.h"
#include "input_common/keyboard.h"
#include "input_common/main.h"
#include "input_common/motion_emu.h"
#include "input_common/sdl/sdl.h"
#include "network/network.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

void EmuWindow_SDL2::OnMouseMotion(s32 x, s32 y) {
    int w, h, dw, dh;
    SDL_GetWindowSize(render_window, &w, &h);
    SDL_GL_GetDrawableSize(render_window, &dw, &dh);
    TouchMoved(unsigned(std::max(x, 0)) * dw / std::max(w, 1),
               unsigned(std::max(y, 0)) * dh / std::max(h, 1));
    InputCommon::GetMotionEmu()->Tilt(x, y);
}

void EmuWindow_SDL2::OnMouseButton(u32 button, u8 state, s32 x, s32 y) {
    if (button == SDL_BUTTON_LEFT) {
        if (state == SDL_PRESSED) {
            int w, h, dw, dh;
            SDL_GetWindowSize(render_window, &w, &h);
            SDL_GL_GetDrawableSize(render_window, &dw, &dh);
            TouchPressed(unsigned(std::max(x, 0)) * dw / std::max(w, 1),
                         unsigned(std::max(y, 0)) * dh / std::max(h, 1));
        } else {
            TouchReleased();
        }
    } else if (button == SDL_BUTTON_RIGHT) {
        if (state == SDL_PRESSED) {
            InputCommon::GetMotionEmu()->BeginTilt(x, y);
        } else {
            InputCommon::GetMotionEmu()->EndTilt();
        }
    }
}

std::pair<unsigned, unsigned> EmuWindow_SDL2::TouchToPixelPos(float touch_x, float touch_y) const {
    int w, h;
    SDL_GL_GetDrawableSize(render_window, &w, &h);

    touch_x *= w;
    touch_y *= h;

    return {static_cast<unsigned>(std::max(std::round(touch_x), 0.0f)),
            static_cast<unsigned>(std::max(std::round(touch_y), 0.0f))};
}

void EmuWindow_SDL2::OnFingerDown(float x, float y) {
    // TODO(NeatNit): keep track of multitouch using the fingerID and a dictionary of some kind
    // This isn't critical because the best we can do when we have that is to average them, like the
    // 3DS does

    const auto [px, py] = TouchToPixelPos(x, y);
    TouchPressed(px, py);
}

void EmuWindow_SDL2::OnFingerMotion(float x, float y) {
    const auto [px, py] = TouchToPixelPos(x, y);
    TouchMoved(px, py);
}

void EmuWindow_SDL2::OnFingerUp() {
    TouchReleased();
}

void EmuWindow_SDL2::OnKeyEvent(int key, u8 state) {
    if (state == SDL_PRESSED) {
        InputCommon::GetKeyboard()->PressKey(key);
    } else if (state == SDL_RELEASED) {
        InputCommon::GetKeyboard()->ReleaseKey(key);
    }
}

bool EmuWindow_SDL2::IsOpen() const {
    return is_open;
}

void EmuWindow_SDL2::OnResize() {
    ValidateDiagnosticGeometry("resize", true);
    int width, height;
    SDL_GL_GetDrawableSize(render_window, &width, &height);
    UpdateFramebufferLayout(width, height);
}

void EmuWindow_SDL2::Fullscreen() {
    if (SDL_SetWindowFullscreen(render_window, SDL_WINDOW_FULLSCREEN) == 0) {
        return;
    }

    LOG_ERROR(Frontend, "Fullscreening failed: {}", SDL_GetError());

    // Try a different fullscreening method
    LOG_INFO(Frontend, "Attempting to use borderless fullscreen...");
    if (SDL_SetWindowFullscreen(render_window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0) {
        return;
    }

    LOG_ERROR(Frontend, "Borderless fullscreening failed: {}", SDL_GetError());

    // Fallback algorithm: Maximise window.
    // Works on all systems (unless something is seriously wrong), so no fallback for this one.
    LOG_INFO(Frontend, "Falling back on a maximised window...");
    SDL_MaximizeWindow(render_window);
}

namespace {
bool NativeWindowFlag(const char* name) {
    const char* value = std::getenv(name);
    return value && std::string(value) == "1";
}
int NativeExpectedDrawable(const char* name) {
    const char* value = std::getenv(name);
    if (!value || !*value)
        throw std::runtime_error("Locked native benchmark requires expected drawable dimensions");
    unsigned parsed = 0;
    for (const char* p = value; *p; ++p) {
        if (*p < '0' || *p > '9' || parsed > 16384)
            throw std::runtime_error("Invalid expected native drawable dimension");
        parsed = parsed * 10 + unsigned(*p - '0');
    }
    if (!parsed || parsed > 16384)
        throw std::runtime_error("Invalid expected native drawable dimension");
    return static_cast<int>(parsed);
}
} // namespace

void EmuWindow_SDL2::ValidateDiagnosticGeometry(const char* boundary, bool record) const {
    if (!diagnostic_window_locked)
        return;
    int width = 0, height = 0, drawable_width = 0, drawable_height = 0;
    SDL_GetWindowSize(render_window, &width, &height);
    SDL_GL_GetDrawableSize(render_window, &drawable_width, &drawable_height);
    const auto flags = SDL_GetWindowFlags(render_window);
    const bool valid = width == 400 && height == 480 &&
        drawable_width == diagnostic_drawable_width &&
        drawable_height == diagnostic_drawable_height &&
        !(flags & (SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN |
                   SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) &&
        (flags & SDL_WINDOW_SHOWN);
    if (record || !valid) {
        const auto path = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) +
                          "native-geometry-" + boundary + ".json";
        std::ofstream out(path);
        out << "{\"locked\":true,\"valid\":" << (valid ? "true" : "false")
            << ",\"logical_width\":" << width << ",\"logical_height\":" << height
            << ",\"drawable_width\":" << drawable_width
            << ",\"drawable_height\":" << drawable_height
            << ",\"expected_drawable_width\":" << diagnostic_drawable_width
            << ",\"expected_drawable_height\":" << diagnostic_drawable_height
            << ",\"sdl_window_flags\":" << flags << "}\n";
        out.close();
        if (!out)
            throw std::runtime_error("Cannot write native benchmark geometry receipt");
    }
    if (!valid)
        throw std::runtime_error("Native benchmark window/drawable geometry changed or mismatched");
}

EmuWindow_SDL2::EmuWindow_SDL2(bool fullscreen) {
    diagnostic_window_locked = NativeWindowFlag("CARA_NATIVE_LOCK_WINDOW");
    if (diagnostic_window_locked) {
        if (!NativeWindowFlag("CARA_NATIVE_DIAGNOSTICS") ||
            !NativeWindowFlag("CARA_NATIVE_BENCHMARK") || fullscreen)
            throw std::runtime_error("Native window lock requires diagnostic windowed benchmark mode");
        diagnostic_drawable_width = NativeExpectedDrawable("CARA_NATIVE_EXPECT_DRAWABLE_WIDTH");
        diagnostic_drawable_height = NativeExpectedDrawable("CARA_NATIVE_EXPECT_DRAWABLE_HEIGHT");
    }
    if (Cara::NativeScriptOnlyInput::Enabled()) {
        SDL_SetHint("SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN", "1");
        SDL_SetHint("SDL_WINDOW_ACTIVATE_WHEN_SHOWN", "0");
    }
    // Initialize the window
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        LOG_CRITICAL(Frontend, "Failed to initialize SDL2! Exiting...");
        exit(1);
    }

    InputCommon::Init();
    Network::Init();

    SDL_SetMainReady();

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    if (Settings::values.use_gles) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    const std::string window_title = Cara::NativeScriptOnlyInput::Enabled()
        ? "TEST | Cara | Starting" : "Cara | Starting";
    render_window =
        SDL_CreateWindow(window_title.c_str(),
                         SDL_WINDOWPOS_UNDEFINED, // x position
                         SDL_WINDOWPOS_UNDEFINED, // y position
                         Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI |
                             (diagnostic_window_locked ? 0u : SDL_WINDOW_RESIZABLE));

    if (render_window == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to create SDL2 window: {}", SDL_GetError());
        exit(1);
    }

    if (diagnostic_window_locked) {
        SDL_SetWindowMinimumSize(render_window, 400, 480);
        SDL_SetWindowMaximumSize(render_window, 400, 480);
        SDL_SetWindowResizable(render_window, SDL_FALSE);
        SDL_SetWindowSize(render_window, 400, 480);
    }

    if (fullscreen) {
        Fullscreen();
    }

    window_context = SDL_GL_CreateContext(render_window);

    if (window_context == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to create SDL2 GL context: {}", SDL_GetError());
        exit(1);
    }
    // The independent core frame limiter remains at 100%. Display vsync is opt-in
    // because Cocoa/SDL can block the first VBlank when no display ticks arrive.
    const int swap_interval = Settings::values.vsync_enabled ? 1 : 0;
    if (SDL_GL_SetSwapInterval(swap_interval) != 0) {
        throw std::runtime_error(fmt::format("Failed to set native swap interval {}: {}",
                                             swap_interval, SDL_GetError()));
    }
    auto gl_load_func = Settings::values.use_gles ? gladLoadGLES2Loader : gladLoadGLLoader;

    if (!gl_load_func(static_cast<GLADloadproc>(SDL_GL_GetProcAddress))) {
        LOG_CRITICAL(Frontend, "Failed to initialize GL functions: {}", SDL_GetError());
        exit(1);
    }

    OnResize();
    OnMinimalClientAreaChangeRequest({400, 480});
    SDL_PumpEvents();
    ValidateDiagnosticGeometry("startup", true);
    LOG_INFO(Frontend, "Cara Version: {} | {}-{}", Common::g_build_fullname, Common::g_scm_branch,
             Common::g_scm_desc);
    Settings::LogSettings();
}

void EmuWindow_SDL2::StartCommandQueue() {
    if (NativeWindowFlag("CARA_GL_COMMAND_QUEUE")) {
        for(const char* conflict:{"CARA_GL_QUEUE_SELF_TEST","CARA_GL_QUERY_CACHE_SELF_TEST","CARA_GL_QUEUE_COHERENCE_TEST","CARA_TEXTURE_COPY_TRACE","CARA_GPU_VERTEX_PROVENANCE","CARA_GPU_QUATERNION_SLICE","CARA_NATIVE_CPU_CATEGORY_PROFILE","CARA_GPU_TRACE","CARA_GPU_TRACE_COMMAND_ORIGINS","CARA_GPU_TRACE_RECORD_SNAPSHOTS","CARA_GPU_TRACE_WAIT_ORIGINS","CARA_SURFACE_VALIDATION_COPY_TRACE","CARA_SURFACE_VALIDATION_COPY_JOIN","CARA_QUAD_UV_INPUT_CENSUS","CARA_QUAD_UV_PLAN_CENSUS","CARA_NATIVE_GL_QUERY_CENSUS","CARA_PICA_COMMAND_PROFILE","CARA_NATIVE_TIMING_COUNTERS","CARA_PHASE_CPU_TIMING","CARA_CPU_CATEGORY_TIMING","CARA_TEXTURE_ACQUISITION_PROFILE","CARA_DRAW_COST_PROFILE","CARA_RENDER_BOOKKEEPING_PROFILE"})
            if(NativeWindowFlag(conflict))throw std::runtime_error(std::string("GL transactions conflict with diagnostic: ")+conflict);
        if (Settings::values.use_present_thread)
            throw std::runtime_error("The GL command queue requires one presentation context");
        // Startup-only raw query on the still-current producer context. A token
        // cannot distinguish nested driver callbacks from the transaction body.
        if(GLAD_GL_AMD_debug_output)
            throw std::runtime_error("GL transactions reject unqueryable AMD debug callbacks");
        if(GLAD_GL_KHR_debug || GLAD_GL_ARB_debug_output || GLAD_GL_ES_VERSION_3_2) {
            void* callback=reinterpret_cast<void*>(std::uintptr_t(1));
            if(glad_glGetPointerv)glad_glGetPointerv(GL_DEBUG_CALLBACK_FUNCTION,&callback);
            else if(glad_glGetPointervKHR)glad_glGetPointervKHR(GL_DEBUG_CALLBACK_FUNCTION_KHR,&callback);
            else throw std::runtime_error("Cannot inspect current GL debug callback");
            if(callback)throw std::runtime_error("GL transactions reject installed GL debug callbacks");
        }
        if (SDL_GL_MakeCurrent(render_window, nullptr) != 0)
            throw std::runtime_error("Cannot release GL context to the render worker");
        try {
            render_queue = std::make_unique<Common::HostRenderQueue>(128, 8 * 1024 * 1024,
                [this] {
                    if (SDL_GL_MakeCurrent(render_window, window_context) != 0)
                        throw std::runtime_error("Cannot acquire GL context on the render worker");
                },
                [this] { SDL_GL_MakeCurrent(render_window, nullptr); });
            OpenGL::CommandQueue::Install(*render_queue);
        } catch (...) {
            const auto failure=std::current_exception();
            try { OpenGL::CommandQueue::Uninstall(); } catch (...) {}
            if (render_queue) { try { render_queue->Stop(); } catch (...) {} render_queue.reset(); }
            SDL_GL_MakeCurrent(render_window, window_context);
            std::rethrow_exception(failure);
        }
        LOG_INFO(Render_OpenGL, "Owned GL command queue enabled: 128 batches, 8 MiB in flight");
    }
}

bool EmuWindow_SDL2::PrepareForCoreShutdown() noexcept {
    if (!render_queue) return !render_queue_failed;
    try { OpenGL::CommandQueue::Drain(); return !render_queue_failed; }
    catch (const std::exception& error) {
        LOG_ERROR(Render_OpenGL, "Render worker failed; reclaiming context for cleanup: {}", error.what());
    } catch (...) {
        LOG_ERROR(Render_OpenGL, "Render worker failed; reclaiming context for cleanup");
    }
    render_queue_failed = true;
    // A poisoned queue rejects deletion commands too. Join it and restore raw
    // entrypoints BEFORE noexcept GL resource destructors run on the core owner.
    try { OpenGL::CommandQueue::Uninstall(); } catch (...) {}
    try { render_queue->Stop(); } catch (...) {} // Stop joins even on poison.
    const auto ended = render_queue->GetStats();
    render_queue.reset();
    SDL_GL_MakeCurrent(render_window, window_context);
    const bool current = IsOwnedContextCurrent();
    try {
        std::ofstream receipt(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) +
                              "native-gl-queue-recovery.json");
        receipt << "{\"worker_joined\":" << (!ended.worker_running ? "true":"false")
                << ",\"raw_entrypoints_restored\":" << (!OpenGL::CommandQueue::Enabled()?"true":"false")
                << ",\"context_reacquired\":" << (current?"true":"false") << "}\n";
    } catch (...) {}
    if (!current) LOG_CRITICAL(Render_OpenGL, "Failed to reacquire GL context for cleanup");
    return false;
}

bool EmuWindow_SDL2::StopCommandQueue() noexcept {
    if(!render_queue)return !render_queue_failed;
    try{OpenGL::CommandQueue::Drain();}catch(...){render_queue_failed=true;}
    try{OpenGL::CommandQueue::Uninstall();}catch(...){render_queue_failed=true;}
    try{render_queue->Stop();}catch(...){render_queue_failed=true;}
    render_queue.reset();
    SDL_GL_MakeCurrent(render_window,window_context);
    if(!IsOwnedContextCurrent())render_queue_failed=true;
    return !render_queue_failed;
}

bool EmuWindow_SDL2::IsOwnedContextCurrent() const noexcept {
    return window_context!=nullptr && SDL_GL_GetCurrentContext()==window_context;
}

EmuWindow_SDL2::~EmuWindow_SDL2() {
    StopCommandQueue();
    TouchReleased();
    Network::Shutdown();
    InputCommon::Shutdown();
    DoneCurrent();
    SDL_GL_DeleteContext(window_context);
    SDL_DestroyWindow(render_window);
    SDL_Quit();
}
void EmuWindow_SDL2::SwapBuffers() {
    if (render_queue) {
        OpenGL::CommandQueue::Flush();
        auto* window = render_window;
        render_queue->Submit([window] { SDL_GL_SwapWindow(window); });
    } else {
        SDL_GL_SwapWindow(render_window);
    }
}

static std::vector<u32> NativeFrameList(const char* name) {
    std::vector<u32> result;
    const char* value = std::getenv(name);
    if (!value)
        return result;
    std::istringstream in(value);
    std::string item;
    while (std::getline(in, item, ',')) {
        char* end = nullptr;
        const auto at = std::strtoul(item.c_str(), &end, 10);
        if (end != item.c_str() && *end == '\0' && at > 0 && at <= 100000)
            result.push_back(u32(at));
    }
    return result;
}
namespace {
EmuWindow_SDL2::StateCommand pending_state_command = EmuWindow_SDL2::StateCommand::None;
bool benchmark_active = false;
bool benchmark_stop = false;
bool benchmark_capture_pending = false;
bool benchmark_capture_done = false;
u32 benchmark_base_frame = 0;
u32 benchmark_warmup = 300;
u32 benchmark_end = 1200;
std::vector<NativeMac::CircleSegment> benchmark_route;
} // namespace
static u32 NativeFrameOption(const char* name, u32 fallback);

EmuWindow_SDL2::StateCommand EmuWindow_SDL2::TakeStateCommand() {
    const auto command = pending_state_command;
    pending_state_command = StateCommand::None;
    return command;
}
bool EmuWindow_SDL2::TakeBenchmarkStop() {
    const bool stop = benchmark_stop;
    benchmark_stop = false;
    return stop;
}
void EmuWindow_SDL2::BeginBenchmark() {
    ValidateDiagnosticGeometry("origin", true);
    benchmark_warmup = NativeFrameOption("CARA_NATIVE_BENCHMARK_WARMUP", 300);
    benchmark_end = NativeFrameOption("CARA_NATIVE_BENCHMARK_FRAMES", 1200);
    if (benchmark_warmup >= benchmark_end)
        throw std::runtime_error("Native benchmark requires warmup < end frame");
    const auto* route = std::getenv("CARA_NATIVE_CIRCLE_SEGMENTS");
    benchmark_route = NativeMac::ParseCircleSegments(route ? route : "");
    benchmark_base_frame = VideoCore::GetCurrentFrame();
    benchmark_active = true;
    WriteNativeProfileSnapshot(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) +
                                   "native-benchmark-origin",
                               benchmark_base_frame);
}

static u32 NativeFrameOption(const char* name, u32 fallback = 0) {
    const char* value = std::getenv(name);
    if (!value)
        return fallback;
    char* end = nullptr;
    const auto parsed = std::strtoul(value, &end, 10);
    if (end == value || *end || parsed > 100000)
        throw std::runtime_error("Invalid native frame control");
    return static_cast<u32>(parsed);
}

void EmuWindow_SDL2::NativeFrameDiagnostics() {
    static const bool enabled = [] {
        const auto* p = std::getenv("CARA_NATIVE_DIAGNOSTICS");
        return p && std::string(p) == "1";
    }();
    if (!enabled || !VideoCore::Renderer())
        return;
    static const auto presses = NativeFrameList("CARA_NATIVE_PRESS_A_FRAMES");
    static const auto starts = NativeFrameList("CARA_NATIVE_PRESS_START_FRAMES");
    static const auto captures = NativeFrameList("CARA_NATIVE_CAPTURE_FRAMES");
    const u32 completed = VideoCore::GetCurrentFrame() + 1;
    if (diagnostic_touch && (completed>=diagnostic_touch_release ||
        static_cast<s32>(SDL_GetTicks()-diagnostic_touch_deadline)>=0)) {
        TouchReleased(); diagnostic_touch=false;
    }
    if (benchmark_active) {
        const u32 relative =
            completed >= benchmark_base_frame ? completed - benchmark_base_frame : 0;
        const auto desired = NativeMac::CircleKeys(benchmark_route, relative);
        static std::array<bool, 4> previous_keys{};
        constexpr std::array<int, 4> keys{
            {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN}};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (desired[i] != previous_keys[i]) {
                if (desired[i])
                    InputCommon::GetKeyboard()->PressKey(keys[i]);
                else
                    InputCommon::GetKeyboard()->ReleaseKey(keys[i]);
                previous_keys[i] = desired[i];
            }
        }
        static bool wrote_begin = false, wrote_end = false;
        if (!wrote_begin && relative >= benchmark_warmup) {
            wrote_begin = true;
            ValidateDiagnosticGeometry("before", true);
            WriteNativeProfileSnapshot(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) +
                                           "native-benchmark-before",
                                       completed,true);
        }
        if (!wrote_end && relative >= benchmark_end) {
            wrote_end = true;
            ValidateDiagnosticGeometry("after", true);
            WriteNativeProfileSnapshot(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) +
                                           "native-benchmark-after",
                                       completed,true);
            benchmark_capture_pending = true;
        }
    }
    if (benchmark_active && benchmark_capture_done)
        benchmark_stop = true;
    const bool down = std::any_of(presses.begin(), presses.end(),
                                  [&](u32 at) { return completed >= at && completed < at + 6; });
    static bool previous = false;
    if (down != previous) {
        if (down)
            InputCommon::GetKeyboard()->PressKey(SDL_SCANCODE_A);
        else
            InputCommon::GetKeyboard()->ReleaseKey(SDL_SCANCODE_A);
        previous = down;
    }
    const bool start_down = std::any_of(starts.begin(), starts.end(), [&](u32 at) {
        return completed >= at && completed < at + 6;
    });
    static bool start_previous = false;
    if (start_down != start_previous) {
        if (start_down)
            InputCommon::GetKeyboard()->PressKey(SDL_SCANCODE_M);
        else
            InputCommon::GetKeyboard()->ReleaseKey(SDL_SCANCODE_M);
        start_previous = start_down;
    }
    static int command_key = 0;
    static u32 command_release = 0;
    if (command_key && completed >= command_release) {
        InputCommon::GetKeyboard()->ReleaseKey(command_key);
        command_key = 0;
    }
    const bool final_benchmark_capture = benchmark_capture_pending;
    bool requested_capture = final_benchmark_capture;
    benchmark_capture_pending = false;
    static const bool developer_commands = NativeMac::DeveloperCommands::EnabledFromEnvironment();
    if (developer_commands && completed % 30 == 0) {
        const auto request =
            FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "native-command.txt";
        std::ifstream input(request);
        if (input) {
            char line[96]{};
            input.getline(line, sizeof(line));
            const bool complete_line=!input.fail();
            input.close();
            std::remove(request.c_str());
            std::istringstream command(line);
            std::string name;
            unsigned duration = 6;
            command >> name >> duration;
            duration = std::clamp(duration, 1u, 600u);
            if (name == "touch" && complete_line) {
                if (const auto touch=NativeMac::ParseTouchCommand(line)) {
                    const auto& layout=GetFramebufferLayout();
                    const auto& screen=layout.bottom_screen;
                    if (const auto pixel=NativeMac::TouchFramebufferPoint(*touch,screen.left,
                            screen.top,screen.right,screen.bottom,layout.bottom_screen_enabled)) {
                        TouchReleased();
                        TouchPressed(pixel->first,pixel->second);
                        diagnostic_touch=true;
                        diagnostic_touch_release=completed+touch->frames;
                        diagnostic_touch_deadline=SDL_GetTicks()+30000;
                    }
                }
            } else if (name == "save") {
                pending_state_command = EmuWindow_SDL2::StateCommand::Save;
            } else if (name == "load") {
                pending_state_command = EmuWindow_SDL2::StateCommand::Load;
            } else if (name == "profile") {
                WriteNativeProfileSnapshot(FileUtil::GetUserPath(FileUtil::UserPath::UserDir) +
                                               "native-frame-" + std::to_string(completed),
                                           completed);
            } else if (name == "capture")
                requested_capture = true;
            else {
                int key = name == "a"       ? SDL_SCANCODE_A
                          : name == "b"     ? SDL_SCANCODE_S
                          : name == "start" ? SDL_SCANCODE_M
                          : name == "right" ? SDL_SCANCODE_RIGHT
                          : name == "left"  ? SDL_SCANCODE_LEFT
                          : name == "up"    ? SDL_SCANCODE_UP
                          : name == "down"  ? SDL_SCANCODE_DOWN
                                            : 0;
                if (key) {
                    if (command_key)
                        InputCommon::GetKeyboard()->ReleaseKey(command_key);
                    command_key = key;
                    command_release = completed + duration;
                    InputCommon::GetKeyboard()->PressKey(key);
                }
            }
        }
    }
    const u32 target = completed + 1;
    if (!requested_capture && std::find(captures.begin(), captures.end(), target) == captures.end())
        return;
    VideoCore::g_screenshot_complete_callback = [target, final_benchmark_capture](
                                                    u32 width, u32 height,
                                                    const std::vector<u32>& pixels) {
        const auto stem = FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "native-frame-" +
                          std::to_string(target);
        const auto* data = reinterpret_cast<const u8*>(pixels.data());
        const std::vector<u8> rgba(data, data + pixels.size() * 4);
        std::ofstream raw(stem + ".rgba", std::ios::binary);
        raw.write(reinterpret_cast<const char*>(rgba.data()), rgba.size());
        raw.close();
        LodePNGImageInterface image;
        const bool png = image.EncodePNG(stem + ".png", rgba, width, height);

        std::ostringstream lcd;
        lcd << "[";
        for (unsigned screen = 0; screen < 2; ++screen) {
            const auto& fb = GPU::g_regs.framebuffer_config[screen];
            const auto fill = screen ? LCD::g_regs.color_fill_bottom : LCD::g_regs.color_fill_top;
            const auto format = fb.color_format.Value();
            const u32 w = screen ? 320 : 400, h = 240;
            const PAddr address = fb.active_fb ? fb.address_left2 : fb.address_left1;
            const u32 bpp = static_cast<u32>(format) <= 4 ? GPU::Regs::BytesPerPixel(format) : 0;
            const u64 span = u64(w - 1) * fb.stride + u64(h) * bpp, end = u64(address) + span;
            const bool valid =
                bpp && fb.width >= h && fb.height >= w && fb.stride >= h * bpp &&
                ((address >= Memory::VRAM_PADDR && end <= Memory::VRAM_PADDR_END) ||
                 (address >= Memory::FCRAM_PADDR && end <= Memory::FCRAM_N3DS_PADDR_END));
            const auto* source = valid ? VideoCore::Memory()->GetPhysicalPointer(address) : nullptr;
            std::vector<u8> cpu(w * h * 4);
            u64 nonblack = 0;
            for (u32 y = 0; y < h; ++y)
                for (u32 x = 0; x < w; ++x) {
                    Common::Vec4<u8> color{0, 0, 0, 255};
                    if (fill.is_enabled)
                        color = {fill.color_r, fill.color_g, fill.color_b, 255};
                    else if (source) {
                        const auto* ptr = source + x * fb.stride + (h - 1 - y) * bpp;
                        switch (format) {
                        case GPU::Regs::PixelFormat::RGBA8:
                            color = Color::DecodeRGBA8(ptr);
                            break;
                        case GPU::Regs::PixelFormat::RGB8:
                            color = Color::DecodeRGB8(ptr);
                            break;
                        case GPU::Regs::PixelFormat::RGB565:
                            color = Color::DecodeRGB565(ptr);
                            break;
                        case GPU::Regs::PixelFormat::RGB5A1:
                            color = Color::DecodeRGB5A1(ptr);
                            break;
                        case GPU::Regs::PixelFormat::RGBA4:
                            color = Color::DecodeRGBA4(ptr);
                            break;
                        }
                    }
                    const auto i = (y * w + x) * 4;
                    cpu[i] = color.r();
                    cpu[i + 1] = color.g();
                    cpu[i + 2] = color.b();
                    cpu[i + 3] = 255;
                    nonblack += color.r() || color.g() || color.b();
                }
            image.EncodePNG(stem + "-cpu-lcd-" + std::to_string(screen) + ".png", cpu, w, h);
            lcd << (screen ? "," : "") << "{\"address\":" << address << ",\"fb_width\":" << fb.width
                << ",\"fb_height\":" << fb.height << ",\"stride\":" << fb.stride
                << ",\"format\":" << static_cast<u32>(format) << ",\"fill_raw\":" << fill.raw
                << ",\"source_valid\":" << (source ? "true" : "false")
                << ",\"cpu_nonblack_pixels\":" << nonblack << "}";
        }
        lcd << "]";

        auto& system = Core::System::GetInstance();
        const auto& counters = Common::FrameTiming::counters;
        u64 colored = 0;
        for (size_t i = 0; i < rgba.size(); i += 4)
            colored += rgba[i] || rgba[i + 1] || rgba[i + 2];
        std::ofstream out(stem + ".json");
        out << "{\"capture_refresh\":" << target
            << ",\"video_core_frame_before_increment\":" << VideoCore::GetCurrentFrame()
            << ",\"ticks\":" << system.CoreTiming().GetTicks()
            << ",\"draw_requests\":" << counters.pica_draw_requests
            << ",\"command_lists\":" << counters.pica_command_lists << ",\"width\":" << width
            << ",\"height\":" << height << ",\"cpu_lcd_authoritative\":"
            << (!Settings::values.use_hw_renderer ? "true" : "false") << ",\"rtc_fixed\":"
            << (Settings::values.init_clock == Settings::InitClock::FixedTime ? "true" : "false")
            << ",\"rtc_init_epoch\":" << Settings::values.init_time
            << ",\"nonblack_pixels\":" << colored << ",\"png_written\":" << (png ? "true" : "false")
            << ",\"guest_cpu_percent\":" << Cara::NativeDiagnosticClock::EffectivePercent(system.CoreTiming())
            << ",\"diagnostic_clock\":";
        Cara::NativeDiagnosticClock::WriteJSON(out,system.CoreTiming(),system.GetNumCores());
        out << ",\"audio\":\"sdl2\",\"instrumented\":true"
            << ",\"timing_counters_enabled\":" << (counters.enabled ? "true" : "false")
            << ",\"capture_readback\":true,\"pcs\":[";
        for (unsigned i = 0; i < 4; ++i)
            out << (i ? "," : "") << system.GetCore(i).GetPC();
        out << "],\"lcd\":" << lcd.str() << "}\n";
        if (final_benchmark_capture)
            benchmark_capture_done = true;
        std::cout << "Native capture refresh=" << target << " nonblack=" << colored
                  << " draws=" << counters.pica_draw_requests << std::endl;
    };
}

void EmuWindow_SDL2::PollEvents() {
    ValidateDiagnosticGeometry("poll");
    NativeFrameDiagnostics();
    SDL_Event event;

    // SDL_PollEvent returns 0 when there are no more events in the event queue
    while (SDL_PollEvent(&event)) {
        if (Cara::NativeScriptOnlyInput::Ignore(event.type))
            continue;
        switch (event.type) {
        case SDL_WINDOWEVENT:
            switch (event.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
            case SDL_WINDOWEVENT_RESIZED:
            case SDL_WINDOWEVENT_MAXIMIZED:
            case SDL_WINDOWEVENT_RESTORED:
            case SDL_WINDOWEVENT_MINIMIZED:
                OnResize();
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
                if (Cara::NativeScriptOnlyInput::ReleaseTouchOnFocusLost(
                        Cara::NativeScriptOnlyInput::Enabled())) {
                    TouchReleased(); diagnostic_touch=false;
                }
                break;
            case SDL_WINDOWEVENT_CLOSE:
                TouchReleased(); diagnostic_touch=false;
                is_open = false;
                break;
            }
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            OnKeyEvent(static_cast<int>(event.key.keysym.scancode), event.key.state);
            break;
        case SDL_MOUSEMOTION:
            // ignore if it came from touch
            if (event.button.which != SDL_TOUCH_MOUSEID)
                OnMouseMotion(event.motion.x, event.motion.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            // ignore if it came from touch
            if (event.button.which != SDL_TOUCH_MOUSEID) {
                OnMouseButton(event.button.button, event.button.state, event.button.x,
                              event.button.y);
            }
            break;
        case SDL_FINGERDOWN:
            OnFingerDown(event.tfinger.x, event.tfinger.y);
            break;
        case SDL_FINGERMOTION:
            OnFingerMotion(event.tfinger.x, event.tfinger.y);
            break;
        case SDL_FINGERUP:
            OnFingerUp();
            break;
        case SDL_QUIT:
            TouchReleased(); diagnostic_touch=false;
            is_open = false;
            break;
        default:
            break;
        }
    }

    const u32 current_time = SDL_GetTicks();
    if (current_time > last_time + 2000) {
        const auto results = Core::System::GetInstance().GetAndResetPerfStats();
        const auto title = fmt::format("{}{:.0f} FPS | {:.0%} speed | Cara",
                                       Cara::NativeScriptOnlyInput::Enabled() ? "TEST | " : "",
                                       results.game_fps, results.emulation_speed);
        SDL_SetWindowTitle(render_window, title.c_str());
        last_time = current_time;
    }
}

void EmuWindow_SDL2::MakeCurrent() {
    if (render_queue) return; // Context remains on its sole GL owner.
    SDL_GL_MakeCurrent(render_window, window_context);
}

void EmuWindow_SDL2::DoneCurrent() {
    if (render_queue) { OpenGL::CommandQueue::Drain(); return; }
    SDL_GL_MakeCurrent(render_window, nullptr);
}

void EmuWindow_SDL2::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    if (diagnostic_window_locked) {
        SDL_SetWindowMinimumSize(render_window, 400, 480);
        return;
    }
    SDL_SetWindowMinimumSize(render_window, minimal_size.first, minimal_size.second);
}
