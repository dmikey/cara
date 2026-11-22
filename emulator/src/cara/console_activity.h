// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <charconv>
#include <iostream>
#include <optional>
#include <string_view>
#include "core/hle/service/cecd/streetpass.h"
#include "core/hle/service/ptm/step_history.h"

namespace Cara {
inline std::optional<int> RunConsoleActivity(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) != "--activity") return std::nullopt;
    try {
        if (argc != 5) throw std::runtime_error("Usage: --activity PROFILE steps COUNT | export FILE | import FILE");
        const auto nand = std::filesystem::path(argv[2]) / "nand";
        if (!std::filesystem::is_directory(nand))
            throw std::runtime_error("Select an existing Cara user directory containing nand");
        const std::string_view command = argv[3];
        if (command == "export") {
            const auto count = Service::CECD::ExportStreetPass(nand, argv[4]);
            std::cout << count << " public StreetPass messages exported\n";
        } else if (command == "import") {
            std::cout << Service::CECD::ImportStreetPass(nand, argv[4], std::time(nullptr))
                      << " StreetPass messages delivered\n";
        } else if (command == "steps") {
            unsigned count;
            std::string_view argument = argv[4];
            auto end = argument.data() + argument.size();
            auto parsed = std::from_chars(argument.data(), end, count);
            if (parsed.ec != std::errc{} || parsed.ptr != end || !count)
                throw std::runtime_error("Enter a positive integer step count");
            auto now = std::time(nullptr);
            const auto tm = *std::localtime(&now);
            using namespace std::chrono;
            const auto days = sys_days{year{1900 + tm.tm_year}/unsigned(tm.tm_mon + 1)/unsigned(tm.tm_mday)}
                            - sys_days{year{2000}/January/1};
            Service::PTM::StepHistory::Record(nand / "cara-steps.txt", days.count() * 24 + tm.tm_hour, count);
            std::cout << count << " simulated steps recorded\n";
        } else throw std::runtime_error("Unknown activity command");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
} // namespace Cara
