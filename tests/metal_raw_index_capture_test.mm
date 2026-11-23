// SPDX-License-Identifier: GPL-2.0-or-later
// Exercises the production raw-index capture without a GPU or commercial data.
// Run in four fresh processes with INDEX_SCAN_MEMO/SPECIALIZE independently 0/1.
#include "video_core/renderer_metal/pica_metal_capture_raw_indices.hh"
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>

namespace {
using Topology = Pica::PipelineRegs::TriangleTopology;
constexpr PAddr base = 0x20000000;
constexpr u32 attributes = 0x40000; // Separate from the largest authored index payload.
u64 digest = 1469598103934665603ULL;
unsigned cases = 0;
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void Hash(std::span<const u8> data) {
    for (const auto byte : data) { digest ^= byte; digest *= 1099511628211ULL; }
}
template <class T> void HashObject(const T& object) {
    Hash({reinterpret_cast<const u8*>(&object), sizeof(object)});
}
struct Fixture {
    std::unique_ptr<Pica::State> state = std::make_unique<Pica::State>();
    std::vector<u8> memory = std::vector<u8>(2 * 1024 * 1024);
    unsigned reads = 0;
    u32 index_offset = 0x100;
    std::function<void()> on_read;
    bool reject_index = false, reject_attribute = false;
    Fixture() {
        state->Reset();
        auto& pipe = state->regs.pipeline;
        pipe.vertex_attributes.base_address.Assign(base / 16);
        pipe.vertex_attributes.format0.Assign(Pica::PipelineRegs::VertexAttributeFormat::FLOAT);
        pipe.vertex_attributes.size0.Assign(3);
        auto& loader = pipe.vertex_attributes.attribute_loaders[0];
        loader.data_offset.Assign(attributes);
        loader.comp0.Assign(0);
        loader.component_count.Assign(1);
        loader.byte_count.Assign(16);
        pipe.index_array.offset.Assign(0x100);
        for (std::size_t i = 0; i < memory.size(); ++i)
            memory[i] = static_cast<u8>((i * 17 + i / 7) & 255);
    }
    bool Read(PAddr address, std::span<u8> out) {
        ++reads;
        if (address < base || u64{address - base} + out.size() > memory.size()) return false;
        if ((reject_index && address == base + index_offset) ||
            (reject_attribute && address >= base + attributes)) return false;
        std::memcpy(out.data(), memory.data() + address - base, out.size());
        if (on_read) { auto callback=std::move(on_read);on_read={};callback(); }
        return true;
    }
    auto Capture(bool indexed, Cara::PicaMetalPackedInputs* staging = nullptr) {
        return Cara::CaptureMetalRawIndices(*state, indexed, 1,
            [&](PAddr address, std::span<u8> out) { return Read(address, out); }, staging);
    }
};
void Check(Fixture& f, Topology topology, unsigned width, const std::vector<u32>& source,
           Cara::PicaMetalPackedInputs* staging = nullptr) {
    auto& pipe = f.state->regs.pipeline;
    pipe.num_vertices = source.size();
    pipe.index_array.format.Assign(width == 2 ? decltype(pipe.index_array)::SHORT
                                             : decltype(pipe.index_array)::BYTE);
    pipe.vertex_offset = source.front();
    f.state->primitive_assembler.Reconfigure(topology);
    if (width) for (std::size_t i = 0; i < source.size(); ++i) {
        f.memory[f.index_offset + i * width] = static_cast<u8>(source[i]);
        if (width == 2) f.memory[f.index_offset + 1 + i * width] = static_cast<u8>(source[i] >> 8);
    }
    const auto active = topology == Topology::Strip || topology == Topology::Fan
                            ? source.size() : source.size() / 3 * 3;
    const auto first = *std::min_element(source.begin(), source.begin() + active);
    const auto last = *std::max_element(source.begin(), source.begin() + active);
    const std::set<u32> unique(source.begin(), source.begin() + active);
    const auto triangles = topology == Topology::Strip || topology == Topology::Fan
                               ? (active - 2) * 3 : active;
    auto captured = f.Capture(width != 0, staging);
    if (last - first + 1 > unique.size()) {
        Require(!captured, "sparse snapshot exceeding packed-byte budget accepted");
        const u32 rejected = 0xffffffff;
        HashObject(rejected);
        ++cases;
        return;
    }
    Require(captured.has_value(), "valid capture rejected");
    Require(captured->vertex_count == triangles, "wrong triangle count");
    const u32 expected_unique = unique.size() == last - first + 1 && unique.size() < triangles
                                   ? static_cast<u32>(unique.size()) : 0;
    Require(captured->unique_dense_vertices == expected_unique, "wrong dense interval admission");
    Require(captured->indices[1] == (width ? first : 0), "wrong index rebase");
    const auto& layout = captured->layout[0];
    Require(layout[1] == 16 && layout[2] == 3 && layout[3] == 4, "wrong attribute layout");
    const auto bytes = (last - first + 1) * 16;
    Require(std::memcmp(captured->vertices.data() + layout[0],
                        f.memory.data() + attributes + first * 16, bytes) == 0,
            "live attribute bytes changed or stale");
    HashObject(captured->vertex_count); HashObject(captured->unique_dense_vertices);
    Hash({captured->vertices.data(), captured->vertices.size()});
    Hash({reinterpret_cast<const u8*>(captured->indices.data()), captured->indices.size() * 2});
    Cara::ExpandCapturedMetalRawIndices(*captured);
    std::vector<u16> oracle;
    const auto append = [&](std::size_t a, std::size_t b, std::size_t c) {
        oracle.insert(oracle.end(), {static_cast<u16>(source[a] - first),
                                    static_cast<u16>(source[b] - first),
                                    static_cast<u16>(source[c] - first)});
    };
    if (topology == Topology::List || topology == Topology::Shader) {
        for (std::size_t i = 0; i < active; i += 3) append(i, i + 1, i + 2);
    } else for (std::size_t i = 2; i < active; ++i) {
        if (topology == Topology::Fan) append(0, i - 1, i);
        else if (i & 1) append(i - 1, i - 2, i);
        else append(i - 2, i - 1, i);
    }
    Require(std::equal(oracle.begin(), oracle.end(), captured->indices.begin()),
            "raw index expansion differs from independent triangle oracle");
    Hash({reinterpret_cast<const u8*>(oracle.data()), oracle.size() * 2});
    ++cases;
    if (staging) *staging = std::move(*captured);
}
}
int main() { try {
    setenv("CARA_METAL_FRAME_COST", "1", 1);
    Fixture f;
    Cara::PicaMetalPackedInputs staging;
    for (const auto topology : {Topology::List, Topology::Strip, Topology::Fan, Topology::Shader}) {
        for (const unsigned width : {1u, 2u}) {
            for (unsigned length = 3; length <= 65; ++length) {
                std::vector<u32> source(length);
                for (unsigned i = 0; i < length; ++i) source[i] = 10 + i % std::min(length, 19u);
                if (width == 2) for (auto& v : source) v += 1000;
                Check(f, topology, width, source);
                Check(f, topology, width, source, &staging); // exact memo hit
                source[length / 2] = width == 1 ? 250 : 65000; // changed interior
                Check(f, topology, width, source, &staging);
                source[length / 2] = source.front();
                Check(f, topology, width, source); // restore another full payload
                f.memory[attributes + source.front() * 16 + 5] ^= 0x5a;
                Check(f, topology, width, source); // indices same, attribute changed
            }
        }
        for (unsigned length = 3; length < 40; ++length) {
            std::vector<u32> source(length);
            for (unsigned i = 0; i < length; ++i) source[i] = 20 + i;
            Check(f, topology, 0, source);
        }
    }
    for (const auto topology : {Topology::List,Topology::Strip,Topology::Fan,Topology::Shader}) {
        for(unsigned width:{1u,2u}) {
            if(width==1 && (topology==Topology::Strip || topology==Topology::Fan))continue;
            const unsigned length=(topology==Topology::Strip || topology==Topology::Fan)?17001:40002;
            std::vector<u32> source(length);
            for(unsigned i=0;i<length;++i)source[i]=7+i%3;
            Check(f,topology,width,source,&staging);
            Check(f,topology,width,source,&staging); // >32 KiB complete-byte memo hit.
            source[length/2]=11;Check(f,topology,width,source,&staging);
            source[length-1]=12;Check(f,topology,width,source,&staging);
        }
    }
    for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<16;++i) {
        f.index_offset=0x100+i*65536;
        f.state->regs.pipeline.index_array.offset.Assign(f.index_offset);
        Check(f,Topology::List,1,{7,8,9,7,9,8}); // All sixteen collide in the old direct map.
    }
    f.index_offset=0x100;f.state->regs.pipeline.index_array.offset.Assign(f.index_offset);
    {
        Fixture nested;
        f.on_read=[&]{Check(nested,Topology::List,1,{10,11,12,10,12,11});};
        Check(f,Topology::List,1,{7,8,9,7,9,8});
    }
    f.state->primitive_assembler.Reconfigure(Topology::List);
    f.state->regs.pipeline.num_vertices = 6;
    f.reject_index = true;
    Require(!f.Capture(true), "failed index read accepted");
    f.reject_index = false; f.reject_attribute = true;
    Require(!f.Capture(true), "failed attribute read accepted");
    f.reject_attribute = false;
    Check(f, Topology::List, 1, {7, 8, 9, 7, 9, 8}); // recover after reader failure
    f.state->regs.pipeline.num_vertices = 2;
    Require(!f.Capture(true), "too few vertices accepted");
    f.state->regs.pipeline.num_vertices = 65537;
    Require(!f.Capture(true), "too many vertices accepted");
    f.state->regs.pipeline.num_vertices = 6;
    f.state->regs.pipeline.use_gs.Assign(Pica::PipelineRegs::UseGS::Yes);
    Require(!f.Capture(true), "geometry shader accepted");
    f.state->regs.pipeline.use_gs.Assign(Pica::PipelineRegs::UseGS::No);
    f.state->primitive_assembler.SubmitVertex({}, [](const auto&, const auto&, const auto&) {});
    Require(!f.Capture(true), "nonempty primitive state accepted");
    f.state->primitive_assembler.Reset();
    f.state->regs.pipeline.vertex_offset = 0xfffffffdu;
    Require(!f.Capture(false), "sequential vertex wrap accepted");
    std::cout << "cases=" << cases << " digest=" << std::hex << digest << std::dec << '\n';
    using namespace Cara::NativeMetalCost;
    std::cerr << "index_memo_hits=" << spans[IndexMemoHits].calls
              << " index_memo_misses=" << spans[IndexMemoMisses].calls
              << " index_scan8=" << spans[IndexScanEightBit].calls
              << " index_scan16=" << spans[IndexScanSixteenBit].calls
              << " guest_reads=" << f.reads << '\n';
    return 0;
} catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; } }
