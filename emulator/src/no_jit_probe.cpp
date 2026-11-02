// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <memory>
#include <array>
#include <fstream>
#include <iostream>
#include "common/detached_tasks.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/input.h"
#include "core/settings.h"

class ScriptButton final : public Input::ButtonDevice {
    unsigned code;
    const unsigned& frame;
    const std::vector<unsigned>& presses;
public:
    ScriptButton(unsigned code_, const unsigned& frame_, const std::vector<unsigned>& presses_)
        : code(code_), frame(frame_), presses(presses_) {}
    bool GetStatus() const override {
        if (code != Settings::NativeButton::A) return false;
        return std::any_of(presses.begin(), presses.end(), [&](unsigned at) {
            return frame >= at && frame - at < 6;
        });
    }
};
class ScriptButtonFactory final : public Input::Factory<Input::ButtonDevice> {
    const unsigned& frame;
    const std::vector<unsigned>& presses;
public:
    ScriptButtonFactory(const unsigned& frame_, const std::vector<unsigned>& presses_) : frame(frame_), presses(presses_) {}
    std::unique_ptr<Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        return std::make_unique<ScriptButton>(unsigned(params.Get("code", 0)), frame, presses);
    }
};

class ProbeWindow final : public Frontend::EmuWindow {
public:
    unsigned frames = 0;
    std::string output;
    std::ofstream hashes;
    explicit ProbeWindow(std::string path, bool record_hashes) : output(std::move(path)) {
        if (record_hashes) hashes.open(output + "/frame-hashes.txt");
        UpdateFramebufferLayout(400, 480);
    }
    void PollEvents() override {}
    void MakeCurrent() override {}
    void DoneCurrent() override {}
    void SwapBuffers() override {}
    void PresentSoftwareFrame(unsigned screen, unsigned width, unsigned height,
                              const std::vector<u8>& rgba) override {
        if (screen == 0) ++frames;
        if (hashes.is_open()) {
            u64 hash = 14695981039346656037ULL;
            for (u8 byte : rgba) { hash ^= byte; hash *= 1099511628211ULL; }
            hashes << frames << ' ' << screen << ' ' << width << ' ' << height << ' ' << hash << '\n';
        }
        const char* burst_start_env = std::getenv("CARA_CAPTURE_BURST_START");
        const unsigned burst_start = burst_start_env ? unsigned(std::max(0L, std::strtol(burst_start_env, nullptr, 10))) : 0;
        const char* burst_count_env = std::getenv("CARA_CAPTURE_BURST_COUNT");
        const unsigned burst_count = burst_count_env ? unsigned(std::clamp(std::strtol(burst_count_env, nullptr, 10), 1L, 120L)) : 36;
        const bool burst = burst_start_env && frames >= burst_start && frames - burst_start < burst_count;
        if (frames == 1 || frames % 30 == 0 || burst) {
            const std::string filename = burst ? "/frame-" + std::to_string(frames) + "-screen-" + std::to_string(screen) + ".ppm"
                                               : "/screen-" + std::to_string(screen) + ".ppm";
            std::ofstream image(output + filename, std::ios::binary);
            image << "P6\n" << width << " " << height << "\n255\n";
            for (size_t i = 0; i < rgba.size(); i += 4)
                image.write(reinterpret_cast<const char*>(rgba.data() + i), 3);
            if (screen == 0) std::cout << "Presented frames: " << frames << std::endl;
        }
    }
};

static int SelfTest(const std::string& output) {
    FileUtil::CreateFullPath(output + "/");
    const std::string path = output + "/native-io-test.bin";
    FileUtil::IOFile file(path, "w+b");
    const std::array<u8, 4> initial{1, 2, 3, 4};
    if (!file.IsOpen() || file.WriteArray(initial.data(), initial.size()) != 4 ||
        !file.Flush() || file.Tell() != 4 || file.GetSize() != 4 || !file.Seek(1, SEEK_SET)) return 10;
    const u8 replacement = 9;
    if (file.WriteBytes(&replacement, 1) != 1 || !file.Flush() || !file.Resize(8) ||
        file.GetSize() != 8 || !file.Seek(0, SEEK_SET)) return 11;
    std::array<u8, 8> actual{};
    const std::array<u8, 8> expected{1, 9, 3, 4, 0, 0, 0, 0};
    if (file.ReadArray(actual.data(), actual.size()) != 8 || actual != expected ||
        file.Seek(-1, SEEK_SET)) return 12;
    file.Close();
    FileUtil::IOFile reopened(path, "rb");
    actual.fill(0);
    if (reopened.ReadArray(actual.data(), actual.size()) != 8 || actual != expected) return 13;
    reopened.Close();
    FileUtil::Delete(path);
    std::cout << "Native I/O: write, flush, tell, seek, resize, read and reopen passed" << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: citra-no-jit-probe ROM OUTPUT_DIRECTORY [seconds] [target_frames]\n";
        return 2;
    }
    if (std::string(argv[1]) == "--self-test") return SelfTest(argv[2]);
    Common::DetachedTasks tasks;
    const std::string output = argv[2];
    FileUtil::CreateFullPath(output + "/");
    FileUtil::SetUserPath(output + "/userdata/");
    Log::SetGlobalFilter(Log::Filter(Log::Level::Info));
    Log::AddBackend(std::make_unique<Log::ColorConsoleBackend>());
    Log::AddBackend(std::make_unique<Log::FileBackend>(output + "/emulator.log"));
    auto& s = Settings::values;
    s.use_cpu_jit = false;
    s.use_shader_jit = false;
    s.use_hw_renderer = false;
    s.use_hw_shader = false;
    s.use_virtual_sd = true;
    s.region_value = Settings::REGION_VALUE_AUTO_SELECT;
    s.resolution_factor = 1;
    s.frame_limit = 100;
    const unsigned target_frames = argc > 4 ? std::stoul(argv[4]) : 0;
    s.init_clock = target_frames ? Settings::InitClock::FixedTime : Settings::InitClock::SystemTime;
    s.init_time = 946684801;
    s.sink_id = "null";
    s.audio_volume = 1.0f;
    s.shared_font_type = Settings::SharedFontType::Auto;
    Settings::Apply();
    Frontend::RegisterDefaultApplets();
    ProbeWindow window(output, target_frames != 0);
    std::vector<unsigned> scripted_presses;
    if (const char* value = target_frames ? std::getenv("CARA_TEST_PRESS_A_FRAMES") : nullptr) {
        std::istringstream stream(value);
        std::string item;
        while (std::getline(stream, item, ',')) {
            const long at = std::strtol(item.c_str(), nullptr, 10);
            if (at > 0 && at < target_frames) scripted_presses.push_back(unsigned(at));
        }
    }
    if (!scripted_presses.empty()) {
        Input::RegisterFactory<Input::ButtonDevice>("probe-script", std::make_shared<ScriptButtonFactory>(window.frames, scripted_presses));
        for (unsigned button = 0; button < Settings::NativeButton::NumButtons; ++button)
            s.current_input_profile.buttons[button] = "engine:probe-script,code:" + std::to_string(button);
    }
    auto& system = Core::System::GetInstance();
    const auto loaded = system.Load(window, argv[1]);
    std::cout << "Load result: " << static_cast<unsigned>(loaded) << std::endl;
    if (loaded != Core::System::ResultStatus::Success) return 3;
    const auto start = std::chrono::steady_clock::now();
    const auto duration = std::chrono::seconds(argc > 3 ? std::stoi(argv[3]) : 30);
    unsigned loops = 0;
    while (std::chrono::steady_clock::now() - start < duration &&
           (!target_frames || window.frames < target_frames)) {
        const auto result = system.RunLoop();
        ++loops;
        if (result != Core::System::ResultStatus::Success) {
            std::cerr << "Run result: " << static_cast<unsigned>(result) << std::endl;
            system.Shutdown();
            return 4;
        }
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "Run loops: " << loops << "; LCD frames: " << window.frames
              << "; elapsed: " << elapsed << "; FPS: " << window.frames / elapsed << std::endl;
    system.Shutdown();
    return window.frames ? 0 : 5;
}
