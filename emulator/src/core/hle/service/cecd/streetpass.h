// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <ctime>
#include <filesystem>

namespace Service::CECD {
// User-initiated offline exchange. Both operations run while the local game
// is stopped. Only public, unbatched, title-authenticated messages are shared.
bool HasNewStreetPass(const std::filesystem::path& nand);
unsigned ExportStreetPass(const std::filesystem::path& nand, const std::filesystem::path& file);
unsigned ImportStreetPass(const std::filesystem::path& nand, const std::filesystem::path& file,
                          std::time_t received);
} // namespace Service::CECD
