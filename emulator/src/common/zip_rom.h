// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>

namespace FileUtil {
class IOHandler;

bool IsZipRomPath(const std::string& path);
bool IsEncryptedTitleZip(const std::string& path);
std::unique_ptr<IOHandler> OpenIndexedZipRom(const std::string& path,
                                             std::unique_ptr<IOHandler> source);
} // namespace FileUtil
