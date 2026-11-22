// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

namespace Service::PTM {

// Hour zero is 2000-01-01 00:00 on the guest's local clock.
struct StepHistory {
    std::map<std::uint32_t, std::uint16_t> hours;
    std::uint32_t total = 0;

    static StepHistory Load(const std::filesystem::path& path) {
        StepHistory history;
        if (!std::filesystem::exists(path)) return history;
        std::ifstream in(path);
        std::uint64_t hour, steps;
        while (in >> std::ws && !in.eof()) {
            if (!(in >> hour >> steps) || hour > 87658200 || !steps || steps > 65535 ||
                steps > UINT32_MAX - history.total ||
                !history.hours.emplace(hour, steps).second)
                throw std::runtime_error("Invalid Cara step history");
            history.total += steps;
        }
        if (in.bad() || !in.eof()) throw std::runtime_error("Could not read Cara step history");
        return history;
    }

    // Called with the profile stopped. Rename commits a complete new history.
    static void Record(const std::filesystem::path& path, std::uint32_t hour, std::uint32_t steps) {
        auto history = Load(path);
        if (hour > 87658200 || !steps || steps > 65535u - history.hours[hour] ||
            steps > UINT32_MAX - history.total)
            throw std::runtime_error("Step count is outside the pedometer range");
        history.hours[hour] += steps;
        history.Save(path);
    }

    void Save(const std::filesystem::path& path) const {
        std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        std::ofstream out;
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.open(temporary);
        for (const auto& [key, count] : hours) out << key << ' ' << count << '\n';
        out.close();
        std::filesystem::rename(temporary, path);
    }

    // The HOME Menu normally awards coins. Consoles booted straight into a
    // cartridge still need their shared coin file reconciled before launch.
    template <typename Coin>
    bool AwardCoins(Coin& coin) const {
        using namespace std::chrono;
        bool changed = false;
        std::uint32_t cumulative = 0;
        for (auto it = hours.begin(); it != hours.end();) {
            const auto day_index = it->first / 24;
            std::uint32_t daily = 0;
            do {
                daily += it->second;
                ++it;
            } while (it != hours.end() && it->first / 24 == day_index);
            cumulative += daily;
            const year_month_day date{sys_days{year{2000}/January/1} + days{day_index}};
            const year_month_day previous{year{coin.year}, month{coin.month}, day{coin.day}};
            if (date < previous) continue;
            const auto earned = std::min<std::uint32_t>(10, daily / 100);
            const auto credited = date == previous ? coin.total_coins_on_date : 0u;
            if (earned <= credited) continue;
            coin.total_coins = std::min<std::uint32_t>(300, coin.total_coins + earned - credited);
            coin.total_coins_on_date = earned;
            coin.step_count = cumulative - daily + earned * 100;
            coin.last_step_count = earned * 100;
            coin.year = int(date.year());
            coin.month = unsigned(date.month());
            coin.day = unsigned(date.day());
            changed = true;
        }
        return changed;
    }
};

} // namespace Service::PTM
