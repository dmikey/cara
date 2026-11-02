// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
#include <array>
bool RunGPUDepthSelfTest(bool quantize, std::string& error, std::array<bool, 2>* formats = nullptr);
