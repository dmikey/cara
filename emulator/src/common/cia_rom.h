// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <optional>
#include <string>
#include "common/common_types.h"

namespace FileUtil {
class IOHandler;
// Validates the archive layout and reads its TMD title ID without extracting content.
std::optional<u64> GetCiaTitleId(const std::string& path);
// Read-only view of the main executable in an ordinary 3DS application CIA.
std::unique_ptr<IOHandler> OpenCiaRom(std::unique_ptr<IOHandler> source);
} // namespace FileUtil
