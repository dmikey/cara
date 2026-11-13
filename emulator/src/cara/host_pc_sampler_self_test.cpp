// SPDX-License-Identifier: GPL-2.0-or-later
// Native macOS ARM64 own-process smoke. No simulator/device validation implied.
#include "host_pc_sampler.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

static bool Exercise(const std::string& path, bool time_limit) {
    std::atomic<unsigned long long> progress{0};
    std::atomic<bool> after_stop{false};
    std::atomic<bool> completed{false};
    bool written = false;
    std::thread target([&] {
        Cara::HostPcSampler::Options options;
        options.start_frame = 0; options.end_frame = 1;
        options.capacity = time_limit ? 6000 : 32;
        options.max_seconds = time_limit ? .08 : 3;
        Cara::HostPcSampler sampler(true, options);
        sampler.OnFrame(0);
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(time_limit ? 600 : 1800);
        while (std::chrono::steady_clock::now() < end) {
            const auto spin_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            while (std::chrono::steady_clock::now() < spin_end) progress.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        sampler.OnFrame(1);
        written = sampler.StopAndWrite(path, "native-own-process-smoke");
        after_stop.store(true);
        const auto before = progress.load();
        for (unsigned i = 0; i < 100000; ++i) progress.fetch_add(1, std::memory_order_relaxed);
        completed.store(progress.load() > before);
    });
    target.join();
    if (!written || !after_stop || !completed || progress == 0) return false;
    std::ifstream file(path);
    const std::string json((std::istreambuf_iterator<char>(file)), {});
    for (const char* key : {"info_failures", "suspend_failures", "state_failures", "resume_failures"}) {
        if (json.find(std::string("\"") + key + "\":0") == std::string::npos) return false;
    }
    if (json.find(time_limit ? "\"stop_reason\":\"time_limit\"" : "\"stop_reason\":\"capacity\"") == std::string::npos) return false;
    if (json.find("\"uuid\":\"") == std::string::npos || json.find("\"pc\":0") != std::string::npos) return false;
    std::size_t count = 0, position = 0;
    while ((position = json.find("\"before_ns\":", position)) != std::string::npos) { ++count; ++position; }
    if (time_limit ? (count == 0 || count > 12) : count != 32) return false;
    // THREAD_WAITING=3. Preserve natural waiting states; never call them CPU work.
    if (!time_limit && json.find("\"natural_run_state\":3") == std::string::npos) return false;
    std::cout << path << ": " << count << " samples; resumed, bounded, joined; progress after stop=" << completed << '\n';
    return true;
}
int main(int argc, char** argv) {
    if (!Cara::HostPcSampler::CompiledAvailable()) { std::cerr << "requires Apple ARM64\n"; return 77; }
    const std::string prefix = argc > 1 ? argv[1] : "/tmp/cara-host-pc-smoke";
    if (!Exercise(prefix + "-capacity.json", false) || !Exercise(prefix + "-time.json", true)) return 1;
    // Destruction without explicit export must still stop and join before exit.
    std::thread early([] {
        Cara::HostPcSampler::Options o; o.start_frame=0; o.end_frame=100;
        Cara::HostPcSampler sampler(true,o); sampler.OnFrame(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    });
    early.join();
    std::cout << "early destruction joined successfully\n";
}
