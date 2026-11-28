// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <optional>
#include <span>
#include "pica_metal_index_snapshot_proof.h"

namespace Cara {

// One page is leased to one uncommitted render/transfer command buffer. A page
// cannot be used by another submission until Metal has completed that buffer.
// All access, including completion polling, is on the renderer owner thread.
class PicaMetalUploadArena {
public:
    struct Range {
        id<MTLBuffer> buffer;
        NSUInteger offset;
        NSUInteger bytes;
    };

    PicaMetalUploadArena(id<MTLDevice> device, NSUInteger page_bytes);
    PicaMetalUploadArena(const PicaMetalUploadArena&) = delete;
    PicaMetalUploadArena& operator=(const PicaMetalUploadArena&) = delete;

    bool Begin(id<MTLCommandBuffer> command);
    std::optional<Range> Copy(std::span<const std::byte> source, NSUInteger alignment);
    id<MTLTexture> Copy2D(std::span<const std::byte> source, MTLPixelFormat format,
                         NSUInteger width, NSUInteger height, NSUInteger source_row_bytes,
                         const MetalIndexUploadProof* proof = nullptr);
    bool Submit();
    void Cancel();
    bool Failed() const { return failed; }
    std::uint64_t TakeWaitNs() { return std::exchange(wait_ns, 0); }

private:
    struct Page {
        id<MTLBuffer> buffer = nil;
        id<MTLCommandBuffer> command = nil;
        NSUInteger next = 0;
    };

    std::array<Page, 8> pages{};
    id<MTLDevice> device;
    NSUInteger page_bytes;
    std::size_t next_page = 0;
    Page* active = nullptr;
    bool failed = false;
    std::uint64_t wait_ns = 0;

    std::optional<Range> Reserve(NSUInteger bytes, NSUInteger alignment);
};

} // namespace Cara
