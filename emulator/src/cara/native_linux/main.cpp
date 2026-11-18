// Copyright 2026 Cara contributors. GPL-2.0-or-later.
// Uses Cara's shared core and the SDL input conventions inherited from Citra.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <SDL.h>
#include <glad/glad.h>
#include "cara/lodepng_image_interface.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/scm_rev.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/settings.h"
#include "input_common/main.h"
#include "input_common/keyboard.h"
#include "video_core/video_core.h"

namespace {
class Window final : public Frontend::EmuWindow {
public:
    explicit Window(bool fullscreen) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        window = SDL_CreateWindow("Cara", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            800, 960, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
            (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
        if (!window) throw std::runtime_error(SDL_GetError());
        context = SDL_GL_CreateContext(window);
        if (!context || !gladLoadGLLoader(SDL_GL_GetProcAddress)) {
            SDL_GL_DeleteContext(context);
            SDL_DestroyWindow(window);
            throw std::runtime_error("Unable to initialize desktop OpenGL 4.3");
        }
        SDL_GL_SetSwapInterval(0); // The core's 100% limiter owns normal pacing.
        Resize();
        std::cout << "OpenGL: " << glGetString(GL_VERSION) << "\nRenderer: "
                  << glGetString(GL_RENDERER) << std::endl;
    }
    ~Window() override {
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
    }
    void MakeCurrent() override { SDL_GL_MakeCurrent(window, context); }
    void DoneCurrent() override { SDL_GL_MakeCurrent(window, nullptr); }
    void SwapBuffers() override { SDL_GL_SwapWindow(window); }
    bool IsOpen() const { return open; }
    void PollEvents() override {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: open = false; break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) Resize();
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    InputCommon::GetKeyboard()->ReleaseAllKeys();
                    TouchReleased();
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) open = false;
                if (event.key.state == SDL_PRESSED)
                    InputCommon::GetKeyboard()->PressKey(event.key.keysym.scancode);
                else InputCommon::GetKeyboard()->ReleaseKey(event.key.keysym.scancode);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT) Touch(event.button.x, event.button.y, true);
                break;
            case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT) TouchReleased();
                break;
            case SDL_MOUSEMOTION:
                if (event.motion.state & SDL_BUTTON_LMASK) Touch(event.motion.x, event.motion.y, false);
                break;
            }
        }
    }
    void SelfTest() {
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        std::array<unsigned char, 4> pixel{};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        if (glGetError() != GL_NO_ERROR || std::abs(int(pixel[0]) - 64) > 1 ||
            std::abs(int(pixel[1]) - 128) > 1 || std::abs(int(pixel[2]) - 191) > 1)
            throw std::runtime_error("OpenGL clear/readback pixel check failed");
        std::cout << "OpenGL pixel self-test passed" << std::endl;
    }
private:
    void Resize() {
        int width, height;
        SDL_GL_GetDrawableSize(window, &width, &height);
        UpdateFramebufferLayout(width, height);
    }
    void Touch(int x, int y, bool pressed) {
        int width, height, drawable_width, drawable_height;
        SDL_GetWindowSize(window, &width, &height);
        SDL_GL_GetDrawableSize(window, &drawable_width, &drawable_height);
        if (width <= 0 || height <= 0) return;
        const auto px = unsigned(std::max(0, x) * drawable_width / width);
        const auto py = unsigned(std::max(0, y) * drawable_height / height);
        if (pressed) TouchPressed(px, py); else TouchMoved(px, py);
    }
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    bool open = true;
};
struct SDLSession {
    SDLSession() {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
            throw std::runtime_error(SDL_GetError());
    }
    ~SDLSession() { SDL_Quit(); }
};
struct InputSession {
    InputSession() { InputCommon::Init(); }
    ~InputSession() { InputCommon::Shutdown(); }
};
struct CoreSession {
    ~CoreSession() {
        auto& system = Core::System::GetInstance();
        if (system.IsPoweredOn()) system.Shutdown();
    }
};
void Configure() {
    auto& v = Settings::values;
    v.use_cpu_jit = false;
    v.use_shader_jit = false;
    v.use_gles = false;
    v.use_hw_renderer = true;
    v.use_hw_shader = true;
    v.hw_gs_mode = 2;
    v.use_present_thread = false;
    v.shaders_accurate_mul = Settings::AccurateMul::SAFE;
    v.resolution_factor = 1;
    v.use_frame_limit = true;
    v.frame_limit = 100;
    v.factor_3d = 0;
    v.layout_option = Settings::LayoutOption::Default;
    v.use_virtual_sd = true;
    v.region_value = Settings::REGION_VALUE_AUTO_SELECT;
    v.init_clock = Settings::InitClock::SystemTime;
    v.shared_font_type = Settings::SharedFontType::Auto;
    v.sink_id = "sdl2";
    v.audio_device_id = "auto";
    v.audio_volume = 1.0f;
    v.joystick_range = 1.0f;
    v.joystick_deadzone = 0.15f;
    const std::array keys{SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_Z, SDL_SCANCODE_X,
        SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE,
        SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_B};
    for (unsigned i = 0; i < v.current_input_profile.buttons.size(); ++i)
        v.current_input_profile.buttons[i] = InputCommon::GenerateKeyboardParam(keys.at(i));
    v.current_input_profile.analogs[0] = InputCommon::GenerateAnalogParamFromKeys(
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_D, 0.5f);
    v.current_input_profile.analogs[1] = InputCommon::GenerateAnalogParamFromKeys(
        SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L, SDL_SCANCODE_D, 0.5f);
    v.current_input_profile.touch_device = "engine:emu_window";
    Settings::Apply();
}
}
int main(int argc, char** argv) {
    try {
        bool self_test = false, fullscreen = false;
        unsigned frames = 0;
        std::string game;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout << "Cara Linux OpenGL / FastInterp\nUsage: cara [--fullscreen] [--frames N] GAME\n"
                             "       cara --self-test\n       cara --version\n";
                return 0;
            } else if (arg == "--version") {
                std::cout << "Cara " << Common::g_scm_rev << " (Linux OpenGL, no JIT)\n"; return 0;
            } else if (arg == "--self-test") self_test = true;
            else if (arg == "--fullscreen") fullscreen = true;
            else if (arg == "--frames" && i + 1 < argc) {
                const std::string count = argv[++i];
                if (count.empty() || count.find_first_not_of("0123456789") != std::string::npos)
                    throw std::runtime_error("--frames requires a positive integer");
                const auto value = std::stoul(count);
                if (!value || value > 1000000) throw std::runtime_error("--frames out of range");
                frames = unsigned(value);
            } else if (arg.starts_with("-") || !game.empty()) throw std::runtime_error("Unknown argument: " + arg);
            else game = arg;
        }
        if (!self_test && game.empty()) throw std::runtime_error("Provide a game path or --self-test");
        setenv("CARA_CPU_BACKEND", "fastinterp", 1);
        if (const char* path = std::getenv("CARA_USER_DIR")) {
            std::string user = path;
            if (user.empty()) throw std::runtime_error("CARA_USER_DIR must not be empty");
            if (user.back() != '/') user += '/';
            FileUtil::CreateFullPath(user);
            FileUtil::SetUserPath(user);
        }
        Log::Filter filter(Log::Level::Info);
        Log::SetGlobalFilter(filter);
        Log::AddBackend(std::make_unique<Log::ColorConsoleBackend>());
        SDLSession sdl;
        Configure();
        Window window(fullscreen);
        if (self_test) { window.SelfTest(); return 0; }
        InputSession input;
        Frontend::RegisterDefaultApplets();
        auto& system = Core::System::GetInstance();
        system.RegisterImageInterface(std::make_shared<LodePNGImageInterface>());
        CoreSession session;
        const auto result = system.Load(window, game);
        if (result != Core::System::ResultStatus::Success)
            throw std::runtime_error("Game load failed, code " + std::to_string(unsigned(result)));
        while (window.IsOpen() && (!frames || VideoCore::GetCurrentFrame() < frames)) {
            const auto status = system.RunLoop();
            if (status == Core::System::ResultStatus::ShutdownRequested) break;
            if (status != Core::System::ResultStatus::Success)
                throw std::runtime_error("Emulation failed: " + system.GetStatusDetails());
        }
        std::cout << "Completed refreshes: " << VideoCore::GetCurrentFrame() << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Cara: " << e.what() << std::endl;
        return 1;
    }
}
