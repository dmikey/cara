// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/guest_replay_counts.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/arm/arm_interface.h"
#include "input_common/main.h"
#include "input_common/keyboard.h"
#include <SDL.h>
#ifdef CARA_MAC_CPU_JIT
#include "core/arm/dynarmic/arm_dynarmic.h"
#endif

namespace Cara::NativeReplay {
inline bool Flag(const char* name) {
    const auto* value = std::getenv(name);
    return value && std::strcmp(value, "1") == 0;
}
// GL uses the same post-RunLoop, next-refresh A schedule as the existing
// Metal compatibility input. Only the private replay requests this route.
class GLScript {
    struct Event { std::uint32_t frame; bool down; };
public:
    explicit GLScript(const std::string& directory) {
        if (!Common::GuestReplayCounts::Enabled()) return;
        if (!Flag("CARA_NATIVE_SCRIPT_ONLY_INPUT") || !Flag("CARA_NATIVE_BENCHMARK") ||
            std::getenv("CARA_NATIVE_PRESS_A_FRAMES") || std::getenv("CARA_NATIVE_PRESS_START_FRAMES") ||
            std::getenv("CARA_NATIVE_CIRCLE_SEGMENTS"))
            throw std::runtime_error("Matched replay must exclude the older GL input route");
        std::ifstream in(directory + "inputs.txt");
        std::string line;
        bool previous_down = false;
        std::uint32_t previous = 0;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            std::uint32_t frame;
            std::string action, key, extra;
            if (!(fields >> frame >> action >> key) || fields >> extra || key != "A" ||
                frame <= previous || (action != "down" && action != "up") ||
                ((action == "down") == previous_down) || events.size() >= 16384)
                throw std::runtime_error("Invalid matched replay A input");
            previous = frame;
            previous_down = action == "down";
            events.push_back({frame, previous_down});
        }
        if (!in.eof() || events.empty() || previous_down)
            throw std::runtime_error("Incomplete matched replay A input");
    }
    void Apply(std::uint32_t frame) {
        if (next == events.size()) return;
        if (events[next].frame < frame) throw std::runtime_error("Missed matched replay input");
        if (events[next].frame != frame) return;
        const auto event = events[next++];
        if (event.down)
            InputCommon::GetKeyboard()->PressKey(SDL_SCANCODE_A);
        else
            InputCommon::GetKeyboard()->ReleaseKey(SDL_SCANCODE_A);
        std::cout << "INPUT frame=" << event.frame << (event.down ? " down A 0 0" : " up A 0 0") << std::endl;
    }
    void Finish() const {
        if (next != events.size()) throw std::runtime_error("Matched replay input unfinished");
    }
private:
    std::vector<Event> events;
    std::size_t next = 0;
};

class Ledger {
    using Clock = std::chrono::steady_clock;
    struct Row {
        std::uint32_t frame;
        std::int64_t elapsed_ns;
        std::uint64_t owner_cpu_ns;
        std::uint64_t process_cpu_ns;
        std::int64_t ticks;
        std::int64_t global_ticks;
        std::array<std::uint64_t, Common::GuestReplayCounts::Count> work;
        std::array<std::uint32_t, 4> pcs;
    };
    static std::uint64_t CPU(clockid_t clock) {
        timespec t{};
        if (clock_gettime(clock, &t) != 0 || t.tv_sec < 0 || t.tv_nsec < 0 ||
            t.tv_nsec >= 1000000000L)
            throw std::runtime_error("Could not read replay CPU counter");
        return std::uint64_t(t.tv_sec) * 1000000000ULL + std::uint64_t(t.tv_nsec);
    }
public:
    Ledger(Core::System& core, std::uint32_t frame) : enabled(Common::GuestReplayCounts::Enabled()) {
        if (!enabled)
            return;
        if (frame != 0 || core.GetNumCores() != 4 || !Flag("CARA_NATIVE_SCRIPT_ONLY_INPUT"))
            throw std::runtime_error("Replay ledger requires origin 0, four guest cores and script-only input");
#ifdef CARA_MAC_CPU_JIT
        for (std::uint32_t i = 0; i < 4; ++i)
            if (!dynamic_cast<ARM_Dynarmic*>(&core.GetCore(i)))
                throw std::runtime_error("Replay ledger requires four actual JIT guest cores");
#else
        throw std::runtime_error("Replay ledger compiled without CPU JIT");
#endif
        rows.reserve(4096);
        last = Clock::now();
        Observe(core, frame);
    }
    void Observe(Core::System& core, std::uint32_t frame) {
        if (!enabled || (!rows.empty() && frame == rows.back().frame))
            return;
        if ((!rows.empty() && frame != rows.back().frame + 1) || rows.size() >= 100002)
            throw std::runtime_error("Replay ledger discontinuity or observer capacity reached");
        const auto now = Clock::now();
        Row row{frame, rows.empty() ? 0 : std::chrono::duration_cast<std::chrono::nanoseconds>(now-last).count(),
            CPU(CLOCK_THREAD_CPUTIME_ID), CPU(CLOCK_PROCESS_CPUTIME_ID),
            core.CoreTiming().GetTicks(), core.CoreTiming().GetGlobalTicks(),
            Common::GuestReplayCounts::Read(), {}};
        for (std::uint32_t i = 0; i < 4; ++i)
            row.pcs[i] = core.GetCore(i).GetPC();
        rows.push_back(row);
        last = now;
    }
    void Write(const std::string& directory) const {
        if (!enabled)
            return;
        std::ofstream out(directory + "native-replay-ledger.csv");
        out << "refresh,elapsed_ns,owner_cpu_ns,process_cpu_ns,ticks,global_ticks,"
               "draw_requests,command_lists,display_transfers,texture_copies,fills,"
               "software_vertex_draws,skipped_draws,pc0,pc1,pc2,pc3\n";
        for (const auto& r : rows) {
            out << r.frame << ',' << r.elapsed_ns << ',' << r.owner_cpu_ns << ','
                << r.process_cpu_ns << ',' << r.ticks << ',' << r.global_ticks;
            for (auto value : r.work) out << ',' << value;
            for (auto value : r.pcs) out << ',' << value;
            out << '\n';
        }
        out.close();
        if (!out)
            throw std::runtime_error("Could not retain native replay ledger");
    }
private:
    bool enabled = false;
    Clock::time_point last{};
    std::vector<Row> rows;
};
}
