// SPDX-License-Identifier: GPL-2.0-or-later
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_metal.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include "cara/config.h"
#include "common/detached_tasks.h"
#include "common/apple_emulation_policy.h"
#include "common/apple_fastinterp_policy.h"
#include "common/apple_metal_policy.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/arm/fastinterp/fastinterp.h"
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
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/pica_metal_resources.h"
#include "video_core/video_core.h"

namespace Cara {
void AppendNativeMetalBindingWindow(std::ostream&, std::array<std::uint64_t, 20>&);
void AppendNativeMetalWatchProfile(std::ostream&, bool);
}

namespace {
void WriteHostEnvironment(const std::string& path, SDL_Window* window, unsigned frame,
                          id<MTLDevice> device) {
    const auto flags = SDL_GetWindowFlags(window);
    double load[3]{};
    const bool have_load = getloadavg(load, 3) == 3;
    std::ofstream out(path);
    out << "{\"frame\":" << frame
        << ",\"thermal_state\":" << static_cast<int>(NSProcessInfo.processInfo.thermalState)
        << ",\"low_power\":" << (NSProcessInfo.processInfo.lowPowerModeEnabled ? "true" : "false")
        << ",\"thread_qos\":" << static_cast<unsigned>(qos_class_self())
        << ",\"window_input_focus\":" << ((flags & SDL_WINDOW_INPUT_FOCUS) ? "true" : "false")
        << ",\"window_minimized\":" << ((flags & SDL_WINDOW_MINIMIZED) ? "true" : "false")
        << ",\"window_hidden\":" << ((flags & SDL_WINDOW_HIDDEN) ? "true" : "false")
        << ",\"load_average\":";
    if (have_load)
        out << '[' << load[0] << ',' << load[1] << ',' << load[2] << ']';
    else
        out << "null";
    out << ",\"metal_allocated_bytes\":" << device.currentAllocatedSize
        << ",\"metal_recommended_working_set_bytes\":" << device.recommendedMaxWorkingSetSize;
    Cara::AppendNativeMetalWatchProfile(out, true);
    out << "}\n";
    if (!out)
        throw std::runtime_error("Could not retain host environment boundary");
}

void WriteMetalCostSnapshot(const std::string& prefix, unsigned frame) {
    if (!Cara::NativeMetalCost::Enabled())
        return;
    std::ofstream csv(prefix + ".csv");
    csv << "phase,calls,elapsed_ns\n";
    Cara::NativeMetalCost::Append(csv);
    Cara::AppendNativeMetalWatchProfile(csv, false);
    std::ofstream json(prefix + ".json");
    std::array<std::uint64_t, 20> totals{};
    json << "{\"frame\":" << frame;
    Cara::AppendNativeMetalBindingWindow(json, totals);
    Cara::AppendNativeMetalWatchProfile(json, true);
    json << "}\n";
    if (!csv || !json)
        throw std::runtime_error("Could not retain Metal cost boundary");
}
std::optional<double> CpuClockSeconds(clockid_t clock) {
    timespec value{};
    if (clock_gettime(clock, &value) != 0) return std::nullopt;
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) * 1e-9;
}

void WriteCpuExecutionProfile(Core::System& core, const std::string& path, unsigned frame) {
    std::ofstream profile(path);
    profile << "{\"frame\":" << frame << ",\"new_3ds\":"
            << static_cast<bool>(Settings::values.is_new_3ds)
            << ",\"core_ticks_hack\":" << Settings::values.core_ticks_hack << ",\"cores\":[";
    for (unsigned i = 0; i < core.GetNumCores(); ++i) {
        if (i) profile << ',';
        auto& cpu = core.GetCore(i);
        profile << "{\"core\":" << i << ",\"timer_ticks\":"
                << core.CoreTiming().GetTimer(i)->GetTicks() << ",\"clock_scale\":"
                << core.CoreTiming().GetTimer(i)->GetClockScaleForDiagnostics();
#if defined(CARA_MAC_CPU_JIT)
        if (auto* jit = dynamic_cast<ARM_Dynarmic*>(&cpu)) {
            profile << ",\"backend\":\"jit\",\"diagnostics\":";
            jit->WriteDiagnosticJSON(profile);
        } else
#endif
        {
            profile << ",\"backend\":\"fastinterp\"";
        }
        profile << '}';
    }
    profile << "]}\n";
    if (!profile) throw std::runtime_error("Could not retain CPU execution diagnostics");
}

void SnapshotCheckpointFilesystem(const std::filesystem::path& user) {
    const auto destination = user / "checkpoint-filesystem";
    if (!std::filesystem::create_directory(destination))
        throw std::runtime_error("Checkpoint filesystem already exists");
    for (const auto* name : {"sdmc", "nand", "sysdata"}) {
        const auto source = user / name;
        if (!std::filesystem::exists(source)) continue;
        if (std::filesystem::is_symlink(source) || !std::filesystem::is_directory(source))
            throw std::runtime_error("Checkpoint filesystem must contain regular directories");
        std::filesystem::create_directory(destination / name);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
            const auto status = entry.symlink_status();
            if (std::filesystem::is_symlink(status))
                throw std::runtime_error("Checkpoint filesystem cannot contain symlinks");
            const auto target = destination / name / entry.path().lexically_relative(source);
            if (std::filesystem::is_directory(status)) {
                std::filesystem::create_directories(target);
            } else if (std::filesystem::is_regular_file(status)) {
                std::filesystem::copy_file(entry.path(), target);
                std::filesystem::last_write_time(target, entry.last_write_time());
            } else {
                throw std::runtime_error("Checkpoint filesystem contains a nonregular entry");
            }
        }
    }
}

unsigned ParseUnsigned(std::string_view value) {
    unsigned number{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !number)
        throw std::runtime_error("Invalid frame number");
    return number;
}

void WriteFastInterpProfile(Core::System& core, const std::string& path, unsigned frame) {
    if (!std::getenv("CARA_FASTINTERP_PROFILE")) return;
    std::ofstream profile(path);
    profile << "{\"frame\":" << frame
            << ",\"kind\":\"resident_block_census_not_time_samples\",\"cores\":[";
    bool comma = false;
    for (unsigned i = 0; i < core.GetNumCores(); ++i) {
        if (auto* cpu = dynamic_cast<Core::FastInterp::ARM_FastInterp*>(&core.GetCore(i))) {
            if (comma) profile << ',';
            cpu->WriteBlockProfile(profile);
            comma = true;
        }
    }
    profile << "]}\n";
    if (!profile) throw std::runtime_error("Could not retain FastInterp block profile");
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
            const char* save_frame_env = std::getenv("CARA_COMPAT_SAVE_STATE_FRAME");
            const unsigned save_frame = save_frame_env ? ParseUnsigned(save_frame_env) : 0;
            const char* save_end_env = std::getenv("CARA_COMPAT_SAVE_STATE_AT_END");
            const unsigned save_at_end = save_end_env ? ParseUnsigned(save_end_env) : 0;
            if (save_at_end > 1 || (save_frame && save_at_end))
                throw std::runtime_error("Select one checkpoint mode: intermediate frame or end");
            if (save_frame >= limit)
                throw std::runtime_error("Checkpoint boundary must precede the refresh limit");
            const auto inputs = ReadInputs(output, limit);
            const auto should_capture = [&](unsigned frame) {
                return frame == limit || (interval && frame && frame % interval == 0);
            };
            // Share proven production renderer defaults with the iPad; retain
            // its wider upload arena as an explicit Mac experiment.
            if (!Common::ApplyAppleMetalDefaults())
                throw std::runtime_error("Could not set the Apple Metal renderer policy");
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
            if (const char* sync = std::getenv("CARA_METAL_HOST_DISPLAY_SYNC")) {
                if (std::string_view(sync) != "0" && std::string_view(sync) != "1")
                    throw std::runtime_error("Host display sync must be 0 or 1");
                layer.displaySyncEnabled = std::string_view(sync) == "1";
            }
            std::cout << "HOST_PRESENTATION display_sync=" << layer.displaySyncEnabled
                      << " maximum_drawables=" << layer.maximumDrawableCount << std::endl;
            NSError* error = nil;
            NSURL* library = [NSBundle.mainBundle URLForResource:@"cara-pica" withExtension:@"metallib"];
            const auto assets = Cara::LoadPicaMetalResources(layer.device, library,
                                                            layer.pixelFormat, &error);
            if (!assets)
                throw std::runtime_error(error.localizedDescription.UTF8String ?: "Missing Metal assets");

            Common::ApplyAppleFastInterpDefaults();
            const auto host_policy=Common::ApplyAppleEmulationQoS();
            std::cout << "HOST_QOS initial=" << static_cast<unsigned>(host_policy.initial)
                      << " requested=" << static_cast<unsigned>(host_policy.requested)
                      << " effective=" << static_cast<unsigned>(host_policy.effective)
                      << " result=" << host_policy.result << std::endl;
            const auto* latency_option = std::getenv("CARA_METAL_HOST_LATENCY_CRITICAL");
            const bool latency_critical = latency_option && std::string_view(latency_option) == "1";
            const NSActivityOptions activity_options = NSActivityUserInitiatedAllowingIdleSystemSleep |
                (latency_critical ? NSActivityLatencyCritical : 0);
            id activity=[NSProcessInfo.processInfo beginActivityWithOptions:activity_options
                reason:@"Running user-requested Nintendo 3DS emulation"];
            SCOPE_EXIT({ if(activity) [NSProcessInfo.processInfo endActivity:activity]; });
            std::cout << "HOST_ACTIVITY options=" << static_cast<unsigned long long>(activity_options)
                      << " latency_critical=" << latency_critical
                      << " active=" << (activity != nil) << std::endl;
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
                if (const auto* state_path = std::getenv("CARA_COMPAT_LOAD_STATE")) {
                    core.LoadStateFromPath(state_path);
                    std::cout << "CHECKPOINT loaded=" << state_path << std::endl;
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
                for(unsigned i=0;i<core.GetNumCores();++i)
                    if(core.CoreTiming().GetTimer(i)->GetClockScaleForDiagnostics()!=1.0)
                        throw std::runtime_error("This profiling runner requires 100 percent guest clock on every core");
                constexpr unsigned guest_cpu_percent=100;
                unsigned fastinterp_cores=0;
                for(unsigned i=0;i<core.GetNumCores();++i)
                    fastinterp_cores+=dynamic_cast<Core::FastInterp::ARM_FastInterp*>(&core.GetCore(i))!=nullptr;
                if(!jit && fastinterp_cores!=core.GetNumCores())
                    throw std::runtime_error("FastInterp was requested but not active on every core");
                std::cout << "EXECUTION backend=" << backend << " fastinterp_cores=" << fastinterp_cores
                          << " guest_cpu_percent=" << guest_cpu_percent
                          << " speed_cap=" << Settings::values.frame_limit << std::endl;
                std::cout << "GUEST_CONFIG new_3ds=" << static_cast<bool>(Settings::values.is_new_3ds)
                          << " core_ticks_hack=" << Settings::values.core_ticks_hack << std::endl;
                for(const char* name:{"CARA_FASTINTERP_ALU_OPERANDS","CARA_FASTINTERP_TICK_BUCKETS","CARA_FASTINTERP_CHAIN_BUDGET","CARA_FASTINTERP_CACHE_WAYS","CARA_FASTINTERP_CACHE_ENTRIES","CARA_FASTINTERP_RAM_BURST","CARA_FASTINTERP_AL_DISPATCH","CARA_FASTINTERP_ALU_PAIRS"})
                    std::cout << "CPU_POLICY " << name << '=' << (std::getenv(name)?std::getenv(name):"core-default") << std::endl;
                for (const auto* name : Common::AppleMetalDefaultOptions)
                    std::cout << "METAL_POLICY " << name << '=' << std::getenv(name) << std::endl;
                for (const auto* name : {"CARA_METAL_EIGHT_UPLOAD_PAGES", "CARA_METAL_DRAW_GROUP_64",
                                         "CARA_METAL_CACHE_LARGE_TEXTURE_SNAPSHOTS"})
                    std::cout << "METAL_POLICY " << name << '='
                              << (std::getenv(name) ? std::getenv(name) : "0") << std::endl;
                std::cout << "METAL_POLICY CARA_METAL_DRAW_GROUP_LIMIT="
                          << Cara::NativeMetalCost::DrawGroupLimit() << std::endl;
                std::cout << "HOST_POWER thermal_state=" << static_cast<int>(NSProcessInfo.processInfo.thermalState)
                          << " low_power=" << NSProcessInfo.processInfo.lowPowerModeEnabled << std::endl;
                std::ofstream performance(output+"metal-performance.jsonl");
                if(!performance) throw std::runtime_error("Could not open performance report");
                std::cout << "TITLE " << std::hex << core.GetTitleID() << std::dec << std::endl;
                const bool learning_requested =
                    Common::AppleMetalPipelineLearning(std::getenv("CARA_NATIVE_DIAGNOSTICS") != nullptr);
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
                unsigned performance_start_frame=previous_frame;
                const char* warmup_option = std::getenv("CARA_METAL_BENCHMARK_WARMUP");
                const unsigned warmup = warmup_option ? ParseUnsigned(warmup_option) : limit;
                const char* cpu_timeline_option = std::getenv("CARA_METAL_REFRESH_CPU_TIMELINE");
                const bool trace_refresh_cpu =
                    cpu_timeline_option && std::string_view(cpu_timeline_option) == "1";
                std::optional<std::chrono::steady_clock::time_point> measured_start;
                std::optional<double> measured_thread_cpu, measured_process_cpu;
                std::optional<double> last_refresh_cpu;
                unsigned measured_start_frame = 0;
                bool checkpoint_saved = false;
                auto last_refresh_at = std::chrono::steady_clock::now();
                std::vector<std::pair<unsigned, double>> refresh_intervals;
                refresh_intervals.reserve(std::min(limit, 1000000u));
                std::vector<std::pair<unsigned, double>> refresh_cpu_intervals;
                if (trace_refresh_cpu && warmup < limit)
                    refresh_cpu_intervals.reserve(std::min(limit - warmup, 1000000u));
                while (!host.closed && VideoCore::GetCurrentFrame() < limit) {
                    while (next_input < inputs.size() && inputs[next_input].frame <= previous_frame)
                        host.ApplyInput(inputs[next_input++]);
                    @autoreleasepool {
                        const auto step = core.RunLoop();
                        if (step != Core::System::ResultStatus::Success)
                            throw std::runtime_error("Core run status " + std::to_string(unsigned(step)));
                    }
                    const unsigned frame = VideoCore::GetCurrentFrame();
                    if (frame != previous_frame) {
                        const auto now = std::chrono::steady_clock::now();
                        refresh_intervals.emplace_back(frame,
                            std::chrono::duration<double, std::milli>(now - last_refresh_at).count());
                        last_refresh_at = now;
                        if (trace_refresh_cpu && measured_start) {
                            const auto cpu = CpuClockSeconds(CLOCK_THREAD_CPUTIME_ID);
                            if (frame == previous_frame + 1 && last_refresh_cpu && cpu &&
                                *cpu >= *last_refresh_cpu) {
                                refresh_cpu_intervals.emplace_back(
                                    frame, (*cpu - *last_refresh_cpu) * 1000);
                            }
                            last_refresh_cpu = cpu;
                        }
                    }
                    if (save_frame && !checkpoint_saved && frame >= save_frame) {
                        const auto path = output + "checkpoint.carastate";
                        core.SaveStateToPath(path);
                        SnapshotCheckpointFilesystem(output);
                        checkpoint_saved = true;
                        std::cout << "CHECKPOINT saved=" << path << " frame=" << frame << std::endl;
                        // Checkpoint serialization is diagnostic I/O, not a refresh interval.
                        last_refresh_at = std::chrono::steady_clock::now();
                        if (trace_refresh_cpu && measured_start)
                            last_refresh_cpu = CpuClockSeconds(CLOCK_THREAD_CPUTIME_ID);
                    }
                    if (!measured_start && warmup < limit && frame >= warmup) {
                        WriteHostEnvironment(output + "host-environment-warmup.json", window, frame, layer.device);
                        WriteMetalCostSnapshot(output + "metal-cost-warmup", frame);
                        WriteFastInterpProfile(core, output + "fastinterp-blocks-warmup.json", frame);
                        WriteCpuExecutionProfile(core, output + "cpu-execution-warmup.json", frame);
                        last_refresh_at = std::chrono::steady_clock::now();
                        measured_start = std::chrono::steady_clock::now();
                        measured_thread_cpu = CpuClockSeconds(CLOCK_THREAD_CPUTIME_ID);
                        if (trace_refresh_cpu)
                            last_refresh_cpu = measured_thread_cpu;
                        measured_process_cpu = CpuClockSeconds(CLOCK_PROCESS_CPUTIME_ID);
                        measured_start_frame = frame;
                    }
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
                            performance << "{\"start_refresh\":" << performance_start_frame
                                        << ",\"end_refresh\":" << frame
                                        << ",\"wall_seconds\":" << double(now-title_updated_at)/1000.0
                                        << ",\"game_fps\":" << stats.game_fps
                                        << ",\"emulation_speed_percent\":" << stats.emulation_speed*100.0
                                        << ",\"guest_cpu_percent\":" << guest_cpu_percent
                                        << ",\"cpu_backend\":\"" << backend << "\"}" << std::endl;
                            std::cout << "PERF frame=" << frame << " game_fps=" << stats.game_fps
                                      << " speed_percent=" << stats.emulation_speed*100.0 << std::endl;
                            performance_start_frame=frame;
                            SDL_SetWindowTitle(window, title);
                            title_updated_at = now;
                        }
                    }
                    previous_frame = frame;
                }
                const auto frames = VideoCore::GetCurrentFrame();
                const bool captured = frames >= limit && Cara::WriteNativeMetalCapture(output);
                {
                    // RunLoop/VBlank boundaries, not physical display scanout or unique game FPS.
                    std::ofstream refresh_report(output + "metal-refresh-intervals.csv");
                    refresh_report.precision(17);
                    refresh_report << "refresh,wall_ms\n";
                    for (const auto& [refresh, milliseconds] : refresh_intervals)
                        refresh_report << refresh << ',' << milliseconds << '\n';
                    if (!refresh_report) throw std::runtime_error("Could not retain refresh timing");
                }
                if (measured_start && frames > measured_start_frame) {
                    const auto thread_end = CpuClockSeconds(CLOCK_THREAD_CPUTIME_ID);
                    const auto process_end = CpuClockSeconds(CLOCK_PROCESS_CPUTIME_ID);
                    const auto seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - *measured_start).count();
                    std::ofstream interval(output + "metal-measured-interval.json");
                    interval.precision(17);
                    interval << "{\"start_refresh\":" << measured_start_frame
                             << ",\"end_refresh\":" << frames << ",\"wall_seconds\":" << seconds
                             << ",\"wall_ms_per_refresh\":" << seconds * 1000 / (frames - measured_start_frame)
                             << ",\"includes_final_gpu_capture_drain\":true";
                    if (measured_thread_cpu && thread_end && *thread_end >= *measured_thread_cpu)
                        interval << ",\"thread_cpu_seconds\":" << *thread_end - *measured_thread_cpu
                                 << ",\"thread_cpu_ms_per_refresh\":"
                                 << (*thread_end - *measured_thread_cpu) * 1000 / (frames - measured_start_frame);
                    if (measured_process_cpu && process_end && *process_end >= *measured_process_cpu)
                        interval << ",\"process_cpu_seconds\":" << *process_end - *measured_process_cpu
                                 << ",\"process_cpu_ms_per_refresh\":"
                                 << (*process_end - *measured_process_cpu) * 1000 / (frames - measured_start_frame);
                    interval << "}\n";
                    if (!interval) throw std::runtime_error("Could not retain measured interval");
                }
                if (trace_refresh_cpu) {
                    std::ofstream report(output + "metal-refresh-cpu.csv");
                    report.precision(17);
                    report << "refresh,thread_cpu_ms\n";
                    for (const auto& [refresh, milliseconds] : refresh_cpu_intervals)
                        report << refresh << ',' << milliseconds << '\n';
                    if (!report)
                        throw std::runtime_error("Could not retain refresh CPU timing");
                }
                if (learning && frames == limit && presentations == limit && captured) {
                    if (!Cara::FinishNativeMetalWarmup())
                        throw std::runtime_error("Could not finish local Metal pipeline learning");
                    std::cout << "METAL_PIPELINE_LEARNING finished=1" << std::endl;
                }
                // Boundary evidence only: this does not serialize all guest memory/services.
                std::ofstream boundary(output + "guest-boundary.json");
                boundary << "{\"refresh\":" << frames << ",\"ticks\":" << core.CoreTiming().GetTicks()
                         << ",\"cores\":[";
                for (unsigned i = 0; i < core.GetNumCores(); ++i) {
                    auto& arm = core.GetCore(i);
                    if (i) boundary << ',';
                    boundary << "{\"ticks\":" << core.CoreTiming().GetTimer(i)->GetTicks()
                             << ",\"cpsr\":" << arm.GetCPSR() << ",\"registers\":[";
                    for (int r = 0; r < 16; ++r) {
                        if (r) boundary << ',';
                        boundary << arm.GetReg(r);
                    }
                    boundary << "],\"vfp\":[";
                    for (int r = 0; r < 32; ++r) {
                        if (r) boundary << ',';
                        boundary << arm.GetVFPReg(r);
                    }
                    boundary << "]}";
                }
                boundary << "]}\n";
                boundary.close();
                if (!boundary) throw std::runtime_error("Could not retain guest boundary evidence");
                WriteFastInterpProfile(core, output + "fastinterp-blocks.json", frames);
                WriteCpuExecutionProfile(core, output + "cpu-execution.json", frames);
                WriteHostEnvironment(output + "host-environment-final.json", window, frames, layer.device);
                WriteMetalCostSnapshot(output + "metal-cost-final", frames);
                if (save_at_end && frames == limit && presentations == limit && captured) {
                    // Keep serialization/global renderer invalidation outside
                    // timing and boundary snapshots, preserving normal history.
                    const auto path = output + "checkpoint.carastate";
                    core.SaveStateToPath(path);
                    SnapshotCheckpointFilesystem(output);
                    std::cout << "CHECKPOINT saved=" << path << " frame=" << frames
                              << " after_measurement=1" << std::endl;
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
