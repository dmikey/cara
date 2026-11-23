// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "video_core/renderer_metal/pica_metal_upload.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
void Require(bool condition, const char* detail) {
    if (!condition) {
        std::cerr << detail << '\n';
        std::exit(1);
    }
}
}

int main() { @autoreleasepool {
    setenv("CARA_METAL_DRAW_INPUT_REUSE", "1", 1);
    setenv("CARA_METAL_FRAME_COST", "1", 1);
    const auto device = MTLCreateSystemDefaultDevice();
    Require(device != nil, "A Metal device is required for upload portability checks");
    const auto queue = [device newCommandQueue];
    Require(queue != nil, "Metal queue allocation failed");
    auto command = [queue commandBuffer];

    Cara::PicaMetalUploadArena empty(device, 0);
    Require(!empty.Begin(command), "Zero-sized upload page accepted");
    if (device.maxBufferLength < std::numeric_limits<NSUInteger>::max()) {
        Cara::PicaMetalUploadArena oversized(device, device.maxBufferLength + 1);
        Require(!oversized.Begin(command), "Upload page exceeds the selected device's limit");
    }
    // A foreign device token must be rejected before it receives an allocation
    // or capability message. The actual command belongs to the real device.
    NSObject* foreignToken = [NSObject new];
    Cara::PicaMetalUploadArena foreign((id<MTLDevice>)foreignToken, 4096);
    Require(!foreign.Begin(command), "Command/device mismatch accepted");

    Cara::PicaMetalUploadArena arena(device, 4096);
    Require(arena.Begin(command), "Valid upload lease rejected");
    std::array<std::byte, 128> source{};
    for (std::size_t i = 0; i != source.size(); ++i)
        source[i] = std::byte(i);
    const auto queries=Cara::NativeMetalCost::spans[Cara::NativeMetalCost::LinearTextureAlignmentQueries].calls;
    const auto hits=Cara::NativeMetalCost::spans[Cara::NativeMetalCost::LinearTextureAlignmentHits].calls;
    auto texture = arena.Copy2D(source, MTLPixelFormatRGBA32Uint, 3, 2, 64);
    Require(texture != nil && texture.device == device, "Device-aligned texture upload failed");
    std::array<std::byte, 96> actual{};
    [texture getBytes:actual.data() bytesPerRow:48 fromRegion:MTLRegionMake2D(0, 0, 3, 2)
          mipmapLevel:0];
    for (std::size_t row = 0; row != 2; ++row)
        for (std::size_t i = 0; i != 48; ++i)
            Require(actual[row * 48 + i] == source[row * 64 + i],
                    "Upload alignment or source padding changed meaningful texels");
    Require(arena.Copy2D(source, MTLPixelFormatRGBA32Uint, 3, 2, 32) == nil,
            "Undersized source row accepted");
    Require(arena.Copy2D(source, MTLPixelFormatRGBA32Uint,
                        std::numeric_limits<NSUInteger>::max(), 2, 64) == nil,
            "Overflowing texture row accepted");
    arena.Cancel();
    command = [queue commandBuffer];
    Require(arena.Begin(command), "Canceled page could not be leased again");
    auto repeated = arena.Copy2D(source, MTLPixelFormatRGBA32Uint, 3, 2, 64);
    Require(repeated == texture, "Identical immutable source did not reuse its texture");
    source[0] = std::byte{255};
    auto changed = arena.Copy2D(source, MTLPixelFormatRGBA32Uint, 3, 2, 64);
    Require(changed != nil && changed != repeated, "Changed immutable source reused stale texels");
    // Interleaved formats have independent device answers. A second arena on
    // the same retained device can reuse capability facts, never input bytes.
    for (auto format : {MTLPixelFormatR16Uint, MTLPixelFormatRGBA32Float}) {
        auto first=arena.Copy2D(source,format,3,2,64);
        Require(first!=nil,"Additional admitted format failed");
        const unsigned row_bytes=format==MTLPixelFormatR16Uint?6u:48u;
        std::array<std::byte,96> transported{};
        [first getBytes:transported.data() bytesPerRow:row_bytes fromRegion:MTLRegionMake2D(0,0,3,2) mipmapLevel:0];
        for(unsigned row=0;row<2;++row)for(unsigned byte=0;byte<row_bytes;++byte)
            Require(transported[row*row_bytes+byte]==source[row*64+byte],"Interleaved format texels changed");
        Require(arena.Copy2D(source,format,3,2,64)==first,"Interleaved format lost immutable identity");
    }
    arena.Cancel();
    Cara::PicaMetalUploadArena second_arena(device,4096);
    Require(second_arena.Begin([queue commandBuffer]),"Second arena lease failed");
    Require(second_arena.Copy2D(source,MTLPixelFormatRGBA32Uint,3,2,64)!=nil,"Cross-arena format answer failed");
    second_arena.Cancel();
    const char* value=std::getenv("CARA_METAL_CACHE_LINEAR_TEXTURE_ALIGNMENT");
    const bool cache=value && std::string_view(value)=="1";
    Require(Cara::NativeMetalCost::spans[Cara::NativeMetalCost::LinearTextureAlignmentQueries].calls-queries==(cache?3u:8u),
            "Device alignment query count differs");
    Require(Cara::NativeMetalCost::spans[Cara::NativeMetalCost::LinearTextureAlignmentHits].calls-hits==(cache?5u:0u),
            "Device alignment cache hit count differs");
    std::cout << "PASS: Metal device/size guards, aligned uploads, padding and immutable-input identity\n";
} }
