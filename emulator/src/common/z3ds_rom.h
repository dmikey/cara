// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>

namespace FileUtil {
class IOHandler;
std::unique_ptr<IOHandler> OpenZ3dsRom(std::unique_ptr<IOHandler> source);
}
