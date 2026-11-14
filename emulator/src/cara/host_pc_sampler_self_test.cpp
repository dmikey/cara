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

// Read scalar fields from this sampler's compact JSON writer. This is not a
// general JSON parser; retain the complete report for independent analysis.
static std::string Field(const std::string& json, const std::string& key) {
    const auto position = json.find("\"" + key + "\":");
    if (position == std::string::npos) return "<missing>";
    const auto begin = position + key.size() + 3;
    return json.substr(begin, json.find_first_of(",}", begin) - begin);
}

static std::size_t SampleCount(const std::string& json) {
    std::size_t count = 0, position = 0;
    while ((position = json.find("\"before_ns\":", position)) != std::string::npos) {
        ++count;
        ++position;
    }
    return count;
}

static std::string CaptureFailure(const std::string& json, bool time_limit) {
    // Keep the original smoke assertions unchanged; identify which one failed.
    for (const char* key : {"info_failures", "suspend_failures", "state_failures", "resume_failures"}) {
        if (json.find(std::string("\"") + key + "\":0") == std::string::npos)
            return std::string(key) + " expected zero, got " + Field(json, key);
    }
    const char* expected = time_limit ? "time_limit" : "capacity";
    if (json.find(std::string("\"stop_reason\":\"") + expected + "\"") == std::string::npos)
        return std::string("stop_reason expected ") + expected + ", got " + Field(json, "stop_reason");
    if (json.find("\"uuid\":\"") == std::string::npos)
        return "image UUID field missing";
    if (json.find("\"pc\":0") != std::string::npos)
        return "sample contains zero PC";
    const auto count = SampleCount(json);
    if (time_limit ? (count == 0 || count > 12) : count != 32)
        return "sample count expected " + std::string(time_limit ? "1..12" : "32") +
               ", got " + std::to_string(count);
    // THREAD_WAITING=3. Preserve natural waiting states; never call them CPU work.
    if (!time_limit && json.find("\"natural_run_state\":3") == std::string::npos)
        return "capacity capture contains no natural THREAD_WAITING sample";
    return {};
}

static void ReportFailure(const std::string& path, const std::string& reason,
                          const std::string& json) {
    std::cerr << "FAIL host PC sampler: " << reason << "\nreport=" << path << '\n';
    // Bounded diagnostics: no image paths, sampled registers or report payload.
    for (const char* key : {"compiled_available", "enabled", "started", "stop_reason",
                           "sampler_preflight_result", "capacity", "max_seconds", "start_ns",
                           "end_ns", "info_failures", "suspend_failures", "state_failures",
                           "resume_failures"})
        std::cerr << key << '=' << Field(json, key) << ' ';
    std::cerr << "samples=" << SampleCount(json) << '\n';
    for (const char* key : {"natural_run_state", "info_result", "suspend_result", "state_result",
                           "resume_result"})
        std::cerr << "first_" << key << '=' << Field(json, key) << ' ';
    std::cerr << '\n';
}

static bool CheckFailureDiagnostics() {
    const std::string header = "{\"info_failures\":0,\"suspend_failures\":0,\"state_failures\":0,"
                               "\"resume_failures\":0,\"stop_reason\":\"capacity\",\"uuid\":\"test\",\"samples\":[";
    std::string valid = header;
    for (unsigned i = 0; i < 32; ++i) {
        if (i) valid += ',';
        valid += "{\"before_ns\":1,\"pc\":1,\"natural_run_state\":3}";
    }
    valid += "]}";
    if (!CaptureFailure(valid, false).empty()) return false;
    for (const auto& key : {"info_failures", "suspend_failures", "state_failures", "resume_failures"}) {
        auto invalid = valid;
        const auto position = invalid.find(std::string("\"") + key + "\":0");
        invalid[position + std::string(key).size() + 3] = '5';
        if (CaptureFailure(invalid, false).find(std::string(key) + " expected zero, got 5") == std::string::npos)
            return false;
    }
    auto invalid = valid;
    invalid.replace(invalid.find("capacity"), 8, "stopped");
    if (CaptureFailure(invalid, false).find("stop_reason expected capacity") == std::string::npos) return false;
    invalid = valid;
    invalid.replace(invalid.find("\"uuid\""), 6, "\"other\"");
    if (CaptureFailure(invalid, false) != "image UUID field missing") return false;
    invalid = valid;
    invalid.replace(invalid.find("\"pc\":1"), 6, "\"pc\":0");
    if (CaptureFailure(invalid, false) != "sample contains zero PC") return false;
    invalid = valid;
    invalid.replace(invalid.find("before_ns"), 9, "other_key");
    if (CaptureFailure(invalid, false).find("sample count expected 32, got 31") == std::string::npos) return false;
    invalid = valid;
    std::size_t position = 0;
    while ((position = invalid.find("\"natural_run_state\":3", position)) != std::string::npos) {
        invalid[position + 20] = '1';
        ++position;
    }
    if (CaptureFailure(invalid, false).find("no natural THREAD_WAITING") == std::string::npos) return false;
    std::cout << "host PC sampler failure diagnostics: 10 cases passed\n";
    return true;
}

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
    std::ifstream file(path);
    const std::string json((std::istreambuf_iterator<char>(file)), {});
    if (!written || !after_stop || !completed || progress == 0) {
        ReportFailure(path, "lifecycle/export: written=" + std::to_string(written) +
            " after_stop=" + std::to_string(after_stop.load()) +
            " completed=" + std::to_string(completed.load()) +
            " progress=" + std::to_string(progress.load()), json);
        return false;
    }
    const auto failure = CaptureFailure(json, time_limit);
    if (!failure.empty()) {
        ReportFailure(path, failure, json);
        return false;
    }
    const auto count = SampleCount(json);
    std::cout << path << ": " << count << " samples; resumed, bounded, joined; progress after stop=" << completed << '\n';
    return true;
}
int main(int argc, char** argv) {
    if (!CheckFailureDiagnostics()) {
        std::cerr << "FAIL host PC sampler diagnostic regression fixture\n";
        return 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--diagnostics-only") return 0;
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
