// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include "common/common_types.h"

namespace OpenGL {
// Per-draw source records; the PICA index domain and our expanded batch cap fit u16.
// Epochs avoid clearing the entire table for every draw. Host cache only, never serialized.
class IOSVertexSourceMap {
public:
    struct Record { u16 index; bool inserted; };
    void BeginDraw() {
        if (++epoch == 0) { stamps.fill(0); epoch = 1; }
        count = 0;
    }
    Record Resolve(u16 source) {
        if (stamps[source] == epoch) return {records[source], false};
        const u16 index = static_cast<u16>(count++);
        stamps[source] = epoch;
        records[source] = index;
        return {index, true};
    }
    u32 Size() const { return count; }
private:
    std::array<u16, 65536> stamps{};
    std::array<u16, 65536> records;
    u16 epoch = 1;
    u32 count = 0;
};
} // namespace OpenGL
