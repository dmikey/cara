// SPDX-License-Identifier: GPL-2.0-or-later
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_metal.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include "cara/config.h"
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/scope_exit.h"
#include "core/core.h"
#if defined(CARA_MAC_CPU_JIT)
#include "core/arm/dynarmic/arm_dynarmic.h"
#endif
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/loader/loader.h"
#include "core/settings.h"
#include "input_common/main.h"
#include "input_common/keyboard.h"
#include "video_core/renderer_metal/pica_metal_native_renderer.h"
#include "video_core/renderer_metal/pica_metal_pipelines.h"
#include "video_core/renderer_metal/pica_metal_resources.h"
#include "video_core/video_core.h"

namespace {
unsigned ParseUnsigned(std::string_view value) {
    unsigned number{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !number)
        throw std::runtime_error("Invalid frame number");
    return number;
}

struct InputEvent {
    unsigned frame;
    std::string action;
    std::string key;
    unsigned x{};
    unsigned y{};
};

std::vector<InputEvent> ReadInputs(const std::string& dir, unsigned limit) {
    const auto path = dir + "inputs.txt";
    if (!std::filesystem::exists(path))
        return {};
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Cannot read compatibility inputs");
    std::vector<InputEvent> events;
    unsigned previous = 0;
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream fields(line);
        InputEvent event;
        std::string frame, extra;
        if (!(fields >> frame >> event.action))
            throw std::runtime_error("Incomplete compatibility input");
        event.frame = ParseUnsigned(frame);
        if (event.frame < previous || event.frame >= limit)
            throw std::runtime_error("Out-of-order or out-of-range compatibility input");
        previous = event.frame;
        if (event.action == "down" || event.action == "up") {
            if (!(fields >> event.key) || SDL_GetScancodeFromName(event.key.c_str()) == SDL_SCANCODE_UNKNOWN)
                throw std::runtime_error("Unknown compatibility key");
        } else if (event.action == "touch") {
            if (!(fields >> event.x >> event.y) || event.x >= 320 || event.y >= 240)
                throw std::runtime_error("Out-of-range 3DS touch input");
        } else if (event.action != "release") {
            throw std::runtime_error("Unknown compatibility action");
        }
        if (fields >> extra)
            throw std::runtime_error("Unexpected trailing compatibility input");
        events.push_back(std::move(event));
        if (events.size() > 16384)
            throw std::runtime_error("Too many compatibility inputs");
    }
    return events;
}

class MetalWindow final : public Frontend::EmuWindow {
public:
    MetalWindow() {
        UpdateFramebufferLayout(400, 480);
    }
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    void SwapBuffers() override {}
    void PollEvents() override {
        for (SDL_Event event; SDL_PollEvent(&event);) {
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
                closed = true;
            if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && !event.key.repeat) {
                const auto key = event.key.keysym.scancode;
                if (event.type == SDL_KEYDOWN)
                    InputCommon::GetKeyboard()->PressKey(key);
                else
                    InputCommon::GetKeyboard()->ReleaseKey(key);
            }
        }
    }
    void ApplyInput(const InputEvent& event) {
        const auto key = SDL_GetScancodeFromName(event.key.c_str());
        if (event.action == "down") {
            InputCommon::GetKeyboard()->PressKey(key);
        } else if (event.action == "up") {
            InputCommon::GetKeyboard()->ReleaseKey(key);
        } else if (event.action == "touch") {
            const auto& screen = GetFramebufferLayout().bottom_screen;
            TouchPressed(screen.left + event.x * screen.GetWidth() / 320,
                         screen.top + event.y * screen.GetHeight() / 240);
        } else {
            TouchReleased();
        }
        std::cout << "INPUT frame=" << event.frame << ' ' << event.action << ' '
                  << event.key << ' ' << event.x << ' ' << event.y << std::endl;
    }
    bool closed{};
};
}

int main(int argc, char** argv) {
    @autoreleasepool {
        unsigned limit{};
        const char* directory = std::getenv("CARA_USER_DIR");
        if (argc != 3 || !directory || directory[0] != '/' || !FileUtil::IsDirectory(directory))
            return 2;
        std::string count = argv[2];
        const auto parsed = std::from_chars(count.data(), count.data() + count.size(), limit);
        if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() || !limit)
            return 2;
        std::string output(directory);
        if (output.back() != '/')
            output += '/';
        try {
            const char* interval_env = std::getenv("CARA_COMPAT_CAPTURE_INTERVAL");
            const unsigned interval = interval_env ? ParseUnsigned(interval_env) : 0;
            const auto inputs = ReadInputs(output, limit);
            const auto should_capture = [&](unsigned frame) {
                return frame == limit || (interval && frame && frame % interval == 0);
            };
            // Use the same ordered Metal transfer queue policy as the iPad by default.
            if (setenv("CARA_METAL_OVERLAP_TRANSFERS", "1", 0) != 0)
                throw std::runtime_error("Could not set the Metal transfer queue policy");
            SDL_SetMainReady();
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
                throw std::runtime_error(SDL_GetError());
            SCOPE_EXIT({ SDL_Quit(); });
            SDL_Window* window = SDL_CreateWindow("Cara Metal compatibility",
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 400, 480, SDL_WINDOW_METAL);
            if (!window)
                throw std::runtime_error(SDL_GetError());
            SCOPE_EXIT({ SDL_DestroyWindow(window); });
            SDL_MetalView view = SDL_Metal_CreateView(window);
            if (!view)
                throw std::runtime_error(SDL_GetError());
            SCOPE_EXIT({ SDL_Metal_DestroyView(view); });
            CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
            layer.device = MTLCreateSystemDefaultDevice();
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.drawableSize = CGSizeMake(400, 480);
            layer.framebufferOnly = NO;
            NSError* error = nil;
            NSURL* library = [NSBundle.mainBundle URLForResource:@"cara-pica" withExtension:@"metallib"];
            const auto assets = Cara::LoadPicaMetalResources(layer.device, library,
                                                            layer.pixelFormat, &error);
            if (!assets)
                throw std::runtime_error(error.localizedDescription.UTF8String ?: "Missing Metal assets");

            FileUtil::SetUserPath(output);
            const char* requested = std::getenv("CARA_CPU_BACKEND");
            const std::string_view backend = requested ? requested : "fastinterp";
            if (backend != "fastinterp" && backend != "jit")
                throw std::runtime_error("Unsupported CPU backend");
            const bool jit = backend == "jit";
#if !defined(CARA_MAC_CPU_JIT)
            if (jit)
                throw std::runtime_error("This Mac Metal runner has no CPU JIT");
#endif
            Common::DetachedTasks tasks;
            Config config;
            InputCommon::Init();
            SCOPE_EXIT({ InputCommon::Shutdown(); });
            Log::SetGlobalFilter(Log::Filter(Log::Level::Info));
            Log::AddBackend(std::make_unique<Log::ColorConsoleBackend>());
            Settings::values.use_cpu_jit = jit;
            Settings::values.use_shader_jit = false;
            Settings::values.use_hw_shader = true;
            Settings::values.use_hw_renderer = true;
            Settings::values.resolution_factor = 1;
            Settings::values.use_frame_limit = true;
            Settings::values.frame_limit = 100;
            Settings::values.sink_id = "sdl2";
            Settings::values.factor_3d = 0;
            Settings::values.shadow_rendering = false;
            Settings::Apply();
            Frontend::RegisterDefaultApplets();
            MetalWindow host;
            unsigned presentations = 0;
            VideoCore::SetNativeRendererFactory([&](Frontend::EmuWindow& w, Memory::MemorySystem& m) {
                return Cara::MakeNativeMetalRenderer(w, m, layer, *assets,
                    [&]() -> std::optional<Cara::NativeLcdTarget> {
                        return Cara::NativeLcdTarget{host.GetFramebufferLayout()};
                    },
                    [&](bool submitted) { presentations += submitted; },
                    [&] { return should_capture(VideoCore::GetCurrentFrame() + 1); });
            });
            SCOPE_EXIT({ VideoCore::SetNativeRendererFactory({}); });
            auto& core = Core::System::GetInstance();
            try {
                const auto status = core.Load(host, argv[1]);
                if (status != Core::System::ResultStatus::Success) {
                    std::cerr << "LOAD_STATUS " << unsigned(status) << '\n';
                    return 3;
                }
#if defined(CARA_MAC_CPU_JIT)
                unsigned jit_cores = 0;
                for (unsigned i = 0; i < core.GetNumCores(); ++i)
                    jit_cores += dynamic_cast<ARM_Dynarmic*>(&core.GetCore(i)) != nullptr;
                if ((jit && jit_cores != core.GetNumCores()) || (!jit && jit_cores))
                    throw std::runtime_error("Requested CPU backend did not initialize");
                std::cout << "CPU_BACKEND " << (jit ? "jit" : "fastinterp")
                          << " jit_cores=" << jit_cores << std::endl;
#endif
                std::cout << "TITLE " << std::hex << core.GetTitleID() << std::dec << std::endl;
                const auto* learning_setting = std::getenv("CARA_MAC_METAL_LEARN");
                const bool learning_requested =
                    learning_setting && std::string_view(learning_setting) == "1";
                bool learning = false;
                if (learning_requested) {
                    learning = Cara::ConfigureNativeMetalLearning(
                                   output, core.GetTitleID(), core.StateGameHash(),
                                   core.GetAppLoader().ReadOnlineGameVersion()) &&
                               Cara::BeginNativeMetalWarmup();
                    if (!learning)
                        throw std::runtime_error("Could not initialize local Metal pipeline learning");
                    std::cout << "METAL_PIPELINE_LEARNING active=1" << std::endl;
                }
                size_t next_input = 0;
                unsigned previous_frame = VideoCore::GetCurrentFrame();
                Uint32 title_updated_at = SDL_GetTicks();
                while (!host.closed && VideoCore::GetCurrentFrame() < limit) {
                    while (next_input < inputs.size() && inputs[next_input].frame <= previous_frame)
                        host.ApplyInput(inputs[next_input++]);
                    @autoreleasepool {
                        const auto step = core.RunLoop();
                        if (step != Core::System::ResultStatus::Success)
                            throw std::runtime_error("Core run status " + std::to_string(unsigned(step)));
                    }
                    const unsigned frame = VideoCore::GetCurrentFrame();
                    if (frame != previous_frame && frame < limit && should_capture(frame)) {
                        const auto capture_dir = output + "frame-" + std::to_string(frame) + '/';
                        std::filesystem::create_directory(capture_dir);
                        if (!Cara::WriteNativeMetalCapture(capture_dir))
                            throw std::runtime_error("Could not retain Metal checkpoint");
                        std::cout << "CAPTURE frame=" << frame << std::endl;
                    }
                    if (frame != previous_frame) {
                        const Uint32 now = SDL_GetTicks();
                        if (now - title_updated_at >= 2000) {
                            const auto stats = core.GetAndResetPerfStats();
                            char title[96];
                            std::snprintf(title, sizeof(title), "%.0f FPS | %.0f%% speed | Cara Metal",
                                          stats.game_fps, stats.emulation_speed * 100.0);
                            SDL_SetWindowTitle(window, title);
                            title_updated_at = now;
                        }
                    }
                    previous_frame = frame;
                }
                const auto frames = VideoCore::GetCurrentFrame();
                const bool captured = frames >= limit && Cara::WriteNativeMetalCapture(output);
                if (learning && frames == limit && presentations == limit && captured) {
                    if (!Cara::FinishNativeMetalWarmup())
                        throw std::runtime_error("Could not finish local Metal pipeline learning");
                    std::cout << "METAL_PIPELINE_LEARNING finished=1" << std::endl;
                }
                core.Shutdown();
                std::cout << "RESULT frames=" << frames << " presented=" << presentations
                          << " captured=" << captured << std::endl;
                return frames == limit && presentations == limit && captured ? 0 : 5;
            } catch (const std::exception& ex) {
                std::cerr << "METAL_STOP frame=" << VideoCore::GetCurrentFrame()
                          << " presented=" << presentations << " reason=" << ex.what() << std::endl;
                if (core.IsPoweredOn()) {
                    Cara::AbortNativeMetalRendering();
                    core.Shutdown();
                }
                return 4;
            }
        } catch (const std::exception& ex) {
            std::cerr << "HOST_ERROR " << ex.what() << std::endl;
            return 6;
        }
    }
}
