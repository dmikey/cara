// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
namespace Cara {
// A completed guest save transaction, not an arbitrary file write.
inline std::atomic<std::uint64_t> save_commit_generation{0};
}
