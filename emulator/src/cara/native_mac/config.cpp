// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdlib>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <SDL.h>
#include <inih/cpp/INIReader.h>
#include "cara/config.h"
#include "cara/default_ini.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/param_package.h"
#include "core/frontend/mic.h"
#include "core/hle/service/service.h"
#include "core/settings.h"
#include "input_common/main.h"
#include "input_common/udp/client.h"

Config::Config() {
    // TODO: Don't hardcode the path; let the frontend decide where to put the config files.
    sdl2_config_loc = FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "sdl2-config.ini";
    sdl2_config = std::make_unique<INIReader>(sdl2_config_loc);

    Reload();
}

Config::~Config() = default;

bool Config::LoadINI(const std::string& default_contents, bool retry) {
    const std::string& location = this->sdl2_config_loc;
    if (sdl2_config->ParseError() < 0) {
        if (retry) {
            LOG_WARNING(Config, "Failed to load {}. Creating file from defaults...", location);
            FileUtil::CreateFullPath(location);
            FileUtil::WriteStringToFile(true, location, default_contents);
            sdl2_config = std::make_unique<INIReader>(location); // Reopen file

            return LoadINI(default_contents, false);
        }
        LOG_ERROR(Config, "Failed.");
        return false;
    }
    LOG_INFO(Config, "Successfully loaded {}", location);
    return true;
}

static const std::array<int, Settings::NativeButton::NumButtons> default_buttons = {
    SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_T, SDL_SCANCODE_G,
    SDL_SCANCODE_F, SDL_SCANCODE_H, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_M, SDL_SCANCODE_N,
    SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_B,
};

static const std::array<std::array<int, 5>, Settings::NativeAnalog::NumAnalogs> default_analogs{{
    {
        SDL_SCANCODE_UP,
        SDL_SCANCODE_DOWN,
        SDL_SCANCODE_LEFT,
        SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_D,
    },
    {
        SDL_SCANCODE_I,
        SDL_SCANCODE_K,
        SDL_SCANCODE_J,
        SDL_SCANCODE_L,
        SDL_SCANCODE_D,
    },
}};

void Config::ReadValues() {
    // Controls
    // TODO: add multiple input profile support
    for (int i = 0; i < Settings::NativeButton::NumButtons; ++i) {
        std::string default_param = InputCommon::GenerateKeyboardParam(default_buttons[i]);
        Settings::values.current_input_profile.buttons[i] = sdl2_config->GetString(
            "Controls",
            i < int(Settings::NativeButton::mapping.size()) ? Settings::NativeButton::mapping[i]
                                                            : "button_power",
            default_param);
        if (Settings::values.current_input_profile.buttons[i].empty())
            Settings::values.current_input_profile.buttons[i] = default_param;
    }

    for (int i = 0; i < Settings::NativeAnalog::NumAnalogs; ++i) {
        std::string default_param = InputCommon::GenerateAnalogParamFromKeys(
            default_analogs[i][0], default_analogs[i][1], default_analogs[i][2],
            default_analogs[i][3], default_analogs[i][4], 0.5f);
        Settings::values.current_input_profile.analogs[i] =
            sdl2_config->GetString("Controls", Settings::NativeAnalog::mapping[i], default_param);
        if (Settings::values.current_input_profile.analogs[i].empty())
            Settings::values.current_input_profile.analogs[i] = default_param;
    }

    Settings::values.current_input_profile.motion_device = sdl2_config->GetString(
        "Controls", "motion_device",
        "engine:motion_emu,update_period:100,sensitivity:0.01,tilt_clamp:90.0");
    Settings::values.current_input_profile.touch_device =
        sdl2_config->GetString("Controls", "touch_device", "engine:emu_window");
    Settings::values.current_input_profile.udp_input_address = sdl2_config->GetString(
        "Controls", "udp_input_address", InputCommon::CemuhookUDP::DEFAULT_ADDR);
    Settings::values.current_input_profile.udp_input_port =
        static_cast<u16>(sdl2_config->GetInteger("Controls", "udp_input_port",
                                                 InputCommon::CemuhookUDP::DEFAULT_PORT));

    auto& v = Settings::values;
    v.use_cpu_jit = false;
    v.use_shader_jit = false;
    if (sdl2_config->GetInteger("Core", "cpu_clock_percentage", 100) != 100)
        throw std::runtime_error("Native developer host requires 100% guest clock");
    v.use_gles = false;
    v.use_hw_renderer = sdl2_config->GetBoolean("Renderer", "use_hw_renderer", false);
    v.use_hw_shader = sdl2_config->GetBoolean("Renderer", "use_hw_shader", false);
    v.resolution_factor = 1;
    v.use_frame_limit = true;
    v.frame_limit = 100;
    v.vsync_enabled = true;
    v.use_present_thread = false;
    v.factor_3d = 0;
    const long accurate_mul = sdl2_config->GetInteger("Renderer", "shaders_accurate_mul", 2);
    if (accurate_mul < 0 || accurate_mul > 2)
        throw std::runtime_error("shaders_accurate_mul must be 0,1,2");
    v.shaders_accurate_mul = static_cast<Settings::AccurateMul>(accurate_mul);
    v.layout_option = Settings::LayoutOption::Default;
    v.swap_screen = false;
    v.custom_layout = false;
    v.custom_textures = false;
    v.use_shader_cache = false;
    v.use_virtual_sd = true;
    v.region_value = Settings::REGION_VALUE_AUTO_SELECT;
    const auto* diagnostic = std::getenv("CARA_NATIVE_DIAGNOSTICS");
    v.init_clock = diagnostic && std::string(diagnostic) == "1" ? Settings::InitClock::FixedTime
                                                                : Settings::InitClock::SystemTime;
    v.init_time = 946684801;
    v.shared_font_type = Settings::SharedFontType::Auto;
    v.sink_id = "sdl2";
    v.audio_device_id = "auto";
    v.audio_volume = 1.0f;
    v.enable_audio_stretching = false;
    v.enable_dsp_lle = false;
    v.mic_volume = 0.0f;
    v.log_filter = "*:Info";
    v.enable_telemetry = false;
    v.joystick_range = 1.0f;
    v.joystick_deadzone = 0.15f;
}

void Config::Reload() {
    LoadINI(DefaultINI::sdl2_config_file);
    ReadValues();
}
