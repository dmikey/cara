// SPDX-License-Identifier: GPL-2.0-or-later
// Production immutable-input cache plus actual GPU blit readback. Run in fresh
// processes with CARA_METAL_VERTEX_TEXTURE_RECENT_LOOKUP independently 0 and 1.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_upload.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Expected {
    id<MTLTexture> texture;
    NSUInteger width, height, texel_bytes, source_pitch;
    std::vector<std::byte> bytes;
};
std::uint64_t digest = 1469598103934665603ULL;
std::size_t checked = 0;
std::vector<std::byte> Source(NSUInteger pitch, NSUInteger height, MTLPixelFormat format,
                              unsigned seed) {
    std::vector<std::byte> result(pitch * height);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = std::byte((i * 17 + seed * 23) & 255);
    if (format == MTLPixelFormatRGBA32Float) {
        // Finite values avoid making the transport oracle depend on special-value
        // arithmetic; the production cache still compares every original bit.
        for (std::size_t i = 0; i + 4 <= result.size(); i += 4) {
            const std::uint32_t bits = 0x3f800000u | ((seed * 97 + i * 11) & 0x7fffffu);
            std::memcpy(result.data() + i, &bits, sizeof(bits));
        }
    }
    return result;
}
void Exercise(id<MTLDevice> device) {
    const auto queue = [device newCommandQueue];
    Require(queue != nil, "Could not allocate queue");
    Cara::PicaMetalUploadArena arena(device, 4096);
    const auto lease = [queue commandBuffer];
    Require(arena.Begin(lease), "Could not begin production upload lease");
    std::vector<Expected> expected;
    const auto capture = [&](const std::vector<std::byte>& bytes, MTLPixelFormat format,
                              NSUInteger width = 3, NSUInteger pitch = 64) {
        const NSUInteger height = bytes.size() / pitch;
        const auto texture = arena.Copy2D(bytes, format, width, height, pitch);
        Require(texture != nil, "Immutable texture upload failed");
        expected.push_back({texture, width, height, format == MTLPixelFormatR16Uint ? 2u : 16u,
                            pitch, bytes});
        return texture;
    };
    constexpr MTLPixelFormat formats[]{MTLPixelFormatR16Uint, MTLPixelFormatRGBA32Uint,
                                       MTLPixelFormatRGBA32Float};
    std::array<id<MTLTexture>, 3> initial{};
    for (unsigned i = 0; i < 3; ++i)
        initial[i] = capture(Source(64, 2, formats[i], 1), formats[i]);
    // Interleaved formats must keep independent recent candidates.
    for (unsigned i = 0; i < 3; ++i) {
        auto bytes = Source(64, 2, formats[i], 1);
        Require(capture(bytes, formats[i]) == initial[i], "Identical source missed cache identity");
        const auto meaningful = formats[i] == MTLPixelFormatR16Uint ? 6u : 48u;
        bytes[meaningful / 2] ^= std::byte{0x05};
        const auto middle = capture(bytes, formats[i]);
        Require(middle != initial[i], "Middle mutation reused stale immutable texture");
        Require(capture(bytes, formats[i]) == middle, "Repeated middle mutation missed identity");
        bytes[64 + meaningful - 1] ^= std::byte{0x01};
        const auto tail = capture(bytes, formats[i]);
        Require(tail != middle, "Tail mutation reused stale immutable texture");
        // Padding is part of the complete CPU snapshot/key even though the GPU
        // copies only meaningful texels. Its mutation must not authorize a hit.
        bytes[63] ^= std::byte{0x33};
        const auto padding = capture(bytes, formats[i]);
        Require(padding != tail, "Padding mutation bypassed full source equality");
        Require(capture(bytes, formats[i]) == padding, "Repeated padded source missed identity");
        Require(capture(Source(64, 2, formats[i], 1), formats[i]) == initial[i],
                "Hash fallback failed to recover earlier exact source");
        Require(capture(bytes, formats[i], 2) != padding, "Different width reused wrong texture");
        capture(Source(96, 2, formats[i], 1), formats[i], 3, 96);
    }
    // R16's recent hint will be evicted by insertions into the other format.
    // Keeping the old texture alive proves cache erasure never mutates its bytes.
    const auto old_r16 = capture(Source(64, 2, formats[0], 1), formats[0]);
    for (unsigned i = 0; i < 410; ++i) {
        auto bytes = Source(64, 2, formats[1], i + 100);
        // Ensure all410 payloads are distinct despite the byte fill's period.
        std::memcpy(bytes.data(), &i, sizeof(i));
        capture(bytes, formats[1]);
    }
    Require(capture(Source(64, 2, formats[0], 1), formats[0]) != old_r16,
            "An evicted recent candidate survived cache erasure");
    arena.Cancel();
    // Cache snapshots are independent of the upload arena's recycled lease.
    const auto next_lease = [queue commandBuffer];
    Require(arena.Begin(next_lease), "Could not recycle canceled upload lease");
    const auto after_eviction = expected.back().texture;
    Require(capture(Source(64, 2, formats[0], 1), formats[0]) == after_eviction,
            "Immutable input identity was tied to a canceled arena page");
    arena.Cancel();

    constexpr NSUInteger stride = 256;
    const auto readback = [device newBufferWithLength:expected.size() * stride * 2
                                              options:MTLResourceStorageModeShared];
    Require(readback != nil, "Readback allocation failed");
    const auto command = [queue commandBuffer];
    const auto encoder = [command blitCommandEncoder];
    Require(encoder != nil, "GPU blit encoder unavailable");
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& e = expected[i];
        [encoder copyFromTexture:e.texture sourceSlice:0 sourceLevel:0
                   sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(e.width, e.height, 1)
                       toBuffer:readback destinationOffset:i * stride * 2
         destinationBytesPerRow:stride destinationBytesPerImage:stride * e.height];
    }
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    Require(command.status == MTLCommandBufferStatusCompleted && !command.error,
            "GPU cache readback failed");
    const auto* actual = static_cast<const std::byte*>(readback.contents);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& e = expected[i];
        for (NSUInteger row = 0; row < e.height; ++row) {
            const auto bytes = e.width * e.texel_bytes;
            const auto* source = e.bytes.data() + row * e.source_pitch;
            const auto* output = actual + i * stride * 2 + row * stride;
            Require(std::memcmp(source, output, bytes) == 0,
                    "GPU observed changed or stale immutable texels");
            for (NSUInteger j = 0; j < bytes; ++j) {
                digest ^= std::to_integer<unsigned char>(output[j]);
                digest *= 1099511628211ULL;
            }
        }
        ++checked;
    }
}
}
int main() { @autoreleasepool { try {
    setenv("CARA_METAL_DRAW_INPUT_REUSE", "1", 1);
    setenv("CARA_METAL_FRAME_COST", "1", 1);
    const char* option = std::getenv("CARA_METAL_VERTEX_TEXTURE_RECENT_LOOKUP");
    const bool recent = option && std::string_view(option) == "1";
    const auto device = MTLCreateSystemDefaultDevice();
    Require(device != nil, "A Metal device is required");
    Exercise(device);
    unsigned devices_checked = 1;
    for (id<MTLDevice> other in MTLCopyAllDevices()) {
        if (other == device) continue;
        Exercise(other);
        Exercise(device); // Returning to the original device must rebuild hints.
        devices_checked += 2;
        break;
    }
    using namespace Cara::NativeMetalCost;
    Require(spans[VertexCacheEvictions].calls > 0, "Eviction coverage did not activate");
    Require(recent ? spans[VertexCacheRecentHits].calls > 0
                   : spans[VertexCacheRecentHits].calls == 0,
            "Recent-cache option/counters disagree");
    std::cout << "gpu_readbacks=" << checked << " digest=" << std::hex << digest << std::dec
              << " devices_checked=" << devices_checked << '\n';
    std::cerr << "recent_probes=" << spans[VertexCacheRecentProbes].calls
              << " recent_hits=" << spans[VertexCacheRecentHits].calls
              << " hashed_bytes=" << spans[VertexCacheHashBytes].calls
              << " compared_bytes=" << spans[VertexCacheCompareBytes].calls << '\n';
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } } }
