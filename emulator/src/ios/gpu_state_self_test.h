// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

// Requires the emulation GLES context current; restores tracked raster state.
bool RunGPUStateSelfTest(std::string& error);
