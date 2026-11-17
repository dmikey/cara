// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version
#pragma once
#include <cstdint>
namespace OpenGL {
struct NativeRGBA8RemapEncoding {
    bool supported = false;
    bool reverse_components = false;
};
// Native packed-u32 uploads encode linear guest bytes directly in RGBA, while
// tiled uploads encode them in ABGR. This is not the GLES byte-upload codec.
// Layout/ownership/size/texture guards remain mandatory at the cache/remapper.
inline NativeRGBA8RemapEncoding PlanNativeRGBA8RemapEncoding(
    bool source_rgba8, bool destination_rgba8, std::uint64_t source_address,
    std::uint64_t destination_address, bool source_tiled, bool destination_tiled) {
    if (!source_rgba8 || !destination_rgba8 || ((source_address | destination_address) & 3))
        return {};
    return {true, source_tiled != destination_tiled};
}
} // namespace OpenGL
