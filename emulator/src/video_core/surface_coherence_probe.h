// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

namespace VideoCore {
// Opt-in production surface-coherence oracle; not normal gameplay work.
bool RunSurfaceCoherenceSelfTest(std::string& error);
}
