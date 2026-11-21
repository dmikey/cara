// SPDX-License-Identifier: GPL-2.0-or-later
#include "pica_metal_upload.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>
#include <string_view>
#include <memory>
#include <functional>
#include <unordered_map>

namespace Cara {
namespace {

bool PowerOfTwo(NSUInteger value) {
    return value && (value & (value - 1)) == 0;
}

std::optional<NSUInteger> Align(NSUInteger bytes, NSUInteger alignment) {
    if (!PowerOfTwo(alignment) || bytes > std::numeric_limits<NSUInteger>::max() - alignment + 1) {
        return std::nullopt;
    }
    return (bytes + alignment - 1) & ~(alignment - 1);
}

// Input textures are snapshots, independent of the command's arena page.
// Equality covers every consumed source byte before a texture can be reused.
id<MTLTexture> ImmutableVertexTexture(id<MTLDevice> device, std::span<const std::byte> source,
                                    MTLPixelFormat format, NSUInteger width, NSUInteger height,
                                    NSUInteger source_pitch, NSUInteger pitch) {
    struct Entry {
        id<MTLTexture> texture;
        std::vector<std::byte> data;
        MTLPixelFormat format;
        NSUInteger width, height, source_pitch, allocation;
        std::uint64_t use;
    };
    struct Cache {
        id<MTLDevice> device;
        std::unordered_multimap<std::size_t, Entry> entries;
        NSUInteger used = 0;
        std::uint64_t time = 0;
    };
    constexpr NSUInteger budget = 24 * 1024 * 1024;
    const NSUInteger allocation = height * pitch;
    const NSUInteger cost = allocation + source.size_bytes();
    if (cost > budget)
        return nil;
    thread_local std::unique_ptr<Cache> cache;
    if (!cache || cache->device != device) {
        cache = std::make_unique<Cache>();
        cache->device = device;
    }
    const auto hash = std::hash<std::string_view>{}(
        {reinterpret_cast<const char*>(source.data()), source.size()});
    const auto candidates = cache->entries.equal_range(hash);
    for (auto it = candidates.first; it != candidates.second; ++it) {
        auto& entry = it->second;
        if (entry.format == format && entry.width == width && entry.height == height &&
            entry.source_pitch == source_pitch && entry.data.size() == source.size() &&
            std::memcmp(entry.data.data(), source.data(), source.size()) == 0) {
            entry.use = ++cache->time;
            NativeMetalCost::Count(NativeMetalCost::VertexUploadHits, 1);
            return entry.texture;
        }
    }
    NativeMetalCost::Count(NativeMetalCost::VertexUploadMisses, 1);
    const auto buffer = [device newBufferWithLength:allocation options:MTLResourceStorageModeShared];
    if (!buffer)
        return nil;
    const auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                    width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    const auto texture = [buffer newTextureWithDescriptor:descriptor offset:0 bytesPerRow:pitch];
    if (!texture)
        return nil;
    auto* dest = static_cast<std::byte*>(buffer.contents);
    const NSUInteger bytes = format == MTLPixelFormatR16Uint ? width * 2 : width * 16;
    for (NSUInteger row = 0; row < height; ++row)
        std::memcpy(dest + row * pitch, source.data() + row * source_pitch, bytes);

    constexpr std::size_t entry_limit = 384;
    while (cache->entries.size() >= entry_limit || cost > budget - cache->used) {
        const auto victim = std::min_element(cache->entries.begin(), cache->entries.end(),
            [](const auto& a, const auto& b) { return a.second.use < b.second.use; });
        cache->used -= victim->second.allocation;
        cache->entries.erase(victim);
    }
    cache->entries.emplace(hash, Entry{texture, {source.begin(), source.end()}, format,
        width, height, source_pitch, cost, ++cache->time});
    cache->used += cost;
    return texture;
}

} // namespace

PicaMetalUploadArena::PicaMetalUploadArena(id<MTLDevice> device_, NSUInteger page_bytes_)
    : device(device_), page_bytes(page_bytes_) {}

bool PicaMetalUploadArena::Begin(id<MTLCommandBuffer> command) {
    if (!device || !command || active || failed || !page_bytes ||
        command.status != MTLCommandBufferStatusNotEnqueued) {
        return false;
    }
    // The control uses the original three pages. The private native Metal
    // arm can use all eight while retaining the same completion checks.
    static const bool wide = [] {
        const auto* env = std::getenv("CARA_METAL_EIGHT_UPLOAD_PAGES");
        return env && std::string_view(env) == "1";
    }();
    const auto page_count = wide ? pages.size() : std::size_t{3};
    for (std::size_t offset = 0; offset < page_count; ++offset) {
        const std::size_t index = (next_page + offset) % page_count;
        auto& page = pages[index];
        if (page.command) {
            const auto status = page.command.status;
            if (status == MTLCommandBufferStatusError) {
                failed = true;
                return false;
            }
            if (status != MTLCommandBufferStatusCompleted) {
                continue;
            }
        }
        if (!page.buffer) {
            page.buffer = [device newBufferWithLength:page_bytes options:MTLResourceStorageModeShared];
            if (!page.buffer) {
                return false;
            }
        }
        page.command = command;
        page.next = 0;
        active = &page;
        next_page = (index + 1) % page_count;
        return true;
    }
    // All pages are in flight. Keep the batch intact and wait for the
    // next reusable page, whose producer was submitted before this batch.
    auto& oldest = pages[next_page];
    if (!oldest.command || oldest.command.status < MTLCommandBufferStatusCommitted)
        return false;
    {
        NativeMetalCost::Scope cost(NativeMetalCost::ArenaWait);
        [oldest.command waitUntilCompleted];
    }
    if (oldest.command.status != MTLCommandBufferStatusCompleted || oldest.command.error) {
        failed = true;
        return false;
    }
    return Begin(command);
}

std::optional<PicaMetalUploadArena::Range> PicaMetalUploadArena::Reserve(NSUInteger bytes,
                                                                        NSUInteger alignment) {
    if (!active || !bytes) {
        return std::nullopt;
    }
    const auto offset = Align(active->next, alignment);
    if (!offset || *offset > page_bytes || bytes > page_bytes - *offset) {
        return std::nullopt;
    }
    active->next = *offset + bytes;
    return Range{active->buffer, *offset, bytes};
}

std::optional<PicaMetalUploadArena::Range> PicaMetalUploadArena::Copy(
    std::span<const std::byte> source, NSUInteger alignment) {
    const auto range = Reserve(source.size_bytes(), alignment);
    if (range) {
        std::memcpy(static_cast<std::byte*>(range->buffer.contents) + range->offset, source.data(),
                    source.size_bytes());
    }
    return range;
}

id<MTLTexture> PicaMetalUploadArena::Copy2D(std::span<const std::byte> source, MTLPixelFormat format,
                                           NSUInteger width, NSUInteger height,
                                           NSUInteger source_row_bytes) {
    if (!active || !width || !height) {
        return nil;
    }
    const NSUInteger texel_bytes = (format == MTLPixelFormatRGBA32Uint ||
                                    format == MTLPixelFormatRGBA32Float) ? 16 :
                                   format == MTLPixelFormatR16Uint ? 2 : 0;
    if (!texel_bytes || width > std::numeric_limits<NSUInteger>::max() / texel_bytes ||
        source_row_bytes < width * texel_bytes ||
        height > std::numeric_limits<NSUInteger>::max() / source_row_bytes ||
        source.size_bytes() != height * source_row_bytes) {
        return nil;
    }
    const NSUInteger alignment = [device minimumLinearTextureAlignmentForPixelFormat:format];
    const auto row_bytes = Align(width * texel_bytes, alignment);
    if (!row_bytes || height > std::numeric_limits<NSUInteger>::max() / *row_bytes) {
        return nil;
    }
    if (NativeMetalCost::ReuseDrawInputs() && active->command.retainedReferences) {
        if (const auto resident = ImmutableVertexTexture(device, source, format, width, height,
                                                        source_row_bytes, *row_bytes))
            return resident;
    }
    const auto previous = active->next;
    const auto range = Reserve(height * *row_bytes, alignment);
    if (!range) {
        return nil;
    }
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> view = [range->buffer newTextureWithDescriptor:descriptor
                                                        offset:range->offset bytesPerRow:*row_bytes];
    if (!view) {
        active->next = previous;
        return nil;
    }
    const auto meaningful = width * texel_bytes;
    auto* destination = static_cast<std::byte*>(range->buffer.contents) + range->offset;
    for (NSUInteger row = 0; row < height; ++row) {
        std::memcpy(destination + row * *row_bytes, source.data() + row * source_row_bytes,
                    meaningful);
    }
    return view;
}

bool PicaMetalUploadArena::Submit() {
    if (!active || active->command.status != MTLCommandBufferStatusNotEnqueued) {
        return false;
    }
    [active->command commit];
    active = nullptr;
    return true;
}

void PicaMetalUploadArena::Cancel() {
    if (!active) {
        return;
    }
    // An externally committed command still owns its page. Treat that contract violation as a fault.
    if (active->command.status == MTLCommandBufferStatusNotEnqueued) {
        active->command = nil;
        active->next = 0;
    } else {
        failed = true;
    }
    active = nullptr;
}

} // namespace Cara
