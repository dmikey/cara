// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// SPDX-License-Identifier: GPL-2.0-or-later
// Native macOS SDL developer host adapted to Cara's current EmuWindow API.
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <SDL_video.h>
#include <glad/glad.h>
#include <unistd.h>
#include "cara/config.h"
#include "cara/lodepng_image_interface.h"
#include "cara/native_mac/emu_window.h"
#include "cara/native_mac/profile_snapshot.h"
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/frame_timing.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/scope_exit.h"
#include "core/arm/fastinterp/fastinterp.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/savestate.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/video_core.h"
static void NativeCPUDefaults() {
    for (const auto& item : {std::pair{"CARA_CPU_BACKEND", "fastinterp"},
                             {"CARA_FASTINTERP_ALU_OPERANDS", "1"},
                             {"CARA_FASTINTERP_TICK_BUCKETS", "1"},
                             {"CARA_FASTINTERP_CHAIN_BUDGET", "1"},
                             {"CARA_FASTINTERP_CACHE_WAYS", "8"},
                             {"CARA_FASTINTERP_CACHE_ENTRIES", "32768"},
                             {"CARA_FASTINTERP_RAM_BURST", "1"},
                             {"CARA_FASTINTERP_AL_DISPATCH", "1"}}) {
        if (!std::getenv(item.first))
            setenv(item.first, item.second, 0);
    }
}
static void WriteNativeManifest(const std::string& directory) {
    const auto& v = Settings::values;
    std::ofstream out(directory + "native-settings.json");
    out << std::boolalpha;
    out << "{\"guest_cpu_percent\":100,\"maximum_speed_percent\":100";
    out << ",\"use_cpu_jit\":" << v.use_cpu_jit;
    out << ",\"use_shader_jit\":" << v.use_shader_jit;
    out << ",\"use_gles\":" << v.use_gles;
    out << ",\"use_hw_renderer\":" << v.use_hw_renderer;
    out << ",\"use_hw_shader\":" << v.use_hw_shader;
    out << ",\"use_separable_shader\":" << v.use_separable_shader;
    out << ",\"allow_shadow\":" << v.allow_shadow;
    out << ",\"shadow_rendering\":" << v.shadow_rendering;
    out << ",\"accurate_max_min\":" << v.accurate_max_min;
    out << ",\"accurate_rcp_rsq\":" << v.accurate_rcp_rsq;
    out << ",\"use_compatible_mode\":" << v.use_compatible_mode;
    out << ",\"disable_clip_coef\":" << v.disable_clip_coef;
    out << ",\"use_linear_filter\":" << v.use_linear_filter;
    out << ",\"use_frame_limit\":" << v.use_frame_limit;
    out << ",\"vsync_enabled\":" << v.vsync_enabled;
    out << ",\"actual_swap_interval\":" << SDL_GL_GetSwapInterval();
    out << ",\"use_present_thread\":" << v.use_present_thread;
    out << ",\"use_shader_cache\":" << v.use_shader_cache;
    out << ",\"custom_textures\":" << v.custom_textures;
    out << ",\"async_shader_compile\":" << v.async_shader_compile;
    out << ",\"use_direct_display\":" << v.use_direct_display;
    out << ",\"use_fence_sync\":" << v.use_fence_sync;
    out << ",\"skip_cpu_write\":" << v.skip_cpu_write;
    out << ",\"skip_texture_copy\":" << v.skip_texture_copy;
    out << ",\"skip_load_buffer\":" << v.skip_load_buffer;
    out << ",\"merge_framebuffer\":" << v.merge_framebuffer;
    out << ",\"display_transfer_hack\":" << v.display_transfer_hack;
    out << ",\"effective_allow_shadow\":" << OpenGL::AllowShadow
        << ",\"accurate_mul\":" << static_cast<unsigned>(v.shaders_accurate_mul)
        << ",\"hw_gs_mode\":" << unsigned(v.hw_gs_mode)
        << ",\"force_texture_filter\":" << unsigned(v.force_texture_filter)
        << ",\"resolution_factor\":" << v.resolution_factor
        << ",\"factor_3d\":" << unsigned(v.factor_3d.load()) << ",\"frame_limit\":" << v.frame_limit
        << ",\"rtc_fixed\":" << (v.init_clock == Settings::InitClock::FixedTime)
        << ",\"rtc_init_epoch\":" << v.init_time << ",\"audio_sink\":\"" << v.sink_id
        << "\",\"audio_stretching\":" << v.enable_audio_stretching << ",\"gl_version\":\""
        << reinterpret_cast<const char*>(glGetString(GL_VERSION)) << "\",\"glsl_version\":\""
        << reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION))
        << "\",\"cpu_environment\":{";
    bool comma = false;
    for (const auto* name : {"CARA_CPU_BACKEND", "CARA_FASTINTERP_ALU_OPERANDS",
                             "CARA_FASTINTERP_TICK_BUCKETS", "CARA_FASTINTERP_CHAIN_BUDGET",
                             "CARA_FASTINTERP_CACHE_WAYS", "CARA_FASTINTERP_CACHE_ENTRIES",
                             "CARA_FASTINTERP_RAM_BURST", "CARA_FASTINTERP_AL_DISPATCH"}) {
        const char* value = std::getenv(name);
        out << (comma ? "," : "") << "\"" << name << "\":\"" << (value ? value : "") << "\"";
        comma = true;
    }
    out << "},\"cores\":[";
    auto& system = Core::System::GetInstance();
    for (u32 i = 0; i < system.GetNumCores(); ++i) {
        if (i)
            out << ',';
        if (auto* cpu = dynamic_cast<Core::FastInterp::ARM_FastInterp*>(&system.GetCore(i)))
            cpu->WriteBlockProfile(out);
        else
            out << "{\"core\":" << i << ",\"backend\":\"non-FastInterp override\"}";
    }
    out << "]}\n";
}
static volatile std::sig_atomic_t stop_requested = 0;
static void StopNative(int) {
    stop_requested = 1;
}
int main(int argc, char** argv) {
    try {
        if (argc < 2 || std::string(argv[1]) == "--help") {
            std::cout << "Cara native no-JIT developer host\nUsage: cara ROM [--fullscreen]\nSet "
                         "CARA_USER_DIR to a fresh absolute data directory.\n";
            return argc < 2 ? 1 : 0;
        }
        const char* configured = std::getenv("CARA_USER_DIR");
        if (!configured || configured[0] != '/')
            throw std::runtime_error("CARA_USER_DIR must name a fresh absolute data directory");
        std::string directory(configured);
        if (directory.back() != '/')
            directory += '/';
        if (!FileUtil::CreateFullPath(directory))
            throw std::runtime_error("Cannot create CARA_USER_DIR");
        FileUtil::SetUserPath(directory);
        std::ofstream(directory + "native.pid") << getpid() << "\n";
        NativeCPUDefaults();
        Common::DetachedTasks tasks;
        Config config;
        Log::Filter filter(Log::Level::Info);
        filter.ParseFilterString(Settings::values.log_filter);
        Log::SetGlobalFilter(filter);
        Log::AddBackend(std::make_unique<Log::ColorConsoleBackend>());
        const auto logdir = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
        FileUtil::CreateFullPath(logdir);
        Log::AddBackend(std::make_unique<Log::FileBackend>(logdir + "emulator.log"));
        Settings::Apply();
        Frontend::RegisterDefaultApplets();
        auto& system = Core::System::GetInstance();
        system.RegisterImageInterface(std::make_shared<LodePNGImageInterface>());
        const bool fullscreen = argc > 2 && std::string(argv[2]) == "--fullscreen";
        EmuWindow_SDL2 window(fullscreen);
        window.MakeCurrent();
        // Release GPU/core resources while the owning window and GL context are alive,
        // including failure paths from checked framebuffer copies.
        SCOPE_EXIT({ system.Shutdown(); });
        const auto loaded = system.Load(window, argv[1]);
        std::cout << "Native load result: " << static_cast<unsigned>(loaded) << std::endl;
        if (loaded != Core::System::ResultStatus::Success)
            return 3;
        const bool diagnostics = std::getenv("CARA_NATIVE_DIAGNOSTICS") &&
                                 std::string(std::getenv("CARA_NATIVE_DIAGNOSTICS")) == "1";
        const auto record_state = [&](const char* operation) {
            std::ofstream state(directory + "native-state-status.json");
            state << "{\"operation\":\"" << operation
                  << "\",\"slot\":1,\"frame\":" << VideoCore::GetCurrentFrame()
                  << ",\"fingerprint\":\"" << Core::GetSaveStateBuildFingerprint()
                  << "\",\"ticks\":" << system.CoreTiming().GetTicks() << "}\n";
        };
        if (const char* load = std::getenv("CARA_NATIVE_LOAD_SLOT");
            diagnostics && load && std::string(load) == "1") {
            system.LoadState(1);
            record_state("load");
        }
        if (const char* diagnostic = std::getenv("CARA_NATIVE_DIAGNOSTICS");
            diagnostic && std::string(diagnostic) == "1")
            Common::FrameTiming::counters.enabled =
                !std::getenv("CARA_NATIVE_TIMING_COUNTERS") ||
                std::string(std::getenv("CARA_NATIVE_TIMING_COUNTERS")) != "0";
        WriteNativeManifest(directory);
        WriteNativeProfileSnapshot(directory + "native-startup", VideoCore::GetCurrentFrame());
        std::signal(SIGTERM, StopNative);
        std::signal(SIGINT, StopNative);
        if (const char* benchmark = std::getenv("CARA_NATIVE_BENCHMARK");
            diagnostics && benchmark && std::string(benchmark) == "1")
            window.BeginBenchmark();
        while (window.IsOpen() && !stop_requested) {
            const auto result = system.RunLoop();
            if (result == Core::System::ResultStatus::ShutdownRequested)
                break;
            if (result != Core::System::ResultStatus::Success) {
                std::cerr << "Run failure: " << static_cast<unsigned>(result) << std::endl;
                return 4;
            }
            // PollEvents only queues requests. Serialize here, after RunLoop and
            // its timing callbacks return/requeue, with the GL context still live.
            switch (window.TakeStateCommand()) {
            case EmuWindow_SDL2::StateCommand::Save:
                system.SaveState(1);
                record_state("save");
                break;
            case EmuWindow_SDL2::StateCommand::Load:
                system.LoadState(1);
                record_state("load");
                break;
            case EmuWindow_SDL2::StateCommand::None:
                break;
            }
            if (window.TakeBenchmarkStop())
                break;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native host failed: " << error.what() << std::endl;
        return 2;
    }
}
