// SPDX-License-Identifier: GPL-2.0-or-later
#include "pica_metal_pair_indices.h"
#include "pica_metal_raw_index_format.hh"
#include "pica_metal_frame_cost.hh"
#include "video_core/pica_state.h"
#include "video_core/primitive_assembly.h"
#include "video_core/renderer_opengl/gl_packed_vertex.h"
#include "video_core/renderer_opengl/gl_vertex_index_cache.h"
#include <cstring>
#include <chrono>
#include <cstdlib>
#include <list>
#include <limits>
#include <ostream>
#include <string_view>
#include <unordered_map>

namespace Cara {
namespace {
constexpr std::size_t Budget = 32 * 1024 * 1024;
constexpr std::size_t EntryLimit = 128;
struct Key {
    u32 address, count, offset, topology, width, first;
    u16 mask;
    OpenGL::PackedVertexLayout layout;
    bool operator==(const Key&) const = default;
};
struct Hash {
    std::size_t operator()(const Key& key) const {
        std::size_t result = key.address;
        const auto mix = [&](u32 word) { result ^= std::size_t(word) + 0x9e3779b9u + (result<<6) + (result>>2); };
        for (u32 word : {key.count,key.offset,key.topology,key.width,key.first,u32(key.mask)}) mix(word);
        for (const auto& descriptor : key.layout) for (s32 word : descriptor) mix(u32(word));
        return result;
    }
};
struct Entry {
    std::shared_ptr<const MetalPairIndexPayload> payload;
    std::size_t bytes;
    std::list<Key>::iterator recent;
};
struct Cache {
    id<MTLDevice> device = nil;
    std::unordered_map<Key,Entry,Hash> entries;
    std::list<Key> recent;
    std::size_t bytes = 0;
    u64 hits = 0, misses = 0, declined_hits = 0, admissions = 0, evictions = 0;
    u64 build_ns = 0, compared_bytes = 0, selected_corners = 0, selected_pairs = 0;
    u64 baseline_runs = 0, predicted_runs = 0;
    void Selected(const MetalPairIndexPayload& payload) {
        selected_corners += payload.elements.size(); selected_pairs += payload.pair_count;
        baseline_runs += payload.baseline_runs; predicted_runs += payload.predicted_runs;
    }
    void Erase(std::unordered_map<Key,Entry,Hash>::iterator entry) {
        bytes -= entry->second.bytes;
        recent.erase(entry->second.recent);
        entries.erase(entry);
    }
};
thread_local Cache cache;
}

bool UseMetalPairIndices() {
    static const bool enabled = [] {
        const auto* value = std::getenv("CARA_METAL_PAIR_INDEXED_VERTICES");
        return value && std::string_view(value) == "1";
    }();
    return enabled;
}

std::shared_ptr<const MetalPairIndexPayload> CaptureMetalPairIndices(
    id<MTLDevice> device, const Pica::State& state, bool indexed, u16 input_mask,
    const PicaMetalPackedInputs& captured) {
    const auto& pipe = state.regs.pipeline;
    if (!device || !UseMetalPairIndices() || !UseMetalRawIndices() ||
        state.regs.lighting.disable || state.regs.framebuffer.IsShadowRendering() ||
        pipe.use_gs != Pica::PipelineRegs::UseGS::No || !state.primitive_assembler.IsEmpty() ||
        captured.vertex_count < 3 || captured.vertex_count > 65535 || captured.vertex_count % 3 ||
        captured.indices.size() < MetalRawIndexHeader)
        return {};
    if (cache.device != device) {
        cache.entries.clear(); cache.recent.clear(); cache.bytes = 0; cache.device = device;
    }
    const unsigned width = indexed ? (pipe.index_array.format ? 2 : 1) : 0;
    const auto topology = state.primitive_assembler.GetTopology();
    using Topology = Pica::PipelineRegs::TriangleTopology;
    const bool expanded = topology == Topology::Strip || topology == Topology::Fan;
    if (pipe.num_vertices < 3 || (!expanded && topology != Topology::List && topology != Topology::Shader)) return {};
    const auto expected_corners = expanded ? (u64(pipe.num_vertices)-2)*3 : u64(pipe.num_vertices/3)*3;
    if (expected_corners != captured.vertex_count ||
        captured.indices[0] != (u16(topology)|u16(width<<2)) ||
        (!indexed && (captured.indices[1] != 0 ||
                      u64(pipe.vertex_offset)+pipe.num_vertices-1 > std::numeric_limits<u32>::max()))) return {};
    const auto source_size = std::size_t(pipe.num_vertices) * width;
    if (source_size > (captured.indices.size()-MetalRawIndexHeader)*sizeof(u16)) return {};
    const auto* source = reinterpret_cast<const u8*>(captured.indices.data()+MetalRawIndexHeader);
    const auto address = u64(pipe.vertex_attributes.GetPhysicalBaseAddress()) + pipe.index_array.offset;
    if (indexed && address > std::numeric_limits<u32>::max()) return {};
    const Key key{indexed ? u32(address) : 0, pipe.num_vertices,
                  indexed ? 0 : u32(pipe.vertex_offset), u32(topology), width, u32(captured.indices[1]), input_mask,
                  OpenGL::CanonicalPackedVertexLayout(captured.layout)};
    if (const auto found = cache.entries.find(key); found != cache.entries.end()) {
        const auto& payload = found->second.payload;
        cache.compared_bytes += source_size;
        if (payload->source.size() == source_size &&
            (!source_size || std::memcmp(payload->source.data(), source, source_size) == 0)) {
            cache.recent.splice(cache.recent.begin(),cache.recent,found->second.recent);
            ++cache.hits;
            if (!payload->admitted) { ++cache.declined_hits; return {}; }
            cache.Selected(*payload);
            return payload;
        }
        cache.Erase(found); // Same addresses do not authorize changed bytes.
    }
    ++cache.misses;
    struct BuildTimer {
        bool enabled = NativeMetalCost::Enabled();
        std::chrono::steady_clock::time_point begin = enabled ? std::chrono::steady_clock::now() :
                                                             std::chrono::steady_clock::time_point{};
        ~BuildTimer() {
            if (enabled) cache.build_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-begin).count();
        }
    } timer;
    auto payload = std::make_shared<MetalPairIndexPayload>();
    payload->source.assign(source,source+source_size);
    std::vector<u32> triangles;
    triangles.reserve(captured.vertex_count);
    Pica::PrimitiveAssembler<u32> assembler(topology);
    for (u32 i=0;i<pipe.num_vertices;++i) {
        u32 vertex = i + pipe.vertex_offset;
        if (width == 1) vertex = source[i];
        else if (width == 2) vertex = u32(source[i*2]) | (u32(source[i*2+1])<<8);
        assembler.SubmitVertex(vertex,[&](u32 a,u32 b,u32 c) { triangles.insert(triangles.end(),{a,b,c}); });
    }
    if (triangles.size() != captured.vertex_count) return {};
    // Reuse the exact shared GLES pair builder and its conservative LRU32 work
    // admission. This temporary builder runs only on a payload/layout miss.
    OpenGL::IOSVertexIndexCache builder(Budget,1);
    const OpenGL::IOSVertexIndexCache::Key build_key{key.address,key.count,key.offset,key.topology,key.width};
    const auto* data = builder.Store(build_key,source,source_size,triangles,false,true);
    if (!data) return {};
    payload->baseline_runs = data->pair_reuse.baseline_vs_runs;
    payload->predicted_runs = data->pair_reuse.lru32_vs_runs;
    const bool useful = data->packable && data->lit_pair_count > 0 && data->lit_pair_count <= 65535 &&
        data->lit_elements.size() == captured.vertex_count && data->pair_reuse.measured &&
        u64(payload->predicted_runs)*4 <= u64(payload->baseline_runs)*3 &&
        (!width || data->first == captured.indices[1]);
    if (useful) {
        payload->pair_count = data->lit_pair_count;
        const auto texels = (std::size_t(payload->pair_count)+127)/128*128;
        payload->pairs.resize(texels*4,0);
        for (std::size_t i=0;i<payload->pair_count;++i) {
            payload->pairs[i*4] = data->lit_pairs[i*2];
            payload->pairs[i*4+1] = data->lit_pairs[i*2+1];
        }
        payload->elements = data->lit_elements;
    }
    const auto cpu_bytes = sizeof(Entry)+2*sizeof(Key)+sizeof(MetalPairIndexPayload)+64+
        payload->source.capacity() + payload->pairs.capacity()*sizeof(u32) + payload->elements.capacity()*sizeof(u16);
    const auto gpu_bytes = payload->pairs.size()*sizeof(u32)+payload->elements.size()*sizeof(u16);
    if (cpu_bytes > Budget || gpu_bytes > Budget-cpu_bytes) return {};
    auto bytes = cpu_bytes + gpu_bytes;
    // Admission and nominal size checks precede GPU allocations. No mutable
    // buffer is ever reused for a different index payload.
    if (useful) {
        const auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint
            width:128 height:payload->pairs.size()/4/128 mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead;
        payload->pair_texture = [device newTextureWithDescriptor:descriptor];
        payload->element_buffer = [device newBufferWithBytes:payload->elements.data()
            length:payload->elements.size()*sizeof(u16) options:MTLResourceStorageModeShared];
        if (!payload->pair_texture || !payload->element_buffer) return {};
        [payload->pair_texture replaceRegion:MTLRegionMake2D(0,0,descriptor.width,descriptor.height)
            mipmapLevel:0 withBytes:payload->pairs.data() bytesPerRow:128*4*sizeof(u32)];
        const auto allocated = std::max<std::size_t>(gpu_bytes,
            payload->pair_texture.allocatedSize + payload->element_buffer.allocatedSize);
        if (allocated > Budget-cpu_bytes) return {};
        bytes = cpu_bytes + allocated;
        payload->admitted = true;
        ++cache.admissions;
    }
    while (!cache.recent.empty() && (cache.bytes+bytes > Budget || cache.entries.size() >= EntryLimit)) {
        cache.Erase(cache.entries.find(cache.recent.back())); ++cache.evictions;
    }
    cache.recent.push_front(key);
    cache.entries.emplace(key,Entry{payload,bytes,cache.recent.begin()});
    cache.bytes += bytes;
    if (payload->admitted) cache.Selected(*payload);
    return payload->admitted ? payload : nullptr;
}

void ReportMetalPairIndices(std::ostream& out) {
    out << "pair_index_hits," << cache.hits << ",0\n"
        << "pair_index_misses," << cache.misses << ",0\n"
        << "pair_index_declined_hits," << cache.declined_hits << ",0\n"
        << "pair_index_admissions," << cache.admissions << ",0\n"
        << "pair_index_evictions," << cache.evictions << ",0\n"
        << "pair_index_cache_bytes," << cache.bytes << ",0\n"
        << "pair_index_build," << cache.misses << ',' << cache.build_ns << '\n'
        << "pair_index_compared_bytes," << cache.compared_bytes << ",0\n"
        << "pair_index_selected_corners," << cache.selected_corners << ",0\n"
        << "pair_index_selected_pairs," << cache.selected_pairs << ",0\n"
        << "pair_index_predicted_baseline_runs," << cache.baseline_runs << ",0\n"
        << "pair_index_predicted_pair_runs," << cache.predicted_runs << ",0\n";
}
} // namespace Cara
