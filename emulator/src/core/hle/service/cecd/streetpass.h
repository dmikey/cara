// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <ctime>
#include <set>
#include <filesystem>

namespace Service::CECD {
// User-initiated offline exchange. Both operations run while the local game
// is stopped. Only public, unbatched, title-authenticated messages are shared.
bool HasNewStreetPass(const std::filesystem::path& nand);
using StreetPassTitles = std::set<std::uint32_t>;
StreetPassTitles RegisteredStreetPassTitles(const std::filesystem::path& nand);
unsigned ExportStreetPass(const std::filesystem::path& nand, const std::filesystem::path& file,
                          const StreetPassTitles* allowed = nullptr);
inline constexpr std::size_t MaxStreetPassFile = 24 * 8 * 0x19000 + 9;
unsigned ImportStreetPass(const std::filesystem::path& nand, const std::filesystem::path& file,
                          std::time_t received);
} // namespace Service::CECD
