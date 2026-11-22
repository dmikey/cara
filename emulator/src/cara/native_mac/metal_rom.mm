// SPDX-License-Identifier: GPL-2.0-or-later
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_metal.h>
#include <SDL_syswm.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <stdexcept>
#include "cara/config.h"
#include "cara/native_mac/mac_preferences.h"
#include "cara/p3d_completion.h"
#include "core/core_timing.h"
#include "common/apple_fastinterp_policy.h"
#include "common/apple_metal_policy.h"
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
constexpr unsigned WindowWidth = 400;
constexpr unsigned WindowHeight = 480;
using Clock = std::chrono::steady_clock;

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
    SDL_Scancode scancode{};
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
            if (!(fields >> event.key))
                throw std::runtime_error("Missing compatibility key");
            event.scancode = SDL_GetScancodeFromName(event.key.c_str());
            if (event.scancode == SDL_SCANCODE_UNKNOWN)
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

enum class LiveInputKind { PressKey, ReleaseKey, PressTouch, MoveTouch, ReleaseTouch };

struct LiveInput {
    LiveInputKind kind;
    SDL_Scancode scancode{};
    unsigned x{};
    unsigned y{};
};

struct HostEvents {
    std::atomic_bool stop{};
    std::atomic_bool finished{};
    std::atomic_uint refresh{};

    void Enqueue(LiveInput event) {
        std::scoped_lock lock(mutex);
        if (event.kind == LiveInputKind::MoveTouch && !input.empty() &&
            input.back().kind == LiveInputKind::MoveTouch) {
            input.back() = event;
        } else {
            input.push_back(event);
        }
    }

    std::vector<LiveInput> TakeInput() {
        std::scoped_lock lock(mutex);
        return std::exchange(input, {});
    }

    void PostTitle(std::string value) {
        std::scoped_lock lock(mutex);
        title = std::move(value);
    }

    std::optional<std::string> TakeTitle() {
        std::scoped_lock lock(mutex);
        return std::exchange(title, std::nullopt);
    }

private:
    std::mutex mutex;
    std::vector<LiveInput> input;
    std::optional<std::string> title;
};

class MetalWindow final : public Frontend::EmuWindow {
public:
    explicit MetalWindow(HostEvents& events_) : events(events_) {
        UpdateFramebufferLayout(WindowWidth, WindowHeight);
    }
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    void SwapBuffers() override {}

    void PollEvents() override {
        // The core reaches this on its owner thread at a refresh boundary.
        // SDL's event pump runs independently on the AppKit main thread.
        for (const auto& event : events.TakeInput()) {
            switch (event.kind) {
            case LiveInputKind::PressKey:
                InputCommon::GetKeyboard()->PressKey(event.scancode);
                break;
            case LiveInputKind::ReleaseKey:
                InputCommon::GetKeyboard()->ReleaseKey(event.scancode);
                break;
            case LiveInputKind::PressTouch:
                TouchPressed(event.x, event.y);
                break;
            case LiveInputKind::MoveTouch:
                TouchMoved(event.x, event.y);
                break;
            case LiveInputKind::ReleaseTouch:
                TouchReleased();
                break;
            }
        }
    }

    void ApplyInput(const InputEvent& event) {
        if (event.action == "down") {
            InputCommon::GetKeyboard()->PressKey(event.scancode);
        } else if (event.action == "up") {
            InputCommon::GetKeyboard()->ReleaseKey(event.scancode);
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

private:
    HostEvents& events;
};

void EnqueuePointer(SDL_Window* window, HostEvents& events, int x, int y, LiveInputKind kind) {
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0)
        return;
    const auto px = static_cast<unsigned>(std::clamp(x, 0, width - 1)) * WindowWidth / width;
    const auto py = static_cast<unsigned>(std::clamp(y, 0, height - 1)) * WindowHeight / height;
    events.Enqueue({kind, SDL_SCANCODE_UNKNOWN, px, py});
}

void QueueLiveInput(SDL_Window* window, HostEvents& events, const SDL_Event& event) {
    if (event.type == SDL_QUIT ||
        (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
        if (!events.stop.exchange(true))
            SDL_HideWindow(window);
        return;
    }
    if (events.stop.load())
        return;
    if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && !event.key.repeat) {
        const auto kind = event.type == SDL_KEYDOWN ? LiveInputKind::PressKey : LiveInputKind::ReleaseKey;
        events.Enqueue({kind, event.key.keysym.scancode});
    } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
        EnqueuePointer(window, events, event.button.x, event.button.y, LiveInputKind::PressTouch);
    } else if (event.type == SDL_MOUSEMOTION && (event.motion.state & SDL_BUTTON_LMASK)) {
        EnqueuePointer(window, events, event.motion.x, event.motion.y, LiveInputKind::MoveTouch);
    } else if ((event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT) ||
               (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
        events.Enqueue({LiveInputKind::ReleaseTouch});
    }
}

Uint32 SDLCALL PostUiProbe(Uint32 interval, void* data) {
    SDL_Event event{};
    event.type = static_cast<Uint32>(reinterpret_cast<std::uintptr_t>(data));
    event.user.code = static_cast<Sint32>(SDL_GetTicks());
    SDL_PushEvent(&event);
    return interval;
}

struct UiPumpSample {
    std::int64_t elapsed_ns;
    unsigned refresh;
    int probe_ms;
};

void RunHostUi(SDL_Window* window, NSWindow* native, CAMetalLayer* layer, HostEvents& events,
               const std::string& output) {
    constexpr auto ProbeIntervalMs = 50;
    constexpr std::size_t MaxSamples = 200'000;
    const bool trace = std::getenv("CARA_MAC_UI_PUMP_TIMING") != nullptr;
    const auto probe_type = trace ? SDL_RegisterEvents(1) : Uint32(-1);
    const auto timer = probe_type != Uint32(-1) ?
        SDL_AddTimer(ProbeIntervalMs, PostUiProbe,
                     reinterpret_cast<void*>(static_cast<std::uintptr_t>(probe_type))) : 0;
    SCOPE_EXIT({ if (timer) SDL_RemoveTimer(timer); });
    if (trace && !timer)
        throw std::runtime_error("Could not start the compatibility window event probe");
    const auto start = Clock::now();
    std::vector<UiPumpSample> samples;
    bool report_fullscreen = native != nil;
    const auto size = CGSizeMake(WindowWidth, WindowHeight);
    while (!events.finished.load()) {
        @autoreleasepool {
            SDL_Event event{};
            if (SDL_WaitEventTimeout(&event, 10)) {
                do {
                    if (trace && event.type == probe_type) {
                        if (samples.size() < MaxSamples)
                            samples.push_back({
                                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count(),
                                events.refresh.load(),
                                static_cast<int>(SDL_GetTicks() - static_cast<Uint32>(event.user.code)),
                            });
                    } else {
                        QueueLiveInput(window, events, event);
                    }
                } while (SDL_PollEvent(&event));
            }
            // SDL can resize the drawable on a Space transition; the
            // compatibility capture uses a fixed native-pixel layout.
            if (!CGSizeEqualToSize(layer.drawableSize, size))
                layer.drawableSize = size;
            if (auto title = events.TakeTitle())
                SDL_SetWindowTitle(window, title->c_str());
            if (report_fullscreen && events.refresh.load() >= 120) {
                std::cout << "HOST_FULLSCREEN active=" << NSApp.active
                          << " native=" << ((native.styleMask & NSWindowStyleMaskFullScreen) != 0)
                          << " display_sync=" << layer.displaySyncEnabled << std::endl;
                report_fullscreen = false;
            }
        }
    }
    if (trace) {
        std::ofstream report(output + "host-ui-pump.csv");
        report << "elapsed_ns,refresh,probe_delay_ms\n";
        for (const auto& sample : samples)
            report << sample.elapsed_ns << ',' << sample.refresh << ',' << sample.probe_ms << '\n';
        if (!report)
            throw std::runtime_error("Could not retain compatibility window event timing");
    }
}

int RunEmulation(const char* path, unsigned limit, unsigned interval, const std::string& output,
                 const std::vector<InputEvent>& inputs, bool jit, CAMetalLayer* layer,
                 id<MTLDevice> device, NSURL* library, HostEvents& events) {
    const auto* performance = std::getenv("CARA_MAC_METAL_PERFORMANCE_MODE");
    std::string ini_text;
    FileUtil::ReadFileToString(false, FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) +
        "sdl2-config.ini", ini_text);
    Cara::ConfigureMetalGeneralFragment(Cara::MacPreferences::PlaySettings::AsyncShadersForMetalSession(
        Cara::MacPreferences::Ini(std::move(ini_text)), performance));
    std::cout << "METAL_ASYNC_SHADERS enabled=" << Cara::UseMetalGeneralFragment() << std::endl;

    NSError* error = nil;
    const auto assets = Cara::LoadPicaMetalResources(device, library, MTLPixelFormatBGRA8Unorm, &error);
    if (!assets)
        throw std::runtime_error(error.localizedDescription.UTF8String ?: "Missing Metal assets");

    const auto should_capture = [&](unsigned frame) {
        return frame == limit || (interval && frame && frame % interval == 0);
    };
    MetalWindow host(events);
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
        // Changing compatibility timing still requires an explicit request.
        Cara::gpu_completion_policy = Cara::GetGPUCompletionPolicy(
            performance && std::string_view(performance) == "1");
        const auto status = core.Load(host, path);
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
        unsigned guest_clock = 100;
        if (const auto* value = std::getenv("CARA_PROFILE_GUEST_CPU_PERCENT")) {
            guest_clock = ParseUnsigned(value);
            if (guest_clock != 50 && guest_clock != 100)
                throw std::runtime_error("Diagnostic guest clock must be 50% or 100%");
            core.CoreTiming().UpdateClockSpeed(guest_clock);
        }
        std::cout << "GUEST_TIMING clock=" << guest_clock
                  << " p3d=" << Cara::ExperimentalP3DCompletionEnabled()
                  << " ppf=" << Cara::ExperimentalPPFDependencyEnabled()
                  << " soft_knee=" << Cara::gpu_completion_policy.soften_render << std::endl;
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
        const bool record_refresh_timing = std::getenv("CARA_REFRESH_TIMING");
        auto last_refresh = Clock::now();
        std::vector<std::pair<unsigned, std::int64_t>> timing;
        size_t next_input = 0;
        unsigned previous_frame = VideoCore::GetCurrentFrame();
        Uint32 title_updated_at = SDL_GetTicks();
        while (!events.stop.load() && VideoCore::GetCurrentFrame() < limit) {
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
                events.refresh.store(frame);
                if (record_refresh_timing) {
                    const auto now = Clock::now();
                    timing.emplace_back(frame, std::chrono::duration_cast<std::chrono::nanoseconds>(
                        now - last_refresh).count());
                    last_refresh = now;
                }
                const Uint32 now = SDL_GetTicks();
                if (now - title_updated_at >= 2000) {
                    const auto stats = core.GetAndResetPerfStats();
                    char title[96];
                    std::snprintf(title, sizeof(title), "%.0f FPS | %.0f%% speed | Cara Metal",
                                  stats.game_fps, stats.emulation_speed * 100.0);
                    events.PostTitle(title);
                    title_updated_at = now;
                }
            }
            previous_frame = frame;
        }
        const auto frames = VideoCore::GetCurrentFrame();
        const bool captured = frames >= limit && Cara::WriteNativeMetalCapture(output);
        if (record_refresh_timing) {
            std::ofstream report(output + "refresh-timing.csv");
            report << "refresh,elapsed_ns\n";
            for (const auto& [refresh, elapsed] : timing)
                report << refresh << ',' << elapsed << '\n';
            if (!report)
                throw std::runtime_error("Could not retain refresh timing");
        }
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
}

} // namespace

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
            if (!Common::ApplyAppleMetalDefaults() ||
                setenv("CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT", "1", 0) != 0 ||
                setenv("CARA_METAL_REUSE_UNIFORM_UPLOADS", "1", 0) != 0)
                throw std::runtime_error("Could not initialize the Apple Metal policy");
            SDL_SetMainReady();
            if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
                throw std::runtime_error(SDL_GetError());
            SCOPE_EXIT({ SDL_Quit(); });
            SDL_Window* window = SDL_CreateWindow("Cara Metal compatibility",
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WindowWidth, WindowHeight, SDL_WINDOW_METAL);
            if (!window)
                throw std::runtime_error(SDL_GetError());
            SCOPE_EXIT({ SDL_DestroyWindow(window); });
            SDL_MetalView view = SDL_Metal_CreateView(window);
            if (!view)
                throw std::runtime_error(SDL_GetError());
            SCOPE_EXIT({ SDL_Metal_DestroyView(view); });
            CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            layer.device = device;
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.drawableSize = CGSizeMake(WindowWidth, WindowHeight);
            layer.framebufferOnly = NO;
            // The core already paces refreshes. A second sync to the Mac
            // display can stall the producer at a different refresh cadence.
            layer.displaySyncEnabled = NO;
            if (const auto* sync = std::getenv("CARA_METAL_HOST_DISPLAY_SYNC")) {
                const std::string_view value(sync);
                if (value != "0" && value != "1")
                    throw std::runtime_error("Host display sync must be 0 or 1");
                layer.displaySyncEnabled = value == "1";
            }
            NSWindow* native = nil;
            if (const auto* fullscreen = std::getenv("CARA_MAC_METAL_FULLSCREEN");
                fullscreen && std::string_view(fullscreen) == "1") {
                SDL_SysWMinfo info{};
                SDL_VERSION(&info.version);
                if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_COCOA)
                    throw std::runtime_error("Could not get the native Metal game window");
                native = info.info.cocoa.window;
                native.collectionBehavior |= NSWindowCollectionBehaviorFullScreenPrimary;
                [native toggleFullScreen:nil];
            }
            NSURL* library = [NSBundle.mainBundle URLForResource:@"cara-pica" withExtension:@"metallib"];

            FileUtil::SetUserPath(output);
            const char* requested = std::getenv("CARA_CPU_BACKEND");
            const std::string_view backend = requested ? requested : "fastinterp";
            if (backend != "fastinterp" && backend != "jit")
                throw std::runtime_error("Unsupported CPU backend");
            const bool jit = backend == "jit";
            if (!jit)
                Common::ApplyAppleFastInterpDefaults();
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
            if (const auto* speed = std::getenv("CARA_MAX_SPEED_PERCENT")) {
                const auto percent = ParseUnsigned(speed);
                if (percent < 50 || percent > 200)
                    throw std::runtime_error("Compatibility speed must be between 50% and 200%");
                Settings::values.frame_limit = percent;
            }
            Settings::values.sink_id = "sdl2";
            Settings::values.factor_3d = 0;
            Settings::values.shadow_rendering = false;
            Settings::Apply();
            Frontend::RegisterDefaultApplets();
            HostEvents events;
            int result = 6;
            std::thread emulation([&] {
                @autoreleasepool {
                    try {
                        result = RunEmulation(argv[1], limit, interval, output, inputs, jit,
                                              layer, device, library, events);
                    } catch (const std::exception& ex) {
                        std::cerr << "WORKER_ERROR " << ex.what() << std::endl;
                    }
                }
                events.finished.store(true);
            });
            SCOPE_EXIT({
                events.stop.store(true);
                if (emulation.joinable())
                    emulation.join();
            });
            RunHostUi(window, native, layer, events, output);
            emulation.join();
            return result;
        } catch (const std::exception& ex) {
            std::cerr << "HOST_ERROR " << ex.what() << std::endl;
            return 6;
        }
    }
}
