// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>
// Runs with a current GLES3 context, before initializing VideoCore. No ROM required.
bool RunGPUVertexSelfTest(std::string& error);
