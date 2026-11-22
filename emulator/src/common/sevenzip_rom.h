// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <string>
namespace FileUtil {
class IOHandler;
std::unique_ptr<IOHandler> OpenSevenZipRom(const std::string& path, std::unique_ptr<IOHandler> source);
}
