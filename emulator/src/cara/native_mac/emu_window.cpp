// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "cara/lodepng_image_interface.h"
#include "common/color.h"
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

EmuWindow_SDL2::EmuWindow_SDL2(bool fullscreen) {
    // Initialize the window
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) < 0) {
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
    std::string window_title = fmt::format("Cara {} | {}-{}", Common::g_build_fullname,
                                           Common::g_scm_branch, Common::g_scm_desc);
    render_window =
        SDL_CreateWindow(window_title.c_str(),
                         SDL_WINDOWPOS_UNDEFINED, // x position
                         SDL_WINDOWPOS_UNDEFINED, // y position
                         Core::kScreenTopWidth, Core::kScreenTopHeight + Core::kScreenBottomHeight,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (render_window == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to create SDL2 window: {}", SDL_GetError());
        exit(1);
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
    LOG_INFO(Frontend, "Cara Version: {} | {}-{}", Common::g_build_fullname, Common::g_scm_branch,
             Common::g_scm_desc);
    Settings::LogSettings();
}

EmuWindow_SDL2::~EmuWindow_SDL2() {
    Network::Shutdown();
    InputCommon::Shutdown();
    DoneCurrent();
    SDL_GL_DeleteContext(window_context);
    SDL_DestroyWindow(render_window);
    SDL_Quit();
}
void EmuWindow_SDL2::SwapBuffers() {
    SDL_GL_SwapWindow(render_window);
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
static void NativeFrameDiagnostics() {
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
    bool requested_capture = false;
    if (completed % 30 == 0) {
        const auto request =
            FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "native-command.txt";
        std::ifstream input(request);
        if (input) {
            char line[96]{};
            input.getline(line, sizeof(line));
            input.close();
            std::remove(request.c_str());
            std::istringstream command(line);
            std::string name;
            unsigned duration = 6;
            command >> name >> duration;
            duration = std::clamp(duration, 1u, 600u);
            if (name == "capture")
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
    VideoCore::g_screenshot_complete_callback = [target](u32 width, u32 height,
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
            << ",\"guest_cpu_percent\":100,\"audio\":\"sdl2\",\"instrumented\":true,\"pcs\":[";
        for (unsigned i = 0; i < 4; ++i)
            out << (i ? "," : "") << system.GetCore(i).GetPC();
        out << "],\"lcd\":" << lcd.str() << "}\n";
        std::cout << "Native capture refresh=" << target << " nonblack=" << colored
                  << " draws=" << counters.pica_draw_requests << std::endl;
    };
}

void EmuWindow_SDL2::PollEvents() {
    NativeFrameDiagnostics();
    SDL_Event event;

    // SDL_PollEvent returns 0 when there are no more events in the event queue
    while (SDL_PollEvent(&event)) {
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
            case SDL_WINDOWEVENT_CLOSE:
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
            is_open = false;
            break;
        default:
            break;
        }
    }

    const u32 current_time = SDL_GetTicks();
    if (current_time > last_time + 2000) {
        const auto results = Core::System::GetInstance().GetAndResetPerfStats();
        const auto title = fmt::format(
            "Cara {} | {}-{} | FPS: {:.0f} ({:.0%})", Common::g_build_fullname,
            Common::g_scm_branch, Common::g_scm_desc, results.game_fps, results.emulation_speed);
        SDL_SetWindowTitle(render_window, title.c_str());
        last_time = current_time;
    }
}

void EmuWindow_SDL2::MakeCurrent() {
    SDL_GL_MakeCurrent(render_window, window_context);
}

void EmuWindow_SDL2::DoneCurrent() {
    SDL_GL_MakeCurrent(render_window, nullptr);
}

void EmuWindow_SDL2::OnMinimalClientAreaChangeRequest(std::pair<u32, u32> minimal_size) {
    SDL_SetWindowMinimumSize(render_window, minimal_size.first, minimal_size.second);
}
