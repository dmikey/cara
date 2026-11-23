// SPDX-License-Identifier: GPL-2.0-or-later
// Production immutable-input cache plus actual GPU blit readback. Run in fresh
// processes with CARA_METAL_VERTEX_TEXTURE_ORDINAL_LOOKUP independently 0 and 1.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_upload.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace VideoCore {
std::uint32_t fixture_frame=0;
std::uint32_t GetCurrentFrame() { return fixture_frame; }
}
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
void CheckOrdinalHints(id<MTLDevice> device, bool enabled) {
    using namespace Cara::NativeMetalCost;
    const auto queue=[device newCommandQueue];
    Cara::PicaMetalUploadArena arena(device,4096);
    Require(arena.Begin([queue commandBuffer]),"Ordinal fixture lease failed");
    constexpr MTLPixelFormat formats[]{MTLPixelFormatR16Uint, MTLPixelFormatRGBA32Uint,
                                       MTLPixelFormatRGBA32Float};
    std::array<id<MTLTexture>,3> original{};
    VideoCore::fixture_frame=10;
    for(unsigned i=0;i<3;++i) {
        const auto source=Source(64,2,formats[i],17);
        original[i]=arena.Copy2D(source,formats[i],3,2,64);
        Require(original[i]!=nil,"Ordinal initial upload failed");
    }
    const auto before=spans[VertexCacheHash].calls;
    VideoCore::fixture_frame=11;
    for(unsigned i=0;i<3;++i) {
        const auto source=Source(64,2,formats[i],17);
        Require(arena.Copy2D(source,formats[i],3,2,64)==original[i],"Frame ordinal exact candidate failed");
    }
    Require(spans[VertexCacheHash].calls-before==(enabled ? 0u : 3u),
            "Stable frame ordinal did not bypass exactly the payload hashes");
    // Changed draw membership/order must use normal exact fallback, including
    // when pair textures share the same RGBA32Uint format as vertex payloads.
    VideoCore::fixture_frame=12;
    auto changed=Source(64,2,formats[1],17);changed[95]^=std::byte{1};
    const auto moved=arena.Copy2D(changed,formats[1],3,2,64);
    Require(moved && moved!=original[1],"Ordinal tail mutation reused old texture");
    const auto source=Source(64,2,formats[1],17);
    Require(arena.Copy2D(source,formats[1],3,2,64)==original[1],"Reordered original payload lost exact identity");
    VideoCore::fixture_frame=13;
    Require(arena.Copy2D(source,formats[1],3,2,64)==original[1],"Ordinal changed-order fallback failed");
    Require(arena.Copy2D(changed,formats[1],3,2,64)==moved,"Ordinal reverse-order fallback failed");
    // The metadata cap cannot reject draws or wrap into an earlier hint slot.
    for(unsigned i=0;i<1050;++i)
        Require(arena.Copy2D(source,formats[1],3,2,64)==original[1],"Ordinal metadata cap changed draw validity");
    arena.Cancel();
    Require(arena.Begin([queue commandBuffer]),"Canceled ordinal lease could not restart");
    const auto canceled=spans[VertexCacheHash].calls;
    Require(arena.Copy2D(source,formats[1],3,2,64)==original[1],"Cancel lost immutable resource owner");
    Require(spans[VertexCacheHash].calls==canceled+1,"Cancel retained ordinal candidate metadata");
    arena.Cancel();
    // Observe the production textures through actual GPU copies, including the
    // original and changed tail owners after later draws/cancel used the cache.
    std::array<id<MTLTexture>,4> textures{original[0],original[1],original[2],moved};
    const auto readback=[device newBufferWithLength:4*512 options:MTLResourceStorageModeShared];
    const auto command=[queue commandBuffer];const auto blit=[command blitCommandEncoder];
    Require(readback && blit,"Ordinal GPU readback resources unavailable");
    for(unsigned i=0;i<textures.size();++i)
        [blit copyFromTexture:textures[i] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                   sourceSize:MTLSizeMake(3,2,1) toBuffer:readback destinationOffset:i*512
                   destinationBytesPerRow:256 destinationBytesPerImage:512];
    [blit endEncoding];[command commit];[command waitUntilCompleted];
    Require(command.status==MTLCommandBufferStatusCompleted && !command.error,"Ordinal GPU copy failed");
    const auto* bytes=static_cast<const std::byte*>(readback.contents);
    for(unsigned i=0;i<textures.size();++i) {
        const auto format=formats[i==3 ? 1 : i];
        const auto expected=i==3 ? changed : Source(64,2,format,17);
        for(unsigned row=0;row<2;++row)
            Require(!std::memcmp(bytes+i*512+row*256,expected.data()+row*64,
                                 format==MTLPixelFormatR16Uint ? 6 : 48),
                    "Ordinal GPU consumed stale or mutated bytes");
    }
    // Counter rollback, such as state restore, resets cursors and hint history.
    Require(arena.Begin([queue commandBuffer]),"Ordinal rollback lease failed");
    VideoCore::fixture_frame=0;
    const auto rollback=spans[VertexCacheHash].calls;
    Require(arena.Copy2D(source,formats[1],3,2,64)==original[1],"Counter rollback lost resource identity");
    Require(spans[VertexCacheHash].calls==rollback+1,"Counter rollback retained old ordinal metadata");
    arena.Cancel();
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
    ++VideoCore::fixture_frame; // Probe ordinal0 after its original entry was evicted.
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
    const char* option = std::getenv("CARA_METAL_VERTEX_TEXTURE_ORDINAL_LOOKUP");
    const bool ordinal = option && std::string_view(option) == "1";
    const auto device = MTLCreateSystemDefaultDevice();
    Require(device != nil, "A Metal device is required");
    CheckOrdinalHints(device,ordinal);
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
    std::cout << "gpu_readbacks=" << checked << " digest=" << std::hex << digest << std::dec
              << " devices_checked=" << devices_checked << '\n';
    std::cerr << "recent_probes=" << spans[VertexCacheRecentProbes].calls
              << " recent_hits=" << spans[VertexCacheRecentHits].calls
              << " hashed_bytes=" << spans[VertexCacheHashBytes].calls
              << " compared_bytes=" << spans[VertexCacheCompareBytes].calls << '\n';
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } } }
