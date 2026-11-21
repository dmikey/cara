// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <bitset>
#include <cstring>
#include <limits>
#include "pica_metal_frame_cost.hh"
#include "pica_metal_packed_inputs.h"
#include "pica_metal_raw_index_format.hh"
#include "video_core/pica_state.h"
#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
#include "video_core/renderer_opengl/gl_vertex_upload_ranges.h"
#include "video_core/vertex_loader.h"

namespace Cara {

// Only the original guest index bytes are staged. The Metal vertex function
// expands each triangle slot and rebases its index at the existing fetch.
inline std::optional<PicaMetalPackedInputs> CaptureMetalRawIndices(
    const Pica::State& state, bool indexed, u16 selected_inputs,
    const MetalGuestAuthority::GuestReader& read) {
    const auto& regs = state.regs;
    const auto& pipe = regs.pipeline;
    using Topology = Pica::PipelineRegs::TriangleTopology;
    const auto topology = state.primitive_assembler.GetTopology();
    const bool expanded = topology == Topology::Strip || topology == Topology::Fan;
    const u32 count = pipe.num_vertices;
    const u32 limit = OpenGL::IOS_MAX_EXPANDED_VERTICES;
    if (!read || pipe.use_gs != Pica::PipelineRegs::UseGS::No ||
        regs.framebuffer.IsShadowRendering() || !state.primitive_assembler.IsEmpty() ||
        count < 3 || count > (expanded ? limit / 3 : limit))
        return std::nullopt;

    const auto active_count = expanded ? count : (count / 3) * 3;
    const auto base = u64{pipe.vertex_attributes.GetPhysicalBaseAddress()};
    const unsigned width = indexed ? (pipe.index_array.format ? 2 : 1) : 0;
    const auto index_bytes = std::size_t{count} * width;
    const auto index_words = MetalRawIndexHeader + (index_bytes + 1) / 2;
    PicaMetalPackedInputs result;
    result.vertex_count = expanded ? (count - 2) * 3 : active_count;
    result.indices.resize(((index_words + MetalRawIndexRow - 1) / MetalRawIndexRow) *
                          MetalRawIndexRow);
    result.indices[0] = static_cast<u16>(topology) | static_cast<u16>(width << 2);

    u32 first = pipe.vertex_offset;
    u32 span = active_count - 1;
    std::size_t unique = active_count;
    if (indexed) {
        auto* bytes = reinterpret_cast<u8*>(result.indices.data() + MetalRawIndexHeader);
        const auto address = base + pipe.index_array.offset;
        if (address + index_bytes > std::numeric_limits<PAddr>::max() ||
            !read(static_cast<PAddr>(address), {bytes, index_bytes}))
            return std::nullopt;
        static const bool specialize = [] {
            const auto* option = std::getenv("CARA_METAL_INDEX_SCAN_SPECIALIZE");
            return option && std::string_view(option) == "1";
        }();
        static const bool memoize = [] {
            const auto* option = std::getenv("CARA_METAL_INDEX_SCAN_MEMO");
            return option && std::string_view(option) == "1";
        }();
        struct ScanMemo {
            u64 address{};
            unsigned width{};
            u32 first{}, last{};
            std::size_t unique{};
            std::vector<u8> input;
        };
        constexpr std::size_t max_bytes = 32 * 1024;
        ScanMemo* slot = nullptr;
        const auto active_bytes = std::size_t{active_count} * width;
        bool reused = false;
        first = std::numeric_limits<u16>::max();
        u32 last = 0;
        if (memoize && active_bytes <= max_bytes) {
            thread_local std::array<ScanMemo, 128> cache{};
            const auto key = (address >> 2) ^ (address >> 9) ^ active_count ^ (width << 4);
            slot = &cache[key % cache.size()];
            reused = slot->address == address && slot->width == width &&
                slot->input.size() == active_bytes &&
                std::memcmp(slot->input.data(), bytes, active_bytes) == 0;
            NativeMetalCost::Count(reused ? NativeMetalCost::IndexMemoHits
                                          : NativeMetalCost::IndexMemoMisses, 1);
        }
        if (reused) {
            first = slot->first;
            last = slot->last;
            unique = slot->unique;
        } else if (specialize) {
            const auto scan = [&]<unsigned Width>() {
                // Count new bits while visiting indices. The byte stream only
                // needs 256 bits and neither width scans unused words afterward.
                std::array<u64, Width == 1 ? 4 : 1024> seen{};
                std::size_t count_unique = 0;
                for (u32 i = 0; i < active_count; ++i) {
                    const u32 value = Width == 1 ? u32{bytes[i]}
                        : u32{bytes[i * 2]} | (u32{bytes[i * 2 + 1]} << 8);
                    auto& word = seen[value / 64];
                    const u64 bit = u64{1} << (value % 64);
                    count_unique += (word & bit) == 0;
                    word |= bit;
                    first = std::min(first, value);
                    last = std::max(last, value);
                }
                return count_unique;
            };
            if (width == 1) {
                unique = scan.template operator()<1>();
                NativeMetalCost::Count(NativeMetalCost::IndexScanEightBit, 1);
            } else {
                unique = scan.template operator()<2>();
                NativeMetalCost::Count(NativeMetalCost::IndexScanSixteenBit, 1);
            }
        } else {
            std::bitset<65536> seen;
            for (u32 i = 0; i < active_count; ++i) {
                const u32 value = width == 2 ? u32{bytes[2 * i]} | (u32{bytes[2 * i + 1]} << 8)
                                             : u32{bytes[i]};
                first = std::min(first, value);
                last = std::max(last, value);
                seen.set(value);
            }
            unique = seen.count();
        }
        if (slot && !reused) {
            slot->input.assign(bytes, bytes + active_bytes);
            slot->address = address;
            slot->width = width;
            slot->first = first;
            slot->last = last;
            slot->unique = unique;
        }
        span = last - first;
        result.indices[1] = static_cast<u16>(first);
    } else if (u64{first} + count - 1 > std::numeric_limits<u32>::max()) {
        return std::nullopt;
    }
    if (span >= 65536)
        return std::nullopt;
    // A dense interval is already compact in the rebased attribute snapshot.
    // Leave sparse intervals on the original indexed raster path.
    if (unique == span + 1 && unique < result.vertex_count)
        result.unique_dense_vertices = static_cast<u32>(unique);

    // Match the existing packed input layout and the original authoritative
    // guest-read sequence. Only index staging and topology expansion differ.
    constexpr std::size_t max_vertex_bytes = 8 * 1024 * 1024;
    const OpenGL::IOSVertexInputLayout layout(regs.vs, selected_inputs);
    const Pica::VertexLoader loader(pipe);
    OpenGL::BoundedVertexUploadRanges<16> ranges;
    std::array<u64, 16> addresses{};
    std::array<bool, 16> loaded{};
    std::array<std::array<float, 4>, 16> constants{};
    for (unsigned reg = 0; reg < layout.offsets.size(); ++reg) {
        const auto offset = layout.offsets[reg];
        if (offset < 0)
            continue;
        const auto attr = layout.source_attributes[offset];
        const auto desc = loader.GetAttributeLayout(attr);
        if (!desc.elements) {
            if (desc.is_default) {
                const auto& value = state.input_default_attributes.attr[attr];
                constants[reg] = {value.x.ToFloat32(), value.y.ToFloat32(), value.z.ToFloat32(),
                                  value.w.ToFloat32()};
            }
            result.layout[reg] = {static_cast<s32>(reg * sizeof(constants[reg])), 0, 3, 4};
            continue;
        }
        const auto format = static_cast<unsigned>(desc.format);
        if (desc.elements > 4 || format > 3)
            return std::nullopt;
        const unsigned bytes = format == 3 ? 4 : format == 2 ? 2 : 1;
        const auto start = base + desc.offset + u64{first} * desc.stride;
        const auto end = start + u64{span} * desc.stride + bytes * desc.elements;
        if (end > std::numeric_limits<PAddr>::max() || end - start > max_vertex_bytes ||
            !ranges.Add(start, end))
            return std::nullopt;

        addresses[reg] = start;
        loaded[reg] = true;
        result.layout[reg] = {0, static_cast<s32>(desc.stride), static_cast<s32>(format),
                              static_cast<s32>(desc.elements)};
    }
    ranges.SortAndMerge();
    std::size_t buffer_bytes = sizeof(constants);
    for (auto& range : ranges) {
        range.destination = (buffer_bytes + 3) & ~std::size_t{3};
        buffer_bytes = range.destination + (range.end - range.begin);
    }
    if (buffer_bytes > max_vertex_bytes ||
        buffer_bytes > sizeof(constants) + unique * layout.stride * sizeof(constants[0]))
        return std::nullopt;
    constexpr auto row_bytes = OpenGL::IOS_VERTEX_INPUT_WIDTH * sizeof(constants[0]);
    result.vertices.resize(((buffer_bytes + row_bytes - 1) / row_bytes) * row_bytes);
    std::memcpy(result.vertices.data(), constants.data(), sizeof(constants));
    for (const auto& range : ranges) {
        if (!read(static_cast<PAddr>(range.begin),
                  {result.vertices.data() + range.destination, range.end - range.begin}))
            return std::nullopt;
        for (unsigned reg = 0; reg < loaded.size(); ++reg) {
            if (loaded[reg] && addresses[reg] >= range.begin && addresses[reg] < range.end) {
                result.layout[reg][0] =
                    static_cast<s32>(range.destination + addresses[reg] - range.begin);
            }
        }
    }
    return result;
}

// An unavailable alternate Metal function can consume the exact same snapshot
// through the original index ABI without a second authoritative guest read.
inline void ExpandCapturedMetalRawIndices(PicaMetalPackedInputs& packed) {
    const auto topology = static_cast<Pica::PipelineRegs::TriangleTopology>(packed.indices[0] & 3);
    const unsigned width = packed.indices[0] >> 2;
    const u16 first = packed.indices[1];
    const auto* source = reinterpret_cast<const u8*>(packed.indices.data() + MetalRawIndexHeader);
    constexpr auto row = OpenGL::IOS_VERTEX_INPUT_WIDTH;
    std::vector<u16> expanded((packed.vertex_count + row - 1) / row * row);
    std::size_t next = 0;
    Pica::PrimitiveAssembler<u32> assembler(topology);
    const Pica::PrimitiveAssembler<u32>::TriangleHandler append = [&](u32 a, u32 b, u32 c) {
        expanded[next++] = static_cast<u16>(a);
        expanded[next++] = static_cast<u16>(b);
        expanded[next++] = static_cast<u16>(c);
    };
    using Topology = Pica::PipelineRegs::TriangleTopology;
    const u32 count = topology == Topology::Strip || topology == Topology::Fan
                          ? packed.vertex_count / 3 + 2
                          : packed.vertex_count;
    for (u32 i = 0; i < count; ++i) {
        u16 value = static_cast<u16>(i);
        if (width == 1)
            value = source[i] - first;
        else if (width == 2)
            value = (u16{source[2 * i]} | (u16{source[2 * i + 1]} << 8)) - first;
        assembler.SubmitVertex(value, append);
    }
    packed.indices = std::move(expanded);
}

} // namespace Cara
