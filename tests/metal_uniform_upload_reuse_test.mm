// SPDX-License-Identifier: GPL-2.0-or-later
// GPU regression for the production grouped draw encoder. Run in fresh processes
// with both values of CARA_METAL_REUSE_UNIFORM_UPLOADS and
// CARA_METAL_REUSE_FRAGMENT_BUFFER_BINDINGS; options are process-static.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "common/logging/log.h"
#include "video_core/renderer_metal/pica_metal_draw.h"
#include "video_core/renderer_metal/pica_metal_raw_index_format.hh"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

// Fail on production encoder diagnostics without pulling the application's
// asynchronous logging worker into this standalone GPU test.
namespace Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned, const char*, const char* format,
                       const fmt::format_args&) {
    throw std::runtime_error(format);
}
}
namespace Cara {
bool EncodeGroupedMetalDraw(id<MTLCommandBuffer>, PicaMetalUploadArena&, const TexturedMetalDraw&);
bool CloseGroupedMetalDraw(id<MTLCommandBuffer>);
}
namespace {
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
constexpr NSUInteger width = 64;
constexpr char shaders[] = R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; float blue; };
Out make(uint i, float blue) {
    const float2 p[] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return {float4(p[i],0,1),blue};
}
vertex Out stage(uint i [[vertex_id]], constant float* common [[buffer(0)]]) {
    return make(i,common[1]);
}
vertex Out decoded(uint i [[vertex_id]], constant float* vs [[buffer(0)]]) {
    return make(i,vs[0]);
}
vertex Out raw(uint i [[vertex_id]], constant float* vs [[buffer(1)]]) {
    return make(i,vs[0]);
}
fragment float4 shade(Out in [[stage_in]], constant float* common [[buffer(0)]],
                      constant float* light [[buffer(1)]]) {
    return float4(common[0],light[0],in.blue,1);
}
fragment float4 shade_common(Out in [[stage_in]], constant float* common [[buffer(2)]]) {
    return float4(common[0],0,in.blue,1);
}
)";
struct Totals { std::uint64_t requests{}, hits{}, copied{}; };
}
int main(int argc, char** argv) { @autoreleasepool { try {
    const char* flag = std::getenv("CARA_METAL_REUSE_UNIFORM_UPLOADS");
    const bool reuse = flag && std::string_view(flag) == "1";
    setenv("CARA_METAL_GROUPED_PASS", "1", 1);
    setenv("CARA_METAL_FRAME_COST", "1", 1);
    auto device = MTLCreateSystemDefaultDevice();
    Require(device != nil && device.hasUnifiedMemory, "Requires supported Apple unified-memory GPU");
    auto queue = [device newCommandQueue];
    NSError* error = nil;
    auto library = [device newLibraryWithSource:[NSString stringWithUTF8String:shaders]
                                        options:nil error:&error];
    if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
    const auto pipeline = [&](NSString* name, NSString* fragment = @"shade") {
        auto desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = [library newFunctionWithName:name];
        desc.fragmentFunction = [library newFunctionWithName:fragment];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        desc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        auto result = [device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!result) throw std::runtime_error(error.localizedDescription.UTF8String);
        return result;
    };
    auto stage = pipeline(@"stage"), decoded = pipeline(@"decoded"), raw = pipeline(@"raw");
    auto common_only = pipeline(@"stage", @"shade_common");
    auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                            width:width height:1 mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageRenderTarget;
    auto color = [device newTextureWithDescriptor:desc];
    desc.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    desc.storageMode = MTLStorageModePrivate;
    auto depth = [device newTextureWithDescriptor:desc];
    Require(color && depth, "Attachment allocation failed");
    auto sampler = [device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    auto depth_state = [device newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
    Cara::PicaMetalUploadArena arena(device, 256 * 1024), second_arena(device, 256 * 1024);
    std::array<float,72> common{};
    std::array<float,260> light{};
    std::array<float,464> vs{};
    std::array<std::byte,256> layout{};
    std::array<std::byte,2048> vertices{};
    std::array<std::byte,512> indices{}; // Valid sequential raw metadata (mode=0).
    std::array<Cara::PicaMetalStageVertex,3> stage_vertices{};
    std::array<std::uint8_t,width*4> golden{};
    std::array<Totals,3> totals{};
    NSUInteger pixel = 0;
    auto command = [queue commandBuffer];
    Require(arena.Begin(command), "Initial arena lease failed");
    const auto uniforms = [&](bool b, bool changed_vs = false) {
        common.fill(0); light.fill(0); vs.fill(0);
        common[0] = b ? 1.f : 0.f;
        common[1] = 1.f;
        light[0] = b ? 0.f : 1.f;
        vs[0] = changed_vs ? 0.f : 1.f;
    };
    const auto draw = [&](Cara::PicaMetalUploadArena& owner, Cara::PicaMetalVertexMode mode,
                          bool common_hit, bool vs_hit, bool use_common_only = false) {
        Cara::TexturedMetalDraw d{};
        const bool stage_in = mode == Cara::PicaMetalVertexMode::StageIn;
        const bool raw_indices = mode == Cara::MetalRawIndexMode;
        d.pipeline = use_common_only ? common_only : stage_in ? stage : raw_indices ? raw : decoded;
        d.depth_stencil_state = depth_state;
        d.texel_fetch_sampler = sampler;
        d.color = color; d.depth_stencil = depth;
        d.lut_texture_slot = d.lut_sampler_slot = NSNotFound;
        d.texture0_slot = d.texture0_sampler_slot = NSNotFound;
        d.fragment_common_slot = use_common_only ? 2 : 0;
        d.fragment_light_slot = use_common_only ? NSNotFound : 1;
        d.viewport = {0,0,double(width),1,0,1};
        d.scissor = {pixel,0,1,1};
        d.vertex_count = 3;
        d.vertex_mode = mode;
        d.common_uniform = std::as_bytes(std::span(common));
        d.light_uniform = std::as_bytes(std::span(light));
        if (stage_in) {
            d.packed_vertices = std::as_bytes(std::span(stage_vertices));
        } else {
            d.packed_vertices = vertices;
            d.expanded_indices = raw_indices ? std::span<const std::byte>(indices) :
                                               std::span<const std::byte>(indices).first(256);
            d.vs_uniform = std::as_bytes(std::span(vs));
            if (raw_indices) d.layout_uniform = layout;
        }
        Require(pixel < width && Cara::EncodeGroupedMetalDraw(command, owner, d), "Grouped draw failed");
        golden[pixel*4] = common[0] ? 255 : 0;
        golden[pixel*4+1] = !use_common_only && light[0] ? 255 : 0;
        golden[pixel*4+2] = (stage_in ? common[1] : vs[0]) ? 255 : 0;
        golden[pixel*4+3] = 255;
        ++pixel;
        for (unsigned i=0; i != (stage_in ? 2u : 3u); ++i) {
            const bool hit = reuse && (i == 2 ? vs_hit : common_hit);
            ++totals[i].requests;
            totals[i].hits += hit;
            totals[i].copied += hit ? 0 : i == 0 ? 288 : i == 1 ? 1040 : 1856;
        }
    };
    const auto stage_mode = Cara::PicaMetalVertexMode::StageIn;
    const auto decoded_mode = Cara::PicaMetalVertexMode::DecodedIndexed;
    const auto finish = [&](Cara::PicaMetalUploadArena& owner) {
        Require(Cara::CloseGroupedMetalDraw(command), "Closing pass failed");
        Require(owner.Submit(), "Submitting arena failed");
        [command waitUntilCompleted];
        if (command.error) throw std::runtime_error(command.error.localizedDescription.UTF8String);
        Require(command.status == MTLCommandBufferStatusCompleted, "GPU command did not complete");
    };
    uniforms(false); draw(arena,stage_mode,false,false); draw(arena,stage_mode,true,false);
    draw(arena,stage_mode,true,false,true); // Clears slots 0 and 1.
    draw(arena,stage_mode,true,false); // Restores both slots from the same ranges.
    uniforms(true); draw(arena,stage_mode,false,false); draw(arena,stage_mode,true,false);
    uniforms(false); draw(arena,stage_mode,false,false);
    draw(arena,decoded_mode,true,false); draw(arena,decoded_mode,true,true);
    uniforms(false,true); draw(arena,decoded_mode,true,false);
    draw(arena,Cara::MetalRawIndexMode,true,true);
    draw(arena,stage_mode,true,false);
    Require(Cara::CloseGroupedMetalDraw(command), "Mid-command close failed");
    draw(arena,stage_mode,false,false); // New pass cannot reuse prior pass cache.
    finish(arena);
    // More commands than either the standard or eight-page upload arena rotates.
    // Recycled pages must never inherit a prior command's uniform cache.
    for (unsigned i=0;i<10;++i) {
        command = [queue commandBuffer];
        Require(arena.Begin(command), "Rotated lease failed");
        uniforms(i&1);
        draw(arena,stage_mode,false,false); draw(arena,stage_mode,true,false);
        finish(arena);
    }
    command = [queue commandBuffer];
    Require(second_arena.Begin(command), "Second arena lease failed");
    uniforms(false);
    draw(second_arena,decoded_mode,false,false); draw(second_arena,decoded_mode,true,true);
    finish(second_arena);
    MTLCommandBufferDescriptor* command_desc = [MTLCommandBufferDescriptor new];
    command_desc.retainedReferences = NO;
    command = [queue commandBufferWithDescriptor:command_desc];
    Require(arena.Begin(command), "Unretained command lease failed");
    draw(arena,stage_mode,false,false); draw(arena,stage_mode,false,false);
    finish(arena);
    // Read only initialized pixels; no load-action-dependent untouched bytes.
    std::vector<std::uint8_t> actual(pixel*4);
    [color getBytes:actual.data() bytesPerRow:actual.size()
        fromRegion:MTLRegionMake2D(0,0,pixel,1) mipmapLevel:0];
    Require(std::equal(actual.begin(),actual.end(),golden.begin()), "GPU pixels differ from golden");
    const std::array<Cara::NativeMetalCost::Phase,3> requests{
        Cara::NativeMetalCost::UniformCommonUploadRequests,
        Cara::NativeMetalCost::UniformLightUploadRequests,
        Cara::NativeMetalCost::UniformVsUploadRequests};
    for (unsigned i=0;i<3;++i) {
        const auto base = requests[i];
        const auto& spans = Cara::NativeMetalCost::spans;
        Require(spans[base].calls == totals[i].requests, "Uniform request count differs");
        Require(spans[base+1].calls == totals[i].hits, "Uniform reuse count differs");
        const std::uint64_t bytes = i == 0 ? 288 : i == 1 ? 1040 : 1856;
        Require(spans[base+2].calls == totals[i].requests*bytes, "Requested byte count differs");
        Require(spans[base+3].calls == totals[i].copied, "Copied byte count differs");
    }
    if (argc == 2) {
        std::ofstream out(argv[1],std::ios::binary);
        out.write(reinterpret_cast<const char*>(actual.data()),actual.size());
        Require(bool(out), "Writing pixel artifact failed");
    }
    std::cout << "PASS: reuse=" << reuse << ", " << pixel << " golden GPU pixels; uniform hits "
              << totals[0].hits << '/' << totals[1].hits << '/' << totals[2].hits << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
} } }
