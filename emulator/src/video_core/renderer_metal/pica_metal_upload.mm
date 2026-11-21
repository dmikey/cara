// SPDX-License-Identifier: GPL-2.0-or-later
#include "pica_metal_upload.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "common/common_types.h"
#include "video_core/video_core.h"

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

// Only candidate-order metadata is invalidated here. Immutable input textures
// remain independent snapshots and keep their existing resource lifetime.
thread_local std::uint64_t vertex_ordinal_cancel_epoch = 0;

bool UntrackedUploadSources() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_UNTRACKED_UPLOAD_SOURCES");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

MTLResourceOptions UploadSourceOptions() {
    // Only CPU-written/GPU-read sources. Page completion still protects CPU
    // reuse; this does not change dependencies of any GPU-written resource.
    return MTLResourceStorageModeShared | (UntrackedUploadSources() ?
        MTLResourceHazardTrackingModeUntracked : MTLResourceHazardTrackingModeDefault);
}

bool PowerOfTwo(NSUInteger value) {
    return value && (value & (value - 1)) == 0;
}

std::optional<NSUInteger> Align(NSUInteger bytes, NSUInteger alignment) {
    if (!PowerOfTwo(alignment) || bytes > std::numeric_limits<NSUInteger>::max() - alignment + 1) {
        return std::nullopt;
    }
    return (bytes + alignment - 1) & ~(alignment - 1);
}

// Device capability queries are immutable for a retained Metal device. Keep
// separate answers for the three formats Copy2D admits, including zero or
// unusual answers: Align retains all existing validation/failure behavior.
NSUInteger LinearTextureAlignment(id<MTLDevice> device, MTLPixelFormat format) {
    static const bool reuse = [] {
        const auto* value = std::getenv("CARA_METAL_CACHE_LINEAR_TEXTURE_ALIGNMENT");
        return value && std::string_view(value) == "1";
    }();
    const auto query = [&] {
        NativeMetalCost::Count(NativeMetalCost::LinearTextureAlignmentQueries, 1);
        return [device minimumLinearTextureAlignmentForPixelFormat:format];
    };
    if (!reuse)
        return query();
    const unsigned slot = format == MTLPixelFormatR16Uint ? 0u :
        format == MTLPixelFormatRGBA32Uint ? 1u :
        format == MTLPixelFormatRGBA32Float ? 2u : 3u;
    if (slot == 3u)
        return query();
    struct Cache {
        id<MTLDevice> owner = nil;
        std::array<NSUInteger,3> values{};
        std::array<bool,3> valid{};
    };
    // Retaining owner prevents pointer recycling from impersonating a device.
    // A device change discards all answers, with no unbounded registry.
    static thread_local Cache cache;
    if (cache.owner != device)
        cache = Cache{device};
    if (cache.valid[slot]) {
        NativeMetalCost::Count(NativeMetalCost::LinearTextureAlignmentHits, 1);
        return cache.values[slot];
    }
    const auto alignment = query();
    cache.values[slot] = alignment;
    cache.valid[slot] = true;
    return alignment;
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
        std::size_t hash;
        std::uint64_t identity;
    };
    struct Cache {
        struct Hint { std::size_t hash{}; std::uint64_t identity{}; };
        id<MTLDevice> device;
        std::unordered_multimap<std::size_t, Entry> entries;
        // References survive unordered-map rehash. Erasing their entry must
        // clear the hints first; a device change replaces the whole cache.
        std::array<Entry*, 3> recent{};
        // No resource ownership and no raw node pointers. An evicted identity
        // cannot match a newly allocated node, even if its address is recycled.
        std::array<std::array<Hint, 1024>, 3> ordinal{};
        std::array<std::size_t, 3> ordinal_next{};
        std::optional<std::uint32_t> frame;
        std::uint64_t cancel_epoch = 0, next_identity = 0;
        NSUInteger used = 0;
        std::uint64_t time = 0;
    };
    constexpr NSUInteger budget = 24 * 1024 * 1024;
    const NSUInteger allocation = height * pitch;
    // Check each term before adding: a malformed oversized request must not
    // wrap below the cache budget and reach a Metal allocation.
    if (allocation > budget || source.size_bytes() > budget - allocation)
        return nil;
    const NSUInteger cost = allocation + source.size_bytes();
    thread_local std::unique_ptr<Cache> cache;
    if (!cache || cache->device != device) {
        cache = std::make_unique<Cache>();
        cache->device = device;
    }
    static const bool reuse_recent = [] {
        const auto* option = std::getenv("CARA_METAL_VERTEX_TEXTURE_RECENT_LOOKUP");
        return option && std::string_view(option) == "1";
    }();
    static const bool reuse_ordinal = [] {
        const auto* option = std::getenv("CARA_METAL_VERTEX_TEXTURE_ORDINAL_LOOKUP");
        return option && std::string_view(option) == "1";
    }();
    const unsigned recent_slot = format == MTLPixelFormatR16Uint ? 0
                                  : format == MTLPixelFormatRGBA32Uint ? 1 : 2;
    Cache::Hint* ordinal_hint = nullptr;
    if (reuse_ordinal) {
        const auto frame = VideoCore::GetCurrentFrame();
        if (cache->cancel_epoch != vertex_ordinal_cancel_epoch) {
            cache->ordinal = {};
            cache->frame.reset();
            cache->cancel_epoch = vertex_ordinal_cancel_epoch;
        }
        if (cache->frame != frame) {
            if (cache->frame && frame < *cache->frame)
                cache->ordinal = {};
            cache->frame = frame;
            cache->ordinal_next = {};
        }
        auto& next = cache->ordinal_next[recent_slot];
        if (next < cache->ordinal[recent_slot].size())
            ordinal_hint = &cache->ordinal[recent_slot][next++];
    }
    const auto matches_shape = [&](const Entry& entry) {
        return entry.format == format && entry.width == width && entry.height == height &&
               entry.source_pitch == source_pitch && entry.data.size() == source.size();
    };
    const auto matches_bytes = [&](const Entry& entry) {
        NativeMetalCost::Scope cost(NativeMetalCost::VertexCacheCompare);
        NativeMetalCost::Count(NativeMetalCost::VertexCacheCompareBytes, source.size_bytes());
        return std::memcmp(entry.data.data(), source.data(), source.size()) == 0;
    };
    const auto remember_ordinal = [&](const Entry& entry) {
        if (ordinal_hint)
            *ordinal_hint = {entry.hash, entry.identity};
    };
    Entry* ordinal_candidate = nullptr;
    if (ordinal_hint && ordinal_hint->identity) {
        // The saved hash only selects an existing bucket. A live node identity,
        // every shape field and the full original bytes still have to agree.
        const auto candidates = cache->entries.equal_range(ordinal_hint->hash);
        for (auto it = candidates.first; it != candidates.second; ++it) {
            auto& entry = it->second;
            if (entry.identity != ordinal_hint->identity)
                continue;
            ordinal_candidate = &entry;
            if (matches_shape(entry) && matches_bytes(entry)) {
                entry.use = ++cache->time;
                if (reuse_recent)
                    cache->recent[recent_slot] = &entry;
                NativeMetalCost::Count(NativeMetalCost::VertexUploadHits, 1);
                return entry.texture;
            }
            break;
        }
    }
    if (reuse_recent) {
        if (auto* candidate = cache->recent[recent_slot]) {
            NativeMetalCost::Count(NativeMetalCost::VertexCacheRecentProbes, 1);
            // This is only a candidate-order hint. Every consumed byte, even
            // source-row padding, must match before bypassing the hash lookup.
            if (candidate != ordinal_candidate && matches_shape(*candidate) && matches_bytes(*candidate)) {
                candidate->use = ++cache->time;
                remember_ordinal(*candidate);
                NativeMetalCost::Count(NativeMetalCost::VertexCacheRecentHits, 1);
                NativeMetalCost::Count(NativeMetalCost::VertexUploadHits, 1);
                return candidate->texture;
            }
        }
    }
    const auto hash = [&] {
        NativeMetalCost::Scope cost(NativeMetalCost::VertexCacheHash);
        NativeMetalCost::Count(NativeMetalCost::VertexCacheHashBytes, source.size_bytes());
        return std::hash<std::string_view>{}(
            {reinterpret_cast<const char*>(source.data()), source.size()});
    }();
    const auto candidates = cache->entries.equal_range(hash);
    for (auto it = candidates.first; it != candidates.second; ++it) {
        auto& entry = it->second;
        if (matches_shape(entry)) {
            if (!matches_bytes(entry))
                continue;
            entry.use = ++cache->time;
            remember_ordinal(entry);
            if (reuse_recent)
                cache->recent[recent_slot] = &entry;
            NativeMetalCost::Count(NativeMetalCost::VertexUploadHits, 1);
            return entry.texture;
        }
    }
    NativeMetalCost::Count(NativeMetalCost::VertexUploadMisses, 1);
    const auto buffer = [device newBufferWithLength:allocation options:UploadSourceOptions()];
    if (!buffer)
        return nil;
    const auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                    width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    if (UntrackedUploadSources())
        descriptor.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    descriptor.usage = MTLTextureUsageShaderRead;
    const auto texture = [buffer newTextureWithDescriptor:descriptor offset:0 bytesPerRow:pitch];
    if (!texture)
        return nil;
    auto* dest = static_cast<std::byte*>(buffer.contents);
    const NSUInteger bytes = format == MTLPixelFormatR16Uint ? width * 2 : width * 16;
    {
        NativeMetalCost::Scope copy_cost(NativeMetalCost::VertexCacheTransport);
        NativeMetalCost::Count(NativeMetalCost::VertexCacheTransportBytes, bytes * height);
        for (NSUInteger row = 0; row < height; ++row)
            std::memcpy(dest + row * pitch, source.data() + row * source_pitch, bytes);
    }

    constexpr std::size_t entry_limit = 384;
    while (cache->entries.size() >= entry_limit || cost > budget - cache->used) {
        const auto victim = std::min_element(cache->entries.begin(), cache->entries.end(),
            [](const auto& a, const auto& b) { return a.second.use < b.second.use; });
        NativeMetalCost::Count(NativeMetalCost::VertexCacheEvictions, 1);
        NativeMetalCost::Count(NativeMetalCost::VertexCacheEvictedBytes, victim->second.allocation);
        cache->used -= victim->second.allocation;
        for (auto*& hint : cache->recent)
            if (hint == &victim->second)
                hint = nullptr;
        cache->entries.erase(victim);
    }
    if (cache->next_identity == std::numeric_limits<std::uint64_t>::max()) {
        // Invalidate all hints before recycling IDs; existing entries cannot
        // accidentally acquire the identity of a future insertion.
        cache->ordinal = {};
        std::uint64_t identity = 0;
        for (auto& [key, entry] : cache->entries)
            entry.identity = ++identity;
        cache->next_identity = identity;
    }
    const auto inserted = cache->entries.emplace(hash, Entry{texture, {source.begin(), source.end()}, format,
        width, height, source_pitch, cost, ++cache->time, hash, ++cache->next_identity});
    remember_ordinal(inserted->second);
    if (reuse_recent)
        cache->recent[recent_slot] = &inserted->second;
    cache->used += cost;
    return texture;
}

} // namespace

PicaMetalUploadArena::PicaMetalUploadArena(id<MTLDevice> device_, NSUInteger page_bytes_)
    : device(device_), page_bytes(page_bytes_) {}

bool PicaMetalUploadArena::Begin(id<MTLCommandBuffer> command) {
    if (!device || !command || command.device != device || active || failed || !page_bytes ||
        page_bytes > device.maxBufferLength ||
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
            page.buffer = [device newBufferWithLength:page_bytes options:UploadSourceOptions()];
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
        using Clock = std::chrono::steady_clock;
        NativeMetalCost::Scope cost(NativeMetalCost::ArenaWait);
        const auto start = Clock::now();
        [oldest.command waitUntilCompleted];
        wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
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
    const NSUInteger alignment = LinearTextureAlignment(device, format);
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
    if (UntrackedUploadSources())
        descriptor.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> view = [range->buffer newTextureWithDescriptor:descriptor
                                                        offset:range->offset bytesPerRow:*row_bytes];
    if (!view) {
        active->next = previous;
        return nil;
    }
    const auto meaningful = width * texel_bytes;
    auto* destination = static_cast<std::byte*>(range->buffer.contents) + range->offset;
    {
        NativeMetalCost::Scope copy_cost(NativeMetalCost::VertexArenaTransport);
        NativeMetalCost::Count(NativeMetalCost::VertexArenaTransportBytes, meaningful * height);
        for (NSUInteger row = 0; row < height; ++row) {
            std::memcpy(destination + row * *row_bytes, source.data() + row * source_row_bytes,
                        meaningful);
        }
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
    ++vertex_ordinal_cancel_epoch;
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
