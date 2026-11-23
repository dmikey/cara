#include <SDL.h>
#include "common/apple_emulation_policy.h"
#include "common/apple_fastinterp_policy.h"
#include "cara/native_mac/native_command_policy.h"
#include "cara/native_mac/game_menu.h"
#include "cara/native_mac/mac_preferences_ui.h"
#include "cara/native_mac/controller_play_test.h"
#ifdef CARA_MAC_CPU_JIT
#include "cara/native_mac/arm_dynarmic_adapter_test.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#endif
#include "input_common/main.h"
#include "video_core/renderer_opengl/gl_command_queue.h"
#include "cara/native_mac/diagnostic_clock.h"
#include <atomic>
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/video_core.h"
// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// SPDX-License-Identifier: GPL-2.0-or-later
// Native macOS SDL developer host adapted to Cara's current EmuWindow API.
#include <csignal>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include "cara/native_mac/profile_gpu_boundary.h"
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <SDL_video.h>
#include <glad/glad.h>
#include <unistd.h>
#include "cara/config.h"
#include "cara/guest_gpu_trace.h"
#include "cara/texture_copy_trace.h"
#include "cara/lodepng_image_interface.h"
#include "cara/native_mac/emu_window.h"
#include "cara/native_mac/cpu_category_profile.h"
#include "cara/native_mac/profile_snapshot.h"
#include "cara/native_mac/behavior_fixture.h"
#include "cara/native_mac/required_copy_fixture.h"
#include "cara/native_mac/gl_query_census.h"
#include "cara/native_mac/relay_config.h"
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
// Optional audit wraps existing driver queries; it never adds a query in the loop.
static PFNGLGETERRORPROC native_original_get_error=nullptr;
static std::atomic<GLenum> native_first_gl_error{GL_NO_ERROR};
static GLenum NativeAuditedGetError() {
    const auto error=native_original_get_error();
    if(error!=GL_NO_ERROR) {
        GLenum expected=GL_NO_ERROR;
        native_first_gl_error.compare_exchange_strong(expected,error,std::memory_order_relaxed);
    }
    return error;
}
static bool NativeAuditHookConnected() {
    // Explicitly registered exclusive wrappers; never accept an arbitrary hook.
    return Cara::NativeGLQueryCensus::AuditConnected(NativeAuditedGetError) ||
           OpenGL::CommandQueue::ForwardsGetError(NativeAuditedGetError);
}
static void NativeCPUDefaults() { Common::ApplyAppleFastInterpDefaults(); }
static void WriteNativeManifest(const std::string& directory) {
    auto& system=Core::System::GetInstance();
    const auto& v = Settings::values;
    unsigned jit_cores = 0, fastinterp_cores = 0;
    for (unsigned i = 0; i < system.GetNumCores(); ++i) {
#ifdef CARA_MAC_CPU_JIT
        jit_cores += dynamic_cast<ARM_Dynarmic*>(&system.GetCore(i)) != nullptr;
#endif
        fastinterp_cores += dynamic_cast<Core::FastInterp::ARM_FastInterp*>(&system.GetCore(i)) != nullptr;
    }
    if ((v.use_cpu_jit && jit_cores != system.GetNumCores()) ||
        (!v.use_cpu_jit && fastinterp_cores != system.GetNumCores()))
        throw std::runtime_error("Requested CPU backend did not initialize on every core");
    std::cout << "CPU_BACKEND " << (v.use_cpu_jit ? "jit" : "fastinterp")
              << " jit_cores=" << jit_cores << " fastinterp_cores=" << fastinterp_cores
              << " cores=" << system.GetNumCores() << " is_new_3ds=" << v.is_new_3ds
              << " core_ticks_hack=" << v.core_ticks_hack << std::endl;
    std::ofstream out(directory + "native-settings.json");
    out << std::boolalpha;
    out << "{\"guest_cpu_percent\":" << Cara::NativeDiagnosticClock::EffectivePercent(system.CoreTiming())
        << ",\"maximum_speed_percent\":" << v.frame_limit << ",\"diagnostic_clock\":";
    Cara::NativeDiagnosticClock::WriteJSON(out,system.CoreTiming(),system.GetNumCores());
    out << ",\"use_cpu_jit\":" << v.use_cpu_jit;
    out << ",\"cpu_backend\":\"" << (v.use_cpu_jit ? "jit" : "fastinterp") << "\""
        << ",\"jit_cores\":" << jit_cores << ",\"fastinterp_cores\":" << fastinterp_cores
        << ",\"core_count\":" << system.GetNumCores() << ",\"is_new_3ds\":" << v.is_new_3ds
        << ",\"core_ticks_hack\":" << v.core_ticks_hack << ",\"clock_scales\":[";
    for (unsigned i = 0; i < system.GetNumCores(); ++i)
        out << (i ? "," : "") << system.CoreTiming().GetTimer(i)->GetClockScaleForDiagnostics();
    out << ']';
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
                             "CARA_FASTINTERP_RAM_BURST", "CARA_FASTINTERP_AL_DISPATCH",
                             "CARA_FASTINTERP_ALU_PAIRS"}) {
        const char* value = std::getenv(name);
        out << (comma ? "," : "") << "\"" << name << "\":\"" << (value ? value : "") << "\"";
        comma = true;
    }
    out << "},\"cores\":[";
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
static std::string NativeOracleJSONQuote(const std::string& input) {
    static constexpr char hex[]="0123456789abcdef";
    std::string out="\"";
    for (unsigned char c : input) {
        if (c=='"' || c=='\\') { out+='\\'; out+=c; }
        else if(c<0x20) { out+="\\u00";out+=hex[c>>4];out+=hex[c&15]; }
        else out+=c;
    }
    out+='"';return out;
}
static volatile std::sig_atomic_t stop_requested = 0;
static void StopNative(int) {
    stop_requested = 1;
}
static int RunNativeMain(int argc, char** argv) {
    const auto host_policy=Common::ApplyAppleEmulationQoS();
    std::cout << "HOST_QOS initial=" << static_cast<unsigned>(host_policy.initial)
              << " requested=" << static_cast<unsigned>(host_policy.requested)
              << " effective=" << static_cast<unsigned>(host_policy.effective)
              << " result=" << host_policy.result << std::endl;
    try {
        if (argc < 2 || std::string(argv[1]) == "--help") {
            std::cout << "Cara Mac host\nUsage: cara [ROM] [--fullscreen] [--relay-stdin]\nSet "
                         "CARA_USER_DIR to a fresh absolute data directory.\n";
            return argc < 2 ? 1 : 0;
        }
        if (NativeMac::DeveloperCommands::InvalidRequest(
                std::getenv("CARA_NATIVE_DEV_COMMANDS"),std::getenv("CARA_NATIVE_DIAGNOSTICS")))
            throw std::runtime_error("Developer commands require CARA_NATIVE_DIAGNOSTICS=1");
        const bool required_copy_integration=std::getenv("CARA_NATIVE_UNCHECKED_BLIT_INTEGRATION_TEST")&&
            std::string(std::getenv("CARA_NATIVE_UNCHECKED_BLIT_INTEGRATION_TEST"))=="1";
        const bool original_required_copy_fixture=required_copy_integration ||std::getenv("CARA_NATIVE_REQUIRED_COPY_COUNTERS_OFF_TEST")&&
            std::string(std::getenv("CARA_NATIVE_REQUIRED_COPY_COUNTERS_OFF_TEST"))=="1";
        const bool cpu_adapter_fixture=std::getenv("CARA_NATIVE_CPU_ADAPTER_TEST") &&
            std::string(std::getenv("CARA_NATIVE_CPU_ADAPTER_TEST"))=="1";
#ifndef CARA_MAC_CPU_JIT
        if(cpu_adapter_fixture)throw std::runtime_error("CPU adapter test unavailable in this build");
#endif
        const bool controller_fixture=std::getenv("CARA_NATIVE_CONTROLLER_INPUT_TEST") && std::string(std::getenv("CARA_NATIVE_CONTROLLER_INPUT_TEST"))=="1";
        const bool original_behavior_fixture=controller_fixture || cpu_adapter_fixture || original_required_copy_fixture ||
            (std::getenv("CARA_NATIVE_BEHAVIOR_FIXTURE")&&
             std::string(std::getenv("CARA_NATIVE_BEHAVIOR_FIXTURE"))=="1");
        if(original_behavior_fixture) {
            std::string error;
            if(!Cara::NativeBehavior::ValidateBootstrap(argv[1],error))throw std::runtime_error(error);
            const char* user=std::getenv("CARA_USER_DIR");
            if(!user||!std::filesystem::path(user).is_absolute()||
               (std::filesystem::exists(user)&&(!std::filesystem::is_directory(user)||!std::filesystem::is_empty(user))))
                throw std::runtime_error("original behavior fixture requires a fresh empty private user directory");
            for(const char* flag:{"CARA_NATIVE_DIAGNOSTICS","CARA_NATIVE_SCRIPT_ONLY_INPUT"}) {
                const char* value=std::getenv(flag);
                if(!value||std::string(value)!="1")throw std::runtime_error("original behavior fixture requires diagnostics and script-only input");
            }
            for(const char* flag:{"CARA_GPU_ABLATE_FRAGMENT_LIGHTING","CARA_GPU_ABLATE_PARTICLE_FAMILY",
                "CARA_GPU_ABLATE_COMPOSITE_PASS","CARA_GPU_ABLATE_RAY_CANDIDATE",
                "CARA_GPU_LIT_VERTEX_REUSE","CARA_GPU_LIT_PAIR_REUSE","CARA_GPU_VERTEX_REUSE",
                "CARA_NATIVE_NEAREST_BORROW","CARA_GPU_FEEDBACK_GROW_ONLY"}) {
                const char* value=std::getenv(flag);
                if(value&&std::string(value)=="1")throw std::runtime_error("original behavior fixture requires default rendering feature selection");
            }
        }
        const bool original_software_fixture=std::getenv("CARA_NATIVE_BEHAVIOR_SOFTWARE")&&
            std::string(std::getenv("CARA_NATIVE_BEHAVIOR_SOFTWARE"))=="1";
        const bool original_software_indexed=std::getenv("CARA_NATIVE_BEHAVIOR_INDEXED")&&
            std::string(std::getenv("CARA_NATIVE_BEHAVIOR_INDEXED"))=="1";
        if(original_software_indexed&&!original_software_fixture)
            throw std::runtime_error("indexed fixture requires software fixture");
        if(original_software_fixture) {
            if(!original_behavior_fixture||original_required_copy_fixture)
                throw std::runtime_error("software fixture requires original one-draw bootstrap mode");
            const char* queue=std::getenv("CARA_GL_COMMAND_QUEUE");
            if(queue&&std::string(queue)=="1")throw std::runtime_error("software fixture requires queue OFF");
        }
        bool fullscreen = false, relay_requested = false;
        for (int i=2; i<argc; ++i) {
            const std::string option(argv[i]);
            if (option=="--fullscreen") fullscreen=true;
            else if (option=="--relay-stdin") relay_requested=true;
            else throw std::runtime_error("Unknown native host option");
        }
        Network::RelayClient::Config relay_config;
        if (relay_requested) {
            if (isatty(STDIN_FILENO))
                throw std::runtime_error("Relay configuration requires a private stdin pipe");
            std::string input;
            char ch;
            while (std::cin.get(ch) && ch!='\n') {
                if (input.size()>=16384) throw std::runtime_error("Invalid relay configuration");
                input.push_back(ch);
            }
            if (!NativeMac::ParseRelayConfig(input, relay_config))
                throw std::runtime_error("Invalid relay configuration");
            input.assign(input.size(), '\0');
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
        (void)Cara::NativeDiagnosticClock::Get(); // validate explicit request before SDL/window/core load
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
        std::shared_ptr<Network::RelayClient> relay;
        if (relay_requested) relay=Network::RelayClient::Create();
        Network::InstallRelaySession(relay);
        SCOPE_EXIT({ if (relay) relay->Disconnect(); Network::InstallRelaySession(nullptr); });
        EmuWindow_SDL2 window(fullscreen);
        if (!NativeMac::DeveloperCommands::One(std::getenv("CARA_NATIVE_DIAGNOSTICS")))
            Cara::NativeGameMenu::Install();
        window.MakeCurrent();
        const bool native_gl_audit=std::getenv("CARA_NATIVE_GL_ERROR_AUDIT") &&
            std::string(std::getenv("CARA_NATIVE_GL_ERROR_AUDIT"))=="1" &&
            std::getenv("CARA_NATIVE_DIAGNOSTICS") &&
            std::string(std::getenv("CARA_NATIVE_DIAGNOSTICS"))=="1";
        if(native_gl_audit) {
            if(!glad_glGetError)throw std::runtime_error("GL audit missing driver entry point");
            native_original_get_error=glad_glGetError;native_first_gl_error.store(GL_NO_ERROR);
            glad_glGetError=NativeAuditedGetError;
        }
        SCOPE_EXIT({if(native_gl_audit)glad_glGetError=native_original_get_error;});
        if(std::getenv("CARA_NATIVE_GL_AUDIT_SELF_TEST") &&
           std::string(std::getenv("CARA_NATIVE_GL_AUDIT_SELF_TEST"))=="1") {
            if(!native_gl_audit)throw std::runtime_error("GL audit self-test requires installed audit");
            window.StartCommandQueue();
            SCOPE_EXIT({window.StopCommandQueue();});
            const auto before=glGetError();
            glEnable(GLenum(0xffffffffu)); // Deliberate invalid enum in this test-only exit path.
            const auto consumed=glGetError();
            const auto pending=glGetError();
            const auto first=native_first_gl_error.load();
            const bool passed=before==GL_NO_ERROR && consumed==GL_INVALID_ENUM &&
                pending==GL_NO_ERROR && first==GL_INVALID_ENUM && NativeAuditHookConnected();
            std::ofstream report(directory+"native-gl-audit-self-test.json");
            report << "{\"passed\":" << (passed?"true":"false") << ",\"consumed_error\":" << consumed
                   << ",\"pending_error\":" << pending << ",\"first_observed_error\":" << first
                   << ",\"queue_enabled\":" << (OpenGL::CommandQueue::Enabled()?"true":"false")
                   << ",\"guest_instructions_executed\":false,\"source_fingerprint\":"
                   << NativeOracleJSONQuote(Core::GetSaveStateBuildFingerprint()) << "}\n";
            report.flush();return passed && report?0:4;
        }
        // Release GPU/core resources while the owning window and GL context are alive,
        // including failure paths from checked framebuffer copies.
        window.StartCommandQueue();
        bool core_closed=false,shutdown_failed=false,core_load_started=false;
        const auto close_core=[&]() noexcept {
            if(core_closed)return;core_closed=true;
            try{if(relay)relay->Disconnect();}catch(...){shutdown_failed=true;}
            if(!window.PrepareForCoreShutdown())shutdown_failed=true;
            // Drain/join/unhook and reacquire the context before resource teardown.
            if(!window.StopCommandQueue())shutdown_failed=true;
            if(Cara::NativeProfile::MustExitWithoutGLDestructors(core_load_started,window.IsOwnedContextCurrent())) {
                try{std::ofstream fatal(directory+"native-fatal-context-loss.json");fatal<<"{\"fatal\":true,\"core_load_started\":true,\"owned_context_current\":false,\"gl_destructors_skipped\":true,\"exit_code\":70}\n";fatal.flush();}catch(...){}
                std::fputs("Fatal native GL context loss after Core load; skipping unsafe GL destructors\n",stderr);
                std::fflush(stderr);
                std::_Exit(70);
            }
            try{system.Shutdown();}catch(...){shutdown_failed=true;}
            try{std::ofstream status(directory+"native-shutdown-status.json");status<<"{\"core_closed\":true,\"powered_on\":"<<(system.IsPoweredOn()?"true":"false")<<",\"shutdown_failed\":"<<(shutdown_failed?"true":"false")<<"}\n";}catch(...){}
        };
        SCOPE_EXIT({close_core();});
        const bool native_query_census=std::getenv("CARA_NATIVE_GL_QUERY_CENSUS") &&
            std::string(std::getenv("CARA_NATIVE_GL_QUERY_CENSUS"))=="1";
        if(native_query_census) {
            const bool cpu=std::getenv("CARA_NATIVE_GL_QUERY_CENSUS_CPU") &&
                std::string(std::getenv("CARA_NATIVE_GL_QUERY_CENSUS_CPU"))=="1";
            Cara::NativeGLQueryCensus::Install(cpu);
        }
        // Preserve the census report/uninstall boundary before any core teardown.
        bool query_census_closed=false;
        const auto finish_query_census=[&] {
            if(native_query_census&&!query_census_closed) {
                query_census_closed=true;
                Cara::NativeGLQueryCensus::WriteReport(directory+"native-gl-query-census.json",
                    Core::GetSaveStateBuildFingerprint());
                Cara::NativeGLQueryCensus::Uninstall();
            }
        };
        SCOPE_EXIT({finish_query_census();});
        // LIFO: stop worker while audit/census hooks and all core owners still live.
        SCOPE_EXIT({if(!window.StopCommandQueue())shutdown_failed=true;});
        core_load_started=true;
        const auto loaded = system.Load(window, argv[1]);
        std::cout << "Native load result: " << static_cast<unsigned>(loaded) << std::endl;
        if (loaded != Core::System::ResultStatus::Success)
            return 3;
        if(controller_fixture) {
            const auto checks=Cara::ControllerPlayTest::Run(system);
            std::ofstream report(directory+"native-controller-play-test.json");
            report<<"{\"passed\":true,\"checks\":"<<checks<<",\"cpu_jit_active\":"<<(Settings::values.use_cpu_jit?"true":"false")<<",\"actual_live_hid\":true,\"same_running_core_rebind\":true,\"arrows\":true,\"cpu_objects_and_four_timer_scales_unchanged\":true,\"authored_guest_run\":true,\"commercial_game_loaded\":false}\n";
            report.flush();finish_query_census();close_core();return report&&!shutdown_failed?0:4;
        }
#ifdef CARA_MAC_CPU_JIT
        if(cpu_adapter_fixture) {
            if(!Settings::values.use_cpu_jit || Settings::values.use_shader_jit ||
               Cara::NativeDiagnosticClock::EffectivePercent(system.CoreTiming())!=100)
                throw std::runtime_error("CPU test requires JIT/100% clock and shader JIT off");
            for(unsigned i=0;i<4;++i)if(!dynamic_cast<ARM_Dynarmic*>(&system.GetCore(i)))
                throw std::runtime_error("CPU factory did not select Dynarmic");
            const auto* queue=std::getenv("CARA_GL_COMMAND_QUEUE");
            if(queue && std::string(queue)=="1")throw std::runtime_error("CPU fixture requires queue off");
            auto test=Cara::CPUJitTest::RunActualAdapterTest(system);
            Cara::CPUJitTest::RunFactorySystemArchiveTest(system,test);
            const bool passed=test.adapter_state&&test.mmio&&test.monitor&&test.full_system_archive;
            std::ofstream report(directory+"native-cpu-adapter-test.json");
            report<<"{\"passed\":"<<(passed?"true":"false")<<",\"checks\":"<<test.checks
                  <<",\"full_system_archive\":"<<(test.full_system_archive?"true":"false")
                  <<",\"game_guest_run\":false,\"cpu_backend\":2,\"shader_jit\":false,\"fingerprint\":"
                  <<NativeOracleJSONQuote(Core::GetSaveStateBuildFingerprint())<<"}\n";
            report.flush();finish_query_census();close_core();
            return passed&&report&&!shutdown_failed?0:4;
        }
#endif
        if(original_behavior_fixture) {
            if(Cara::NativeDiagnosticClock::EffectivePercent(system.CoreTiming())!=100)
                throw std::runtime_error("original behavior fixture requires 100 percent guest CPU clock");
            const auto fixture_stem=directory+(required_copy_integration ? "native-unchecked-blit-integration" : original_required_copy_fixture ? "native-original-required-copy" :
                original_software_fixture ? (original_software_indexed ? "native-original-software-indexed" : "native-original-software-unindexed") : "native-original-behavior");
            WriteNativeProfileSnapshot(fixture_stem+"-origin",VideoCore::GetCurrentFrame());
            auto fixture=required_copy_integration ? Cara::NativeBehavior::RunRequiredCopyIntegration(system) : original_required_copy_fixture ? Cara::NativeBehavior::RunRequiredCopy(system)
                                                       : Cara::NativeBehavior::RunOneDraw(system,original_software_fixture,original_software_indexed);
            if(native_gl_audit&&(native_first_gl_error.load()!=GL_NO_ERROR||!NativeAuditHookConnected())) {
                fixture.passed=false;fixture.error+="; GL audit observed error or lost hook";
            }
            const bool fixture_system_live=system.IsPoweredOn();
            if(!fixture_system_live){fixture.passed=false;fixture.error+="; System closed during fixture: after-profile unavailable";}
            std::ofstream report(fixture_stem+".json");
            report << "{\"passed\":" << (fixture.passed?"true":"false") << ",\"error\":" << NativeOracleJSONQuote(fixture.error)
                   << ",\"source_fingerprint\":" << NativeOracleJSONQuote(Core::GetSaveStateBuildFingerprint())
                   << ",\"after_profile_available\":" << (fixture_system_live?"true":"false")
                   << ",\"scope\":\"authored bootstrap and one actual-engine host batch only\",\"detail\":" << fixture.detail_json << "}\n";
            report.flush();if(fixture_system_live)WriteNativeProfileSnapshot(fixture_stem+"-after",VideoCore::GetCurrentFrame());
            finish_query_census();close_core();
            return fixture.passed&&report&&!shutdown_failed?0:4;
        }
        for (const auto* oracle : {"CARA_SW_BATCH_VISIBILITY_TEST", "CARA_LUT_EFFECTIVE_CHANGE_TEST", "CARA_NATIVE_REQUIRED_COPY_TEST"}) {
            const auto* flag=std::getenv(oracle);
            if (!flag || std::string(flag)!="1") continue;
            auto* rasterizer=dynamic_cast<OpenGL::RasterizerOpenGL*>(VideoCore::Rasterizer());
            std::string error;
            const bool visibility=std::string(oracle)=="CARA_SW_BATCH_VISIBILITY_TEST";
            const bool required_copy=std::string(oracle)=="CARA_NATIVE_REQUIRED_COPY_TEST";
            bool passed=rasterizer && (visibility ? rasterizer->RunSoftwareBatchVisibilitySelfTest(error)
                : required_copy ? rasterizer->RunNativeRequiredCopySelfTest(error)
                                : rasterizer->RunLUTEffectiveChangeSelfTest(error));
            if(native_gl_audit && (native_first_gl_error.load()!=GL_NO_ERROR || !NativeAuditHookConnected())) {
                passed=false;error+="; GL audit error or lost driver hook "+std::to_string(native_first_gl_error.load());
            }
            const auto stem=directory+(visibility ? "native-software-batch-visibility-oracle" :
                required_copy ? "native-required-copy-oracle" : "native-lut-effective-change-oracle");
            std::ofstream report(stem+".json");
            report << "{\"passed\":" << (passed?"true":"false")
                   << ",\"error\":" << NativeOracleJSONQuote(error)
                   << ",\"source_fingerprint\":" << NativeOracleJSONQuote(Core::GetSaveStateBuildFingerprint())
                   << ",\"guest_instructions_executed\":false"
                   << ",\"native_copytex_surface_copy_calls\":" << (rasterizer?rasterizer->NativeCopyTexSurfaceCopyCalls():0)
                   << ",\"native_shader_surface_copy_calls\":" << (rasterizer?rasterizer->NativeShaderSurfaceCopyCalls():0)
                   << ",\"software_batching\":";
            OpenGL::SoftwareBatching::WriteJSON(report);report << "}\n";report.flush();
            WriteNativeProfileSnapshot(stem,VideoCore::GetCurrentFrame());
            finish_query_census();close_core();
            return passed && report && !shutdown_failed ? 0 : 4;
        }
        const bool diagnostics = std::getenv("CARA_NATIVE_DIAGNOSTICS") &&
                                 std::string(std::getenv("CARA_NATIVE_DIAGNOSTICS")) == "1";
        const auto record_state = [&](const char* operation, unsigned slot = 1) {
            std::ofstream state(directory + "native-state-status.json");
            state << "{\"operation\":\"" << operation
                  << "\",\"slot\":" << slot << ",\"frame\":" << VideoCore::GetCurrentFrame()
                  << ",\"fingerprint\":\"" << Core::GetSaveStateBuildFingerprint()
                  << "\",\"ticks\":" << system.CoreTiming().GetTicks()
                  << ",\"guest_cpu_percent\":" << Cara::NativeDiagnosticClock::EffectivePercent(system.CoreTiming())
                  << ",\"diagnostic_clock\":";
            Cara::NativeDiagnosticClock::WriteJSON(state,system.CoreTiming(),system.GetNumCores());
            state << "}\n";
        };
        if (const char* load = std::getenv("CARA_NATIVE_LOAD_SLOT");
            diagnostics && load && std::string(load) == "1") {
            if (relay) relay->Disconnect();
            system.LoadState(1);
            Cara::NativeDiagnosticClock::ApplyAfterLoad(system.CoreTiming(),system.GetNumCores(),
                Cara::NativeScriptOnlyInput::Enabled(),"startup-state-load");
            record_state("load");
        } else {
            Cara::NativeDiagnosticClock::ApplyAfterLoad(system.CoreTiming(),system.GetNumCores(),
                Cara::NativeScriptOnlyInput::Enabled(),"startup-normal-load");
        }
        const char* copy_trace_flag = std::getenv("CARA_TEXTURE_COPY_TRACE");
        const bool copy_trace_requested = diagnostics && copy_trace_flag &&
                                          std::string(copy_trace_flag) == "1";
        const auto flush_copy_trace = [&](bool force) {
            if (!copy_trace_requested) return;
            Cara::TextureCopyTrace::Flush(force);
            if (Cara::TextureCopyTrace::TakeExportError()) {
                std::ofstream error(directory + "native-texture-copy-trace-error.json");
                error << "{\"export_failed\":true,\"forced\":" << (force ? "true" : "false") << "}\n";
                throw std::runtime_error("Native TextureCopy trace export failed");
            }
        };
        // Configure only on this owner thread, after state load and with no live GPU hooks.
        if (copy_trace_requested) {
            Cara::TextureCopyTrace::Arm(directory + "native-texture-copy-trace.json");
            std::ofstream config(directory + "native-texture-copy-trace-config.json");
            config << "{\"enabled\":true,\"start_frame\":" << Cara::TextureCopyTrace::start_frame
                   << ",\"duration\":" << Cara::TextureCopyTrace::duration
                   << ",\"path\":\"native-texture-copy-trace.json\",\"frame_origin\":\"vblank_since_arm\"}\n";
            if (!config) throw std::runtime_error("Native TextureCopy trace config export failed");
        }
        SCOPE_EXIT({
            try { flush_copy_trace(true); }
            catch (const std::exception& error) {
                std::cerr << "Native TextureCopy shutdown export failed: " << error.what() << std::endl;
            }
        });
        const char* trace_flag = std::getenv("CARA_GPU_TRACE");
        const bool gpu_trace_requested = diagnostics && trace_flag && std::string(trace_flag) == "1";
        const auto flush_gpu_trace = [&](bool force) {
            if (!gpu_trace_requested)
                return;
            Cara::GuestGPUTrace::Flush(force);
            if (Cara::GuestGPUTrace::TakeExportError()) {
                std::ofstream error(directory + "native-gpu-trace-error.json");
                error << "{\"export_failed\":true,\"forced\":" << (force ? "true" : "false")
                      << ",\"frame\":" << VideoCore::GetCurrentFrame() << "}\n";
                throw std::runtime_error("Native guest GPU trace export failed");
            }
        };
        // State restoration precedes ownership/arming; trace data is never restored guest state.
        if (gpu_trace_requested)
            Cara::GuestGPUTrace::Arm(directory + "native-gpu-trace.csv");
        // Exception paths must force export before core/window teardown. Never throw
        // from a scope-exit destructor; ordinary exits call the checked helper below.
        SCOPE_EXIT({
            if (gpu_trace_requested) {
                try {
                    flush_gpu_trace(true);
                } catch (const std::exception& error) {
                    std::cerr << "Native trace shutdown export failed: " << error.what() << std::endl;
                }
            }
        });
        if (relay && !relay->Connect(std::move(relay_config)))
            throw std::runtime_error("Relay connection could not start");
        auto relay_state = Network::RelayClient::State::Idle;
        u64 relay_status_frame = ~u64{0};
        if (const char* diagnostic = std::getenv("CARA_NATIVE_DIAGNOSTICS");
            diagnostic && std::string(diagnostic) == "1")
            Common::FrameTiming::counters.enabled =
                !std::getenv("CARA_NATIVE_TIMING_COUNTERS") ||
                std::string(std::getenv("CARA_NATIVE_TIMING_COUNTERS")) != "0";
        WriteNativeManifest(directory);
        WriteNativeProfileSnapshot(directory + "native-startup", VideoCore::GetCurrentFrame());
        std::unique_ptr<NativeMac::CPUCategoryProfiler> category_profile;
        if (NativeMac::CategoryFlag("CARA_NATIVE_CPU_CATEGORY_PROFILE"))
            category_profile = std::make_unique<NativeMac::CPUCategoryProfiler>(
                directory, Core::GetSaveStateBuildFingerprint(), VideoCore::GetCurrentFrame());
        std::signal(SIGTERM, StopNative);
        std::signal(SIGINT, StopNative);
        if (const char* benchmark = std::getenv("CARA_NATIVE_BENCHMARK");
            diagnostics && benchmark && std::string(benchmark) == "1")
            window.BeginBenchmark();
        const auto* cadence_option = std::getenv("CARA_NATIVE_REFRESH_CADENCE");
        const bool capture_cadence = diagnostics && cadence_option &&
                                     std::strcmp(cadence_option, "1") == 0;
        std::vector<std::pair<u32, double>> refresh_intervals;
        constexpr std::size_t max_refresh_intervals = 100000;
        if (capture_cadence)
            refresh_intervals.reserve(max_refresh_intervals);
        auto last_refresh = VideoCore::GetCurrentFrame();
        auto last_refresh_at = std::chrono::steady_clock::now();
        while (window.IsOpen() && !stop_requested) {
            if (relay) {
                const auto frame=VideoCore::GetCurrentFrame();
                if (frame!=relay_status_frame) {
                    relay_status_frame=frame;
                    const auto status=relay->GetStatus();
                    if (status.state!=relay_state) {
                        std::cout << "Local multiplayer: " << Network::RelayClient::StatusText(status) << std::endl;
                        relay_state=status.state;
                    }
                }
                Network::DrainRelayPackets(system);
            }
            const auto result = system.RunLoop();
            if (capture_cadence) {
                const auto frame = VideoCore::GetCurrentFrame();
                if (frame != last_refresh) {
                    if (refresh_intervals.size() == max_refresh_intervals)
                        throw std::runtime_error("Native refresh cadence capacity exceeded");
                    const auto now = std::chrono::steady_clock::now();
                    const auto elapsed = std::chrono::duration<double, std::milli>(now - last_refresh_at);
                    refresh_intervals.emplace_back(frame, elapsed.count());
                    last_refresh = frame;
                    last_refresh_at = now;
                }
            }
            flush_copy_trace(false);
            // Export only after RunLoop and nested GPU/kernel hooks have returned.
            if (gpu_trace_requested)
                flush_gpu_trace(false);
            if (result == Core::System::ResultStatus::ShutdownRequested)
                break;
            if (result != Core::System::ResultStatus::Success) {
                std::cerr << "Run failure: " << static_cast<unsigned>(result) << std::endl;
                flush_copy_trace(true);
                flush_gpu_trace(true);
                return 4;
            }
            // This explicit attribution mode samples only after RunLoop and all
            // nested timing scopes have returned. It never injects input.
            if (category_profile && category_profile->Observe(VideoCore::GetCurrentFrame(),
                    system.CoreTiming().GetTicks(), system.CoreTiming().GetGlobalTicks()))
                break;
            // PollEvents only queues requests. Serialize here, after RunLoop and
            // its timing callbacks return/requeue, with the GL context still live.
            auto state_command = window.TakeStateCommand();
            const auto menu_state = Cara::NativeGameMenu::TakeStateRequest();
            const unsigned state_slot = menu_state.action == Cara::NativeGameMenu::StateAction::None ? 1 : menu_state.slot;
            if (menu_state.action != Cara::NativeGameMenu::StateAction::None)
                state_command = menu_state.action == Cara::NativeGameMenu::StateAction::Save ? EmuWindow_SDL2::StateCommand::Save : EmuWindow_SDL2::StateCommand::Load;
            if (category_profile && state_command != EmuWindow_SDL2::StateCommand::None)
                throw std::runtime_error("Save/load during native category measurement is unsupported");
            try {
                switch (state_command) {
                case EmuWindow_SDL2::StateCommand::Save:
                    system.SaveState(state_slot);
                    record_state("save", state_slot);
                    if (menu_state.action != Cara::NativeGameMenu::StateAction::None) window.StateCompleted(true, state_slot);
                    WriteNativeProfileSnapshot(directory + "native-saved-state", VideoCore::GetCurrentFrame());
                    break;
                case EmuWindow_SDL2::StateCommand::Load:
                    if (relay) relay->Disconnect();
                    flush_copy_trace(true);
                    flush_gpu_trace(true);
                    system.LoadState(state_slot);
                    Cara::NativeDiagnosticClock::ApplyAfterLoad(system.CoreTiming(),system.GetNumCores(),
                        Cara::NativeScriptOnlyInput::Enabled(),"runtime-state-load");
                    record_state("load", state_slot);
                    if (menu_state.action != Cara::NativeGameMenu::StateAction::None) window.StateCompleted(false, state_slot);
                    if (copy_trace_requested)
                        Cara::TextureCopyTrace::Arm(directory + "native-texture-copy-trace.json");
                    if (gpu_trace_requested)
                        Cara::GuestGPUTrace::Arm(directory + "native-gpu-trace.csv");
                    break;
                case EmuWindow_SDL2::StateCommand::None:
                    break;
                }
            } catch (const std::exception& error) {
                if (menu_state.action == Cara::NativeGameMenu::StateAction::None) throw;
                Cara::NativeGameMenu::StateFailure(error.what());
                if (!system.IsPoweredOn()) break;
            }
            if (window.TakeBenchmarkStop())
                break;
        }
        if (capture_cadence) {
            // Match the Metal runner's RunLoop boundaries. Write after sampling.
            std::ofstream cadence(directory + "native-gl-refresh-intervals.csv");
            cadence.precision(17);
            cadence << "refresh,wall_ms\n";
            for (const auto& [refresh, milliseconds] : refresh_intervals)
                cadence << refresh << ',' << milliseconds << '\n';
            cadence.flush();
            if (!cadence)
                throw std::runtime_error("Could not write native refresh cadence");
        }
        // Explicit test boundary, after the measured run and before GL teardown.
        // Normal play performs no additional queries. Nonzero errors fail closed.
        if (diagnostics && std::getenv("CARA_NATIVE_END_GL_ERROR_CHECK") &&
            std::string(std::getenv("CARA_NATIVE_END_GL_ERROR_CHECK"))=="1") {
            const auto error=glGetError();
            const auto first=native_gl_audit?native_first_gl_error.load():GL_NO_ERROR;
            const bool hook_connected=!native_gl_audit || NativeAuditHookConnected();
            const bool passed=error==GL_NO_ERROR && first==GL_NO_ERROR && hook_connected;
            std::ofstream report(directory+"native-end-gl-error.json");
            report << "{\"passed\":" << (passed?"true":"false")
                   << ",\"error\":" << error << ",\"audited_existing_queries\":" << (native_gl_audit?"true":"false")
                   << ",\"first_observed_error\":" << first << ",\"hook_connected\":" << (hook_connected?"true":"false") << ",\"source_fingerprint\":"
                   << NativeOracleJSONQuote(Core::GetSaveStateBuildFingerprint()) << "}\n";
            report.flush();
            if (!report || !passed)
                throw std::runtime_error("Native final GL error check failed");
        }
        flush_copy_trace(true);
        flush_gpu_trace(true);
        finish_query_census();
        close_core();
        return shutdown_failed?2:0;
    } catch (const std::exception& error) {
        std::cerr << "Native host failed: " << error.what() << std::endl;
        return 2;
    }
}

// Replacing the process happens only after the Core, queue, window and all
// RunNativeMain scope guards have completed their ordinary shutdown.
int main(int argc, char** argv) {
    // Avoid worker-to-AppKit synchronous dispatch while main waits on the render queue.
    // An explicit launcher override survives; guest timing and GL work are unchanged.
    setenv("SDL_MAC_OPENGL_ASYNC_DISPATCH", "1", 0);
    // A normal launch is a chooser, not a hardcoded game or automatic restore.
    const bool ui_test=argc>1 && std::string(argv[1])=="--mac-ui-self-test";
    std::string chosen;
    char* startup_args[]={argv[0],nullptr,nullptr};
    if (argc<2 || ui_test) {
        if (!std::getenv("CARA_USER_DIR")) {
            const char* home=std::getenv("HOME");
            if(!home)return 2;
            const auto user=std::string(home)+"/Library/Application Support/Cara";
            setenv("CARA_USER_DIR",user.c_str(),0);
        }
        try {
            std::string user=std::getenv("CARA_USER_DIR");
            if(user.empty()||user.front()!='/')throw std::runtime_error("Invalid Cara data directory");
            if(user.back()!='/')user+='/';FileUtil::SetUserPath(user);
            NativeCPUDefaults();Config startup_config;
            if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_JOYSTICK|SDL_INIT_GAMECONTROLLER)!=0)
                throw std::runtime_error(SDL_GetError());
            InputCommon::Init();
            if(ui_test){const int code=argc==3?Cara::MacUI::SelfTest(argv[2]):2;InputCommon::Shutdown();SDL_Quit();return code;}
            chosen=Cara::MacUI::Welcome();
            if(chosen.empty()){InputCommon::Shutdown();SDL_Quit();return 0;}
            startup_args[1]=chosen.data();argc=2;argv=startup_args;
        } catch(const std::exception& e){InputCommon::Shutdown();SDL_Quit();std::cerr<<"Cara startup failed: "<<e.what()<<'\n';return 2;}
    }
    const int result = RunNativeMain(argc, argv);
    if (result != 0)
        return result;
    std::string next_rom = Cara::NativeGameMenu::TakeRequestedROM();
    if (next_rom.empty())
        return 0;
    bool fullscreen = false;
    for (int i = 2; i < argc; ++i)
        fullscreen |= std::string(argv[i]) == "--fullscreen";
    char fullscreen_arg[] = "--fullscreen";
    char* next_argv[] = {argv[0], next_rom.data(), fullscreen ? fullscreen_arg : nullptr, nullptr};
    execv(argv[0], next_argv);
    std::cerr << "Could not open selected game: " << std::strerror(errno) << std::endl;
    return 2;
}
