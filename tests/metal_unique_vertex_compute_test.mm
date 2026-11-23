// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone GPU equivalence test. Links the production translator, source
// wrappers, grouped draw encoder and upload arena; no ROM or shader dump needed.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "common/logging/log.h"
#include "video_core/renderer_metal/batched_vertex_source.h"
#include "video_core/renderer_metal/pica_metal_draw.h"
#include "video_core/renderer_metal/pica_metal_draw_group.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/pica_metal_raw_index_format.hh"
#include "video_core/renderer_metal/pica_metal_runtime_shader.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <vector>

namespace Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned, const char*, const char* format,
                       const fmt::format_args&) {
    throw std::runtime_error(format);
}
}
namespace Cara {
bool EncodeGroupedMetalDraw(id<MTLCommandBuffer>, PicaMetalUploadArena&, const TexturedMetalDraw&);
bool CloseGroupedMetalDraw(id<MTLCommandBuffer>);
void AppendNativeMetalDeferredAdmissionProfile(std::ostream&, bool);
}
namespace {
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
constexpr NSUInteger edge = 16;
// Authored instance of the generated packed ABI. The loader/main forms are
// deliberately identical to the strict production recognizer. Each invocation
// resets temporary/output state, as GenerateVertexShader does.
constexpr char vertex_source[] = R"(
layout(location=0) out vec4 primary_color;
layout(location=5) out vec4 normquat;
uniform highp usampler2D cara_vertex_inputs;
uniform highp usampler2D cara_vertex_indices;
uniform ivec4 cara_vertex_layout[16];
layout(std140) uniform vs_config { vec4 values[116]; };
layout(std140) uniform shader_data { vec4 common_fields[18]; };
vec4 input_position, input_quaternion, input_color;
vec4 vs_out_attr0, vs_out_attr1, vs_out_attr2;
vec4 reg_tmp0;
vec4 cara_decode_vertex(int input_vertex_id, int reg) {
    ivec4 info = cara_vertex_layout[reg];
    int texel = (info.x + input_vertex_id * info.y) / 16;
    vec4 value = uintBitsToFloat(texelFetch(cara_vertex_inputs, ivec2(texel % 128, texel / 128), 0));
    if (info.w != 4) return vec4(0.0);
    return value;
}
void cara_load_vertex(int vertex_id) {
int input_vertex_id = int(texelFetch(cara_vertex_indices, ivec2(vertex_id % 128, vertex_id / 128), 0).r);
    input_position = cara_decode_vertex(input_vertex_id, 0);
    input_quaternion = cara_decode_vertex(input_vertex_id, 1);
    input_color = cara_decode_vertex(input_vertex_id, 2);
}
void cara_run_vertex(int vertex_id) {
    cara_load_vertex(vertex_id);
    reg_tmp0 = vec4(0.0,0.0,0.0,1.0);
    vs_out_attr0 = vec4(0.0,0.0,0.0,1.0);
    vs_out_attr1 = vec4(0.0,0.0,0.0,1.0);
    vs_out_attr2 = vec4(0.0,0.0,0.0,1.0);
    reg_tmp0 = input_color;
    vs_out_attr0 = input_position;
    vs_out_attr1 = input_quaternion * values[0].x;
    vs_out_attr2 = vec4(reg_tmp0.x,reg_tmp0.y * values[115].y,common_fields[17].z,1.0);
}
void main() {
vec4 first_quat=vec4(0.0);
if (gl_VertexID % 3 != 0) { cara_run_vertex(gl_VertexID - gl_VertexID % 3); first_quat = vec4(vs_out_attr1.x, vs_out_attr1.y, vs_out_attr1.z, vs_out_attr1.w); }
cara_run_vertex(gl_VertexID);
normquat = vec4(vs_out_attr1.x, vs_out_attr1.y, vs_out_attr1.z, vs_out_attr1.w);
if (gl_VertexID % 3 != 0 && dot(first_quat, normquat) < 0.0) normquat = -normquat;
    gl_Position = vs_out_attr0;
    gl_Position.z = 2.0 * gl_Position.z + gl_Position.w;
    primary_color = vs_out_attr2;
}
)";
constexpr char fragment_source[] = R"(
layout(location=0) in vec4 primary_color;
layout(location=5) in vec4 normquat;
layout(location=0) out vec4 color;
void main() { color = vec4(normquat.x + 0.5, primary_color.yz, 1.0); }
)";
struct Vertex {
    std::array<float,4> position, quaternion, color;
};
static_assert(sizeof(Vertex) == 48);
struct Programs {
    id<MTLRenderPipelineState> direct;
    std::shared_ptr<const Cara::BatchedMetalVertexProgram> unique;
};
struct Case {
    unsigned topology, index_width, first;
    bool sparse = false;
    unsigned declared_dense = 0;
    unsigned input_count = 0;
    unsigned dense_vertices = 0;
};
}
int main() { @autoreleasepool { try {
    setenv("CARA_METAL_GROUPED_PASS", "1", 1);
    setenv("CARA_METAL_DRAW_INPUT_REUSE", "1", 1);
    const char* dedup_option=std::getenv("CARA_METAL_DEDUP_VERTEX_TRANSFORMS");
    const bool dedup=dedup_option && std::string_view(dedup_option)=="1";
    const auto* admission_option=std::getenv("CARA_METAL_UNIQUE_TRANSFORM_ADMISSION");
    const bool transform_admission=admission_option && std::string_view(admission_option)=="1";
    const auto* fallback_option=std::getenv("CARA_METAL_DEFER_RASTER_FALLBACKS");
    const bool raster_fallbacks=fallback_option && std::string_view(fallback_option)=="1";
    setenv("CARA_METAL_REUSE_UNIFORM_UPLOADS", "0", 1);
    setenv("CARA_METAL_FRAME_COST", "1", 1);
    setenv("CARA_METAL_DRAW_STAGE_PROFILE", "0", 1);
    setenv("CARA_METAL_ISOLATE_BLEND_PASSES", "0", 1);
    auto unique_glsl = Cara::MakeUniquePackedVertexSource(vertex_source);
    Require(bool(unique_glsl), "Generated-ABI unique rewrite rejected");
    std::string unknown(vertex_source);
    const auto sign = unknown.find("first_quat = vec4(vs_out_attr1.x");
    Require(sign != unknown.npos, "Test source did not include reference quaternion");
    unknown.replace(sign, std::strlen("first_quat = vec4(vs_out_attr1.x"),
                          "first_quat = vec4(-vs_out_attr1.x");
    Require(!Cara::MakeUniquePackedVertexSource(unknown), "Different reference expression accepted");
    unknown = vertex_source;
    unknown += "\nivec4 info = cara_vertex_layout[reg];\n";
    Require(!Cara::MakeUniquePackedVertexSource(unknown), "Ambiguous loader form accepted");
    std::string error;
    auto direct_source = Cara::TranslateRuntimeMetalShaders(vertex_source, fragment_source,
        Cara::RuntimeMetalVertexMode::Packed, true, true, error);
    if (!direct_source) throw std::runtime_error("Baseline translation: " + error);
    auto unique_source = Cara::TranslateRuntimeMetalShaders(*unique_glsl, fragment_source,
        Cara::RuntimeMetalVertexMode::Packed, false, false, error);
    if (!unique_source) throw std::runtime_error("Unique translation: " + error);
    auto batched_source = Cara::MakeBatchedVertexSource(unique_source->vertex, true);
    Require(bool(batched_source), "Unique MSL wrapper rejected translated source");
    Require(!Cara::MakeBatchedVertexSource(direct_source->vertex, true),
            "Gather wrapper accepted original index-reading shader");

    auto device = MTLCreateSystemDefaultDevice();
    Require(device != nil && device.hasUnifiedMemory &&
            device.argumentBuffersSupport == MTLArgumentBuffersTier2,
            "Requires supported Apple unified-memory/Tier2 GPU");
    auto queue = [device newCommandQueue];
    auto options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_3;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    const auto compile = [&](std::string_view source) {
        NSError* failure = nil;
        auto text = [[NSString alloc] initWithBytes:source.data() length:source.size()
                                           encoding:NSUTF8StringEncoding];
        auto result = [device newLibraryWithSource:text options:options error:&failure];
        if (!result) throw std::runtime_error(failure.localizedDescription.UTF8String);
        return result;
    };
    auto direct_library = compile(direct_source->vertex);
    auto fragment_library = compile(direct_source->fragment);
    auto unique_library = compile(*batched_source);
    const auto render_pipeline = [&](id<MTLFunction> vertex) {
        NSError* failure = nil;
        auto desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = vertex;
        desc.fragmentFunction = [fragment_library newFunctionWithName:@"cara_pica_frag"];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        desc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        auto result = [device newRenderPipelineStateWithDescriptor:desc error:&failure];
        if (!result) throw std::runtime_error(failure.localizedDescription.UTF8String);
        return result;
    };
    auto compute_function = [unique_library newFunctionWithName:@"cara_compute_batch"];
    NSError* failure = nil;
    auto compute = [device newComputePipelineStateWithFunction:compute_function error:&failure];
    if (!compute) throw std::runtime_error(failure.localizedDescription.UTF8String);
    auto arguments = [compute_function newArgumentEncoderWithBufferIndex:0];
    Require(arguments && arguments.encodedLength, "Compute argument ABI unavailable");
    std::uint8_t resources = 0;
    constexpr std::array members{"layout [[id(0)]]", "uniforms [[id(1)]]", "common [[id(2)]]",
        "vertices [[id(3)]]", "indices [[id(4)]]", "vertex_sampler [[id(5)]]", "index_sampler [[id(6)]]"};
    for (unsigned i=0;i<members.size();++i)
        if (batched_source->find(members[i]) != batched_source->npos) resources |= 1u << i;
    Programs programs{render_pipeline([direct_library newFunctionWithName:@"cara_pica_vert"]),
        std::make_shared<Cara::BatchedMetalVertexProgram>(Cara::BatchedMetalVertexProgram{
            compute,arguments,render_pipeline([unique_library newFunctionWithName:@"cara_compute_raster"]),
            resources,true})};
    auto sampler_desc = [MTLSamplerDescriptor new]; sampler_desc.supportArgumentBuffers = YES;
    auto sampler = [device newSamplerStateWithDescriptor:sampler_desc];
    auto depth_state = [device newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
    const auto texture = [&](MTLPixelFormat format) {
        auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                   width:edge height:edge mipmapped:NO];
        desc.storageMode = format == MTLPixelFormatRGBA8Unorm ? MTLStorageModeShared : MTLStorageModePrivate;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        return [device newTextureWithDescriptor:desc];
    };
    const std::array<std::array<float,4>,4> positions{{
        {-1,-1,0,1},{1,-1,0,1},{-1,1,0,1},{1,1,0,1}}};
    unsigned cases_checked = 0;
    std::uint64_t expected_dispatches = 0, expected_draws = 0, expected_transforms = 0;
    std::uint64_t expected_hits=0, expected_avoided=0;
    std::uint64_t expected_corners=0;
    const auto run = [&](Case c, unsigned draw_count, bool identical=false, unsigned mutation=0,
                         bool mixed=false, bool feedback=false) {
        std::vector<unsigned> order = c.topology == 0 ? std::vector<unsigned>{0,1,2,2,1,3} :
            c.topology == 1 ? std::vector<unsigned>{0,1,2,3} : std::vector<unsigned>{0,1,3,2};
        if (c.input_count) {
            Require(c.input_count>=3 && (c.topology || c.input_count%3==0),"Invalid authored large input count");
            order.resize(c.input_count);
            for(unsigned i=0;i<order.size();++i)
                order[i]=c.index_width ? i%c.dense_vertices : i;
        }
        const unsigned corner_count = c.topology == 0 ? order.size() : 3*(order.size()-2);
        const unsigned vertex_count = c.index_width == 0 ? order.size() : c.dense_vertices ? c.dense_vertices : c.sparse ? 7 : 4;
        const unsigned dense = c.declared_dense ? c.declared_dense :
            c.sparse || vertex_count >= corner_count ? 0 : vertex_count;
        const bool eligible = dense > 0 && dense < corner_count && dense<=8192 &&
            corner_count <= (transform_admission ? 65536 : 8192);
        std::vector<std::byte> vertices((vertex_count*sizeof(Vertex)+2047)/2048*2048);
        for (unsigned i=0;i<vertex_count;++i) {
            unsigned logical = (c.index_width == 0 ? order[i] : c.sparse ? i/2 : i)%4;
            // A fan needs cyclic quad perimeter order0,1,3,2. The short
            // fixtures encode that in their index sequence; long streams
            // enumerate dense IDs, so assign those positions to the IDs here.
            if(c.input_count && c.topology==2)
                logical ^= logical>>1;
            Vertex vertex{positions[logical],{(logical&1) ? -0.5f : 0.5f,0,0,0},{1,1,1,1}};
            std::memcpy(vertices.data()+i*sizeof(Vertex),&vertex,sizeof(vertex));
        }
        auto changed_vertices=vertices;
        for (unsigned i=0;i<vertex_count;++i) {
            const float zero=0;
            std::memcpy(changed_vertices.data()+i*sizeof(Vertex)+offsetof(Vertex,color)+sizeof(float),&zero,sizeof(zero));
        }
        std::vector<std::uint16_t> indices((2+(order.size()*c.index_width+1)/2+255)/256*256);
        const unsigned header = c.topology | (c.index_width << 2);
        indices[0] = header; indices[1] = c.first;
        if (c.index_width) {
            auto* payload = reinterpret_cast<std::uint8_t*>(indices.data()+2);
            for (unsigned i=0;i<order.size();++i) {
                const auto value = c.first + order[i]*(c.sparse ? 2 : 1);
                payload[i*c.index_width] = value & 255;
                if (c.index_width == 2) payload[i*2+1] = value >> 8;
            }
        }
        std::array<std::array<std::int32_t,4>,16> layout{};
        for (unsigned i=0;i<3;++i) layout[i] = {int(i*16),48,3,4};
        layout[0][3] |= (header << 3) | (c.first << 7);
        const auto render = [&](bool unique) {
            auto color = texture(MTLPixelFormatRGBA8Unorm);
            auto depth = texture(MTLPixelFormatDepth32Float_Stencil8);
            Require(color && depth, "Test attachment allocation failed");
            std::array<std::uint8_t,edge*edge*4> zeros{};
            [color replaceRegion:MTLRegionMake2D(0,0,edge,edge) mipmapLevel:0
                      withBytes:zeros.data() bytesPerRow:edge*4];
            Cara::PicaMetalUploadArena arena(device,2*1024*1024);
            auto command = [queue commandBuffer];
            const bool real_group = feedback || (mixed && draw_count>64);
            Cara::PicaMetalDrawGroup group(queue,2*1024*1024);
            std::vector<id<MTLCommandBuffer>> commands;
            const auto commit_group=[&] {
                auto ticket=group.Commit();Require(ticket.has_value(),"Production group commit failed");
                commands.push_back(ticket->command);
            };
            if(!real_group) Require(arena.Begin(command), "Test command lease failed");
            std::array<std::array<float,4>,116> vs{};
            std::array<std::array<float,4>,18> common{};
            for (unsigned n=0;n<draw_count;++n) {
                const bool changed=identical && mutation && n==draw_count/2;
                const bool alternate=!identical && (n&1);
                const bool vs_changed=alternate || (changed && mutation==1);
                vs[0][0] = vs_changed ? -1.f : 1.f;
                vs[115][1] = vs_changed ? 0.f : 1.f;
                common[17][2] = alternate || (changed && mutation==2) ? 1.f : 0.f;
                auto current_layout=layout;
                if (changed && mutation==3) current_layout[15][0]=123;
                auto current_indices=indices;
                if (changed && mutation==5) {
                    constexpr std::array<unsigned,6> rotated{1,2,0,1,3,2};
                    for (unsigned j=0;j<rotated.size();++j) current_indices[j+2]=c.first+rotated[j];
                }
                Cara::TexturedMetalDraw d{};
                d.pipeline = programs.direct; d.depth_stencil_state = depth_state;
                d.texel_fetch_sampler = sampler; d.color = color; d.depth_stencil = depth;
                d.lut_texture_slot = d.lut_sampler_slot = NSNotFound;
                d.texture0_slot = d.texture0_sampler_slot = NSNotFound;
                d.fragment_common_slot = 0; d.fragment_light_slot = NSNotFound;
                d.viewport = {0,0,double(edge),double(edge),0,1};
                d.scissor = draw_count == 1 ? MTLScissorRect{0,0,edge,edge} :
                    draw_count == 2 ? MTLScissorRect{n*edge/2,0,edge/2,edge} :
                    MTLScissorRect{n%edge,n/edge,1,1};
                d.vertex_count = corner_count; d.vertex_mode = Cara::MetalRawIndexMode;
                d.packed_vertices = changed && mutation==4 ? changed_vertices : vertices;
                d.expanded_indices = std::as_bytes(std::span(current_indices));
                d.layout_uniform = std::as_bytes(std::span(current_layout));
                d.vs_uniform = std::as_bytes(std::span(vs));
                d.common_uniform = std::as_bytes(std::span(common));
                std::array<std::byte,1040> light{};
                d.light_uniform = light;
                if (unique) d.batched_vertex = programs.unique;
                d.unique_source_vertices = mixed && n%3==1 ? 0 : dense;
                if(real_group) {
                    Cara::PicaMetalDrawViews views{};
                    views.color={color,{0x18010000,0x18010400},1};
                    views.depth_stencil={depth,{0x18020000,0x18020400},2};
                    views.color_dirty=views.color.authoritative_bytes;
                    if(feedback && n==1) {
                        auto aliased=d;auto alias_views=views;
                        aliased.texture0=color;aliased.texture0_sampler=sampler;
                        aliased.texture0_slot=aliased.texture0_sampler_slot=0;
                        aliased.primary_material_unit=0;
                        alias_views.sampled=views.color;
                        Require(group.Append(aliased,alias_views)==Cara::PicaMetalDrawGroup::Result::BoundaryRequired,
                                "Attachment feedback did not force original publication boundary");
                        commit_group();
                    }
                    auto result=group.Append(d,views);
                    if(result==Cara::PicaMetalDrawGroup::Result::BoundaryRequired) {
                        commit_group();result=group.Append(d,views);
                    }
                    Require(result==Cara::PicaMetalDrawGroup::Result::Recorded,"Production draw group rejected authored packet");
                } else Require(Cara::EncodeGroupedMetalDraw(command,arena,d), "Production grouped encode failed");
            }
            if(real_group) commit_group();
            else {
                Require(Cara::CloseGroupedMetalDraw(command) && arena.Submit(), "Production batch submit failed");
                commands.push_back(command);
            }
            for(const auto& submitted:commands) {
                [submitted waitUntilCompleted];
                if (submitted.error) throw std::runtime_error(submitted.error.localizedDescription.UTF8String);
                Require(submitted.status == MTLCommandBufferStatusCompleted, "GPU command failed");
            }
            if(real_group) Require(commands.size()==2,"Feedback/capacity boundary lost a publication");
            std::array<std::uint8_t,edge*edge*4> pixels{};
            [color getBytes:pixels.data() bytesPerRow:edge*4
                    fromRegion:MTLRegionMake2D(0,0,edge,edge) mipmapLevel:0];
            for (unsigned y=0;y<edge;++y) for (unsigned x=0;x<edge;++x) {
                const unsigned i = (y*edge+x)*4;
                const unsigned draw_index = draw_count <= 2 ? unsigned(x>=edge/2) : y*edge+x;
                if (draw_count > 2 && draw_index >= draw_count) {
                    Require(pixels[i] == 0 && pixels[i+1] == 0 &&
                            pixels[i+2] == 0 && pixels[i+3] == 0,
                            "Draw escaped its scissor or touched an unassigned pixel");
                    continue;
                }
                const bool second = !identical && draw_count != 1 && (draw_index&1);
                const bool changed=identical && mutation && draw_index==draw_count/2;
                Require(!(changed && mutation==5) || pixels[i]==0,
                        "Dedup reused raster indices instead of only transformed vertices");
                const bool green_zero=second || (changed && (mutation==1 || mutation==4));
                const bool blue_one=second || (changed && mutation==2);
                if (!((pixels[i] == 0 || pixels[i] == 255) &&
                       pixels[i+1] == (green_zero ? 0 : 255) &&
                       pixels[i+2] == (blue_one ? 255 : 0) && pixels[i+3] == 255))
                    throw std::runtime_error("Quaternion/uniform golden mismatch: topology="+
                        std::to_string(c.topology)+" width="+std::to_string(c.index_width)+
                        " corners="+std::to_string(corner_count)+" unique="+std::to_string(dense)+
                        " candidate="+std::to_string(unique)+" pixel="+std::to_string(x)+","+
                        std::to_string(y)+" rgba="+std::to_string(pixels[i])+","+
                        std::to_string(pixels[i+1])+","+std::to_string(pixels[i+2])+","+
                        std::to_string(pixels[i+3]));
            }
            return pixels;
        };
        const auto encoders_before = Cara::NativeMetalCost::spans[Cara::NativeMetalCost::EncoderStarts].calls;
        const auto baseline = render(false);
        const auto encoders_direct = Cara::NativeMetalCost::spans[Cara::NativeMetalCost::EncoderStarts].calls;
        const auto candidate = render(true);
        Require(baseline == candidate, "Unique compute raster differs from direct raw-index raster");
        const auto limit = Cara::NativeMetalCost::DrawComputeGroupLimit();
        unsigned expected_passes=0;bool open=false;
        std::vector<unsigned> packets;
        const auto flush=[&] {
            if(packets.empty())return;
            std::vector<unsigned> computed;
            for(auto n:packets)if(eligible && !(mixed && n%3==1))computed.push_back(n);
            if(computed.size()>=2) {
                ++expected_dispatches;expected_draws+=computed.size();
                expected_corners+=std::uint64_t(computed.size())*corner_count;
                unsigned representatives=computed.size();
                if(dedup) {
                    std::array<bool,2> identities{};
                    for(auto n:computed)identities[identical ? unsigned(mutation && mutation<=4 && n==draw_count/2) : n%2]=true;
                    representatives=unsigned(identities[0])+unsigned(identities[1]);
                    expected_hits+=computed.size()-representatives;
                    expected_avoided+=dense*(computed.size()-representatives);
                }
                expected_transforms+=dense*representatives;
            }
            ++expected_passes;open=true;packets.clear();
        };
        for(unsigned n=0;n<draw_count;++n) {
            if((feedback && n==1) || (mixed && draw_count>64 && n && n%64==0)) {flush();open=false;}
            const bool compute=eligible && !(mixed && n%3==1);
            const bool packet=compute || (raster_fallbacks && corner_count<=65536);
            if(packet) {if(packets.size()==limit)flush();packets.push_back(n);}
            else {flush();if(!open){++expected_passes;open=true;}}
        }
        flush();
        const unsigned baseline_passes=feedback || (mixed && draw_count>64) ? 2 : 1;
        Require(encoders_direct-encoders_before == baseline_passes &&
                Cara::NativeMetalCost::spans[Cara::NativeMetalCost::EncoderStarts].calls-encoders_direct == expected_passes,
                "Unexpected raster encoder boundary inside deferred group");
        ++cases_checked;
    };
    for (unsigned topology=0;topology<3;++topology) {
        run({topology,0,0},2);
        run({topology,1,7},2);
        run({topology,2,257},2);
    }
    run({0,2,257,true},2); // Sparse capture metadata must take unchanged raster fallback.
    run({0,1,7},1); // A singleton shader group must take unchanged raster fallback.
    run({0,2,257,false,6},2); // No reduction: unique_count equals raster corner count.
    run({0,2,257,false,8193},2); // Invalid metadata cannot bypass the local output bound.
    // Real input/index streams, not merely an oversized declared count.
    run({0,1,7,false,0,8193,4},2);
    run({0,2,257,false,0,8193,8192},2); // Maximum admitted unique output, >8192 corners.
    run({0,2,257,false,0,8193,8192},64); // Full64-job output bound: exactly64MiB.
    run({0,2,257,false,0,8196,8193},2); // Valid dense metadata; output must remain capped.
    run({1,0,0,false,0,4096,0},2); // Nonindexed strip has12282 corners/4096 sources.
    run({2,0,0,false,0,4096,0},2);
    run({1,2,257,false,0,4096,4},2);
    run({2,2,257,false,0,4096,4},2);
    run({0,2,257,false,0,65535,4},2); // Largest whole-triangle input within capture bound.
    run({0,2,257,false,0,65538,4},2); // Beyond existing raw capture corner bound: fallback.
    run({0,2,257,false,0,8193,4},1); // Large singleton stays original raster.
    // Straddle both capacities. Every draw has its own pixel and alternating
    // uniform bytes, so output-range overlap and wrong argument-array elements
    // cannot hide behind a later opaque draw.
    for (const unsigned count : {32u,33u,63u,64u,65u}) run({0,2,257},count);
    for (unsigned mutation=0;mutation<=5;++mutation) run({0,2,257},64,true,mutation);
    run({0,2,257},3,false,0,true); // Compute/fallback/compute, original raster order.
    run({0,2,257},64,false,0,true);
    run({0,2,257},65,false,0,true); // Actual production group capacity/commit.
    for(unsigned mutation=1;mutation<=5;++mutation)run({0,2,257},64,true,mutation,true);
    run({0,2,257},3,false,0,true,true); // Actual attachment-feedback boundary remains.
    const auto& spans = Cara::NativeMetalCost::spans;
    const char* uniform_flag = std::getenv("CARA_METAL_REUSE_DEFERRED_UNIFORMS");
    const bool uniform_reuse = uniform_flag && std::string_view(uniform_flag) == "1";
    for (const auto requests : {Cara::NativeMetalCost::UniformCommonUploadRequests,
                               Cara::NativeMetalCost::UniformLightUploadRequests,
                               Cara::NativeMetalCost::UniformVsUploadRequests}) {
        const auto hits = spans[requests+1].calls;
        Require(uniform_reuse ? hits > 0 : hits == 0,
                "Deferred uniform memo was unused or active without opt-in");
        const std::uint64_t bytes = requests == Cara::NativeMetalCost::UniformCommonUploadRequests ? 288 :
            requests == Cara::NativeMetalCost::UniformLightUploadRequests ? 1040 : 1856;
        Require(spans[requests+2].calls == spans[requests].calls * bytes &&
                    spans[requests+3].calls == (spans[requests].calls - hits) * bytes,
                "Uniform memo transport counters do not match actual copies");
    }
    Require(spans[Cara::NativeMetalCost::BatchedVertexDispatches].calls == expected_dispatches,
            "Unexpected compute dispatch count");
    Require(spans[Cara::NativeMetalCost::BatchedVertexDraws].calls == expected_draws,
            "Unexpected computed draw count");
    Require(spans[Cara::NativeMetalCost::BatchedUniqueVertexTransforms].calls == expected_transforms,
            "Unique transform count differs from dense snapshot cardinality");
    Require(spans[Cara::NativeMetalCost::BatchedVertexCorners].calls == expected_corners,
            "Computed raster corner count changed");
    Require(spans[Cara::NativeMetalCost::VertexTransformDedupCandidates].calls == (dedup ? expected_draws : 0),
            "Unexpected transform dedup candidate count");
    Require(spans[Cara::NativeMetalCost::VertexTransformDedupHits].calls == expected_hits &&
            spans[Cara::NativeMetalCost::VertexTransformsAvoided].calls == expected_avoided,
            "Transform dedup merged changed inputs or missed identical jobs");
    std::ostringstream profile;
    Cara::AppendNativeMetalDeferredAdmissionProfile(profile,false);
    Require(profile.str().find("deferred_hist_c16384_u8192_draws,")!=std::string::npos &&
            profile.str().find("deferred_hist_c65536_u32_draws,")!=std::string::npos &&
            profile.str().find("deferred_admission_transform_limit,")!=std::string::npos,
            "Large-corner or unique-bound diagnostics missing");
    std::ostringstream json_profile;
    json_profile << "{\"fixture\":true";
    Cara::AppendNativeMetalDeferredAdmissionProfile(json_profile,true);
    json_profile << '}';
    std::cout << "ADMISSION_PROFILE " << json_profile.str() << '\n';
    std::cout << "PASS: raster_fallbacks=" << raster_fallbacks << ", transform_admission=" << transform_admission << ", deferred_uniform_reuse=" << uniform_reuse << ", dedup=" << dedup << ", dedup_hits=" << expected_hits << ", avoided=" << expected_avoided
              << ", group_limit=" << Cara::NativeMetalCost::DrawGroupLimit() << ", "
              << cases_checked << " paired GPU cases, " << expected_dispatches
              << " dispatches, " << expected_draws << " computed draws, " << expected_transforms
              << " unique transforms, argument_stride=" << arguments.encodedLength
              << "; exact pixels and quaternion/uniform golden channels\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
} } }
