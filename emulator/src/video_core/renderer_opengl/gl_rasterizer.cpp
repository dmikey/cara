#include "cara/quad_uv_plan.h"
#include "cara/guest_gpu_trace.h"
#include "cara/quad_input_capture_trace.h"
#include "video_core/renderer_opengl/gl_effect_family_ablation.h"
#include "cara/texture_copy_trace.h"
// Modified for Cara on 2026-11-12; see NOTICE for attribution.
// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <set>
#include <cstdlib>
#include <memory>
#include <string>
#include <stdexcept>
#include <limits>
#include <tuple>
#include <utility>
#include <glad/glad.h>
#ifdef ARCHITECTURE_ARM64
#include <arm_neon.h>
#endif
#include "common/alignment.h"
#include "common/assert.h"
#include "common/frame_timing.h"
#include "common/cpu_category_timing.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/microprofile.h"
#include "common/scope_exit.h"
#include "common/vector_math.h"
#include "core/hw/gpu.h"
#include "core/settings.h"
#include "core/memory.h"
#include "video_core/pica_state.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/primitive_assembly.h"
#include "video_core/vertex_loader.h"
#include "video_core/regs_framebuffer.h"
#include "video_core/regs_rasterizer.h"
#include "video_core/regs_texturing.h"
#include "video_core/renderer_opengl/gl_ios_compat.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_scissor_damage.h"
#include "video_core/renderer_opengl/gl_host_scissor.h"
#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
#include "video_core/renderer_opengl/gl_vertex_upload_ranges.h"
#include "video_core/renderer_opengl/gl_packed_vertex_padding.h"
#include "video_core/renderer_opengl/gl_vertex_upload_order_reuse.h"
#include "video_core/renderer_opengl/gl_packed_snapshot_compare.h"
#include "video_core/renderer_opengl/gl_shader_gen.h"
#include "video_core/renderer_opengl/gl_vars.h"
#include "video_core/renderer_opengl/on_screen_display.h"
#include "video_core/renderer_opengl/pica_to_gl.h"
#include "video_core/renderer_opengl/renderer_opengl.h"
#include "video_core/video_core.h"

namespace OpenGL {
// Diagnostics suppress final raster submission only. CPU VS/GS, primitive
// assembly, uploads, state validation, batch clear and dirty bookkeeping run.
// PICA command_processor invokes DrawTriangles synchronously before the draw
// trigger returns; entries/textures here still belong to that live trigger.
static bool SuppressEffectFamilyHostDraw(std::uint64_t vertices) {
    namespace E=EffectFamilyAblation;
    if (!E::ParticleEnabled() && !E::CompositeEnabled() && !E::RayEnabled()) return false;
    const auto& r=Pica::g_state.regs;
    const auto& t=r.texturing;
    const auto& f=r.framebuffer.framebuffer;
    const E::Snapshot s{static_cast<u32>(r.pipeline.use_gs.Value()),
        static_cast<u32>(r.pipeline.triangle_topology.Value()),r.pipeline.num_vertices,
        r.vs.main_offset,r.gs.main_offset,r.reg_array[PICA_REG_INDEX(texturing.main_config)],
        t.texture0.GetPhysicalAddress(),t.texture1.GetPhysicalAddress(),t.texture2.GetPhysicalAddress(),
        static_cast<u32>(t.texture0_format.Value()),static_cast<u32>(t.texture1_format.Value()),
        static_cast<u32>(t.texture2_format.Value()),f.GetColorBufferPhysicalAddress(),f.GetWidth(),f.GetHeight(),
        r.reg_array[PICA_REG_INDEX(framebuffer.output_merger.depth_test_enable)],
        r.reg_array[PICA_REG_INDEX(framebuffer.output_merger.fragment_operation_mode)],
        r.reg_array[PICA_REG_INDEX(framebuffer.output_merger.alpha_blending)]};
    return E::Suppress(s,vertices);
}


using PixelFormat = SurfaceParams::PixelFormat;
using SurfaceType = SurfaceParams::SurfaceType;

// Begin draw-cost diagnostic. CPU measurements describe the calling host thread,
// not hardware GPU execution. No GL queries, allocations, scans, or clocks occur
// when disabled. Samples are selected independently in each path/size bucket.
class DrawCostProfile {
public:
    enum class Path : std::size_t {
        AcceleratedArrays, AcceleratedElements, FeedbackArrays, SoftwareArrays, DesktopRange, Count
    };
    struct Bucket {
        std::uint64_t full_calls = 0, full_vertices = 0;
        std::uint64_t attempted_samples = 0, samples = 0, sampled_vertices = 0;
        std::uint64_t cpu_ns = 0, wall_ns = 0, clock_failures = 0;
    };
    static bool Enabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("CARA_DRAW_COST_PROFILE");
            return value && std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }
    static std::size_t BucketIndex(std::uint64_t vertices) {
        return vertices <= 64 ? 0 : vertices <= 256 ? 1 : vertices <= 1024 ? 2
             : vertices <= 4096 ? 3 : 4;
    }
    static void Reset() {
        if (!Enabled()) return;
        auto& state = State();
        const auto next_session = state.session + 1;
        state = {};
        state.session = next_session;
        LOG_INFO(Render_OpenGL,
                 "PICA draw cost options session={} enabled=1 sample_rate=32 log_calls=65536",
                 state.session);
    }
    class Scope {
    public:
        Scope(Path path, std::uint64_t vertices) : vertices(vertices) {
            if (!Enabled()) return;
            auto& state = State();
            bucket = &state.buckets[static_cast<std::size_t>(path)][BucketIndex(vertices)];
            ++bucket->full_calls;
            bucket->full_vertices += vertices;
            report = (++state.calls % 65536) == 0;
            // Avoid repeatedly selecting one fixed draw slot in recurring scenes.
            const auto mixed = std::uint32_t(bucket->full_calls) * 0x9E3779B9u;
            if ((mixed >> 27) != 0) return;
            ++bucket->attempted_samples;
            if (!Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr, cpu_start)) {
                ++bucket->clock_failures;
                return;
            }
            // CPU encloses the wall-clock reads. Clock overhead is retained,
            // so tiny-call CPU samples may exceed wall samples; do not treat
            // their difference as a precise wait-time measurement.
            wall_start = std::chrono::steady_clock::now();
            sampled = true;
        }
        ~Scope() {
            if (!bucket) return;
            if (sampled) {
                const auto wall_end = std::chrono::steady_clock::now();
                std::uint64_t cpu_end;
                const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    wall_end - wall_start).count();
                if (!Common::CPUCategoryTiming::ReadThreadCPUTime(nullptr, cpu_end) ||
                    cpu_end < cpu_start || wall_ns < 0) {
                    ++bucket->clock_failures;
                } else {
                    ++bucket->samples;
                    bucket->sampled_vertices += vertices;
                    bucket->cpu_ns += cpu_end - cpu_start;
                    bucket->wall_ns += static_cast<std::uint64_t>(wall_ns);
                }
            }
            if (report) Report();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        Bucket* bucket = nullptr;
        std::uint64_t vertices, cpu_start = 0;
        std::chrono::steady_clock::time_point wall_start;
        bool sampled = false, report = false;
    };
private:
    struct Stats {
        std::uint64_t session = 0, calls = 0, snapshots = 0;
        std::array<std::array<Bucket, 5>, static_cast<std::size_t>(Path::Count)> buckets{};
    };
    static Stats& State() {
        // The rasterizer and its GL context run on one emulation thread. Reset
        // on context construction; no state is serialized into guest saves.
        static Stats state;
        return state;
    }
    static void Report() {
        auto& state = State();
        ++state.snapshots;
        constexpr std::array<const char*, 5> paths = {
            "accelerated_arrays", "accelerated_elements", "feedback_arrays", "software_arrays",
            "desktop_range"};
        constexpr std::array<std::uint64_t, 5> limits = {64, 256, 1024, 4096, 0};
        for (std::size_t path = 0; path < paths.size(); ++path) {
            for (std::size_t index = 0; index < limits.size(); ++index) {
                const auto& bucket = state.buckets[path][index];
                LOG_INFO(Render_OpenGL,
                         "PICA draw cost session={} snapshot={} total_calls={} path={} "
                         "bucket_max={} full_calls={} full_vertices={} attempted_samples={} "
                         "samples={} sampled_vertices={} cpu_ns={} wall_ns={} clock_failures={}",
                         state.session, state.snapshots, state.calls, paths[path], limits[index],
                         bucket.full_calls, bucket.full_vertices, bucket.attempted_samples,
                         bucket.samples, bucket.sampled_vertices, bucket.cpu_ns, bucket.wall_ns,
                         bucket.clock_failures);
            }
        }
    }
};
// End draw-cost diagnostic.

static bool IsVendorMali() {
    std::string gpu_vendor{reinterpret_cast<char const*>(glGetString(GL_VENDOR))};
    return gpu_vendor.find("ARM") != std::string::npos;
}

static GLsizeiptr LightingLUTBufferSize(std::size_t fallback) {
    // Keep the whole ring addressable by the minimum GLES 3.2 RG32F buffer view.
    if (UseNativeLUTBuffers())
        return 65536 * sizeof(GLvec2);
    return fallback;
}

RasterizerOpenGL::RasterizerOpenGL()
    : is_mali_gpu(IsVendorMali()), shader_dirty(true),
      vertex_buffer(GL_ARRAY_BUFFER, VERTEX_BUFFER_SIZE),
      uniform_buffer(GL_UNIFORM_BUFFER, UNIFORM_BUFFER_SIZE),
      index_buffer(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE),
      texture_buffer(GL_TEXTURE_BUFFER, (is_mali_gpu || UseNativeLUTBuffers()) ? 11264 : TEXTURE_BUFFER_SIZE),
      texture_lf_buffer(GL_TEXTURE_BUFFER, LightingLUTBufferSize(is_mali_gpu ? 525312 : TEXTURE_BUFFER_SIZE)) {

    SetPicaWriteHooksActive(SoftwareBatching::Active());
    DrawCostProfile::Reset();
    IndexPayloadReuse::ResetStats();
    OpenGLState::InvalidateRasterState();

#ifdef CARA_IOS_GLES
    glGetIntegerv(GL_MAX_TEXTURE_SIZE,&ios_vertex_max_texture_size);
    LOG_INFO(Render_OpenGL,"GLES vertex atlas: width={} max_texture={} max_vertices={}",
             IOS_VERTEX_INPUT_WIDTH,ios_vertex_max_texture_size,IOS_MAX_EXPANDED_VERTICES);
#endif

    AllowShadow = (GLAD_GL_ARB_shader_image_load_store && GLAD_GL_ARB_shader_image_size &&
                   GLAD_GL_ARB_framebuffer_no_attachments) ||
                  Settings::values.allow_shadow;

    // Clipping plane 0 is always enabled for PICA fixed clip plane z <= 0
    state.clip_distance[0] = true;

    // Create sampler objects
    for (std::size_t i = 0; i < texture_samplers.size(); ++i) {
        texture_samplers[i].Create();
        state.texture_units[i].sampler = texture_samplers[i].sampler.handle;
    }

    // Create cubemap texture and sampler objects
    texture_cube_sampler.Create();
    state.texture_cube_unit.sampler = texture_cube_sampler.sampler.handle;

    // Generate VAO
    sw_vao.Create();
    hw_vao.Create();

    // Create render framebuffer
    framebuffer.Create();
    state.draw.draw_framebuffer = framebuffer.handle;

    // null texture
    texture_null.Create();
    state.texture_units[0].texture_2d = texture_null.handle;

    // Allocate and bind texture buffer lut textures
    texture_buffer_lut_lf.Create();
    texture_buffer_lut_rg.Create();
    texture_buffer_lut_rgba.Create();
    state.texture_buffer_lut_lf.texture_buffer = texture_buffer_lut_lf.handle;
    state.texture_buffer_lut_rg.texture_buffer = texture_buffer_lut_rg.handle;
    state.texture_buffer_lut_rgba.texture_buffer = texture_buffer_lut_rgba.handle;

    uniform_block_data.dirty = true;

    // Equal CPU shadow values do not imply that a GPU range exists. In
    // particular, a newly created all-zero LUT must not alias another LUT's
    // first upload at offset zero. Valid uploaded offsets are nonnegative.
    for (auto& offsets : uniform_block_data.light_data.lighting_lut_offset)
        offsets = {-1, -1, -1, -1};
    uniform_block_data.data.fog_lut_offset = -1;
    uniform_block_data.data.proctex_noise_lut_offset = -1;
    uniform_block_data.data.proctex_color_map_offset = -1;
    uniform_block_data.data.proctex_alpha_map_offset = -1;
    uniform_block_data.data.proctex_lut_offset = -1;
    uniform_block_data.data.proctex_diff_lut_offset = -1;
    uniform_block_data.lighting_lut_dirty.fill(true);
    uniform_block_data.lighting_lut_dirty_any = true;
    uniform_block_data.light_dirty = true;

    uniform_block_data.fog_lut_dirty = true;

    uniform_block_data.proctex_noise_lut_dirty = true;
    uniform_block_data.proctex_color_map_dirty = true;
    uniform_block_data.proctex_alpha_map_dirty = true;
    uniform_block_data.proctex_lut_dirty = true;
    uniform_block_data.proctex_diff_lut_dirty = true;

    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniform_buffer_alignment);
    uniform_size_aligned_vs =
        Common::AlignUp<std::size_t>(sizeof(VSUniformData), uniform_buffer_alignment);
    uniform_size_aligned_fs =
        Common::AlignUp<std::size_t>(sizeof(UniformData), uniform_buffer_alignment);
    uniform_size_aligned_light =
        Common::AlignUp<std::size_t>(sizeof(UniformLightData), uniform_buffer_alignment);

    // Set vertex attributes for software shader path
    state.draw.vertex_array = sw_vao.handle;
    state.draw.vertex_buffer = vertex_buffer.GetHandle();
    state.Apply();

    glVertexAttribPointer(ATTRIBUTE_POSITION, 4, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, position));
    glEnableVertexAttribArray(ATTRIBUTE_POSITION);

    glVertexAttribPointer(ATTRIBUTE_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, color));
    glEnableVertexAttribArray(ATTRIBUTE_COLOR);

    glVertexAttribPointer(ATTRIBUTE_TEXCOORD0, 2, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord0));
    glVertexAttribPointer(ATTRIBUTE_TEXCOORD1, 2, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord1));
    glVertexAttribPointer(ATTRIBUTE_TEXCOORD2, 2, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord2));
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD0);
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD1);
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD2);

    glVertexAttribPointer(ATTRIBUTE_TEXCOORD0_W, 1, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, tex_coord0_w));
    glEnableVertexAttribArray(ATTRIBUTE_TEXCOORD0_W);

    glVertexAttribPointer(ATTRIBUTE_NORMQUAT, 4, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, normquat));
    glEnableVertexAttribArray(ATTRIBUTE_NORMQUAT);

    glVertexAttribPointer(ATTRIBUTE_VIEW, 3, GL_FLOAT, GL_FALSE, sizeof(HardwareVertex),
                          (GLvoid*)offsetof(HardwareVertex, view));
    glEnableVertexAttribArray(ATTRIBUTE_VIEW);

    // Create a 1x1 clear texture to use in the NULL case,
    // instead of OpenGL's default of solid black
    // For some reason alpha 0 wraps around to 1.0, so use 1/255 instead
    u8 null_data[4] = {0, 0, 0, 1};
    glActiveTexture(TextureUnits::PicaTexture(0).Enum());
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, null_data);

    glActiveTexture(TextureUnits::TextureBufferLUT_LF.Enum());
#ifdef CARA_IOS_GLES
    if (!UseNativeLUTBuffers())
        AllocateIOSLUT(GL_RG32F, texture_lf_buffer.GetSize());
    else
#endif
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RG32F, texture_lf_buffer.GetHandle());
    glActiveTexture(TextureUnits::TextureBufferLUT_RG.Enum());
#ifdef CARA_IOS_GLES
    if (!UseNativeLUTBuffers())
        AllocateIOSLUT(GL_RG32F, texture_buffer.GetSize());
    else
#endif
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RG32F, texture_buffer.GetHandle());
    glActiveTexture(TextureUnits::TextureBufferLUT_RGBA.Enum());
#ifdef CARA_IOS_GLES
    if (!UseNativeLUTBuffers())
        AllocateIOSLUT(GL_RGBA32F, texture_buffer.GetSize());
    else
#endif
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, texture_buffer.GetHandle());

    // restore
    state.texture_units[0].texture_2d = 0;
    // Bind index buffer for hardware shader path
    state.draw.vertex_array = hw_vao.handle;
    state.Apply();

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer.GetHandle());

    // 845需要开启分离着色器，但开启后Mali GPU会挂掉，究极日也有显示问题！
    const bool use_separable_shader = Settings::values.use_separable_shader;
    shader_program_manager = std::make_unique<ShaderProgramManager>(use_separable_shader);

    OpenGLState::InvalidateRasterState();
    // init opengl state
    glEnable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);

    SyncEntireState();
}

RasterizerOpenGL::~RasterizerOpenGL() = default;

void RasterizerOpenGL::SyncEntireState() {
    FlushSoftwareBatch(SoftwareBatching::Reason::ExplicitVisibility);
#ifdef CARA_IOS_GLES
    ios_packed_snapshot_lease.reset();
    if (IndexPayloadReuse::Enabled()) {
        ios_vertex_index_cache.ResetIdentities();
        for (auto& bank : ios_vertex_frame_uploads)
            for (auto& pool : bank.sizes)
                for (auto& entry : pool) entry.index_shadow.InvalidateIdentity();
        IndexPayloadReuse::RecordReset();
    }
#endif
    // Sync fixed function OpenGL state
    SyncClipEnabled();
    SyncCullMode();
    SyncBlendEnabled();
    SyncBlendFuncs();
    SyncBlendColor();
    SyncLogicOp();
    SyncStencilTest();
    SyncDepthTest();
    SyncColorWriteMask();
    SyncStencilWriteMask();
    SyncDepthWriteMask();

    // Sync uniforms
    SyncClipCoef();
    SyncDepthScale();
    SyncDepthOffset();
    SyncAlphaTest();
    SyncCombinerColor();
    auto& tev_stages = Pica::g_state.regs.texturing.GetTevStages();
    for (std::size_t index = 0; index < tev_stages.size(); ++index)
        SyncTevConstColor(index, tev_stages[index]);

    SyncGlobalAmbient();
    SyncLightingLutData();
    for (u32 light_index = 0; light_index < 8; light_index++) {
        SyncLightSpecular0(light_index);
        SyncLightSpecular1(light_index);
        SyncLightDiffuse(light_index);
        SyncLightAmbient(light_index);
        SyncLightPosition(light_index);
        SyncLightDistanceAttenuationBias(light_index);
        SyncLightDistanceAttenuationScale(light_index);
    }

    SyncFogColor();
    SyncFogLutData();
    SyncProcTexNoise();
    SyncProcTexBias();
    SyncShadowBias();
    SyncShadowTextureBias();
    SyncLightingLutScale();
}

/**
 * This is a helper function to resolve an issue when interpolating opposite quaternions. See below
 * for a detailed description of this issue (yuriks):
 *
 * For any rotation, there are two quaternions Q, and -Q, that represent the same rotation. If you
 * interpolate two quaternions that are opposite, instead of going from one rotation to another
 * using the shortest path, you'll go around the longest path. You can test if two quaternions are
 * opposite by checking if Dot(Q1, Q2) < 0. In that case, you can flip either of them, therefore
 * making Dot(Q1, -Q2) positive.
 *
 * This solution corrects this issue per-vertex before passing the quaternions to OpenGL. This is
 * correct for most cases but can still rotate around the long way sometimes. An implementation
 * which did `lerp(lerp(Q1, Q2), Q3)` (with proper weighting), applying the dot product check
 * between each step would work for those cases at the cost of being more complex to implement.
 *
 * Fortunately however, the 3DS hardware happens to also use this exact same logic to work around
 * these issues, making this basic implementation actually more accurate to the hardware.
 */
static bool AreQuaternionsOpposite(Common::Vec4<Pica::float24> qa, Common::Vec4<Pica::float24> qb) {
#ifdef ARCHITECTURE_ARM64
    const float32_t a[4] = {qa.x.ToFloat32(), qa.y.ToFloat32(), qa.z.ToFloat32(), qa.w.ToFloat32()};
    const float32_t b[4] = {qb.x.ToFloat32(), qb.y.ToFloat32(), qb.z.ToFloat32(), qb.w.ToFloat32()};
    const float32x4_t aa = vld1q_f32(a);
    const float32x4_t bb = vld1q_f32(b);
    const float32x4_t mm = vmulq_f32(aa, bb);
    const float32x2_t s2 = vadd_f32(vget_high_f32(mm), vget_low_f32(mm));
    return (vget_lane_f32(vpadd_f32(s2, s2), 0) < 0.f);
#else
    Common::Vec4f a{qa.x.ToFloat32(), qa.y.ToFloat32(), qa.z.ToFloat32(), qa.w.ToFloat32()};
    Common::Vec4f b{qb.x.ToFloat32(), qb.y.ToFloat32(), qb.z.ToFloat32(), qb.w.ToFloat32()};
    return (Common::Dot(a, b) < 0.f);
#endif
}

void RasterizerOpenGL::AddTriangle(const Pica::Shader::OutputVertex& v0,
                                   const Pica::Shader::OutputVertex& v1,
                                   const Pica::Shader::OutputVertex& v2) {
    if (SoftwareBatching::Active()) {
        SoftwareBatching::Add(software_vertices_since_request,3);
        // The first triangle proves the SW path, but later vertices/indices in
        // this same request may still be unread. Check their full envelope now
        // before a later size-cap flush could publish this request early.
        if (SoftwareBatching::LazySoftwareAdmission(SoftwareBatching::Enabled(),
                SoftwareBatching::CensusEnabled(),vertex_batch.empty(),
                software_has_deferred,software_framebuffer_visibility.pending)) {
            using namespace SoftwareBatching;
            const auto before_texture=stats.unsafe_texture,before_target=stats.unsafe_target_alias,
                       before_input=stats.unsafe_vertex_index;
            Add(stats.first_software_admission_checks);
            software_batch_draw_safe=software_batch_planner.in_list &&
                SoftwareBatchResourcesSafe(software_batch_input_guard_required,software_batch_input_indexed);
            if(!software_batch_draw_safe)FlushSoftwareBatch(stats.unsafe_target_alias!=before_target?Reason::TargetAlias:stats.unsafe_texture!=before_texture?Reason::TextureAlias:stats.unsafe_vertex_index!=before_input?Reason::VertexIndexAlias:Reason::Unsupported);
        }
        if (SoftwareBatching::Enabled() && software_batch_planner.in_list && software_batch_draw_safe &&
            vertex_batch.size()+3>SoftwareBatching::Capacity)
            FlushSoftwareBatch(SoftwareBatching::Reason::SizeCap,false);
    }
    vertex_batch.emplace_back(v0, false);
    vertex_batch.emplace_back(v1, AreQuaternionsOpposite(v0.quat, v1.quat));
    vertex_batch.emplace_back(v2, AreQuaternionsOpposite(v0.quat, v2.quat));
}

static constexpr std::array<GLenum, 4> vs_attrib_types{
    GL_BYTE,          // VertexAttributeFormat::BYTE
    GL_UNSIGNED_BYTE, // VertexAttributeFormat::UBYTE
    GL_SHORT,         // VertexAttributeFormat::SHORT
    GL_FLOAT          // VertexAttributeFormat::FLOAT
};

RasterizerOpenGL::VertexArrayInfo RasterizerOpenGL::AnalyzeVertexArray(bool is_indexed) {
    const auto& regs = Pica::g_state.regs;
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;

    u32 vertex_min;
    u32 vertex_max;
    if (is_indexed) {
        const auto& index_info = regs.pipeline.index_array;
        const PAddr address = vertex_attributes.GetPhysicalBaseAddress() + index_info.offset;
        const u8* index_address_8 = VideoCore::Memory()->GetPhysicalPointer(address);
        const u16* index_address_16 = reinterpret_cast<const u16*>(index_address_8);
        const bool index_u16 = index_info.format != 0;

        vertex_min = 0xFFFF;
        vertex_max = 0;
        // u32 size = regs.pipeline.num_vertices * (index_u16 ? 2 : 1);
        // res_cache.FlushRegion(address, size, nullptr);
        if (index_u16) {
            for (u32 i = 0; i < regs.pipeline.num_vertices; ++i) {
                u32 vertex = index_address_16[i];
                if (vertex < vertex_min)
                    vertex_min = vertex;
                if (vertex > vertex_max)
                    vertex_max = vertex;
            }
        } else {
            for (u32 i = 0; i < regs.pipeline.num_vertices; ++i) {
                u32 vertex = index_address_8[i];
                if (vertex < vertex_min)
                    vertex_min = vertex;
                if (vertex > vertex_max)
                    vertex_max = vertex;
            }
        }
    } else {
        vertex_min = regs.pipeline.vertex_offset;
        vertex_max = regs.pipeline.vertex_offset + regs.pipeline.num_vertices - 1;
    }

    const u32 vertex_num = vertex_max - vertex_min + 1;
    u32 vs_input_size = 0;
    for (const auto& loader : vertex_attributes.attribute_loaders) {
        if (loader.component_count != 0) {
            vs_input_size += loader.byte_count * vertex_num;
        }
    }

    return {vertex_min, vertex_max, vs_input_size};
}

void RasterizerOpenGL::SetupVertexArray(u8* array_ptr, GLintptr buffer_offset,
                                        GLuint vs_input_index_min, GLuint vs_input_index_max) {
    MICROPROFILE_SCOPE(OpenGL_VAO);
    const auto& regs = Pica::g_state.regs;
    const auto& vertex_attributes = regs.pipeline.vertex_attributes;
    PAddr base_address = vertex_attributes.GetPhysicalBaseAddress();

    std::array<bool, 16> enable_attributes{};

    for (const auto& loader : vertex_attributes.attribute_loaders) {
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }

        u32 offset = 0;
        for (u32 comp = 0; comp < loader.component_count && comp < 12; ++comp) {
            u32 attribute_index = loader.GetComponent(comp);
            if (attribute_index < 12) {
                if (vertex_attributes.GetNumElements(attribute_index) != 0) {
                    offset = Common::AlignUp(
                        offset, vertex_attributes.GetElementSizeInBytes(attribute_index));

                    u32 input_reg = regs.vs.GetRegisterForAttribute(attribute_index);
                    GLint size = vertex_attributes.GetNumElements(attribute_index);
                    GLenum type = vs_attrib_types[static_cast<u32>(
                        vertex_attributes.GetFormat(attribute_index))];
                    GLsizei stride = loader.byte_count;
                    glVertexAttribPointer(input_reg, size, type, GL_FALSE, stride,
                                          reinterpret_cast<GLvoid*>(buffer_offset + offset));
                    enable_attributes[input_reg] = true;

                    offset += vertex_attributes.GetStride(attribute_index);
                }
            } else {
                // Attribute ids 12, 13, 14 and 15 signify 4, 8, 12 and 16-byte paddings,
                // respectively
                offset = Common::AlignUp(offset, 4);
                offset += (attribute_index - 11) * 4;
            }
        }

        PAddr data_addr =
            base_address + loader.data_offset + (vs_input_index_min * loader.byte_count);

        u32 vertex_num = vs_input_index_max - vs_input_index_min + 1;
        u32 data_size = loader.byte_count * vertex_num;

        // res_cache.FlushRegion(data_addr, data_size, nullptr);
        std::memcpy(array_ptr, VideoCore::Memory()->GetPhysicalPointer(data_addr), data_size);

        array_ptr += data_size;
        buffer_offset += data_size;
    }

    for (std::size_t i = 0; i < enable_attributes.size(); ++i) {
        if (enable_attributes[i] != hw_vao_enabled_attributes[i]) {
            if (enable_attributes[i]) {
                glEnableVertexAttribArray(i);
            } else {
                glDisableVertexAttribArray(i);
            }
            hw_vao_enabled_attributes[i] = enable_attributes[i];
        }

        if (vertex_attributes.IsDefaultAttribute(i)) {
            const u32 reg = regs.vs.GetRegisterForAttribute(i);
            if (!enable_attributes[reg]) {
                const auto& attr = Pica::g_state.input_default_attributes.attr[i];
                glVertexAttrib4f(reg, attr.x.ToFloat32(), attr.y.ToFloat32(), attr.z.ToFloat32(),
                                 attr.w.ToFloat32());
            }
        }
    }
}

bool RasterizerOpenGL::SetupVertexShader() {
    MICROPROFILE_SCOPE(OpenGL_VS);
    return shader_program_manager->UseProgrammableVertexShader(Pica::g_state.regs,
                                                               Pica::g_state.vs
#ifdef CARA_IOS_GLES
                                                               , ios_lit_pair_vertex_reuse ? 6u : ios_feedback_vertex_reuse ? 5u : ios_direct_vertex_reuse ? 3u : ios_indexed_vertex_inputs, ios_vertex_input_mask
#endif
    );
}

bool RasterizerOpenGL::SetupGeometryShader() {
    MICROPROFILE_SCOPE(OpenGL_GS);
    const auto& regs = Pica::g_state.regs;
    if (regs.pipeline.use_gs == Pica::PipelineRegs::UseGS::No) {
#ifdef CARA_IOS_GLES
        shader_program_manager->UseTrivialGeometryShader();
#else
        shader_program_manager->UseFixedGeometryShader(regs);
#endif
        return true;
    } else {
        LOG_ERROR(Render_OpenGL, "Accelerate draw doesn't support geometry shader");
        return false;
    }
}

bool RasterizerOpenGL::AccelerateDrawBatch(bool is_indexed) {
    if(SoftwareBatching::CensusEnabled())software_census_after_draw=true;
    if(SoftwareBatching::Active()) {software_batch_planner.Hardware();FlushSoftwareBatch(SoftwareBatching::Reason::HardwareDraw,false);}
    const auto& regs = Pica::g_state.regs;
    const auto reject=[&](const char* reason) {
#ifdef CARA_IOS_GLES
        static const bool trace=std::getenv("CARA_VERTEX_FALLBACK_TRACE")!=nullptr;
        if (trace) {
            static std::set<std::string> reported;
            if (reported.insert(reason).second)
                LOG_INFO(Render_OpenGL,"GPU vertex path rejected: {} (vertices={})",reason,u32(regs.pipeline.num_vertices));
        }
#endif
        return false;
    };
    if (regs.pipeline.use_gs != Pica::PipelineRegs::UseGS::No) return reject("geometry shader");
#ifdef CARA_IOS_GLES
    if (PackedPoolObserver::Enabled()) packed_pool_observation = {};
    SCOPE_EXIT({ ios_packed_snapshot_lease.reset(); if (PackedPoolObserver::Enabled()) FinishPackedPoolObservation(); });
    if (!PrepareIOSVertices(is_indexed)) return reject("vertex input preparation");
    static const bool direct_reuse=[] { const char* v=std::getenv("CARA_GPU_VERTEX_REUSE"); return v && std::strcmp(v,"1")==0; }();
    // Quaternion orientation is irrelevant only when fragment lighting is disabled.
    ios_direct_vertex_reuse=direct_reuse && ios_indexed_vertex_inputs==2 && regs.lighting.disable;
    static const bool feedback_reuse=[] { const char* v=std::getenv("CARA_GPU_LIT_VERTEX_REUSE"); return v && std::strcmp(v,"1")==0; }();
    ios_feedback_vertex_reuse=feedback_reuse && ios_indexed_vertex_inputs==2 && !regs.lighting.disable &&
        ios_vertex_input_span>=128 && ios_vertex_input_span<=3072 &&
        ios_vertex_input_span==ios_vertex_input_records &&
        IOSTriangles().size()>=size_t(ios_vertex_input_span)*3;
#endif
#ifdef CARA_IOS_GLES
    static const bool lit_pair_reuse = [] {
        const char* value = std::getenv("CARA_GPU_LIT_PAIR_REUSE");
        return value && std::strcmp(value, "1") == 0;
    }();
    ios_lit_pair_vertex_reuse = lit_pair_reuse && ios_indexed_vertex_inputs == 2 &&
        !regs.lighting.disable && !AllowShadow && !ios_direct_vertex_reuse && !ios_feedback_vertex_reuse &&
        ios_cached_index_data && ios_cached_index_data->lit_pair_count > 0 &&
        ios_cached_index_data->lit_pair_count <= 65535 &&
        ios_cached_index_data->lit_elements.size() == IOSTriangles().size() &&
        std::uint64_t(ios_cached_index_data->pair_reuse.lru32_vs_runs) * 4 <=
            std::uint64_t(ios_cached_index_data->pair_reuse.baseline_vs_runs) * 3;
#endif
    if (!SetupVertexShader()) return reject("vertex shader translation");
    if (!SetupGeometryShader()) return reject("geometry setup");
#ifdef CARA_IOS_GLES
    // Prepared input is current here: PrepareIOSVertices succeeded above and
    // its immutable lease remains alive until the earlier SCOPE_EXIT executes.
    Cara::QuadInputCapture::Scope quad_input_scope;
    if(Cara::QuadInputCapture::Enabled())quad_input_scope.Bind([this,is_indexed](const auto& join){
        namespace Q=Cara::QuadInputCapture;Q::Spec spec;
        spec.mode=ios_indexed_vertex_inputs;spec.indexed=is_indexed;
        spec.span=ios_vertex_input_span;spec.records=ios_vertex_input_records;
        spec.alternate_reuse=ios_direct_vertex_reuse||ios_lit_pair_vertex_reuse||ios_feedback_vertex_reuse;
        const auto& regs=Pica::g_state.regs;const auto& config=regs.vs;
        spec.witness=Q::FromPlan(Cara::QuadUVPlan::Observe(join,Pica::g_state).plan);
        if(spec.mode!=2||is_indexed||spec.span!=4||spec.records!=4||spec.alternate_reuse||!Q::Family(spec.witness))return spec;
        spec.input_mask=ios_vertex_input_mask;spec.shader_register=config.GetRegisterForAttribute(0);
        spec.input_map_low=config.input_attribute_to_register_map_low;spec.input_map_high=config.input_attribute_to_register_map_high;spec.max_input=config.max_input_attribute_index;
        const IOSVertexInputLayout layout(config,ios_vertex_input_mask);
        const auto slot=layout.offsets[spec.shader_register];
        if(slot<0||unsigned(slot)>=layout.stride)return spec;
        spec.selected_attribute=layout.source_attributes[slot];
        const Pica::VertexLoader loader(regs.pipeline);const auto source=loader.GetAttributeLayout(0);
        spec.source_defined=source.elements>0||source.is_default;spec.source_default=source.is_default;
        spec.source_format=static_cast<u32>(source.format);spec.source_elements=source.elements;spec.source_stride=source.stride;
        spec.layout=ios_packed_layout[spec.shader_register];
        if(spec.selected_attribute!=0||!spec.source_defined||spec.source_default||
           source.format!=Pica::PipelineRegs::VertexAttributeFormat::FLOAT||source.elements!=4||source.stride!=16||
           spec.layout[0]<0||spec.layout[1]!=16||spec.layout[2]!=3||spec.layout[3]!=4)return spec;
        spec.data=IOSVertexInputData();spec.bytes=IOSVertexInputBytes();
        const auto& triangles=IOSTriangles();const auto& indices=IOSInputIndices();
        spec.triangle_count=triangles.size();spec.index_count=indices.size();spec.first_vertex=regs.pipeline.vertex_offset;
        if(triangles.size()==6&&indices.size()>=6)for(unsigned i=0;i<6;++i){spec.triangles[i]=triangles[i];spec.indices[i]=indices[i];}
        // Same scalar conversion used by PicaUniformsData::SetFromRegs. No VS runs.
        spec.uniform95_zw={Q::FloatBits(Pica::g_state.vs.uniforms.f[95].z.ToFloat32()),Q::FloatBits(Pica::g_state.vs.uniforms.f[95].w.ToFloat32())};
        spec.viewport_half_raw={regs.rasterizer.viewport_size_x.Value(),regs.rasterizer.viewport_size_y.Value()};
        spec.viewport_half_float_bits={Q::FloatBits(Pica::float24::FromRaw(spec.viewport_half_raw[0]).ToFloat32()),Q::FloatBits(Pica::float24::FromRaw(spec.viewport_half_raw[1]).ToFloat32())};
        spec.viewport_corner={regs.rasterizer.viewport_corner.x.Value(),regs.rasterizer.viewport_corner.y.Value()};
        spec.native_snap_env_present=std::getenv("CARA_UNSNAPPED_VERTICES")!=nullptr;spec.ios_path=true;
#ifdef CARA_MAC_ATLAS_PIPELINE
        spec.atlas_path=true;
#endif
        return spec;
    });
#endif
#ifdef CARA_IOS_GLES
    if(NativeNearestBorrowEnabled()) CaptureNativeNearestBorrow(is_indexed);
#endif
    const bool result=Draw(true,is_indexed);
    if (!result) reject("draw submission");
    return result;
}

#ifdef CARA_IOS_GLES
void RasterizerOpenGL::FinishPackedPoolObservation() {
    using namespace PackedPoolObserver;
    auto& t=packed_pool_observation;
    if (t.state==State::Inactive) return;
    if (t.state==State::Pending) t.state=State::Bypassed;
    struct Totals { std::array<u64,size_t(State::Count)> states{}; u64 draws=0,payload=0,padding=0,index=0,hits=0,merged=0; unsigned logs=0; };
    static Totals totals;
    ++totals.draws; ++totals.states[size_t(t.state)];
    totals.payload+=t.payload_requested; totals.padding+=t.padding_requested; totals.index+=t.index_requested;
    if (t.state==State::Concordant && t.match.input_equal) {++totals.hits; totals.merged+=t.merged;}
    if (totals.logs<32 && (totals.draws & (totals.draws-1))==0) {
        ++totals.logs;
        LOG_INFO(Render_OpenGL,"Packed pool OBSERVATION ONLY frame={} packed_draw_ordinal={} policy_off={} cold_index={} shape={} storage={} sources={} concordant={} owner_changed={} decision_changed={} unsupported_mode={} bypassed={} payload_requested_bytes={} padding_requested_bytes={} index_requested_bytes={} concordant_input_hits={} concordant_merged_source_bytes={} materialization_unchanged=1 no_gain_assertion=1",
            VideoCore::GetCurrentFrame(),totals.draws,totals.states[1],totals.states[2],totals.states[3],totals.states[4],totals.states[5],
            totals.states[7],totals.states[8],totals.states[9],totals.states[10],totals.states[11],totals.payload,totals.padding,totals.index,totals.hits,totals.merged);
    }
    t={};
}

bool RasterizerOpenGL::PrepareIOSPackedVertices(const IOSVertexInputLayout& layout) {
    const auto& pipeline = Pica::g_state.regs.pipeline;
    const auto& triangles = IOSTriangles();
    u32 first, last;
    size_t unique;
    if (ios_cached_index_data) {
        if (!ios_cached_index_data->packable) return false;
        first=ios_cached_index_data->first; last=ios_cached_index_data->last;
        unique=ios_cached_index_data->unique;
    } else {
        const auto bounds = std::minmax_element(triangles.begin(), triangles.end());
        first = *bounds.first; last = *bounds.second;
        if (u64(last) - first >= 65536) return false;
        ios_vertex_source_map.BeginDraw();
        for (u32 vertex : triangles) ios_vertex_source_map.Resolve(u16(vertex-first));
        unique = ios_vertex_source_map.Size();
    }
    BoundedVertexUploadRanges<16> ranges;
    std::array<u64,16> addresses{};
    std::array<bool,16> loaded{};
    std::array<GLvec4,16> constants{};
    Pica::VertexLoader loader(pipeline);
    const u64 base = pipeline.vertex_attributes.GetPhysicalBaseAddress();
    for (unsigned reg=0;reg<16;++reg) {
        if (layout.offsets[reg]<0) continue;
        const unsigned attr=layout.source_attributes[layout.offsets[reg]];
        const auto desc=loader.GetAttributeLayout(attr);
        if (desc.elements) {
            if (desc.elements>4) return false;
            const unsigned format=static_cast<unsigned>(desc.format);
            if (format>3) return false;
            const unsigned bytes=format==3?4:format==2?2:1;
            const u64 start=base+desc.offset+u64(first)*desc.stride;
            const u64 end=start+u64(last-first)*desc.stride+bytes*desc.elements;
            if (end>0x100000000ULL || end<=start || end-start>8*1024*1024) return false;
            if (!ranges.Add(start,end)) return false;
            addresses[reg]=start; loaded[reg]=true;
            ios_packed_layout[reg]={0,GLint(desc.stride),GLint(format),GLint(desc.elements)};
        } else {
            if (desc.is_default) {
                const auto& value=Pica::g_state.input_default_attributes.attr[attr];
                constants[reg]={value.x.ToFloat32(),value.y.ToFloat32(),value.z.ToFloat32(),value.w.ToFloat32()};
            }
            ios_packed_layout[reg]={GLint(reg*sizeof(GLvec4)),0,3,4};
        }
    }
    ranges.SortAndMerge();
    size_t bytes=sizeof(constants);
    for (auto& range:ranges) {
        bytes=(bytes+3)&~size_t(3); range.destination=bytes; bytes+=range.end-range.begin;
    }
    // Bound sparse ranges and avoid transferring more than decoded unique inputs.
    if (bytes>8*1024*1024 || bytes>sizeof(constants)+unique*layout.stride*sizeof(GLvec4)) return false;
    const size_t rows=(bytes+IOS_VERTEX_INPUT_WIDTH*sizeof(GLvec4)-1)/(IOS_VERTEX_INPUT_WIDTH*sizeof(GLvec4));
    if (rows>unsigned(ios_vertex_max_texture_size)) return false;
    static const bool previous_snapshot_trace = [] {
        const char* value = std::getenv("CARA_PACKED_PREVIOUS_SNAPSHOT_TRACE");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (previous_snapshot_trace) {
        // Observation at the original snapshot boundary; NEVER skips materialization.
        using namespace PackedSnapshotCompare;
        struct Totals { u64 draws=0, mismatch=0, equal=0, different=0, invalid=0;
            u64 logical=0, merged=0, padding=0, payload_requested=0, padding_requested=0, avoidable_source=0;
            unsigned logs=0; };
        static Totals totals;
        const size_t logical = rows*IOS_VERTEX_INPUT_WIDTH*sizeof(GLvec4);
        size_t merged = 0;
        for (const auto& range : ranges) merged += range.end-range.begin;
        ++totals.draws; totals.logical += logical; totals.merged += merged;
        totals.padding += logical-sizeof(constants)-merged;
        if (ios_vertex_inputs.size()*sizeof(GLvec4) != logical) ++totals.mismatch;
        else {
            std::array<MemoryRef,16> owners{};
            std::array<Segment,MaxSegments> segments{};
            segments[0] = {reinterpret_cast<const u8*>(constants.data()),sizeof(constants),0,sizeof(constants)};
            size_t count = 1;
            bool valid = true;
            for (const auto& range : ranges) {
                if (count == segments.size()) { valid=false; break; }
                auto& memory = owners[count-1];
                memory = VideoCore::Memory()->GetPhysicalRef(u32(range.begin));
                const size_t size = range.end-range.begin;
                if (!memory || memory.GetSize()<size) { valid=false; break; }
                segments[count++] = {memory.GetPtr(),memory.GetSize(),range.destination,size};
            }
            const auto result = valid ? Compare(segments.data(),count,logical,Padding::Zero,
                reinterpret_cast<const u8*>(ios_vertex_inputs.data()),logical) : Result{};
            totals.payload_requested += result.payload_bytes_requested;
            totals.padding_requested += result.padding_bytes_requested;
            if (result.decision == Decision::Equal) { ++totals.equal; totals.avoidable_source += merged; }
            else if (result.decision == Decision::Different) ++totals.different;
            else ++totals.invalid;
        }
        // At most 32 cumulative observations; no clocks, hashes or cache trust.
        if (totals.logs<32 && (totals.draws & (totals.draws-1))==0) {
            ++totals.logs;
            LOG_INFO(Render_OpenGL, "Packed previous snapshot OBSERVATION ONLY frame={} packed_draw_ordinal={} size_mismatch={} equal={} different={} invalid={} logical_bytes={} merged_source_bytes={} padding_bytes={} payload_requested_bytes={} padding_requested_bytes={} equal_source_copy_bytes={} materialization_unchanged=1 no_gain_assertion=1",
                VideoCore::GetCurrentFrame(),totals.draws,totals.mismatch,totals.equal,totals.different,totals.invalid,
                totals.logical,totals.merged,totals.padding,totals.payload_requested,
                totals.padding_requested,totals.avoidable_source);
        }
    }
    if (PackedPoolObserver::Enabled()) {
        using namespace PackedPoolObserver;
        auto& t=packed_pool_observation;
        const auto enabled=[](const char* key) {const auto* v=std::getenv(key); return v && std::strcmp(v,"1")==0;};
        if (!enabled("CARA_VERTEX_UPLOAD_REUSE") || !enabled("CARA_VERTEX_UPLOAD_ORDER_REUSE")) t.state=State::PolicyOff;
        else if (!ios_cached_index_data) t.state=State::ColdIndex;
        else {
            const auto frame=VideoCore::GetCurrentFrame();
            const auto& bank=ios_vertex_frame_uploads[frame%ios_vertex_frame_uploads.size()];
            const auto* v=std::getenv("CARA_VERTEX_UPLOAD_MAX_DRAWS");
            const auto limit=v?std::clamp(std::strtol(v,nullptr,10),1L,8192L):4096L;
            t=Virtual(frame,ios_vertex_upload_frame,ios_vertex_upload_index,0,unsigned(rows),
                ios_cached_index_data->packed.size(),IOS_VERTEX_INPUT_WIDTH,limit,ios_vertex_max_texture_size);
            if (t.state==State::Pending) {
                t.ordinal=frame==ios_vertex_upload_frame?bank.used[t.size_class]:0;
                std::array<MemoryRef,16> owners{};
                std::array<PackedSnapshotCompare::Segment,17> spans{};
                spans[0]={reinterpret_cast<const u8*>(constants.data()),sizeof(constants),0,sizeof(constants)};
                size_t count=1; t.logical=rows*IOS_VERTEX_INPUT_WIDTH*sizeof(GLvec4);
                for (const auto& range:ranges) {
                    auto& memory=owners[count-1]; memory=VideoCore::Memory()->GetPhysicalRef(u32(range.begin));
                    const size_t length=range.end-range.begin;
                    if (!memory || memory.GetSize()<length) {t.state=State::Sources; break;}
                    spans[count++]={memory.GetPtr(),memory.GetSize(),range.destination,length}; t.merged+=length;
                }
                Probe(t,bank.sizes[t.size_class],spans.data(),count,
                    ios_cached_index_data->packed.data(),ios_cached_index_data->packed.size());
            }
        }
    }
    static const bool snapshot_lease_enabled=[] {
        const auto* value=std::getenv("CARA_PACKED_SNAPSHOT_LEASE");
        return value && std::strcmp(value,"1")==0;
    }();
    static const bool snapshot_lease_trace=[] {
        const auto* value=std::getenv("CARA_PACKED_SNAPSHOT_LEASE_TRACE");
        return value && std::strcmp(value,"1")==0;
    }();
    bool capture_failed=false;
    if (snapshot_lease_enabled && ios_cached_index_data) {
        // Capture CPU bytes at the original snapshot boundary. The later GPU
        // selector still validates/selects/uploads normally using this byte view.
        const auto frame=VideoCore::GetCurrentFrame();
        auto& bank=ios_vertex_frame_uploads[frame%ios_vertex_frame_uploads.size()];
        static const auto limit=[] {
            const auto* value=std::getenv("CARA_VERTEX_UPLOAD_MAX_DRAWS");
            return value?std::clamp(std::strtol(value,nullptr,10),1L,8192L):4096L;
        }();
        auto token=PackedPoolObserver::Virtual(frame,ios_vertex_upload_frame,ios_vertex_upload_index,0,
            unsigned(rows),ios_cached_index_data->packed.size(),IOS_VERTEX_INPUT_WIDTH,limit,ios_vertex_max_texture_size);
        if (token.state==PackedPoolObserver::State::Pending) {
            token.ordinal=frame==ios_vertex_upload_frame?bank.used[token.size_class]:0;
            auto& pool=bank.sizes[token.size_class];
            if (token.ordinal<pool.size()) {
                std::array<MemoryRef,16> owners{};
                std::array<PackedSnapshotCompare::Segment,17> spans{};
                spans[0]={reinterpret_cast<const u8*>(constants.data()),sizeof(constants),0,sizeof(constants)};
                size_t count=1; token.logical=rows*IOS_VERTEX_INPUT_WIDTH*sizeof(GLvec4);
                for (const auto& range:ranges) {
                    auto& memory=owners[count-1]; memory=VideoCore::Memory()->GetPhysicalRef(u32(range.begin));
                    const size_t length=range.end-range.begin;
                    if (!memory || memory.GetSize()<length) {token.state=PackedPoolObserver::State::Sources; break;}
                    spans[count++]={memory.GetPtr(),memory.GetSize(),range.destination,length};
                }
                PackedPoolObserver::Probe(token,pool,spans.data(),count,
                    ios_cached_index_data->packed.data(),ios_cached_index_data->packed.size());
                if (token.state==PackedPoolObserver::State::Pending && token.match.input_equal) {
                    ios_packed_snapshot_lease=pool[token.match.selected].input_shadow.CaptureLease();
                    if (snapshot_lease_trace && !ios_packed_snapshot_lease) capture_failed=true;
                }
            }
        }
    }
    if (snapshot_lease_trace) {
        struct Totals { u64 calls=0,materialized=0,captured=0,logical=0,skipped_source=0,failed=0; unsigned logs=0; };
        static Totals totals;
        ++totals.calls; totals.failed+=capture_failed;
        if (ios_packed_snapshot_lease) {
            ++totals.captured; totals.logical+=ios_packed_snapshot_lease->size();
            for (const auto& range:ranges) totals.skipped_source+=range.end-range.begin;
        } else ++totals.materialized;
        if (totals.logs<32 && (totals.calls & (totals.calls-1))==0) {
            ++totals.logs;
            LOG_INFO(Render_OpenGL,"Packed snapshot lease frame={} packed_call_ordinal={} materialized={} captured={} capture_fail={} captured_logical_bytes={} skipped_merged_source_bytes={} cpu_snapshot_only=1 no_gain_assertion=1",
                VideoCore::GetCurrentFrame(),totals.calls,totals.materialized,totals.captured,
                totals.failed,totals.logical,totals.skipped_source);
        }
    }
    static const bool padding_only_zero=PackedVertexPaddingOnlyEnabled();
    u8* materialized=nullptr;
    if (!ios_packed_snapshot_lease) {
        ios_vertex_inputs.resize(rows*IOS_VERTEX_INPUT_WIDTH);
        materialized=reinterpret_cast<u8*>(ios_vertex_inputs.data());
        if (!padding_only_zero ||
            !ranges.ClearPadding(materialized,ios_vertex_inputs.size()*sizeof(GLvec4),sizeof(constants)))
            std::memset(materialized,0,ios_vertex_inputs.size()*sizeof(GLvec4));
        std::memcpy(materialized,constants.data(),sizeof(constants));
    }
    const u8* output=IOSVertexInputData();
    for (const auto& range:ranges) {
        if (!ios_packed_snapshot_lease) {
            const auto memory=VideoCore::Memory()->GetPhysicalRef(u32(range.begin));
            if (!memory || memory.GetSize()<range.end-range.begin) return false;
            std::memcpy(materialized+range.destination,memory.GetPtr(),range.end-range.begin);
        }
        for (unsigned reg=0;reg<16;++reg)
            if (loaded[reg] && addresses[reg]>=range.begin && addresses[reg]<range.end)
                ios_packed_layout[reg][0]=GLint(range.destination+addresses[reg]-range.begin);
    }
    if (!ios_cached_index_data) {
        ios_vertex_input_indices.resize(((triangles.size()+IOS_VERTEX_INPUT_WIDTH-1)/IOS_VERTEX_INPUT_WIDTH)*IOS_VERTEX_INPUT_WIDTH);
        for (size_t i=0;i<triangles.size();++i) ios_vertex_input_indices[i]=u16(triangles[i]-first);
        std::fill(ios_vertex_input_indices.begin()+triangles.size(),ios_vertex_input_indices.end(),0);
    }
    ios_vertex_input_records=static_cast<u32>(unique);
    ios_vertex_input_span=last-first+1;
    static const bool verify=std::getenv("CARA_PACKED_VERIFY")!=nullptr;
    if (verify) {
        ios_vertex_source_map.BeginDraw();
        Pica::DebugUtils::MemoryAccessTracker accesses;
        for (u32 vertex:triangles) {
            if (!ios_vertex_source_map.Resolve(u16(vertex-first)).inserted) continue;
            Pica::Shader::AttributeBuffer input{};
            loader.LoadVertex(u32(base),0,vertex,input,accesses);
            std::array<GLvec4,16> reference;
            layout.PackAttributes(input,reference);
            for (unsigned reg=0;reg<16;++reg) {
                if (layout.offsets[reg]<0) continue;
                const auto& info=ios_packed_layout[reg];
                const unsigned width=info[2]==3?4:info[2]==2?2:1;
                for (unsigned c=0;c<4;++c) {
                    float actual=c==3?1.f:0.f;
                    if (c<unsigned(info[3])) {
                        const u8* source=output+info[0]+size_t(vertex-first)*info[1]+c*width;
                        if (info[2]==0) { s8 v; std::memcpy(&v,source,1); actual=v; }
                        if (info[2]==1) { u8 v; std::memcpy(&v,source,1); actual=v; }
                        if (info[2]==2) { s16 v; std::memcpy(&v,source,2); actual=v; }
                        if (info[2]==3) std::memcpy(&actual,source,4);
                    }
                    const float expected=reference[layout.offsets[reg]][c];
                    if (std::memcmp(&actual,&expected,4)!=0) {
                        ++Common::FrameTiming::counters.packed_validation_failures;
                        LOG_ERROR(Render_OpenGL,"Packed input validation failed register {} component {}",reg,c);
                        return false;
                    }
                }
            }
        }
        ++Common::FrameTiming::counters.packed_verified_draws;
    }
    return true;
}

bool RasterizerOpenGL::PrepareIOSVertices(bool is_indexed) {
    if(NativeNearestBorrowEnabled()){++native_borrow_input_epoch;native_borrow_prepared=false;}
    ios_packed_snapshot_lease.reset();
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::VertexPrepare);
    constexpr u32 MAX_VERTICES = IOS_MAX_EXPANDED_VERTICES;
    const auto& regs = Pica::g_state.regs;
    const auto& pipeline = regs.pipeline;
    const u32 count = pipeline.num_vertices;
    const auto topology = Pica::g_state.primitive_assembler.GetTopology();
    const bool strip = topology == Pica::PipelineRegs::TriangleTopology::Strip ||
                       topology == Pica::PipelineRegs::TriangleTopology::Fan;
    if (count < 3 || count > (strip ? MAX_VERTICES / 3 : MAX_VERTICES)) return false;
    const u32 base = pipeline.vertex_attributes.GetPhysicalBaseAddress();
    const u32 index_address=base+pipeline.index_array.offset;
    const size_t source_size=is_indexed ? size_t(count)*(pipeline.index_array.format?2:1) : 0;
    const u8* indices=nullptr;
    if (is_indexed) {
        const auto memory=VideoCore::Memory()->GetPhysicalRef(index_address);
        if (!memory || memory.GetSize()<source_size) return false;
        indices=memory.GetPtr();
    }
    static const bool cache_enabled=[] {
        const char* value=std::getenv("CARA_VERTEX_INDEX_CACHE");
        return value && std::strcmp(value,"1")==0;
    }();
    static const bool pair_probe_enabled = [] {
        const char* value = std::getenv("CARA_GPU_LIT_PAIR_PROBE");
        return value && std::strcmp(value, "1") == 0;
    }();
    static const bool build_lit_pairs = [] {
        const char* value = std::getenv("CARA_GPU_LIT_PAIR_REUSE");
        return value && std::strcmp(value, "1") == 0;
    }();
    ios_cached_index_data=nullptr;
    const IOSVertexIndexCache::Key key{is_indexed?index_address:0,count,
        is_indexed?0:u32(pipeline.vertex_offset),static_cast<u32>(topology),
        is_indexed?(pipeline.index_array.format?2u:1u):0u};
    if (cache_enabled) ios_cached_index_data=ios_vertex_index_cache.Find(key,indices,source_size);
    if (Common::FrameTiming::counters.enabled && cache_enabled) {
        if (ios_cached_index_data) ++Common::FrameTiming::counters.vertex_index_cache_hits;
        else ++Common::FrameTiming::counters.vertex_index_cache_misses;
    }
    if (!ios_cached_index_data) {
        ios_triangle_indices.clear();
        Pica::PrimitiveAssembler<u32> assembler(topology);
        const auto triangle = [this](u32 a, u32 b, u32 c) {
            ios_triangle_indices.insert(ios_triangle_indices.end(), {a, b, c});
        };
        for (u32 i = 0; i < count; ++i) {
            u32 vertex=i+pipeline.vertex_offset;
            if (is_indexed) {
                if (pipeline.index_array.format) { u16 value; std::memcpy(&value,indices+i*2,2); vertex=value; }
                else vertex=indices[i];
            }
            assembler.SubmitVertex(vertex, triangle);
        }
        if (ios_triangle_indices.empty()) return false;
        if (cache_enabled)
            ios_cached_index_data=ios_vertex_index_cache.Store(key,indices,source_size,ios_triangle_indices, pair_probe_enabled, build_lit_pairs);
    }
    const auto& triangles=IOSTriangles();
    if (Common::FrameTiming::counters.enabled)
        Common::FrameTiming::counters.vertex_index_cache_bytes=ios_vertex_index_cache.Bytes();
    static const bool used_inputs = [] {
        const char* value = std::getenv("CARA_GPU_USED_INPUTS");
        return value && std::strcmp(value, "1") == 0;
    }();
    ios_vertex_input_mask = IOSVertexInputLayout::MaskFromRegs(regs.vs);
    if (used_inputs) ios_vertex_input_mask &=
        shader_program_manager->GetIOSVertexInputReadMask(regs, Pica::g_state.vs);
    const IOSVertexInputLayout layout(regs.vs, ios_vertex_input_mask);
    if (Common::FrameTiming::counters.enabled) {
        auto& counters = Common::FrameTiming::counters;
        ++counters.vertex_input_stage_draws;
        const auto configured = IOSVertexInputLayout::MaskFromRegs(regs.vs);
        for (unsigned reg = 0; reg < 16; ++reg) {
            counters.vertex_configured_input_registers += (configured >> reg) & 1u;
            counters.vertex_selected_input_registers += (ios_vertex_input_mask >> reg) & 1u;
        }
        counters.vertex_pruned_input_draws += configured != ios_vertex_input_mask;
    }
    static const bool indexed_enabled = [] {
        const char* mode = std::getenv("CARA_GPU_INDEXED_INPUTS");
        return !mode || std::strcmp(mode, "0") != 0;
    }();
    ios_indexed_vertex_inputs = indexed_enabled && (is_indexed || strip);
    ios_vertex_input_records = static_cast<u32>(triangles.size());
    ios_vertex_input_indices.clear();
    static const bool packed_enabled = [] {
        const char* mode = std::getenv("CARA_GPU_PACKED_INPUTS");
        return mode && std::strcmp(mode,"1") == 0;
    }();
    if (ios_indexed_vertex_inputs && packed_enabled && PrepareIOSPackedVertices(layout)) {
        ios_indexed_vertex_inputs = 2;
        return true;
    }
    // Packed verification/preparation may fail after capture; decoded fallback
    // must use its own newly materialized input, never the abandoned lease.
    ios_packed_snapshot_lease.reset();
    if (ios_indexed_vertex_inputs) {
        ios_vertex_source_map.BeginDraw();
        ios_vertex_inputs.clear();
        ios_vertex_inputs.reserve(triangles.size() * layout.stride);
        const size_t index_rows = (triangles.size() + IOS_VERTEX_INPUT_WIDTH - 1) / IOS_VERTEX_INPUT_WIDTH;
        ios_vertex_input_indices.resize(index_rows * IOS_VERTEX_INPUT_WIDTH);
        Pica::VertexLoader loader(pipeline);
        Pica::DebugUtils::MemoryAccessTracker accesses;
        for (size_t i = 0; i < triangles.size(); ++i) {
            const u32 vertex = triangles[i];
            const u32 key = is_indexed ? vertex : vertex - u32(pipeline.vertex_offset);
            if (key >= 65536) return false;
            const auto record = ios_vertex_source_map.Resolve(static_cast<u16>(key));
            ios_vertex_input_indices[i] = record.index;
            if (record.inserted) {
                Pica::Shader::AttributeBuffer input{};
                loader.LoadVertex(base, static_cast<int>(i), vertex, input, accesses);
                std::array<GLvec4, 16> packed;
                layout.PackAttributes(input, packed);
                ios_vertex_inputs.insert(ios_vertex_inputs.end(), packed.begin(), packed.begin() + layout.stride);
            }
        }
        ios_vertex_input_records = ios_vertex_source_map.Size();
        const size_t rows = (ios_vertex_inputs.size() + IOS_VERTEX_INPUT_WIDTH - 1) / IOS_VERTEX_INPUT_WIDTH;
        if (IOS_VERTEX_INPUT_WIDTH > unsigned(ios_vertex_max_texture_size) ||
            std::max(rows, index_rows) > unsigned(ios_vertex_max_texture_size)) return false;
        ios_vertex_inputs.resize(rows * IOS_VERTEX_INPUT_WIDTH);
        return true;
    }
    const size_t rows = (triangles.size() * layout.stride + IOS_VERTEX_INPUT_WIDTH - 1) / IOS_VERTEX_INPUT_WIDTH;
    if (IOS_VERTEX_INPUT_WIDTH>unsigned(ios_vertex_max_texture_size) ||
        rows>unsigned(ios_vertex_max_texture_size)) return false;
    // Optional probe only: measure reuse before choosing a new GPU input layout.
    // No vertex data or command stream is modified by this diagnostic.
    static const bool probe_reuse = std::getenv("CARA_VERTEX_REUSE_TRACE") != nullptr;
    if (Common::FrameTiming::counters.enabled && probe_reuse) {
        std::bitset<65536> seen;
        bool valid = true;
        for (u32 vertex : triangles) {
            const u32 key = is_indexed ? vertex : vertex - u32(pipeline.vertex_offset);
            if (key >= seen.size()) { valid = false; break; }
            seen.set(key);
        }
        auto& counters = Common::FrameTiming::counters;
        if (valid) {
            const size_t unique = seen.count();
            counters.vertex_reuse_expanded += triangles.size();
            counters.vertex_reuse_unique += unique;
            counters.vertex_reuse_attribute_bytes += triangles.size() * layout.stride * sizeof(GLvec4);
            counters.vertex_reuse_unique_attribute_bytes += unique * layout.stride * sizeof(GLvec4);
        } else ++counters.vertex_reuse_invalid_draws;
    }
    ios_vertex_inputs.resize(rows * IOS_VERTEX_INPUT_WIDTH);
    Pica::VertexLoader loader(pipeline);
    Pica::DebugUtils::MemoryAccessTracker accesses;
    // Expanded triangles reuse source vertices, especially strips/fans. Cache only
    // their decoded inputs within this draw; GPU shader evaluation remains unchanged.
    std::array<bool, 64> valid{};
    std::array<u32, 64> ids;
    std::array<std::array<GLvec4, 16>, 64> inputs;
    for (size_t i = 0; i < triangles.size(); ++i) {
        const u32 vertex = triangles[i];
        const u32 slot = vertex & 63;
        if (!valid[slot] || ids[slot] != vertex) {
            Pica::Shader::AttributeBuffer input{};
            loader.LoadVertex(base, static_cast<int>(i), vertex, input, accesses);
            layout.PackAttributes(input, inputs[slot]);
            ids[slot] = vertex; valid[slot] = true;
        }
        std::copy_n(inputs[slot].begin(), layout.stride,
                    ios_vertex_inputs.begin() + i * layout.stride);
    }
    return true;
}
#endif

void RasterizerOpenGL::CheckForConfigChanges() {
    FlushSoftwareBatch(SoftwareBatching::Reason::ConfigOrFrame);
    u16 scale_factor = VideoCore::GetResolutionScaleFactor();
    if (res_cache.GetScaleFactor() != scale_factor) {
        OpenGLState::BindDrawFramebuffer(framebuffer.handle);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        framebuffer_info = {};
        res_cache.SetScaleFactor(scale_factor);
    }
}

void RasterizerOpenGL::OnFrameUpdate() {
    FlushSoftwareBatch(SoftwareBatching::Reason::ConfigOrFrame);
    const u32 current_frame = VideoCore::GetCurrentFrame();
    if (current_frame >= last_clean_frame + CLEAN_FRAME_INTERVAL) {
        res_cache.CleanUp(last_clean_frame);
        last_clean_frame = current_frame;
    }
}

static GLenum GetCurrentPrimitiveMode() {
    const GLenum prims[] = {GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_TRIANGLES};
    return prims[static_cast<u32>(Pica::g_state.regs.pipeline.triangle_topology.Value())];
}

bool RasterizerOpenGL::AccelerateDrawBatchInternal(bool is_indexed) {
#ifdef CARA_IOS_GLES
    const auto* input_data=IOSVertexInputData();
    const size_t input_bytes=IOSVertexInputBytes();
    const auto& input_indices = ios_lit_pair_vertex_reuse ? ios_cached_index_data->lit_pairs : IOSInputIndices();
    const auto& element_indices = ios_lit_pair_vertex_reuse ? ios_cached_index_data->lit_elements : IOSInputIndices();
    // Identity is ONLY the current immutable cached packed-index payload. It is
    // not a guest-address, input snapshot, VS output or register-generation hint.
    const IndexPayloadReuse::Identity current_index_identity =
        IndexPayloadReuse::Enabled() && ios_cached_index_data && ios_indexed_vertex_inputs == 2 &&
        !ios_direct_vertex_reuse && !ios_lit_pair_vertex_reuse && !ios_feedback_vertex_reuse &&
        input_indices.data() == ios_cached_index_data->packed.data() &&
        input_indices.size() == ios_cached_index_data->packed.size() ?
            ios_cached_index_data->identity : IndexPayloadReuse::Identity{};
    const size_t element_bytes = (ios_lit_pair_vertex_reuse || ios_direct_vertex_reuse) ?
        element_indices.size() * sizeof(u16) : 0;
    const GLenum index_internal = ios_lit_pair_vertex_reuse ? GL_RG16UI : GL_R16UI;
    const GLenum index_format = ios_lit_pair_vertex_reuse ? GL_RG_INTEGER : GL_RED_INTEGER;
    const auto& triangles=IOSTriangles();
    GLuint feedback_program=0;
    if (ios_feedback_vertex_reuse) {
        feedback_program=shader_program_manager->GetIOSVertexFeedbackProgram(Pica::g_state.regs,Pica::g_state.vs,ios_vertex_input_mask);
        if (!feedback_program) {
            ios_feedback_vertex_reuse=false;
            if (!SetupVertexShader()) return false;
        }
    }
    state.draw.vertex_array = hw_vao.handle;
    {
        Common::FrameTiming::Scope shader_setup(Common::FrameTiming::Phase::DrawShaderSetup,
                                                   Common::FrameTiming::RenderBookkeepingProfileEnabled());
        shader_program_manager->ApplyTo(state);
    }
    if (!state.draw.shader_program) return false;
    state.Apply();
    const bool raw_inputs = ios_indexed_vertex_inputs == 2;
    if (PackedPoolObserver::Enabled() && packed_pool_observation.state==PackedPoolObserver::State::Pending &&
        (!raw_inputs || ios_direct_vertex_reuse || ios_lit_pair_vertex_reuse || ios_feedback_vertex_reuse))
        packed_pool_observation.state=PackedPoolObserver::State::Mode;
    const GLint input_internal = raw_inputs ? GL_RGBA32UI : GL_RGBA32F;
    const GLenum input_format = raw_inputs ? GL_RGBA_INTEGER : GL_RGBA;
    const GLenum input_type = raw_inputs ? GL_UNSIGNED_INT : GL_FLOAT;
    if (raw_inputs) {
        static const bool reuse_layout=[] { const char* v=std::getenv("CARA_GL_STATE_REUSE"); return v && std::strcmp(v,"1")==0; }();
        if (reuse_layout) {
            auto& cached=ios_layout_uniforms[state.draw.shader_program];
            if (!cached.initialized) cached.location=glGetUniformLocation(state.draw.shader_program,"cara_vertex_layout[0]");
            if (!cached.initialized || cached.values!=ios_packed_layout) {
                glUniform4iv(cached.location,16,ios_packed_layout[0].data());
                cached.values=ios_packed_layout; cached.initialized=true;
            }
        } else glUniform4iv(glGetUniformLocation(state.draw.shader_program,"cara_vertex_layout[0]"),16,ios_packed_layout[0].data());
    }
    glActiveTexture(GL_TEXTURE7);
    const unsigned upload_rows = static_cast<unsigned>(input_bytes / (IOS_VERTEX_INPUT_WIDTH*sizeof(GLvec4)));
    OGLTexture* selected_texture = &ios_vertex_fallback_texture;
    OGLTexture* selected_indices = &ios_vertex_index_fallback_texture;
    IOSVertexUpload* selected_upload = nullptr;
    bool order_reuse_checked = false;
    bool order_reuse_input = false, order_reuse_index = false;
    static const bool self_equal_enabled = [] {
        const char* value = std::getenv("CARA_VERTEX_UPLOAD_SELF_EQUAL");
        return value && std::strcmp(value, "1") == 0;
    }();
    const bool input_self_equal = self_equal_enabled && bool(ios_packed_snapshot_lease);
    static const bool order_reuse_enabled = [] {
        const char* value = std::getenv("CARA_VERTEX_UPLOAD_ORDER_REUSE");
        return value && std::strcmp(value, "1") == 0;
    }();
    static const bool upload_reuse_enabled = [] {
        const char* value = std::getenv("CARA_VERTEX_UPLOAD_REUSE");
        return value && std::strcmp(value, "1") == 0;
    }();
    const unsigned index_rows = static_cast<unsigned>(input_indices.size() /
        (IOS_VERTEX_INPUT_WIDTH * (ios_lit_pair_vertex_reuse ? 2 : 1)));
    const unsigned element_rows = static_cast<unsigned>((element_indices.size() + IOS_VERTEX_INPUT_WIDTH - 1) / IOS_VERTEX_INPUT_WIDTH);
    unsigned storage_rows = std::max({upload_rows, index_rows, ios_lit_pair_vertex_reuse ? element_rows : 0u});
    bool resize_storage = false;
    bool orphan_upload = true;
    const unsigned frame = VideoCore::GetCurrentFrame();
    auto& bank = ios_vertex_frame_uploads[frame % ios_vertex_frame_uploads.size()];
    if (frame != ios_vertex_upload_frame) {
        ios_vertex_upload_frame = frame;
        ios_vertex_upload_index = 0;
        bank.used.fill(0);
    }
    // Pool textures by exact power-of-two height, rather than by draw position.
    // Adjacent frames can reorder small and large draws without reallocating or
    // retaining large storage for a draw that now needs only a few rows.
    static const unsigned max_upload_draws = [] {
        const char* value = std::getenv("CARA_VERTEX_UPLOAD_MAX_DRAWS");
        return value ? unsigned(std::clamp(std::strtol(value, nullptr, 10), 1L, 8192L)) : 4096u;
    }();
    static const size_t max_bank_bytes = [] {
        const char* value = std::getenv("CARA_VERTEX_UPLOAD_BANK_MIB");
        const size_t mib = value ? size_t(std::clamp(std::strtol(value, nullptr, 10), 16L, 128L)) : 64u;
        return mib * 1024 * 1024;
    }();
    unsigned size_class = 0;
    unsigned rows = 1;
    while (rows < std::max({upload_rows, index_rows, ios_lit_pair_vertex_reuse ? element_rows : 0u})) { rows *= 2; ++size_class; }
    constexpr unsigned height_classes = static_cast<unsigned>(IOSVertexUploadBank::HeightClasses);
    const bool supported_class = size_class < height_classes;
    size_class = static_cast<unsigned>(IOSVertexUploadBank::ClassIndex(
        ios_lit_pair_vertex_reuse ? IOSVertexUploadBank::LitPairFormat : ios_direct_vertex_reuse ? 3 : ios_indexed_vertex_inputs, size_class));
    if (supported_class && ios_vertex_upload_index++ < max_upload_draws && size_class < bank.sizes.size() &&
        rows<=unsigned(ios_vertex_max_texture_size)) {
        auto& pool = bank.sizes[size_class];
        const size_t bytes = IOSVertexUploadBank::AllocationBytes(size_class, IOS_VERTEX_INPUT_WIDTH);
        if (bank.used[size_class] == pool.size()) {
            // Reclaim only allocations not yet used in this frame. Largest sizes
            // first free space promptly, without modifying in-flight draw inputs.
            IOSVertexUploadBank::Reclaim(bank, bytes, max_bank_bytes,
                                        max_upload_draws, IOS_VERTEX_INPUT_WIDTH);
            if (bank.bytes + bytes <= max_bank_bytes && bank.textures < max_upload_draws) {
                pool.emplace_back();
                pool.back().rows = rows;
                bank.bytes += bytes;
                ++bank.textures;
                resize_storage = true;
            }
        }
        if (bank.used[size_class] < pool.size()) {
            const size_t ordinal = bank.used[size_class];
            // Only swap unused entries within this exact format/height class.
            // Complete entries move together: textures, shadows and ownership.
            // No GL resource is changed; each entry remains single-use this frame.
            if (order_reuse_enabled && upload_reuse_enabled && raw_inputs &&
                !ios_direct_vertex_reuse && !ios_lit_pair_vertex_reuse &&
                !ios_feedback_vertex_reuse && !resize_storage) {
                Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::VertexReuseCheck);
                const auto eligible = [rows](const IOSVertexUpload& entry) {
                    return entry.rows == rows && entry.texture.handle && entry.indices.handle;
                };
                const auto match = IndexPayloadReuse::Enabled() ?
                    IOSVertexUploadOrderReuse::FindWithIndexIdentity(pool, ordinal, 32, input_data,
                        input_bytes, input_indices.data(), input_indices.size() * sizeof(u16), eligible,
                        current_index_identity, input_self_equal) : input_self_equal ?
                    IOSVertexUploadOrderReuse::FindWithInputSelfEquality(pool, ordinal, 32, input_data,
                        input_bytes, input_indices.data(), input_indices.size() * sizeof(u16), eligible) :
                    IOSVertexUploadOrderReuse::Find(pool, ordinal, 32, input_data,
                        input_bytes, input_indices.data(), input_indices.size() * sizeof(u16), eligible);
                order_reuse_checked = true;
                order_reuse_input = match.input_equal;
                order_reuse_index = match.index_equal;
                if (PackedPoolObserver::Enabled() && packed_pool_observation.state==PackedPoolObserver::State::Pending)
                    packed_pool_observation.state=PackedPoolObserver::Actual(packed_pool_observation,frame,size_class,
                        ordinal,ios_vertex_upload_index-1,match,pool[match.selected].texture.handle,pool[match.selected].indices.handle);
                if (match.selected != ordinal) std::swap(pool[ordinal], pool[match.selected]);
                // Bounded diagnostic proof; frame_timing can expose these later.
                struct Totals { u64 checks=0, candidates=0, full=0, swaps=0; unsigned logs=0, frame=~0u; };
                static Totals totals;
                ++totals.checks; totals.candidates += match.candidates;
                totals.full += match.full_comparisons; totals.swaps += match.selected != ordinal;
                if (totals.logs < 24 && totals.frame != frame && frame % 120 == 0) {
                    totals.frame = frame; ++totals.logs;
                    LOG_INFO(Render_OpenGL, "Vertex order reuse frame={} checks={} candidates={} full_comparisons={} swaps={}",
                             frame, totals.checks, totals.candidates, totals.full, totals.swaps);
                }
            }
            auto& upload = pool[bank.used[size_class]++];
            storage_rows = upload.rows;
            selected_texture = &upload.texture;
            selected_indices = &upload.indices;
            selected_upload = &upload;
            orphan_upload = false;
        }
    }
    if (Common::FrameTiming::counters.enabled) {
        auto& counters = Common::FrameTiming::counters;
        counters.vertex_upload_bank_budget_bytes = max_bank_bytes;
        const size_t index_bytes = input_indices.size() * sizeof(u16) + (ios_lit_pair_vertex_reuse ? element_bytes : 0);
        counters.vertex_upload_bytes += input_bytes + index_bytes;
        counters.vertex_index_upload_bytes += index_bytes;
        if (orphan_upload) {
            ++counters.vertex_upload_fallbacks;
            counters.vertex_orphan_upload_bytes += input_bytes + index_bytes;
        }
        if (orphan_upload || resize_storage) counters.vertex_storage_allocations += ios_lit_pair_vertex_reuse ? 3 : ios_indexed_vertex_inputs ? 2 : 1;
        size_t bytes = 0;
        for (const auto& frame_bank : ios_vertex_frame_uploads) bytes += frame_bank.bytes;
        counters.vertex_pool_peak_bytes = std::max<std::uint64_t>(counters.vertex_pool_peak_bytes, bytes);
    }
    OGLTexture& input_texture = *selected_texture;
    if (!input_texture.handle) {
        input_texture.Create();
        glBindTexture(GL_TEXTURE_2D, input_texture.handle);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (!orphan_upload) resize_storage = true;
    } else {
        glBindTexture(GL_TEXTURE_2D, input_texture.handle);
    }
    glBindSampler(7, 0);
    if (ios_indexed_vertex_inputs && !ios_direct_vertex_reuse) {
        glActiveTexture(GL_TEXTURE8);
        if (!selected_indices->handle) {
            if (selected_upload && IndexPayloadReuse::Enabled())
                selected_upload->index_shadow.InvalidateIdentity();
            selected_indices->Create();
            glBindTexture(GL_TEXTURE_2D, selected_indices->handle);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (!orphan_upload) resize_storage = true;
        } else glBindTexture(GL_TEXTURE_2D, selected_indices->handle);
        glBindSampler(8, 0);
        glActiveTexture(GL_TEXTURE7);
    }
    const bool cache_inputs=raw_inputs && upload_reuse_enabled && selected_upload;
    const size_t index_bytes=input_indices.size()*sizeof(u16);
    bool reuse_input=false,reuse_index=false,reuse_elements=false;
    if (cache_inputs && !resize_storage) {
        Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::VertexReuseCheck);
        const auto matches=[](const auto& shadow,const void* data,size_t bytes) {
            return shadow.size()==bytes && bytes && std::memcmp(shadow.data(),data,bytes)==0;
        };
        size_t input_full_comparisons = 0;
        reuse_input=order_reuse_checked ? order_reuse_input : input_self_equal ?
            IOSVertexUploadOrderReuse::EqualInputSelf(selected_upload->input_shadow,input_data,input_bytes,
                                                     input_full_comparisons,false) :
            matches(selected_upload->input_shadow,input_data,input_bytes);
        reuse_index=order_reuse_checked ? order_reuse_index : IndexPayloadReuse::Enabled() ?
            IOSVertexUploadOrderReuse::EqualIndexIdentity(selected_upload->index_shadow,
                input_indices.data(),index_bytes,current_index_identity,input_full_comparisons,false) :
            matches(selected_upload->index_shadow,input_indices.data(),index_bytes);
        if (ios_lit_pair_vertex_reuse) reuse_elements=matches(selected_upload->elements_shadow,element_indices.data(),element_bytes);
    }
    if (Common::FrameTiming::counters.enabled) {
        auto& counters=Common::FrameTiming::counters;
        const size_t skipped=(reuse_input?input_bytes:0)+(reuse_index?index_bytes:0)+(reuse_elements?element_bytes:0);
        counters.vertex_upload_bytes-=skipped;
        if (reuse_index) counters.vertex_index_upload_bytes-=index_bytes;
        if (reuse_elements) counters.vertex_index_upload_bytes-=element_bytes;
        counters.vertex_upload_reused_bytes+=skipped;
        counters.vertex_upload_reused_textures+=unsigned(reuse_input)+unsigned(reuse_index);
    }
    if (!reuse_input) {
        Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::VertexUpload);
        if (orphan_upload) {
            Common::FrameTiming::Scope orphan_timing(Common::FrameTiming::Phase::VertexOrphanUpload);
            if (Common::FrameTiming::counters.enabled) ++Common::FrameTiming::counters.vertex_gl_image_calls;
            glTexImage2D(GL_TEXTURE_2D, 0, input_internal, IOS_VERTEX_INPUT_WIDTH, upload_rows, 0,
                         input_format, input_type, input_data);
        } else {
            if (resize_storage) {
                Common::FrameTiming::Scope storage_timing(Common::FrameTiming::Phase::VertexStorageAllocate);
                if (Common::FrameTiming::counters.enabled) ++Common::FrameTiming::counters.vertex_gl_image_calls;
                glTexImage2D(GL_TEXTURE_2D, 0, input_internal, IOS_VERTEX_INPUT_WIDTH, storage_rows, 0,
                             input_format, input_type, nullptr);
            }
            Common::FrameTiming::Scope update_timing(Common::FrameTiming::Phase::VertexSubImageUpload);
            if (Common::FrameTiming::counters.enabled) ++Common::FrameTiming::counters.vertex_gl_subimage_calls;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, IOS_VERTEX_INPUT_WIDTH, upload_rows,
                            input_format, input_type, input_data);
        }
    }
    if (ios_indexed_vertex_inputs && !ios_direct_vertex_reuse && !reuse_index) {
        if (selected_upload && IndexPayloadReuse::Enabled())
            selected_upload->index_shadow.InvalidateIdentity();
        Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::VertexUpload);
        glActiveTexture(GL_TEXTURE8);
        if (orphan_upload) {
            Common::FrameTiming::Scope orphan_timing(Common::FrameTiming::Phase::VertexOrphanUpload);
            if (Common::FrameTiming::counters.enabled) ++Common::FrameTiming::counters.vertex_gl_image_calls;
            glTexImage2D(GL_TEXTURE_2D, 0, index_internal, IOS_VERTEX_INPUT_WIDTH, index_rows, 0,
                         index_format, GL_UNSIGNED_SHORT, input_indices.data());
        } else {
            if (resize_storage) {
                Common::FrameTiming::Scope storage_timing(Common::FrameTiming::Phase::VertexStorageAllocate);
                if (Common::FrameTiming::counters.enabled) ++Common::FrameTiming::counters.vertex_gl_image_calls;
                glTexImage2D(GL_TEXTURE_2D, 0, index_internal, IOS_VERTEX_INPUT_WIDTH, storage_rows, 0,
                             index_format, GL_UNSIGNED_SHORT, nullptr);
            }
            Common::FrameTiming::Scope update_timing(Common::FrameTiming::Phase::VertexSubImageUpload);
            if (Common::FrameTiming::counters.enabled) ++Common::FrameTiming::counters.vertex_gl_subimage_calls;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, IOS_VERTEX_INPUT_WIDTH, index_rows,
                            index_format, GL_UNSIGNED_SHORT, input_indices.data());
        }
    }
    OGLBuffer* elements=nullptr;
    if (ios_direct_vertex_reuse || ios_lit_pair_vertex_reuse) {
        elements=selected_upload?&selected_upload->elements:&ios_vertex_element_fallback;
        const bool created=!elements->handle;
        if (created) elements->Create();
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,elements->handle);
        if (created || !(ios_lit_pair_vertex_reuse ? reuse_elements : reuse_index)) {
            Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::VertexUpload);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,element_bytes,element_indices.data(),GL_STREAM_DRAW);
        }
    }
    if (cache_inputs) {
        Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::VertexReuseCheck);
        if (!reuse_input) {
            const auto* data=reinterpret_cast<const u8*>(input_data);
            selected_upload->input_shadow.assign(data,data+input_bytes);
        }
        if (!reuse_index) {
            const auto* data=reinterpret_cast<const u8*>(input_indices.data());
            selected_upload->index_shadow.assign(data,data+index_bytes);
        }
        if (ios_lit_pair_vertex_reuse && !reuse_elements) {
            const auto* data=reinterpret_cast<const u8*>(element_indices.data());
            selected_upload->elements_shadow.assign(data,data+element_bytes);
        }
    }
    if (IndexPayloadReuse::Enabled() && selected_upload) {
        // Match/upload and the original owned-shadow commit have completed.
        // An uncached or non-default payload must never retain an older proof.
        selected_upload->index_shadow.CommitIdentity(cache_inputs ? current_index_identity :
                                                     IndexPayloadReuse::Identity{});
    }
    if (ios_feedback_vertex_reuse) {
        // Re-evaluate every draw: only the current draw's transformed outputs are
        // reused. Storage is orphaned before GPU writes, with no CPU readback.
        const GLsizei rows=static_cast<GLsizei>((ios_vertex_input_span*6+IOS_VERTEX_INPUT_WIDTH-1)/IOS_VERTEX_INPUT_WIDTH);
        const GLsizeiptr bytes=GLsizeiptr(rows)*IOS_VERTEX_INPUT_WIDTH*sizeof(GLvec4);
        if (!ios_feedback_buffer.handle) ios_feedback_buffer.Create();
        if (!ios_feedback_texture.handle) {
            ios_feedback_texture.Create();
            glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D,ios_feedback_texture.handle);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        } else { glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D,ios_feedback_texture.handle); }
        glBindSampler(9,0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,IOS_VERTEX_INPUT_WIDTH,rows,0,GL_RGBA,GL_FLOAT,nullptr);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,ios_feedback_buffer.handle);
        glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,bytes,nullptr,GL_STREAM_COPY);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,ios_feedback_buffer.handle);
        OpenGLState::BindShaderProgram(feedback_program);
        glUniform4iv(glGetUniformLocation(feedback_program,"cara_vertex_layout[0]"),16,ios_packed_layout[0].data());
        glEnable(GL_RASTERIZER_DISCARD);
        glBeginTransformFeedback(GL_POINTS);
        {
            DrawCostProfile::Scope draw_cost(DrawCostProfile::Path::FeedbackArrays,
                                            ios_vertex_input_span);
            glDrawArrays(GL_POINTS,0,static_cast<GLsizei>(ios_vertex_input_span));
        }
        glEndTransformFeedback();
        glDisable(GL_RASTERIZER_DISCARD);
        glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);
        glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,ios_feedback_buffer.handle);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glTexSubImage2D(GL_TEXTURE_2D,0,0,0,IOS_VERTEX_INPUT_WIDTH,rows,GL_RGBA,GL_FLOAT,nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        OpenGLState::BindShaderProgram(state.draw.shader_program);
        ++Common::FrameTiming::counters.gpu_feedback_vertex_draws;
        Common::FrameTiming::counters.gpu_feedback_source_vertices+=ios_vertex_input_span;
    }
    if (ShaderProgramManager::VertexProvenanceEnabled())
        shader_program_manager->ObserveBoundVertexProgram(state.draw.shader_program,
            ios_lit_pair_vertex_reuse?6u:ios_feedback_vertex_reuse?5u:ios_direct_vertex_reuse?3u:ios_indexed_vertex_inputs);
    {
        Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::DrawSubmit);
        if (ios_direct_vertex_reuse || ios_lit_pair_vertex_reuse) {
            {
                DrawCostProfile::Scope draw_cost(DrawCostProfile::Path::AcceleratedElements,
                                                triangles.size());
                if (!SuppressEffectFamilyHostDraw(triangles.size()))
                    glDrawElements(GL_TRIANGLES,static_cast<GLsizei>(triangles.size()),GL_UNSIGNED_SHORT,nullptr);
            }
            if (ios_lit_pair_vertex_reuse) {
                ++Common::FrameTiming::counters.gpu_lit_pair_draws;
                Common::FrameTiming::counters.gpu_lit_pair_vertices+=triangles.size();
                Common::FrameTiming::counters.gpu_lit_pair_ids+=ios_cached_index_data->lit_pair_count;
            } else {
            ++Common::FrameTiming::counters.gpu_direct_vertex_draws;
            Common::FrameTiming::counters.gpu_direct_vertices+=triangles.size();
            Common::FrameTiming::counters.gpu_direct_unique_inputs+=ios_vertex_input_records;
            }
        } else {
            DrawCostProfile::Scope draw_cost(DrawCostProfile::Path::AcceleratedArrays,
                                            triangles.size());
            if (!SuppressEffectFamilyHostDraw(triangles.size())) {
                if(NativeNearestBorrowEnabled() && !BeforeNativeNearestBorrowDraw())return false;
                glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(triangles.size()));
            }
        }
    }
    if (elements) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,index_buffer.GetHandle());
    // Prospective lit pair-indexed VS reuse: diagnostic counters only. The
    // actual uploaded bytes, shader and draw call are deliberately unchanged.
    static const bool pair_probe_enabled = [] {
        const char* value = std::getenv("CARA_GPU_LIT_PAIR_PROBE");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (pair_probe_enabled && Common::FrameTiming::counters.enabled && raw_inputs &&
        !Pica::g_state.regs.lighting.disable && !ios_direct_vertex_reuse && !ios_feedback_vertex_reuse && !ios_lit_pair_vertex_reuse) {
        {
            auto& counters = Common::FrameTiming::counters;
            if (!ios_cached_index_data || !ios_cached_index_data->pair_reuse.measured) {
                ++counters.lit_pair_probe_unavailable_draws;
            } else {
                const auto& stats = ios_cached_index_data->pair_reuse;
                ++counters.lit_pair_probe_draws;
                counters.lit_pair_probe_vertices += stats.vertices;
                counters.lit_pair_probe_unique_pairs += stats.unique_pairs;
                counters.lit_pair_probe_baseline_vs_runs += stats.baseline_vs_runs;
                counters.lit_pair_probe_lru16_misses += stats.lru16_misses;
                counters.lit_pair_probe_lru32_misses += stats.lru32_misses;
                counters.lit_pair_probe_lru16_vs_runs += stats.lru16_vs_runs;
                counters.lit_pair_probe_lru32_vs_runs += stats.lru32_vs_runs;
                counters.lit_pair_probe_pair_table_bytes += std::uint64_t(stats.unique_pairs) * 4;
                // Report candidate work coverage as well as aggregate efficiency.
                if (std::uint64_t(stats.lru32_vs_runs) * 4 <= std::uint64_t(stats.baseline_vs_runs) * 3) {
                    ++counters.lit_pair_probe_selected_draws;
                    counters.lit_pair_probe_selected_baseline_vs_runs += stats.baseline_vs_runs;
                    counters.lit_pair_probe_selected_lru32_vs_runs += stats.lru32_vs_runs;
                }
            }
        }
    }
    ++Common::FrameTiming::counters.gpu_vertex_draws;
    Common::FrameTiming::counters.gpu_vertices += triangles.size();
    Common::FrameTiming::counters.gpu_input_records += ios_vertex_input_records;
    if (ios_indexed_vertex_inputs) ++Common::FrameTiming::counters.gpu_indexed_input_draws;
    if (ios_indexed_vertex_inputs==2) ++Common::FrameTiming::counters.gpu_packed_input_draws;
    static const bool retain_input_bindings = [] {
        const char* value = std::getenv("CARA_GPU_RETAIN_INPUT_BINDINGS");
        return value && std::strcmp(value, "1") == 0;
    }();
    // Units 7/8 belong exclusively to this input path. Every draw still binds
    // its selected textures explicitly; deleting a pooled texture unbinds it in
    // GL, so no cached object IDs can survive deletion or name reuse.
    if (!retain_input_bindings && ios_indexed_vertex_inputs && !ios_direct_vertex_reuse) {
        glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D, 0);
    }
    if (ios_feedback_vertex_reuse) { glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D,0); }
    if (!retain_input_bindings) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glActiveTexture(GL_TEXTURE0);
    return true;
#else
    const auto& regs = Pica::g_state.regs;
    GLenum primitive_mode = GetCurrentPrimitiveMode();

    auto [vs_input_index_min, vs_input_index_max, vs_input_size] = AnalyzeVertexArray(is_indexed);

    if (vs_input_size > VERTEX_BUFFER_SIZE) {
        LOG_WARNING(Render_OpenGL, "Too large vertex input size {}", vs_input_size);
        return false;
    }

    state.draw.vertex_array = hw_vao.handle;
    state.draw.vertex_buffer = vertex_buffer.GetHandle();
    {
        Common::FrameTiming::Scope shader_setup(Common::FrameTiming::Phase::DrawShaderSetup,
                                                   Common::FrameTiming::RenderBookkeepingProfileEnabled());
        shader_program_manager->ApplyTo(state);
    }
    state.Apply();

    u8* buffer_ptr;
    GLintptr buffer_offset;
    std::tie(buffer_ptr, buffer_offset, std::ignore) = vertex_buffer.Map(vs_input_size, 4);
    SetupVertexArray(buffer_ptr, buffer_offset, vs_input_index_min, vs_input_index_max);
    vertex_buffer.Unmap(vs_input_size);

    if (is_indexed) {
        bool index_u16 = regs.pipeline.index_array.format != 0;
        std::size_t index_buffer_size = regs.pipeline.num_vertices * (index_u16 ? 2 : 1);

        if (index_buffer_size > INDEX_BUFFER_SIZE) {
            LOG_WARNING(Render_OpenGL, "Too large index input size {}", index_buffer_size);
            return false;
        }

        const u8* index_data = VideoCore::Memory()->GetPhysicalPointer(
            regs.pipeline.vertex_attributes.GetPhysicalBaseAddress() +
            regs.pipeline.index_array.offset);
        std::tie(buffer_ptr, buffer_offset, std::ignore) = index_buffer.Map(index_buffer_size, 4);
        std::memcpy(buffer_ptr, index_data, index_buffer_size);
        index_buffer.Unmap(index_buffer_size);

        DrawCostProfile::Scope draw_cost(DrawCostProfile::Path::DesktopRange,
                                        regs.pipeline.num_vertices);
        if (!SuppressEffectFamilyHostDraw(regs.pipeline.num_vertices))
            glDrawRangeElementsBaseVertex(
            primitive_mode, vs_input_index_min, vs_input_index_max, regs.pipeline.num_vertices,
            index_u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
            reinterpret_cast<const void*>(buffer_offset), -static_cast<GLint>(vs_input_index_min));
    } else {
        DrawCostProfile::Scope draw_cost(DrawCostProfile::Path::AcceleratedArrays,
                                        regs.pipeline.num_vertices);
        if (!SuppressEffectFamilyHostDraw(regs.pipeline.num_vertices))
            glDrawArrays(primitive_mode, 0, regs.pipeline.num_vertices);
    }

    return true;
#endif
}

void RasterizerOpenGL::DrawTriangles() {
    if(SoftwareBatching::CensusEnabled())software_census_after_draw=true;
    if (!SoftwareBatching::Active()) {if (!vertex_batch.empty()) Draw(false,false);return;}
    const auto vertices=software_vertices_since_request;software_vertices_since_request=0;
    software_batch_planner.Software(vertices,software_batch_draw_safe);
    if (SoftwareBatching::Enabled() && software_batch_planner.in_list && software_batch_draw_safe) {
        if(vertices)++software_completed_requests;
        software_has_deferred=!vertex_batch.empty();
        if(vertex_batch.size()>SoftwareBatching::stats.max_deferred_vertices)
            SoftwareBatching::stats.max_deferred_vertices=vertex_batch.size();
        return;
    }
    // Census-only mode executes the original draw immediately, leaving only the
    // bounded virtual planner pending. Unsafe draws retain their original boundaries.
    software_census_submission_succeeded=false;
    if (!vertex_batch.empty()) software_census_submission_succeeded=Draw(false,false);
}

void RasterizerOpenGL::FlushSoftwareBatch(SoftwareBatching::Reason reason,bool plan_boundary) {
    if(!SoftwareBatching::Active() || software_batch_flushing)return;
    if(plan_boundary)software_batch_planner.Boundary(reason);
    if(!SoftwareBatching::Enabled()) {
        // Census executes the original draw immediately; only its virtual obligation remains.
        if(SoftwareBatching::CensusEnabled())software_framebuffer_visibility.Materialized(reason,true);
        return;
    }
    if(vertex_batch.empty() || (!software_has_deferred && reason!=SoftwareBatching::Reason::SizeCap)) {
        if(software_framebuffer_visibility.pending)throw std::runtime_error("Framebuffer visibility lost its owned software batch");
        return;
    }
    Cara::GuestGPUTrace::Join::Scope validation_copy_deferred_scope;
    if (Cara::GuestGPUTrace::validation_copy_join_enabled)
        validation_copy_deferred_scope.Set(Cara::GuestGPUTrace::Join::state.Deferred(
            Cara::GuestGPUTrace::owner_generation.load(std::memory_order_relaxed)));
    software_batch_flushing=true;
    SoftwareBatching::Add(SoftwareBatching::stats.actual_flushes);
    SoftwareBatching::Add(SoftwareBatching::stats.actual_flushed_vertices,vertex_batch.size());
    SoftwareBatching::Add(SoftwareBatching::stats.actual_flush_reasons[unsigned(reason)]);
    if(software_completed_requests>1)SoftwareBatching::Add(SoftwareBatching::stats.actual_merged_completed_requests,software_completed_requests-1);
    software_completed_requests=0;
    try {if(!Draw(false,false)||!vertex_batch.empty())throw std::runtime_error("Deferred software draw did not consume its batch");software_has_deferred=false;software_framebuffer_visibility.Materialized(reason);software_batch_flushing=false;}
    catch(...){software_batch_flushing=false;throw;}
}
void RasterizerOpenGL::BeginPicaCommandList(){if(!SoftwareBatching::Active())return;FlushSoftwareBatch(SoftwareBatching::Reason::ListBegin);software_batch_planner.Begin();software_batch_draw_safe=false;software_census_after_draw=false;software_batch_input_guard_required=software_batch_input_indexed=true;}
void RasterizerOpenGL::EndPicaCommandList(){if(!SoftwareBatching::Active())return;FlushSoftwareBatch(SoftwareBatching::Reason::ListEnd);software_batch_planner.End();software_batch_draw_safe=false;software_census_after_draw=false;software_batch_input_guard_required=software_batch_input_indexed=true;}
void RasterizerOpenGL::FlushDeferredDraws(){FlushSoftwareBatch(SoftwareBatching::Reason::ExplicitVisibility);}

bool RasterizerOpenGL::SoftwareBatchResourcesSafe(bool include_vertex_inputs,bool indexed) {
    using namespace SoftwareBatching;
    const auto& r=Pica::g_state.regs;const auto& f=r.framebuffer.framebuffer;
    if(Pica::g_debug_context || r.framebuffer.IsShadowRendering() || (vertex_batch.size() && !software_has_deferred)) {Add(stats.unsupported);return false;}
    const auto color_format=unsigned(f.color_format.Value()),depth_format=unsigned(f.depth_format.Value());
    if(color_format>4 || (depth_format!=0&&depth_format!=2&&depth_format!=3)){Add(stats.unsupported);return false;}
    const auto width=std::uint64_t(f.GetWidth()),height=std::uint64_t(f.GetHeight());
    if(!width||!height||width>2048||height>2048||(width&7)||(height&7)){Add(stats.unsupported);return false;}
    // Four bytes per pixel conservatively includes every supported color/depth
    // format and stencil. Treat read-only depth as a potential write as well.
    std::array<Range,2> targets{};unsigned target_count=0;
    for(auto address:{f.GetColorBufferPhysicalAddress(),f.GetDepthBufferPhysicalAddress()}) {
        if(!address)continue;Range range;
        if(!MakeRange(address,width*height*4,range)){Add(stats.unsupported);return false;}
        targets[target_count++]=range;
    }
    if(!target_count){Add(stats.unsupported);return false;}
    // Separate host color/depth views can share guest physical bytes. The immediate
    // path reconciles their ownership after each draw; merging would remove that
    // dependency. Conservatively include even currently unused target envelopes.
    if(target_count==2 && Overlap(targets[0],targets[1])) {
        Add(stats.unsafe_target_alias);return false;
    }
    const auto overlaps=[&](Range range){for(unsigned i=0;i<target_count;++i)if(Overlap(range,targets[i]))return true;return false;};
    // Command fetch is another GPU read. A draw may not be delayed past a
    // later command word that aliases its output. Compare the actual host spans
    // of mapped physical memory without dereferencing or copying guest data.
    const auto& command=Pica::g_state.cmd_list;
    if(!command.head_ptr || command.length>std::numeric_limits<std::uintptr_t>::max()/sizeof(u32)) {Add(stats.unsupported);return false;}
    const auto command_begin=reinterpret_cast<std::uintptr_t>(command.head_ptr);
    const auto command_bytes=std::uintptr_t(command.length)*sizeof(u32);
    if(command_begin>std::numeric_limits<std::uintptr_t>::max()-command_bytes){Add(stats.unsupported);return false;}
    for(unsigned i=0;i<target_count;++i) {
        const auto* pointer=VideoCore::Memory()->GetPhysicalPointer(PAddr(targets[i].begin));
        if(!pointer){Add(stats.unsupported);return false;}
        const auto begin=reinterpret_cast<std::uintptr_t>(pointer),bytes=std::uintptr_t(targets[i].end-targets[i].begin);
        if(begin>std::numeric_limits<std::uintptr_t>::max()-bytes ||
            (begin<command_begin+command_bytes && command_begin<begin+bytes)){Add(stats.unsafe_vertex_index);return false;}
    }
    const auto textures=r.texturing.GetTextures();
    for(unsigned i=0;i<textures.size();++i) {
        const auto& t=textures[i];if(!t.enabled)continue;
        if(unsigned(t.format)>13){Add(stats.unsupported);return false;}
        if(i==0 && t.config.type!=Pica::TexturingRegs::TextureConfig::Texture2D){Add(stats.unsupported);return false;}
        const auto w=std::uint64_t(t.config.width),h=std::uint64_t(t.config.height);
        if(!w||!h||w>2048||h>2048){Add(stats.unsupported);return false;}
        // Conservative envelope: max32bpp, all mip levels and small tiled-level
        // padding. Unknown wraparound ranges are never accepted for deferral.
        const auto bytes=((w+7)/8*8)*((h+7)/8*8)*8+65536;
        Range range;if(!MakeRange(t.config.GetPhysicalAddress(),bytes,range)){Add(stats.unsupported);return false;}
        if(overlaps(range)){Add(stats.unsafe_texture);return false;}
    }
    if(!include_vertex_inputs)return true;
    const auto& p=r.pipeline;const auto n=std::uint64_t(p.num_vertices);
    if(!n)return true;
    const auto base=std::uint64_t(p.vertex_attributes.GetPhysicalBaseAddress());
    const auto max_vertex=indexed?(p.index_array.format?65535u:255u):std::uint64_t(p.vertex_offset)+n-1;
    if(indexed){Range range;const auto first=base+std::uint64_t(p.index_array.offset);
        if(!MakeRange(first,n*(p.index_array.format?2:1),range)||overlaps(range)){Add(stats.unsafe_vertex_index);return false;}}
    for(const auto& loader:p.vertex_attributes.attribute_loaders) {
        if(!loader.component_count)continue;
        // At most12 components of at most16 bytes plus alignment per loader;
        // 4096 bytes deliberately overbounds the scalar VertexLoader layout.
        const auto begin=base+std::uint64_t(loader.data_offset);
        const auto bytes=std::uint64_t(loader.byte_count)*max_vertex+4096;
        Range range;if(!MakeRange(begin,bytes,range)||overlaps(range)){Add(stats.unsafe_vertex_index);return false;}
    }
    return true;
}
void RasterizerOpenGL::PrePicaRegisterWrite(u32 id,u32 value,u32 mask) {
    using namespace SoftwareBatching;if(!Active())return;
    if(Pica::g_debug_context) {
        // A debugger may attach after an eligible draw but before a0x111 trigger.
        // Drain owned work before observing this command, regardless of whether
        // an explicit framebuffer visibility obligation has been requested yet.
        if(software_has_deferred || software_framebuffer_visibility.pending)
            FlushSoftwareBatch(Reason::ExplicitVisibility);
        software_batch_draw_safe=false;
    }
    // Keep the guest masked write and its original notification. Defer only its
    // logical visibility implementation, never a target/state or resource observer.
    if(id==0x111) {
        const bool census_only=!Enabled()&&CensusEnabled();
        const auto owned=census_only?software_batch_planner.pending:vertex_batch.size();
        const bool owned_safe=software_batch_draw_safe &&
            (census_only?software_census_submission_succeeded:software_has_deferred);
        if(software_framebuffer_visibility.Request(value,mask,DeferFramebufferFlushEnabled(),
               software_batch_planner.in_list,owned_safe,bool(Pica::g_debug_context),owned))return;
    }
    if(CensusEnabled() && software_census_after_draw && !InputOnlyRegister(id) && id!=0x22e && id!=0x22f && id<stats.post_draw_other_writes.size()) {
        Add(stats.post_draw_other_writes[id]);
        const auto bytes=(mask&1?0x000000ffu:0u)|(mask&2?0x0000ff00u:0u)|
                         (mask&4?0x00ff0000u:0u)|(mask&8?0xff000000u:0u);
        if(((Pica::g_state.regs.reg_array[id]^value)&bytes)==0)Add(stats.post_draw_other_unchanged[id]);
    }
    if(id==0x22e||id==0x22f) {
        software_batch_input_guard_required=true;software_batch_input_indexed=id==0x22f;
        if(LazySoftwareAdmission(Enabled(),CensusEnabled(),vertex_batch.empty(),
                                 software_has_deferred,software_framebuffer_visibility.pending)) {
            Add(stats.lazy_empty_input_skips);software_batch_draw_safe=false;return;
        }
        Add(stats.pre_input_resource_checks);
        const auto before_texture=stats.unsafe_texture,before_input=stats.unsafe_vertex_index,
                   before_target=stats.unsafe_target_alias;
        software_batch_draw_safe=software_batch_planner.in_list && SoftwareBatchResourcesSafe(true,id==0x22f);
        if(!software_batch_draw_safe)FlushSoftwareBatch(stats.unsafe_target_alias!=before_target?Reason::TargetAlias:stats.unsafe_texture!=before_texture?Reason::TextureAlias:stats.unsafe_vertex_index!=before_input?Reason::VertexIndexAlias:Reason::Unsupported);
        return;
    }
    // Immediate-mode vertex emission consumes only already-owned attributes.
    if(id>=0x232&&id<=0x235) {
        software_batch_input_guard_required=false;software_batch_input_indexed=false;
        if(LazySoftwareAdmission(Enabled(),CensusEnabled(),vertex_batch.empty(),
                                 software_has_deferred,software_framebuffer_visibility.pending)) {
            Add(stats.lazy_empty_input_skips);software_batch_draw_safe=false;return;
        }
        Add(stats.pre_input_resource_checks);
        const auto before_target=stats.unsafe_target_alias;
        software_batch_draw_safe=software_batch_planner.in_list && SoftwareBatchResourcesSafe(false,false);
        if(!software_batch_draw_safe)FlushSoftwareBatch(stats.unsafe_target_alias!=before_target?Reason::TargetAlias:Reason::Unsupported);
        return;
    }
    if(InputOnlyRegister(id))return;
    if(CensusEnabled() && (software_batch_planner.pending || software_batch_planner.last_hardware) && id<stats.fence_registers.size()) {
        Add(stats.fence_registers[id]);
        const auto bytes=(mask&1?0x000000ffu:0u)|(mask&2?0x0000ff00u:0u)|
                         (mask&4?0x00ff0000u:0u)|(mask&8?0xff000000u:0u);
        if(((Pica::g_state.regs.reg_array[id]^value)&bytes)==0) Add(stats.unchanged_fence_registers[id]);
    }
    FlushSoftwareBatch(id==0x111?Reason::FramebufferFlush:id==0x10?Reason::IRQ:(id==0x23c||id==0x23d)?Reason::Jump:Reason::FragmentOrUnknownWrite);
    software_batch_draw_safe=false;
}

void RasterizerOpenGL::DiscardExpiredFramebufferAttachments() {
    if (framebuffer_info.color_attachment && framebuffer_info.color_surface.expired()) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        framebuffer_info.color_attachment = 0;
        framebuffer_info.color_width = framebuffer_info.color_height = 0;
        framebuffer_info.color_surface.reset();
    }
    if (framebuffer_info.depth_attachment && framebuffer_info.depth_surface.expired()) {
        // Deletion may auto-detach only a currently bound FBO. Explicitly clear
        // both slots, also releasing an orphan object retained by an unbound FBO.
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        framebuffer_info.depth_attachment = 0;
        framebuffer_info.depth_width = framebuffer_info.depth_height = 0;
        framebuffer_info.depth_surface.reset();
        framebuffer_info.depth_stencil = false;
    }
}

void RasterizerOpenGL::BindFramebufferColor(OpenGLState& state, const Surface& surface) {
    state.draw.draw_framebuffer = framebuffer.handle;
    OpenGLState::BindDrawFramebuffer(framebuffer.handle);
    DiscardExpiredFramebufferAttachments();
    if (framebuffer_info.color_attachment != surface->texture.handle ||
        !SameFramebufferSurface(framebuffer_info.color_surface, surface)) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               surface->texture.handle, 0);
        framebuffer_info.color_attachment = surface->texture.handle;
        framebuffer_info.color_surface = surface;
        framebuffer_info.color_width = surface->width;
        framebuffer_info.color_height = surface->height;
    }

    if (framebuffer_info.depth_attachment &&
        (framebuffer_info.color_width > framebuffer_info.depth_width ||
         framebuffer_info.color_height > framebuffer_info.depth_height)) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);
        framebuffer_info.depth_attachment = 0;
        framebuffer_info.depth_surface.reset();
        framebuffer_info.depth_stencil = false;
    }
}

void RasterizerOpenGL::BindFramebufferDepthStencil(OpenGLState& state, const Surface& surface) {
    state.draw.draw_framebuffer = framebuffer.handle;
    OpenGLState::BindDrawFramebuffer(framebuffer.handle);
    DiscardExpiredFramebufferAttachments();
    if (framebuffer_info.depth_attachment != surface->texture.handle ||
        !SameFramebufferSurface(framebuffer_info.depth_surface, surface) ||
        framebuffer_info.depth_stencil != true) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
                               surface->texture.handle, 0);
        framebuffer_info.depth_attachment = surface->texture.handle;
        framebuffer_info.depth_surface = surface;
        framebuffer_info.depth_stencil = true;
        framebuffer_info.depth_width = surface->width;
        framebuffer_info.depth_height = surface->height;
    }

    if (framebuffer_info.color_attachment &&
        (framebuffer_info.depth_width > framebuffer_info.color_width ||
         framebuffer_info.depth_height > framebuffer_info.color_height)) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        framebuffer_info.color_attachment = 0;
        framebuffer_info.color_surface.reset();
    }
}

void RasterizerOpenGL::BindFramebufferDepth(OpenGLState& state, const Surface& surface) {
    state.draw.draw_framebuffer = framebuffer.handle;
    OpenGLState::BindDrawFramebuffer(framebuffer.handle);
    DiscardExpiredFramebufferAttachments();
    if (framebuffer_info.depth_attachment != surface->texture.handle ||
        !SameFramebufferSurface(framebuffer_info.depth_surface, surface) ||
        framebuffer_info.depth_stencil != false) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               surface->texture.handle, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        framebuffer_info.depth_attachment = surface->texture.handle;
        framebuffer_info.depth_surface = surface;
        framebuffer_info.depth_stencil = false;
        framebuffer_info.depth_width = surface->width;
        framebuffer_info.depth_height = surface->height;
    }

    if (framebuffer_info.color_attachment &&
        (framebuffer_info.depth_width > framebuffer_info.color_width ||
         framebuffer_info.depth_height > framebuffer_info.color_height)) {
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        framebuffer_info.color_attachment = 0;
        framebuffer_info.color_surface.reset();
    }
}

bool RasterizerOpenGL::NativeNearestBorrowEnabled() {
#ifdef CARA_MAC_ATLAS_PIPELINE
    static const bool enabled=[] { const char* s=std::getenv("CARA_NATIVE_NEAREST_BORROW"); return s&&std::strcmp(s,"1")==0; }();
    return enabled;
#else
    return false;
#endif
}

#ifdef CARA_IOS_GLES
bool RasterizerOpenGL::CaptureNativeNearestBorrow(bool indexed) {
#ifdef CARA_IOS_GLES
    namespace Q=Cara::QuadUVPlan;
    native_borrow_prepared=false;
    const auto& r=Pica::g_state.regs;
    if(indexed || ios_indexed_vertex_inputs!=2 || ios_vertex_input_span!=4 || ios_vertex_input_records!=4 ||
       ios_direct_vertex_reuse || ios_lit_pair_vertex_reuse || ios_feedback_vertex_reuse ||
       r.pipeline.use_gs!=Pica::PipelineRegs::UseGS::No ||
       Pica::g_state.primitive_assembler.GetTopology()!=Pica::PipelineRegs::TriangleTopology::Strip ||
       r.vs.shader_mode!=Pica::ShaderRegs::ShaderMode::VS || r.vs.input_to_uniform!=0 ||
       std::getenv("CARA_UNSNAPPED_VERTICES")) return false;
    Q::View v;v.program=Pica::g_state.vs.program_code.data();v.entry=r.vs.main_offset;
    if(v.entry>Pica::g_state.vs.program_code.size() || Pica::g_state.vs.program_code.size()-v.entry<6)return false;
    v.program_words=v.entry+6; // bounded current program, never census hashes/full shader walks
    v.swizzles=Pica::g_state.vs.swizzle_data.data();v.swizzle_words=Pica::g_state.vs.swizzle_data.size();
    v.max_input=r.vs.max_input_attribute_index;v.input_map_low=r.vs.input_attribute_to_register_map_low;
    v.input_map_high=r.vs.input_attribute_to_register_map_high;v.output_mask=r.vs.output_mask;
    v.output_total=r.rasterizer.vs_output_total;
    for(unsigned i=0;i<v.semantics.size();++i)v.semantics[i]=r.rasterizer.vs_output_attributes[i].raw;
    const auto p=Q::Classify(v);
    if(!p.structural || !p.ended || p.scanned!=6 || p.moves!=5 || p.nops || p.arithmetic)return false;
    const auto attr=[&](unsigned lane,unsigned component) {const auto& s=p.semantic_sources[lane];return Q::Known(s)&&s.source==Q::Source::InputAttribute&&s.index==0&&s.component==component;};
    if(!attr(0,0)||!attr(1,1)||!attr(4,2)||!attr(5,3))return false;
    const auto bits=[](float f){u32 b;std::memcpy(&b,&f,4);return b;};
    const auto uniform=[&](unsigned lane,u32 expected) {
        const auto& s=p.semantic_sources[lane];
        return Q::Known(s)&&s.source==Q::Source::FloatUniform&&s.index<std::size(Pica::g_state.vs.uniforms.f)&&s.component<4&&
            bits(Pica::g_state.vs.uniforms.f[s.index][s.component].ToFloat32())==expected;
    };
    if(!uniform(2,0)||!uniform(3,0x3f800000))return false;
    const auto shader_register=r.vs.GetRegisterForAttribute(0);
    const IOSVertexInputLayout layout(r.vs,ios_vertex_input_mask);
    if(shader_register>=16 || !(ios_vertex_input_mask&(1u<<shader_register)))return false;
    const auto slot=layout.offsets[shader_register];
    if(slot<0||unsigned(slot)>=layout.stride||layout.source_attributes[slot]!=0)return false;
    const Pica::VertexLoader loader(r.pipeline);const auto source=loader.GetAttributeLayout(0);
    const auto packed=ios_packed_layout[shader_register];
    if(source.is_default || source.format!=Pica::PipelineRegs::VertexAttributeFormat::FLOAT ||
       source.elements!=4 || source.stride!=16 || packed[0]<0 || packed[1]!=16||packed[2]!=3||packed[3]!=4)return false;
    const auto offset=std::size_t(packed[0]);const auto bytes=IOSVertexInputBytes();
    if(!IOSVertexInputData()||offset>bytes||64>bytes-offset)return false;
    std::array<std::array<u32,4>,4> data{};
    std::memcpy(data.data(),IOSVertexInputData()+offset,64); // coherent owned shadow only
    const auto& triangles=IOSTriangles();const auto& indices=IOSInputIndices();
    if(triangles.size()!=6 || indices.size()<6 || u32(r.pipeline.vertex_offset)>0xfffffffbu)return false;
    for(unsigned i=0;i<6;++i) {
        if(triangles[i]<u32(r.pipeline.vertex_offset)||triangles[i]-u32(r.pipeline.vertex_offset)>=4||
           indices[i]!=triangles[i]-u32(r.pipeline.vertex_offset))return false;
        native_borrow_inputs.triangles[i]=indices[i];
    }
    for(unsigned i=0;i<4;++i) {
        native_borrow_inputs.vertices[i].position={data[i][0],data[i][1],0,0x3f800000};
        native_borrow_inputs.vertices[i].uv={data[i][2],data[i][3]};
    }
    native_borrow_inputs.viewport_bits={bits(float(r.rasterizer.viewport_corner.x)),bits(float(r.rasterizer.viewport_corner.y)),
        bits(Pica::float24::FromRaw(r.rasterizer.viewport_size_x).ToFloat32()),bits(Pica::float24::FromRaw(r.rasterizer.viewport_size_y).ToFloat32())};
    native_borrow_inputs.native_snap16=true;native_borrow_inputs.no_geometry_shader=true;
    native_borrow_capture_input_epoch=native_borrow_input_epoch;native_borrow_capture_pica_epoch=native_borrow_pica_epoch;
    native_borrow_prepared=true;return true;
#else
    return false;
#endif
}

bool RasterizerOpenGL::NativeNearestBorrowDrawStateSafe() const {
    const auto& r=Pica::g_state.regs;const auto t=r.texturing.GetTextures();
    using C=Pica::TexturingRegs::TextureConfig;
    if(!native_borrow_prepared || !GLAD_GL_NV_texture_barrier || !glTextureBarrierNV ||
       !t[0].enabled || t[1].enabled || t[2].enabled || r.texturing.main_config.texture3_enable ||
       t[0].config.type!=C::TextureType::Texture2D || t[0].config.min_filter!=C::Nearest ||
       t[0].config.mag_filter!=C::Nearest || t[0].config.mip_filter!=C::Nearest ||
       t[0].config.wrap_s!=C::ClampToEdge || t[0].config.wrap_t!=C::ClampToEdge ||
       t[0].config.lod.min_level!=0 || t[0].config.lod.max_level!=0 || t[0].config.lod.bias!=0 ||
       Settings::values.use_linear_filter || !r.lighting.disable || r.framebuffer.IsShadowRendering() ||
       state.depth.write_mask || state.stencil.test_enabled) return false;
    // User clip has no x/y/z term and is nonnegative for W=1, or is disabled.
    if(state.clip_distance[1]) {
        const auto& c=uniform_block_data.data.clip_coef;
        if(c[0]!=0 || c[1]!=0 || c[2]!=0 || !std::isfinite(c[3]) || !(c[3]>=0))return false;
    }
    return true;
}

bool RasterizerOpenGL::BeforeNativeNearestBorrowDraw() {
    if(!native_borrow_lease)return true;
    const auto actual=OpenGLState::GetCurState();
    const std::array<GLuint,3> aliases{actual.texture_units[1].texture_2d,actual.texture_units[2].texture_2d,framebuffer_info.depth_attachment};
    const auto& sampler=texture_samplers[0];
    using C=Pica::TexturingRegs::TextureConfig;
    const bool ok=NativeNearestBorrowDrawStateSafe() && native_borrow_fragment_selected && !shader_dirty &&
        native_borrow_capture_input_epoch==native_borrow_input_epoch && native_borrow_capture_pica_epoch==native_borrow_pica_epoch &&
        actual.clip_distance==state.clip_distance &&
        actual.viewport.x==GLint(native_borrow_inputs.viewport.left) && actual.viewport.y==GLint(native_borrow_inputs.viewport.bottom) &&
        actual.viewport.width==GLsizei(native_borrow_inputs.viewport.right-native_borrow_inputs.viewport.left) &&
        actual.viewport.height==GLsizei(native_borrow_inputs.viewport.top-native_borrow_inputs.viewport.bottom) &&
        actual.draw.draw_framebuffer==framebuffer.handle && state.draw.draw_framebuffer==framebuffer.handle &&
        actual.draw.shader_program==state.draw.shader_program && actual.draw.shader_program &&
        sampler.NativeNearestCompatible() &&
        actual.texture_units[0].sampler==sampler.sampler.handle &&
        res_cache.RevalidateNearestBorrow(*native_borrow_lease,framebuffer_info.color_surface.lock(),
            framebuffer_info.color_attachment,actual.texture_units[0].texture_2d,aliases,true);
    // Diagnostic-only exercise of the bounded baseline recovery before any game draw.
    static const bool force_baseline=[] {
        const char* s=std::getenv("CARA_NATIVE_NEAREST_BORROW_FORCE_FALLBACK");
        return s && std::strcmp(s,"1")==0;
    }();
    if(!ok || force_baseline) {++native_borrow_stats.late_rejections;native_borrow_restart=true;return false;}
    glTextureBarrierNV(); // dependency barrier, after final checks, before this sole host draw
    ++native_borrow_stats.barriers;++native_borrow_stats.borrowed_draws;return true;
}

#endif

bool RasterizerOpenGL::Draw(bool accelerate, bool is_indexed) {
    bool native_borrow_disabled_for_draw=false;
#ifdef CARA_IOS_GLES
    SCOPE_EXIT({ if(NativeNearestBorrowEnabled()) native_borrow_lease.reset(); });
#endif
    for(unsigned native_borrow_attempt=0;native_borrow_attempt<2;++native_borrow_attempt) {
#ifdef CARA_IOS_GLES
    if(NativeNearestBorrowEnabled()) {
        native_borrow_restart=false;native_borrow_lease.reset();
        if(native_borrow_fragment_selected){native_borrow_fragment_selected=false;shader_dirty=true;}
    }
#endif
    Cara::QuadInputCapture::Scope quad_software_scope;
    if(Cara::QuadInputCapture::Enabled()&&!accelerate)quad_software_scope.RejectSoftware();
    // A matching guest join alone is insufficient: multiple host draws can
    // share one trigger. Clear at entry and all exits, including exceptions.
    const bool observe_copy_attachments = res_cache.ValidationCopyTracePresent();
    if (observe_copy_attachments) res_cache.ClearValidationCopyDrawAttachments();
    SCOPE_EXIT({ if (observe_copy_attachments) res_cache.ClearValidationCopyDrawAttachments(); });
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::Draw);
    const auto& regs = Pica::g_state.regs;
    const bool shadow_rendering = regs.framebuffer.IsShadowRendering();
    if (shadow_rendering && !AllowShadow) {
        return true;
    }

    const bool has_stencil = regs.framebuffer.HasStencil();

    const bool write_color_fb = shadow_rendering || state.color_mask.red_enabled == GL_TRUE ||
                                state.color_mask.green_enabled == GL_TRUE ||
                                state.color_mask.blue_enabled == GL_TRUE ||
                                state.color_mask.alpha_enabled == GL_TRUE;
    const bool write_depth_fb =
        (state.depth.test_enabled && state.depth.write_mask == GL_TRUE) ||
        (has_stencil && state.stencil.test_enabled && state.stencil.write_mask != 0);

    const bool using_color_fb =
        regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress() != 0 && write_color_fb;
    const bool using_depth_fb =
        !shadow_rendering && regs.framebuffer.framebuffer.GetDepthBufferPhysicalAddress() != 0 &&
        (write_depth_fb || regs.framebuffer.output_merger.depth_test_enable != 0 ||
         (has_stencil && state.stencil.test_enabled));

    const Common::Rectangle<s32> viewport_rect_unscaled = regs.rasterizer.GetViewportRect();

    Surface color_surface;
    Surface depth_surface;
    Common::Rectangle<u32> surfaces_rect;
    {
        Common::FrameTiming::Scope lookup(Common::FrameTiming::Phase::DrawFramebufferAcquire,
                                         Common::FrameTiming::RenderBookkeepingProfileEnabled());
        std::tie(color_surface, depth_surface, surfaces_rect) =
            res_cache.GetFramebufferSurfaces(using_color_fb, using_depth_fb, viewport_rect_unscaled);
    }

    u32 res_scale = 1;
    GLuint color_attachment = 0;
    if (color_surface) {
        res_scale = color_surface->res_scale;
        color_attachment = color_surface->texture.handle;
        framebuffer_info.color_width = color_surface->width;
        framebuffer_info.color_height = color_surface->height;
    } else if (depth_surface) {
        res_scale = depth_surface->res_scale;
    }

    // Intended selected surfaces, before texture acquisition can change cache
    // state. This is not a query of the currently bound GL framebuffer.
    if (observe_copy_attachments)
        res_cache.RecordValidationCopyDrawAttachments(color_surface, depth_surface);

    if(Cara::QuadInputCapture::Enabled()&&accelerate) {
        using R=Cara::QuadInputCapture::Rect;
        Cara::QuadInputCapture::PublishPlanned(
            R{viewport_rect_unscaled.left,viewport_rect_unscaled.bottom,viewport_rect_unscaled.right,viewport_rect_unscaled.top},
            R{surfaces_rect.left,surfaces_rect.bottom,surfaces_rect.right,surfaces_rect.top},res_scale,
            static_cast<u32>(regs.rasterizer.scissor_test.mode.Value()),
            {regs.rasterizer.scissor_test.x1.Value(),regs.rasterizer.scissor_test.y1.Value(),regs.rasterizer.scissor_test.x2.Value(),regs.rasterizer.scissor_test.y2.Value()});
    }
#ifdef CARA_IOS_GLES
    std::optional<NearestBorrow::Plan> native_plan;
    if(NativeNearestBorrowEnabled() && accelerate && !native_borrow_disabled_for_draw) {
        ++native_borrow_stats.attempts;
        if(!native_borrow_prepared) ++native_borrow_stats.prepared_rejections;
        else if(!NativeNearestBorrowDrawStateSafe() || !color_surface || depth_surface || res_scale!=1 ||
                !PicaFSConfig::SupportsNativeNearestBorrow(regs)) ++native_borrow_stats.state_rejections;
        else {
            const s64 left=s64(surfaces_rect.left)+viewport_rect_unscaled.left;
            const s64 bottom=s64(surfaces_rect.bottom)+viewport_rect_unscaled.bottom;
            const s64 right=s64(surfaces_rect.left)+viewport_rect_unscaled.right;
            const s64 top=s64(surfaces_rect.bottom)+viewport_rect_unscaled.top;
            if(left<0 || bottom<0 || right>UINT32_MAX || top>UINT32_MAX || right<=left || top<=bottom)
                ++native_borrow_stats.plan_rejections;
            else {
                native_borrow_inputs.viewport={u32(left),u32(bottom),u32(right),u32(top)};
                native_borrow_inputs.conservative_write=native_borrow_inputs.viewport;
                native_borrow_inputs.logical_width=regs.texturing.texture0.width;
                native_borrow_inputs.logical_height=regs.texturing.texture0.height;
                const auto plan=NearestBorrow::PlanQuad(native_borrow_inputs);
                if(plan.reason==NearestBorrow::Reason::None)native_plan=plan.plan;
                else ++native_borrow_stats.plan_rejections;
            }
        }
    }
#endif
    // Sync and bind the texture surfaces
    const auto pica_textures = regs.texturing.GetTextures();
    {
    Common::FrameTiming::Scope lookup(Common::FrameTiming::Phase::DrawTextureAcquire,
                                     Common::FrameTiming::RenderBookkeepingProfileEnabled());
    for (unsigned texture_index = 0; texture_index < pica_textures.size(); ++texture_index) {
        const auto& texture = pica_textures[texture_index];

        if (texture.enabled) {
            if (texture_index == 0) {
                using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
                switch (texture.config.type.Value()) {
                case TextureType::Shadow2D: {
                    Surface surface = res_cache.GetTextureSurface(texture);
                    state.image_shadow_texture_px =
                        surface != nullptr ? surface->texture.handle : 0;
                    continue;
                }
                case TextureType::ShadowCube: {
                    Pica::Texture::TextureInfo info = Pica::Texture::TextureInfo::FromPicaRegister(
                        texture.config, texture.format);
                    Surface surface;

                    using CubeFace = Pica::TexturingRegs::CubeFace;
                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveX);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_px =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeX);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_nx =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveY);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_py =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeY);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_ny =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveZ);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_pz =
                        surface != nullptr ? surface->texture.handle : 0;

                    info.physical_address =
                        regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeZ);
                    surface = res_cache.GetTextureSurface(info);
                    state.image_shadow_texture_nz =
                        surface != nullptr ? surface->texture.handle : 0;
                    continue;
                }
                case TextureType::TextureCube:
                    using CubeFace = Pica::TexturingRegs::CubeFace;
                    TextureCubeConfig config;
                    config.px = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveX);
                    config.nx = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeX);
                    config.py = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveY);
                    config.ny = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeY);
                    config.pz = regs.texturing.GetCubePhysicalAddress(CubeFace::PositiveZ);
                    config.nz = regs.texturing.GetCubePhysicalAddress(CubeFace::NegativeZ);
                    config.width = texture.config.width;
                    config.format = texture.format;
                    state.texture_cube_unit.texture_cube =
                        res_cache.GetTextureCube(config).texture.handle;

                    texture_cube_sampler.SyncWithConfig(texture.config);
                    state.texture_units[texture_index].texture_2d = 0;
                    continue; // Texture unit 0 setup finished. Continue to next unit
                }
                state.texture_cube_unit.texture_cube = 0;
            }

            texture_samplers[texture_index].SyncWithConfig(texture.config);
            Surface surface;
#ifdef CARA_IOS_GLES
            if(texture_index==0 && native_plan) {
                const auto info=Pica::Texture::TextureInfo::FromPicaRegister(texture.config,texture.format);
                native_borrow_lease=res_cache.DescribeNearestBorrow(color_surface,info,0,
                    native_plan->logical_read,native_plan->conservative_write);
                if(native_borrow_lease) {
                    surface=native_borrow_lease->owner;
                    state.texture_units[0].texture_2d=native_borrow_lease->handle;
                    native_borrow_fragment_selected=true;shader_dirty=true;
                    uniform_block_data.data.cara_tex0_view=native_borrow_lease->view;
                    uniform_block_data.data.cara_tex0_crop=native_borrow_lease->crop;
                    uniform_block_data.dirty=true;
                    continue; // no destination acquisition/validation/validity publication
                }
                ++native_borrow_stats.cache_rejections;
            }
#endif
            surface = res_cache.GetTextureSurface(texture);
            if (surface) {
                if (surface->texture.handle == color_attachment) {
                    state.texture_units[texture_index].texture_2d = res_cache.GetTextureCopyHandle(surface);
                } else {
                    state.texture_units[texture_index].texture_2d = surface->texture.handle;
                }
            } else {
                /// texture_null
                // Can occur when texture addr is null or its memory is unmapped/invalid
                // HACK: In this case, the correct behaviour for the PICA is to use the last
                // rendered colour. But because this would be impractical to implement, the
                // next best alternative is to use a clear texture, essentially skipping
                // the geometry in question.
                // For example: a bug in Pokemon X/Y causes NULL-texture squares to be drawn
                // on the male character's face, which in the OpenGL default appear black.
                state.texture_units[texture_index].texture_2d = texture_null.handle;
            }
        } else {
            state.texture_units[texture_index].texture_2d = 0;
        }
    }

    }

    Common::Rectangle<u32> draw_rect{
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.left) +
                                             viewport_rect_unscaled.left * res_scale,
                                         surfaces_rect.left, surfaces_rect.right)), // Left
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.bottom) +
                                             viewport_rect_unscaled.top * res_scale,
                                         surfaces_rect.bottom, surfaces_rect.top)), // Top
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.left) +
                                             viewport_rect_unscaled.right * res_scale,
                                         surfaces_rect.left, surfaces_rect.right)), // Right
        static_cast<u32>(std::clamp<s32>(static_cast<s32>(surfaces_rect.bottom) +
                                             viewport_rect_unscaled.bottom * res_scale,
                                         surfaces_rect.bottom, surfaces_rect.top))}; // Bottom

    // Sync the viewport
    state.viewport.x =
        static_cast<GLint>(surfaces_rect.left) + viewport_rect_unscaled.left * res_scale;
    state.viewport.y =
        static_cast<GLint>(surfaces_rect.bottom) + viewport_rect_unscaled.bottom * res_scale;
    state.viewport.width = static_cast<GLsizei>(viewport_rect_unscaled.GetWidth() * res_scale);
    state.viewport.height = static_cast<GLsizei>(viewport_rect_unscaled.GetHeight() * res_scale);

#ifdef CARA_IOS_GLES
    const GLvec4 native_viewport = {
        static_cast<float>(regs.rasterizer.viewport_corner.x),
        static_cast<float>(regs.rasterizer.viewport_corner.y),
        Pica::float24::FromRaw(regs.rasterizer.viewport_size_x).ToFloat32(),
        Pica::float24::FromRaw(regs.rasterizer.viewport_size_y).ToFloat32()};
    if (uniform_block_data.data.cara_viewport != native_viewport) {
        uniform_block_data.data.cara_viewport = native_viewport;
        uniform_block_data.dirty = true;
    }
#endif

    // Bind the framebuffer surfaces
    OpenGLState::BindDrawFramebuffer(framebuffer.handle);
    DiscardExpiredFramebufferAttachments();

    if (shadow_rendering) {
        if (color_surface == nullptr) {
            return true;
        }
        framebuffer_info.color_attachment = 0;
        framebuffer_info.color_surface.reset();
        framebuffer_info.depth_attachment = 0;
        framebuffer_info.depth_surface.reset();
        framebuffer_info.depth_stencil = false;
        glFramebufferParameteri(GL_DRAW_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH,
                                color_surface->GetScaledWidth());
        glFramebufferParameteri(GL_DRAW_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT,
                                color_surface->GetScaledHeight());
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                               0);
        state.image_shadow_buffer = color_surface->texture.handle;
    } else {
        if (framebuffer_info.color_attachment != color_attachment ||
            !SameFramebufferSurface(framebuffer_info.color_surface, color_surface)) {
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   color_attachment, 0);
            framebuffer_info.color_attachment = color_attachment;
            framebuffer_info.color_surface = color_surface;
            framebuffer_info.color_width = color_surface ? color_surface->width : 0;
            framebuffer_info.color_height = color_surface ? color_surface->height : 0;
        }
        if (depth_surface != nullptr) {
            if (has_stencil) {
                if (framebuffer_info.depth_attachment != depth_surface->texture.handle ||
                    !SameFramebufferSurface(framebuffer_info.depth_surface, depth_surface) ||
                    framebuffer_info.depth_stencil != has_stencil) {
                    // attach both depth and stencil
                    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                           GL_TEXTURE_2D, depth_surface->texture.handle, 0);
                    framebuffer_info.depth_attachment = depth_surface->texture.handle;
                framebuffer_info.depth_surface = depth_surface;
                framebuffer_info.depth_stencil = has_stencil;
                    framebuffer_info.depth_width = depth_surface->width;
                    framebuffer_info.depth_height = depth_surface->height;
                }
            } else if (framebuffer_info.depth_attachment != depth_surface->texture.handle ||
                    !SameFramebufferSurface(framebuffer_info.depth_surface, depth_surface) ||
                    framebuffer_info.depth_stencil != has_stencil) {
                // attach depth
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                                       depth_surface->texture.handle, 0);
                // clear stencil attachment
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, 0,
                                       0);
                framebuffer_info.depth_attachment = depth_surface->texture.handle;
                framebuffer_info.depth_surface = depth_surface;
                framebuffer_info.depth_stencil = has_stencil;
                framebuffer_info.depth_width = depth_surface->width;
                framebuffer_info.depth_height = depth_surface->height;
            }
        } else if (framebuffer_info.depth_attachment != 0) {
            if (framebuffer_info.depth_width < surfaces_rect.right / res_scale ||
                framebuffer_info.depth_height < surfaces_rect.top / res_scale) {
                // clear both depth and stencil attachment
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                       GL_TEXTURE_2D, 0, 0);
                framebuffer_info.depth_attachment = 0;
                framebuffer_info.depth_surface.reset();
                framebuffer_info.depth_stencil = false;
            } else {
                state.depth.test_enabled = false;
                state.depth.write_mask = GL_FALSE;
            }
        }
    }

    // Scissor checks are window-, not viewport-relative, which means that if the cached texture
    // sub-rect changes, the scissor bounds also need to be updated.
    GLint scissor_x1 =
        static_cast<GLint>(surfaces_rect.left + regs.rasterizer.scissor_test.x1 * res_scale);
    GLint scissor_y1 =
        static_cast<GLint>(surfaces_rect.bottom + regs.rasterizer.scissor_test.y1 * res_scale);
    // x2, y2 have +1 added to cover the entire pixel area, otherwise you might get cracks when
    // scaling or doing multisampling.
    GLint scissor_x2 =
        static_cast<GLint>(surfaces_rect.left + (regs.rasterizer.scissor_test.x2 + 1) * res_scale);
    GLint scissor_y2 = static_cast<GLint>(surfaces_rect.bottom +
                                          (regs.rasterizer.scissor_test.y2 + 1) * res_scale);

    if (uniform_block_data.data.scissor_x1 != scissor_x1 ||
        uniform_block_data.data.scissor_x2 != scissor_x2 ||
        uniform_block_data.data.scissor_y1 != scissor_y1 ||
        uniform_block_data.data.scissor_y2 != scissor_y2) {

        uniform_block_data.data.scissor_x1 = scissor_x1;
        uniform_block_data.data.scissor_x2 = scissor_x2;
        uniform_block_data.data.scissor_y1 = scissor_y1;
        uniform_block_data.data.scissor_y2 = scissor_y2;
        uniform_block_data.dirty = true;
    }

#ifdef CARA_IOS_GLES
    if(NativeNearestBorrowEnabled() && !native_borrow_lease &&
       (uniform_block_data.data.cara_tex0_view!=GLivec4{} || uniform_block_data.data.cara_tex0_crop!=GLivec4{})) {
        uniform_block_data.data.cara_tex0_view={};uniform_block_data.data.cara_tex0_crop={};uniform_block_data.dirty=true;
    }
    if (GLESRuntimeScissorEnabled()) {
        // GLES has no nearest-borrow crop, so reuse the same slot as the Metal scissor code.
        const auto mode = static_cast<GLint>(regs.rasterizer.scissor_test.mode.Value());
        auto& uniform_mode = uniform_block_data.data.cara_tex0_crop[1];
        if (uniform_mode != mode) {
            uniform_mode = mode;
            uniform_block_data.dirty = true;
        }
    }
#endif
    // Sync and bind the shader
    if (shader_dirty) {
        SetShader();
        shader_dirty = false;
    }

#ifdef CARA_IOS_GLES
    if(native_borrow_restart) {
        if(native_borrow_attempt!=0)return false;
        native_borrow_disabled_for_draw=true;shader_dirty=true;++native_borrow_stats.baseline_restarts;continue;
    }
#endif
    // Sync the LUTs within the texture buffer
    {
        Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::LUTUpload);
        SyncAndUploadLUTs();
        SyncAndUploadLUTsLF();
    }

    // Sync the uniform data
    UploadUniforms(accelerate);

    // Viewport can have negative offsets or larger
    // dimensions than our framebuffer sub-rect.
    // Enable scissor test to prevent drawing
    // outside of the framebuffer region
    state.scissor.enabled = true;
    state.scissor.x = draw_rect.left;
    state.scissor.y = draw_rect.bottom;
    state.scissor.width = draw_rect.GetWidth();
    state.scissor.height = draw_rect.GetHeight();

    // Separate rendering experiment: retain generated shader discard and all
    // pre-draw validation. Only intersect the existing framebuffer host clip.
    static const bool host_include_scissor = [] {
        const char* value = std::getenv("CARA_HOST_INCLUDE_SCISSOR");
        return value && std::strcmp(value, "1") == 0;
    }();
    HostScissor::ScopedInclude host_include_scope(
        state, {scissor_x1, scissor_y1, scissor_x2, scissor_y2},
        static_cast<u32>(regs.rasterizer.scissor_test.mode.Value()), res_scale, shadow_rendering,
        host_include_scissor);

    if (Common::FrameTiming::counters.enabled && !regs.lighting.disable) {
        ++Common::FrameTiming::counters.lit_draws;
        Common::FrameTiming::counters.active_light_sum += regs.lighting.max_light_index + 1;
    }

    // Draw the vertex batch
    bool succeeded = true;
    if (accelerate) {
        succeeded = AccelerateDrawBatchInternal(is_indexed);
    } else {
        state.draw.vertex_array = sw_vao.handle;
        state.draw.vertex_buffer = vertex_buffer.GetHandle();
        shader_program_manager->UseTrivialVertexShader();
        shader_program_manager->UseTrivialGeometryShader();
        {
            Common::FrameTiming::Scope shader_setup(Common::FrameTiming::Phase::DrawShaderSetup,
                                                       Common::FrameTiming::RenderBookkeepingProfileEnabled());
            shader_program_manager->ApplyTo(state);
        }
        state.Apply();

        std::size_t max_vertices = 3 * (VERTEX_BUFFER_SIZE / (3 * sizeof(HardwareVertex)));
        for (std::size_t base_vertex = 0; base_vertex < vertex_batch.size();
             base_vertex += max_vertices) {
            const std::size_t vertices = std::min(max_vertices, vertex_batch.size() - base_vertex);
            const std::size_t vertex_size = vertices * sizeof(HardwareVertex);
            u8* vbo;
            GLintptr offset;
            std::tie(vbo, offset, std::ignore) =
                vertex_buffer.Map(vertex_size, sizeof(HardwareVertex));
            std::memcpy(vbo, vertex_batch.data() + base_vertex, vertex_size);
            vertex_buffer.Unmap(vertex_size);
            DrawCostProfile::Scope draw_cost(DrawCostProfile::Path::SoftwareArrays, vertices);
            if (!SuppressEffectFamilyHostDraw(vertices))
                glDrawArrays(GL_TRIANGLES, static_cast<GLint>(offset / sizeof(HardwareVertex)),
                         static_cast<GLsizei>(vertices));
        }
    }

#ifdef CARA_IOS_GLES
    if(native_borrow_restart) {
        host_include_scope.Restore();
        if(native_borrow_attempt!=0)return false;
        native_borrow_disabled_for_draw=true;shader_dirty=true;++native_borrow_stats.baseline_restarts;continue;
    }
#endif
    vertex_batch.clear();
    host_include_scope.Restore();

    // Reset textures in rasterizer state context because the rasterizer cache might delete them
    for (auto& texture_unit : state.texture_units) {
        texture_unit.texture_2d = 0;
    }
    state.texture_cube_unit.texture_cube = 0;
    if (AllowShadow) {
        state.image_shadow_texture_px = 0;
        state.image_shadow_texture_nx = 0;
        state.image_shadow_texture_py = 0;
        state.image_shadow_texture_ny = 0;
        state.image_shadow_texture_pz = 0;
        state.image_shadow_texture_nz = 0;
        state.image_shadow_buffer = 0;
    }
    state.Apply();

    if (shadow_rendering) {
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                        GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
    }

    // Mark framebuffer surfaces as dirty. Rendering/scissor/prevalidation above
    // remain unchanged; only this conservative ownership footprint is narrowed.
    static const bool include_scissor_damage = [] {
        const char* value = std::getenv("CARA_INCLUDE_SCISSOR_DAMAGE");
        return value && std::strcmp(value, "1") == 0;
    }();
    auto dirty_rect = draw_rect;
    if (include_scissor_damage) {
        const auto damage = ScissorDamage::NarrowInclude(
            {draw_rect.left, draw_rect.bottom, draw_rect.right, draw_rect.top},
            {scissor_x1, scissor_y1, scissor_x2, scissor_y2},
            static_cast<u32>(regs.rasterizer.scissor_test.mode.Value()), res_scale,
            shadow_rendering,
            has_stencil && state.stencil.test_enabled && state.stencil.write_mask != 0);
        if (damage.eligible) {
            dirty_rect = {static_cast<u32>(damage.rect.left), static_cast<u32>(damage.rect.top),
                          static_cast<u32>(damage.rect.right), static_cast<u32>(damage.rect.bottom)};
        }
    }
    Common::Rectangle<u32> draw_rect_unscaled{dirty_rect.left / res_scale, dirty_rect.top / res_scale,
                                              dirty_rect.right / res_scale,
                                              dirty_rect.bottom / res_scale};

    SurfaceInvalidationContext draw_context;
    const SurfaceInvalidationContext* trace_context = nullptr;
    if (res_cache.ValidationCopyTracingEnabled()) {
        draw_context.valid = true;
        draw_context.viewport = {draw_rect.left, draw_rect.bottom, draw_rect.right, draw_rect.top};
        draw_context.actual_damage = {dirty_rect.left, dirty_rect.bottom, dirty_rect.right, dirty_rect.top};
        draw_context.shader_scissor = {scissor_x1, scissor_y1, scissor_x2, scissor_y2};
        draw_context.mode = static_cast<u32>(regs.rasterizer.scissor_test.mode.Value());
        draw_context.scale = res_scale;
        draw_context.shadow = shadow_rendering;
        draw_context.color_write = write_color_fb;
        draw_context.depth_write = state.depth.test_enabled && state.depth.write_mask != 0;
        draw_context.depth_surface_write = write_depth_fb;
        draw_context.damage_narrowing_active = include_scissor_damage;
        draw_context.has_stencil = has_stencil;
        draw_context.stencil_test = state.stencil.test_enabled;
        draw_context.stencil_mask = state.stencil.write_mask;
        draw_context.depth_test = state.depth.test_enabled;
        draw_context.depth_mask = state.depth.write_mask;
        draw_context.color_mask = (state.color_mask.red_enabled ? 1u : 0u) |
                                  (state.color_mask.green_enabled ? 2u : 0u) |
                                  (state.color_mask.blue_enabled ? 4u : 0u) |
                                  (state.color_mask.alpha_enabled ? 8u : 0u);
        trace_context = &draw_context;
    }
    {
    Common::FrameTiming::Scope invalidate(Common::FrameTiming::Phase::DrawDirtyInvalidate,
                                         Common::FrameTiming::RenderBookkeepingProfileEnabled());
    if (color_surface != nullptr) {
        auto interval = color_surface->GetSubRectInterval(draw_rect_unscaled);
        res_cache.InvalidateRegion(boost::icl::first(interval), boost::icl::length(interval),
                                   color_surface, SurfaceInvalidationOrigin::DrawColor, trace_context);
    }
    if (depth_surface != nullptr && write_depth_fb) {
        auto interval = depth_surface->GetSubRectInterval(draw_rect_unscaled);
        res_cache.InvalidateRegion(boost::icl::first(interval), boost::icl::length(interval),
                                   depth_surface, SurfaceInvalidationOrigin::DrawDepth, trace_context);
    }

    }
    return succeeded;
    } // at most one baseline restart before any host game draw
    return false;
}

void RasterizerOpenGL::NotifyPicaRegisterChanged(u32 id) {
#ifdef CARA_IOS_GLES
    if(NativeNearestBorrowEnabled())++native_borrow_pica_epoch;
#endif
    const auto& regs = Pica::g_state.regs;

    switch (id) {
    // Culling
    case PICA_REG_INDEX(rasterizer.cull_mode):
        SyncCullMode();
        break;

    // Clipping plane
    case PICA_REG_INDEX(rasterizer.clip_enable):
        SyncClipEnabled();
        break;

    case PICA_REG_INDEX(rasterizer.clip_coef[0]):
    case PICA_REG_INDEX(rasterizer.clip_coef[1]):
    case PICA_REG_INDEX(rasterizer.clip_coef[2]):
    case PICA_REG_INDEX(rasterizer.clip_coef[3]):
        SyncClipCoef();
        break;

    // Depth modifiers
    case PICA_REG_INDEX(rasterizer.viewport_depth_range):
        SyncDepthScale();
        break;
    case PICA_REG_INDEX(rasterizer.viewport_depth_near_plane):
        SyncDepthOffset();
        break;

    // Depth buffering
    case PICA_REG_INDEX(rasterizer.depthmap_enable):
        shader_dirty = true;
        break;

    // Blending
    case PICA_REG_INDEX(framebuffer.output_merger.alphablend_enable):
        SyncBlendEnabled();
        break;
    case PICA_REG_INDEX(framebuffer.output_merger.alpha_blending):
        SyncBlendFuncs();
        break;
    case PICA_REG_INDEX(framebuffer.output_merger.blend_const):
        SyncBlendColor();
        break;

    // Shadow texture
    case PICA_REG_INDEX(texturing.shadow):
        SyncShadowTextureBias();
        break;

    // Fog state
    case PICA_REG_INDEX(texturing.fog_color):
        SyncFogColor();
        break;
    /*case PICA_REG_INDEX(texturing.fog_lut_data[0]):
    case PICA_REG_INDEX(texturing.fog_lut_data[1]):
    case PICA_REG_INDEX(texturing.fog_lut_data[2]):
    case PICA_REG_INDEX(texturing.fog_lut_data[3]):
    case PICA_REG_INDEX(texturing.fog_lut_data[4]):
    case PICA_REG_INDEX(texturing.fog_lut_data[5]):
    case PICA_REG_INDEX(texturing.fog_lut_data[6]):
    case PICA_REG_INDEX(texturing.fog_lut_data[7]):
        SyncFogLutData();
        break;*/

    // ProcTex state
    case PICA_REG_INDEX(texturing.proctex):
    case PICA_REG_INDEX(texturing.proctex_lut):
    case PICA_REG_INDEX(texturing.proctex_lut_offset):
        SyncProcTexBias();
        break;

    case PICA_REG_INDEX(texturing.proctex_noise_u):
    case PICA_REG_INDEX(texturing.proctex_noise_v):
    case PICA_REG_INDEX(texturing.proctex_noise_frequency):
        SyncProcTexNoise();
        break;

    /*case PICA_REG_INDEX(texturing.proctex_lut_data[0]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[1]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[2]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[3]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[4]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[5]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[6]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[7]):
        SyncProcTexLutData();
        break;*/

    // Alpha test
    case PICA_REG_INDEX(framebuffer.output_merger.alpha_test):
        SyncAlphaTest();
        shader_dirty = true;
        break;

    // Sync GL stencil test + stencil write mask
    // (Pica stencil test function register also contains a stencil write mask)
    case PICA_REG_INDEX(framebuffer.output_merger.stencil_test.raw_func):
        SyncStencilTest();
        SyncStencilWriteMask();
        break;
    case PICA_REG_INDEX(framebuffer.output_merger.stencil_test.raw_op):
        SyncStencilTest();
        break;
    case PICA_REG_INDEX(framebuffer.framebuffer.depth_format):
        SyncStencilTest();
#ifdef CARA_IOS_GLES
        shader_dirty = true;
#endif
        break;

    // Sync GL depth test + depth and color write mask
    // (Pica depth test function register also contains a depth and color write mask)
    case PICA_REG_INDEX(framebuffer.output_merger.depth_test_enable):
        SyncDepthTest();
        SyncDepthWriteMask();
        SyncColorWriteMask();
        break;

    // Sync GL depth and stencil write mask
    // (This is a dedicated combined depth / stencil write-enable register)
    case PICA_REG_INDEX(framebuffer.framebuffer.allow_depth_stencil_write):
        SyncDepthWriteMask();
        SyncStencilWriteMask();
        break;

    // Sync GL color write mask
    // (This is a dedicated color write-enable register)
    case PICA_REG_INDEX(framebuffer.framebuffer.allow_color_write):
        SyncColorWriteMask();
        break;

    case PICA_REG_INDEX(framebuffer.shadow):
        SyncShadowBias();
        break;

    // Scissor test
    case PICA_REG_INDEX(rasterizer.scissor_test.mode):
        shader_dirty = true;
        break;

    // Logic op
    case PICA_REG_INDEX(framebuffer.output_merger.logic_op):
        SyncLogicOp();
        break;

    case PICA_REG_INDEX(texturing.main_config):
        shader_dirty = true;
        break;

    // Texture 0 type
    case PICA_REG_INDEX(texturing.texture0.type):
        shader_dirty = true;
        break;

    // TEV stages
    // (This also syncs fog_mode and fog_flip which are part of tev_combiner_buffer_input)
    case PICA_REG_INDEX(texturing.tev_stage0.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage0.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage0.color_op):
    case PICA_REG_INDEX(texturing.tev_stage0.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage1.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage1.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage1.color_op):
    case PICA_REG_INDEX(texturing.tev_stage1.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage2.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage2.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage2.color_op):
    case PICA_REG_INDEX(texturing.tev_stage2.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage3.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage3.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage3.color_op):
    case PICA_REG_INDEX(texturing.tev_stage3.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage4.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage4.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage4.color_op):
    case PICA_REG_INDEX(texturing.tev_stage4.color_scale):
    case PICA_REG_INDEX(texturing.tev_stage5.color_source1):
    case PICA_REG_INDEX(texturing.tev_stage5.color_modifier1):
    case PICA_REG_INDEX(texturing.tev_stage5.color_op):
    case PICA_REG_INDEX(texturing.tev_stage5.color_scale):
    case PICA_REG_INDEX(texturing.tev_combiner_buffer_input):
        shader_dirty = true;
        break;
    case PICA_REG_INDEX(texturing.tev_stage0.const_r):
        SyncTevConstColor(0, regs.texturing.tev_stage0);
        break;
    case PICA_REG_INDEX(texturing.tev_stage1.const_r):
        SyncTevConstColor(1, regs.texturing.tev_stage1);
        break;
    case PICA_REG_INDEX(texturing.tev_stage2.const_r):
        SyncTevConstColor(2, regs.texturing.tev_stage2);
        break;
    case PICA_REG_INDEX(texturing.tev_stage3.const_r):
        SyncTevConstColor(3, regs.texturing.tev_stage3);
        break;
    case PICA_REG_INDEX(texturing.tev_stage4.const_r):
        SyncTevConstColor(4, regs.texturing.tev_stage4);
        break;
    case PICA_REG_INDEX(texturing.tev_stage5.const_r):
        SyncTevConstColor(5, regs.texturing.tev_stage5);
        break;

    // TEV combiner buffer color
    case PICA_REG_INDEX(texturing.tev_combiner_buffer_color):
        SyncCombinerColor();
        break;

    // Fragment lighting switches
    /*case PICA_REG_INDEX(lighting.disable):
    case PICA_REG_INDEX(lighting.max_light_index):
    case PICA_REG_INDEX(lighting.config0):
    case PICA_REG_INDEX(lighting.config1):
    case PICA_REG_INDEX(lighting.abs_lut_input):
    case PICA_REG_INDEX(lighting.lut_input):
    case PICA_REG_INDEX(lighting.lut_scale):
    case PICA_REG_INDEX(lighting.light_enable):
        break;*/
    case PICA_REG_INDEX(lighting.lut_scale):
        SyncLightingLutScale();
        break;

    // Fragment lighting specular 0 color
    case PICA_REG_INDEX(lighting.light[0].specular_0):
    case PICA_REG_INDEX(lighting.light[1].specular_0):
    case PICA_REG_INDEX(lighting.light[2].specular_0):
    case PICA_REG_INDEX(lighting.light[3].specular_0):
    case PICA_REG_INDEX(lighting.light[4].specular_0):
    case PICA_REG_INDEX(lighting.light[5].specular_0):
    case PICA_REG_INDEX(lighting.light[6].specular_0):
    case PICA_REG_INDEX(lighting.light[7].specular_0):
        id -= PICA_REG_INDEX(lighting.light[0].specular_0);
        SyncLightSpecular0(id / 16);
        break;

    // Fragment lighting specular 1 color
    case PICA_REG_INDEX(lighting.light[0].specular_1):
    case PICA_REG_INDEX(lighting.light[1].specular_1):
    case PICA_REG_INDEX(lighting.light[2].specular_1):
    case PICA_REG_INDEX(lighting.light[3].specular_1):
    case PICA_REG_INDEX(lighting.light[4].specular_1):
    case PICA_REG_INDEX(lighting.light[5].specular_1):
    case PICA_REG_INDEX(lighting.light[6].specular_1):
    case PICA_REG_INDEX(lighting.light[7].specular_1):
        id -= PICA_REG_INDEX(lighting.light[0].specular_1);
        SyncLightSpecular1(id / 16);
        break;

    // Fragment lighting diffuse color
    case PICA_REG_INDEX(lighting.light[0].diffuse):
    case PICA_REG_INDEX(lighting.light[1].diffuse):
    case PICA_REG_INDEX(lighting.light[2].diffuse):
    case PICA_REG_INDEX(lighting.light[3].diffuse):
    case PICA_REG_INDEX(lighting.light[4].diffuse):
    case PICA_REG_INDEX(lighting.light[5].diffuse):
    case PICA_REG_INDEX(lighting.light[6].diffuse):
    case PICA_REG_INDEX(lighting.light[7].diffuse):
        id -= PICA_REG_INDEX(lighting.light[0].diffuse);
        SyncLightDiffuse(id / 16);
        break;

    // Fragment lighting ambient color
    case PICA_REG_INDEX(lighting.light[0].ambient):
    case PICA_REG_INDEX(lighting.light[1].ambient):
    case PICA_REG_INDEX(lighting.light[2].ambient):
    case PICA_REG_INDEX(lighting.light[3].ambient):
    case PICA_REG_INDEX(lighting.light[4].ambient):
    case PICA_REG_INDEX(lighting.light[5].ambient):
    case PICA_REG_INDEX(lighting.light[6].ambient):
    case PICA_REG_INDEX(lighting.light[7].ambient):
        id -= PICA_REG_INDEX(lighting.light[0].ambient);
        SyncLightAmbient(id / 16);
        break;

    // Fragment lighting position
    case PICA_REG_INDEX(lighting.light[0].x):
    case PICA_REG_INDEX(lighting.light[0].z):
    case PICA_REG_INDEX(lighting.light[1].x):
    case PICA_REG_INDEX(lighting.light[1].z):
    case PICA_REG_INDEX(lighting.light[2].x):
    case PICA_REG_INDEX(lighting.light[2].z):
    case PICA_REG_INDEX(lighting.light[3].x):
    case PICA_REG_INDEX(lighting.light[3].z):
    case PICA_REG_INDEX(lighting.light[4].x):
    case PICA_REG_INDEX(lighting.light[4].z):
    case PICA_REG_INDEX(lighting.light[5].x):
    case PICA_REG_INDEX(lighting.light[5].z):
    case PICA_REG_INDEX(lighting.light[6].x):
    case PICA_REG_INDEX(lighting.light[6].z):
    case PICA_REG_INDEX(lighting.light[7].x):
    case PICA_REG_INDEX(lighting.light[7].z):
        id -= PICA_REG_INDEX(lighting.light[0].x);
        SyncLightPosition(id / 16);
        break;

    // Fragment spot lighting direction
    case PICA_REG_INDEX(lighting.light[0].spot_x):
    case PICA_REG_INDEX(lighting.light[0].spot_z):
    case PICA_REG_INDEX(lighting.light[1].spot_x):
    case PICA_REG_INDEX(lighting.light[1].spot_z):
    case PICA_REG_INDEX(lighting.light[2].spot_x):
    case PICA_REG_INDEX(lighting.light[2].spot_z):
    case PICA_REG_INDEX(lighting.light[3].spot_x):
    case PICA_REG_INDEX(lighting.light[3].spot_z):
    case PICA_REG_INDEX(lighting.light[4].spot_x):
    case PICA_REG_INDEX(lighting.light[4].spot_z):
    case PICA_REG_INDEX(lighting.light[5].spot_x):
    case PICA_REG_INDEX(lighting.light[5].spot_z):
    case PICA_REG_INDEX(lighting.light[6].spot_x):
    case PICA_REG_INDEX(lighting.light[6].spot_z):
    case PICA_REG_INDEX(lighting.light[7].spot_x):
    case PICA_REG_INDEX(lighting.light[7].spot_z):
        id -= PICA_REG_INDEX(lighting.light[0].spot_x);
        SyncLightSpotDirection(id / 16);
        break;

    // Fragment lighting light source config
    case PICA_REG_INDEX(lighting.light[0].config):
    case PICA_REG_INDEX(lighting.light[1].config):
    case PICA_REG_INDEX(lighting.light[2].config):
    case PICA_REG_INDEX(lighting.light[3].config):
    case PICA_REG_INDEX(lighting.light[4].config):
    case PICA_REG_INDEX(lighting.light[5].config):
    case PICA_REG_INDEX(lighting.light[6].config):
    case PICA_REG_INDEX(lighting.light[7].config):
        shader_dirty = true;
        break;

    // Fragment lighting distance attenuation bias
    case PICA_REG_INDEX(lighting.light[0].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[1].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[2].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[3].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[4].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[5].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[6].dist_atten_bias):
    case PICA_REG_INDEX(lighting.light[7].dist_atten_bias):
        id -= PICA_REG_INDEX(lighting.light[0].dist_atten_bias);
        SyncLightDistanceAttenuationBias(id / 16);
        break;

    // Fragment lighting distance attenuation scale
    case PICA_REG_INDEX(lighting.light[0].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[1].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[2].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[3].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[4].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[5].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[6].dist_atten_scale):
    case PICA_REG_INDEX(lighting.light[7].dist_atten_scale):
        id -= PICA_REG_INDEX(lighting.light[0].dist_atten_scale);
        SyncLightDistanceAttenuationScale(id / 16);
        break;

    // Fragment lighting global ambient color (emission + ambient * ambient)
    case PICA_REG_INDEX(lighting.global_ambient):
        SyncGlobalAmbient();
        break;

    // Fragment lighting lookup tables
    /*case PICA_REG_INDEX(lighting.lut_data[0]):
    case PICA_REG_INDEX(lighting.lut_data[1]):
    case PICA_REG_INDEX(lighting.lut_data[2]):
    case PICA_REG_INDEX(lighting.lut_data[3]):
    case PICA_REG_INDEX(lighting.lut_data[4]):
    case PICA_REG_INDEX(lighting.lut_data[5]):
    case PICA_REG_INDEX(lighting.lut_data[6]):
    case PICA_REG_INDEX(lighting.lut_data[7]): {
        SyncLightingLutData();
        break;
    }*/
    }
}

void RasterizerOpenGL::FlushAll() {
    FlushSoftwareBatch(SoftwareBatching::Reason::FlushAll);
    res_cache.FlushAll();
}

void RasterizerOpenGL::FlushRegion(PAddr addr, u32 size) {
    FlushSoftwareBatch(SoftwareBatching::Reason::FlushRegion);
    res_cache.FlushRegion(addr, size);
}

void RasterizerOpenGL::InvalidateRegion(PAddr addr, u32 size) {
    FlushSoftwareBatch(SoftwareBatching::Reason::Invalidate);
    res_cache.InvalidateRegion(addr, size, nullptr);
}

void RasterizerOpenGL::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    FlushSoftwareBatch(SoftwareBatching::Reason::Invalidate);
    res_cache.FlushRegion(addr, size);
    res_cache.InvalidateRegion(addr, size, nullptr);
}

bool RasterizerOpenGL::AccelerateDisplayTransfer(const GPU::Regs::DisplayTransferConfig& config) {
    FlushSoftwareBatch(SoftwareBatching::Reason::Transfer);
    SurfaceParams src_params;
    src_params.addr = config.GetPhysicalInputAddress();
    src_params.width = config.output_width;
    src_params.stride = config.input_width;
    src_params.height = config.output_height;
    src_params.is_tiled = !config.input_linear;
    src_params.pixel_format = SurfaceParams::PixelFormatFromGPUPixelFormat(config.input_format);
    src_params.UpdateParams();

    SurfaceParams dst_params;
    dst_params.addr = config.GetPhysicalOutputAddress();
    dst_params.width = config.scaling != config.NoScale ? config.output_width.Value() / 2
                                                        : config.output_width.Value();
    dst_params.height = config.scaling == config.ScaleXY ? config.output_height.Value() / 2
                                                         : config.output_height.Value();
    dst_params.is_tiled = config.input_linear != config.dont_swizzle;
    dst_params.pixel_format = SurfaceParams::PixelFormatFromGPUPixelFormat(config.output_format);
    dst_params.UpdateParams();

    // hack for Tales of the Abyss / Pac Man Party 3D
    if (Settings::values.display_transfer_hack) {
        if (dst_params.height == 400) {
            if (dst_params.addr == 0x183CE430) {
                dst_params.addr -= 0xCE430;
                dst_params.end -= 0xCE430;
            } else if (dst_params.addr == 0x18387F30) {
                dst_params.addr -= 0x41A30;
                dst_params.end -= 0x41A30;
            }
        } else {
            if (dst_params.addr == 0x180B4830) {
                dst_params.addr -= 0x34830;
                dst_params.end -= 0x34830;
            } else if (dst_params.addr == 0x1807C430) {
                dst_params.addr += 0x3BFD0;
                dst_params.end += 0x3BFD0;
            }
        }
    }

    Common::Rectangle<u32> src_rect;
    Surface src_surface;
    std::tie(src_surface, src_rect) =
        res_cache.GetSurfaceSubRect(src_params, ScaleMatch::Ignore, true);
    if (src_surface == nullptr)
        return false;

    dst_params.res_scale = src_surface->res_scale;

    Common::Rectangle<u32> dst_rect;
    Surface dst_surface;
    std::tie(dst_surface, dst_rect) =
        res_cache.GetSurfaceSubRect(dst_params, ScaleMatch::Upscale, false);
    if (dst_surface == nullptr)
        return false;

    if (src_surface->is_tiled != dst_surface->is_tiled)
        std::swap(src_rect.top, src_rect.bottom);

    if (config.flip_vertically)
        std::swap(src_rect.top, src_rect.bottom);

    if (!res_cache.BlitSurfaces(src_surface, src_rect, dst_surface, dst_rect))
        return false;

    res_cache.InvalidateRegion(dst_params.addr, dst_params.size, dst_surface,
                               SurfaceInvalidationOrigin::DisplayTransfer);
    return true;
}

bool RasterizerOpenGL::AccelerateTextureCopy(const GPU::Regs::DisplayTransferConfig& config) {
    FlushSoftwareBatch(SoftwareBatching::Reason::Transfer);
    namespace Trace = Cara::TextureCopyTrace;
    const auto reject = [](Trace::Reason reason) { Trace::SetReason(reason); return false; };
    u32 copy_size = Common::AlignDown(config.texture_copy.size, 16);
    if (copy_size == 0) {
        return reject(Trace::Reason::ZeroSize);
    }

    u32 input_gap = config.texture_copy.input_gap * 16;
    u32 input_width = config.texture_copy.input_width * 16;
    if (input_width == 0 && input_gap != 0) {
        return reject(Trace::Reason::InputWidth);
    }
    if (input_gap == 0 || input_width >= copy_size) {
        input_width = copy_size;
        input_gap = 0;
    }
    if (copy_size % input_width != 0) {
        return reject(Trace::Reason::InputRemainder);
    }

    u32 output_gap = config.texture_copy.output_gap * 16;
    u32 output_width = config.texture_copy.output_width * 16;
    if (output_width == 0 && output_gap != 0) {
        return reject(Trace::Reason::OutputWidth);
    }
    if (output_gap == 0 || output_width >= copy_size) {
        output_width = copy_size;
        output_gap = 0;
    }
    if (copy_size % output_width != 0) {
        return reject(Trace::Reason::OutputRemainder);
    }

    SurfaceParams src_params;
    src_params.addr = config.GetPhysicalInputAddress();
    src_params.stride = input_width + input_gap; // stride in bytes
    src_params.width = input_width;              // width in bytes
    src_params.height = copy_size / input_width;
    src_params.size = ((src_params.height - 1) * src_params.stride) + src_params.width;
    src_params.end = src_params.addr + src_params.size;

    Common::Rectangle<u32> src_rect;
    Surface src_surface;
    std::tie(src_surface, src_rect) = res_cache.GetTexCopySurface(src_params);
    if (src_surface == nullptr) {
        Trace::SetReason(Settings::values.skip_texture_copy ? Trace::Reason::SourceMissSkipped : Trace::Reason::SourceMiss);
        return Settings::values.skip_texture_copy;
    }
    Trace::Source(*src_surface);

    if (output_gap != 0 &&
        (output_width != src_surface->BytesInPixels(src_rect.GetWidth() / src_surface->res_scale) *
                             (src_surface->is_tiled ? 8 : 1) ||
         output_gap % src_surface->BytesInPixels(src_surface->is_tiled ? 64 : 1) != 0)) {
        return reject(Trace::Reason::OutputLayout);
    }

    SurfaceParams dst_params = *src_surface;
    dst_params.addr = config.GetPhysicalOutputAddress();
    dst_params.width = src_rect.GetWidth() / src_surface->res_scale;
    dst_params.stride = dst_params.width + src_surface->PixelsInBytes(
                                               src_surface->is_tiled ? output_gap / 8 : output_gap);
    dst_params.height = src_rect.GetHeight() / src_surface->res_scale;
    dst_params.res_scale = src_surface->res_scale;
    dst_params.UpdateParams();

    // Since we are going to invalidate the gap if there is one, we will have to load it first
    const bool load_gap = output_gap != 0;
    Common::Rectangle<u32> dst_rect;
    Surface dst_surface;
    {
        Trace::Scope phase(Trace::Phase::DestinationAcquire);
        std::tie(dst_surface, dst_rect) =
            res_cache.GetSurfaceSubRect(dst_params, ScaleMatch::Upscale, load_gap);
    }
    if (dst_surface == nullptr) return reject(Trace::Reason::DestinationMiss);
    Trace::Destination(*dst_surface);

    if (dst_surface->type == SurfaceType::Texture) return reject(Trace::Reason::DestinationTexture);

    {
        Trace::Scope phase(Trace::Phase::Blit);
        if (!res_cache.BlitSurfaces(src_surface, src_rect, dst_surface, dst_rect))
            return reject(Trace::Reason::BlitFailed);
    }

    res_cache.InvalidateRegion(dst_params.addr, dst_params.size, dst_surface,
                               SurfaceInvalidationOrigin::TextureCopy);
    Trace::SetReason(Trace::Reason::Accelerated);
    return true;
}

bool RasterizerOpenGL::AccelerateFill(const GPU::Regs::MemoryFillConfig& config) {
    FlushSoftwareBatch(SoftwareBatching::Reason::Transfer);
    Surface dst_surface = res_cache.GetFillSurface(config);
    res_cache.InvalidateRegion(dst_surface->addr, dst_surface->size, dst_surface,
                               SurfaceInvalidationOrigin::Fill);
    return true;
}

bool RasterizerOpenGL::AccelerateDisplay(const GPU::Regs::FramebufferConfig& config,
                                         PAddr framebuffer_addr, u32 pixel_stride,
                                         ScreenInfo& screen_info) {
    FlushSoftwareBatch(SoftwareBatching::Reason::Present);
    if (framebuffer_addr == 0) {
        return false;
    }

    SurfaceParams src_params;
    src_params.addr = framebuffer_addr;
    src_params.width = std::min(config.width.Value(), pixel_stride);
    src_params.height = config.height;
    src_params.stride = pixel_stride;
    src_params.is_tiled = false;
    src_params.pixel_format = SurfaceParams::PixelFormatFromGPUPixelFormat(config.color_format);
    src_params.UpdateParams();

    Common::Rectangle<u32> src_rect;
    Surface src_surface;
    std::tie(src_surface, src_rect) =
        res_cache.GetSurfaceSubRect(src_params, ScaleMatch::Ignore, true);

    if (src_surface == nullptr) {
        return false;
    }

    u32 scaled_width = src_surface->GetScaledWidth();
    u32 scaled_height = src_surface->GetScaledHeight();

    screen_info.display_texcoords = Common::Rectangle<float>(
        (float)src_rect.bottom / (float)scaled_height, (float)src_rect.left / (float)scaled_width,
        (float)src_rect.top / (float)scaled_height, (float)src_rect.right / (float)scaled_width);

    screen_info.display_texture = src_surface->texture.handle;

    return true;
}

void RasterizerOpenGL::SamplerInfo::Create() {
    sampler.Create();
    mag_filter = min_filter = mip_filter = TextureConfig::Linear;
    wrap_s = wrap_t = TextureConfig::Repeat;
    border_color = 0;
    lod_min = lod_max = 0;
    lod_bias = 0;

    // default is 1000 and -1000
    // Other attributes have correct defaults
    glSamplerParameterf(sampler.handle, GL_TEXTURE_MAX_LOD, static_cast<float>(lod_max));
    glSamplerParameterf(sampler.handle, GL_TEXTURE_MIN_LOD, static_cast<float>(lod_min));
}

void RasterizerOpenGL::SamplerInfo::SyncWithConfig(const TextureConfig& config) {
    GLuint s = sampler.handle;

    using TextureFilter = Pica::TexturingRegs::TextureConfig::TextureFilter;
    auto selected_mag = config.mag_filter.Value();
    auto selected_min = config.min_filter.Value();
    bool use_linear_filter =
        Settings::values.use_linear_filter && selected_mag == TextureFilter::Nearest;
#ifdef __ANDROID__
    const auto filter = Settings::values.force_texture_filter;
    if ((filter == 1 || filter == 2) &&
        config.type != TextureConfig::Shadow2D && config.type != TextureConfig::ShadowCube) {
        selected_mag = selected_min = filter == 1 ? TextureFilter::Nearest : TextureFilter::Linear;
        use_linear_filter = false;
    }
#endif

    if (!use_linear_filter) {
        if (mag_filter != selected_mag) {
            mag_filter = selected_mag;
            glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER,
                                PicaToGL::TextureMagFilterMode(mag_filter));
        }

        if (min_filter != selected_min || mip_filter != config.mip_filter) {
            min_filter = selected_min;
            mip_filter = config.mip_filter;
            if (config.type == Pica::TexturingRegs::TextureConfig::TextureCube) {
                // HACK: use mag filter converter for min filter because they are the same anyway
                glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER,
                                    PicaToGL::TextureMagFilterMode(min_filter));
            } else {
                glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER,
                                    PicaToGL::TextureMinFilterMode(min_filter, mip_filter));
            }
        }
    }

    if (wrap_s != config.wrap_s) {
        wrap_s = config.wrap_s;
        glSamplerParameteri(s, GL_TEXTURE_WRAP_S, PicaToGL::WrapMode(wrap_s));
    }
    if (wrap_t != config.wrap_t) {
        wrap_t = config.wrap_t;
        glSamplerParameteri(s, GL_TEXTURE_WRAP_T, PicaToGL::WrapMode(wrap_t));
    }

    if (wrap_s == TextureConfig::ClampToBorder || wrap_t == TextureConfig::ClampToBorder) {
        if (border_color != config.border_color.raw) {
            border_color = config.border_color.raw;
            auto gl_color = PicaToGL::ColorRGBA8(border_color);
            glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, gl_color.data());
        }
    }

    if (lod_min != config.lod.min_level) {
        lod_min = config.lod.min_level;
        glSamplerParameterf(s, GL_TEXTURE_MIN_LOD, static_cast<float>(lod_min));
    }

    if (lod_max != config.lod.max_level) {
        lod_max = config.lod.max_level;
        glSamplerParameterf(s, GL_TEXTURE_MAX_LOD, static_cast<float>(lod_max));
    }

    if (lod_bias != config.lod.bias) {
        lod_bias = config.lod.bias;
        if (GLES) {
            LOG_TRACE(Render_OpenGL, "sync sampler lod bias: {}", lod_bias);
        } else {
            glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS, lod_bias / 256.0f);
        }
    }
}

void RasterizerOpenGL::SetShader() {
    Common::FrameTiming::Scope shader_setup(Common::FrameTiming::Phase::DrawShaderSetup,
                                           Common::FrameTiming::RenderBookkeepingProfileEnabled());
#ifdef CARA_IOS_GLES
    static const bool probe_runtime_tev = [] {
        const char* value = std::getenv("CARA_GLES_RUNTIME_TEV");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (probe_runtime_tev) {
        ShaderProgramManager::RuntimeTevWords next{};
        if (!native_borrow_fragment_selected && shader_program_manager->TryRuntimeTev(Pica::g_state.regs, next)) {
            if (runtime_tev_words != next) {
                runtime_tev_words = next;
                uniform_block_data.light_dirty = true;
            }
            return;
        }
        if (runtime_tev_words) {
            runtime_tev_words.reset();
            uniform_block_data.light_dirty = true;
        }
    }
    if(!shader_program_manager->UseFragmentShader(Pica::g_state.regs,native_borrow_fragment_selected))
        native_borrow_restart=true;
#else
    shader_program_manager->UseFragmentShader(Pica::g_state.regs);
#endif
}

void RasterizerOpenGL::SyncClipEnabled() {
    state.clip_distance[1] = Pica::g_state.regs.rasterizer.clip_enable != 0;
}

void RasterizerOpenGL::SyncClipCoef() {
    if (Settings::values.disable_clip_coef) {
        return;
    }

    const auto raw_clip_coef = Pica::g_state.regs.rasterizer.GetClipCoef();
    const GLvec4 new_clip_coef = {raw_clip_coef.x.ToFloat32(), raw_clip_coef.y.ToFloat32(),
                                  raw_clip_coef.z.ToFloat32(), raw_clip_coef.w.ToFloat32()};
    if (new_clip_coef != uniform_block_data.data.clip_coef) {
        uniform_block_data.data.clip_coef = new_clip_coef;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncCullMode() {
    const auto& regs = Pica::g_state.regs;
    state.cull.enabled = regs.rasterizer.cull_mode != Pica::RasterizerRegs::CullMode::KeepAll;
    state.cull.front_face =
        regs.rasterizer.cull_mode == Pica::RasterizerRegs::CullMode::KeepClockWise ? GL_CW : GL_CCW;
}

void RasterizerOpenGL::SyncDepthScale() {
    float depth_scale =
        Pica::float24::FromRaw(Pica::g_state.regs.rasterizer.viewport_depth_range).ToFloat32();
    if (depth_scale != uniform_block_data.data.depth_scale) {
        uniform_block_data.data.depth_scale = depth_scale;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncDepthOffset() {
    float depth_offset =
        Pica::float24::FromRaw(Pica::g_state.regs.rasterizer.viewport_depth_near_plane).ToFloat32();
    if (depth_offset != uniform_block_data.data.depth_offset) {
        uniform_block_data.data.depth_offset = depth_offset;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncBlendEnabled() {
    state.blend.enabled = (Pica::g_state.regs.framebuffer.output_merger.alphablend_enable == 1);
}

void RasterizerOpenGL::SyncBlendFuncs() {
    const auto& regs = Pica::g_state.regs;
    state.blend.rgb_equation =
        PicaToGL::BlendEquation(regs.framebuffer.output_merger.alpha_blending.blend_equation_rgb);
    state.blend.a_equation =
        PicaToGL::BlendEquation(regs.framebuffer.output_merger.alpha_blending.blend_equation_a);
    state.blend.src_rgb_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_source_rgb);
    state.blend.dst_rgb_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_dest_rgb);
    state.blend.src_a_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_source_a);
    state.blend.dst_a_func =
        PicaToGL::BlendFunc(regs.framebuffer.output_merger.alpha_blending.factor_dest_a);
}

void RasterizerOpenGL::SyncBlendColor() {
    auto blend_color =
        PicaToGL::ColorRGBA8(Pica::g_state.regs.framebuffer.output_merger.blend_const.raw);
    state.blend.color.red = blend_color[0];
    state.blend.color.green = blend_color[1];
    state.blend.color.blue = blend_color[2];
    state.blend.color.alpha = blend_color[3];
}

void RasterizerOpenGL::SyncFogColor() {
    const auto& regs = Pica::g_state.regs;
    GLvec3 fog_color;
    fog_color[0] = uniform_block_data.data.fog_color[0];
    fog_color[1] = uniform_block_data.data.fog_color[1];
    fog_color[2] = uniform_block_data.data.fog_color[2];
    uniform_block_data.data.fog_color = {
        regs.texturing.fog_color.r.Value() / 255.0f,
        regs.texturing.fog_color.g.Value() / 255.0f,
        regs.texturing.fog_color.b.Value() / 255.0f,
    };
    if (fog_color[0] != uniform_block_data.data.fog_color[0] ||
        fog_color[1] != uniform_block_data.data.fog_color[1] ||
        fog_color[2] != uniform_block_data.data.fog_color[2]) {
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncFogLutData() {
    uniform_block_data.fog_lut_dirty = true;
}

void RasterizerOpenGL::SyncProcTexNoise() {
    const auto& regs = Pica::g_state.regs.texturing;
    uniform_block_data.data.proctex_noise_f = {
        Pica::float16::FromRaw(regs.proctex_noise_frequency.u).ToFloat32(),
        Pica::float16::FromRaw(regs.proctex_noise_frequency.v).ToFloat32(),
    };
    uniform_block_data.data.proctex_noise_a = {
        regs.proctex_noise_u.amplitude / 4095.0f,
        regs.proctex_noise_v.amplitude / 4095.0f,
    };
    uniform_block_data.data.proctex_noise_p = {
        Pica::float16::FromRaw(regs.proctex_noise_u.phase).ToFloat32(),
        Pica::float16::FromRaw(regs.proctex_noise_v.phase).ToFloat32(),
    };
    uniform_block_data.dirty = true;
}

void RasterizerOpenGL::SyncProcTexBias() {
    const auto& regs = Pica::g_state.regs.texturing;
    uniform_block_data.data.proctex_bias =
        Pica::float16::FromRaw(regs.proctex.bias_low | (regs.proctex_lut.bias_high << 8))
            .ToFloat32();
    uniform_block_data.dirty = true;
    shader_dirty = true;
}

void RasterizerOpenGL::SyncProcTexLutData() {
    using Pica::TexturingRegs;
    const auto& regs = Pica::g_state.regs;
    switch (regs.texturing.proctex_lut_config.ref_table.Value()) {
    case TexturingRegs::ProcTexLutTable::Noise:
        uniform_block_data.proctex_noise_lut_dirty = true;
        break;
    case TexturingRegs::ProcTexLutTable::ColorMap:
        uniform_block_data.proctex_color_map_dirty = true;
        break;
    case TexturingRegs::ProcTexLutTable::AlphaMap:
        uniform_block_data.proctex_alpha_map_dirty = true;
        break;
    case TexturingRegs::ProcTexLutTable::Color:
        uniform_block_data.proctex_lut_dirty = true;
        break;
    case TexturingRegs::ProcTexLutTable::ColorDiff:
        uniform_block_data.proctex_diff_lut_dirty = true;
        break;
    }
}

void RasterizerOpenGL::SyncAlphaTest() {
    const auto& regs = Pica::g_state.regs;
    if (regs.framebuffer.output_merger.alpha_test.ref != uniform_block_data.data.alphatest_ref) {
        uniform_block_data.data.alphatest_ref = regs.framebuffer.output_merger.alpha_test.ref;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncLogicOp() {
    const auto& regs = Pica::g_state.regs;
    state.logic_op = PicaToGL::LogicOp(regs.framebuffer.output_merger.logic_op);
    /*if (GLES && (GLAD_GL_ARM_shader_framebuffer_fetch || GLAD_GL_EXT_shader_framebuffer_fetch)) {
        shader_dirty = true;
    }*/
    if (GLES) {
        if (!regs.framebuffer.output_merger.alphablend_enable) {
            if (regs.framebuffer.output_merger.logic_op == Pica::FramebufferRegs::LogicOp::NoOp) {
                // Color output is disabled by logic operation. We use color write mask to skip
                // color but allow depth write.
                state.color_mask = {};
            }
        }
    }
}

void RasterizerOpenGL::SyncColorWriteMask() {
    const auto& regs = Pica::g_state.regs;
    if (GLES) {
        if (!regs.framebuffer.output_merger.alphablend_enable) {
            if (regs.framebuffer.output_merger.logic_op == Pica::FramebufferRegs::LogicOp::NoOp) {
                // Color output is disabled by logic operation. We use color write mask to skip
                // color but allow depth write. Return early to avoid overwriting this.
                return;
            }
        }
    }

    auto IsColorWriteEnabled = [&](u32 value) {
        return (regs.framebuffer.framebuffer.allow_color_write != 0 && value != 0) ? GL_TRUE
                                                                                   : GL_FALSE;
    };

    state.color_mask.red_enabled = IsColorWriteEnabled(regs.framebuffer.output_merger.red_enable);
    state.color_mask.green_enabled =
        IsColorWriteEnabled(regs.framebuffer.output_merger.green_enable);
    state.color_mask.blue_enabled = IsColorWriteEnabled(regs.framebuffer.output_merger.blue_enable);
    state.color_mask.alpha_enabled =
        IsColorWriteEnabled(regs.framebuffer.output_merger.alpha_enable);
}

void RasterizerOpenGL::SyncStencilWriteMask() {
    const auto& regs = Pica::g_state.regs;
    state.stencil.write_mask =
        (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0)
            ? static_cast<GLuint>(regs.framebuffer.output_merger.stencil_test.write_mask)
            : 0;
}

void RasterizerOpenGL::SyncDepthWriteMask() {
    const auto& regs = Pica::g_state.regs;
    state.depth.write_mask = (regs.framebuffer.framebuffer.allow_depth_stencil_write != 0 &&
                              regs.framebuffer.output_merger.depth_write_enable)
                                 ? GL_TRUE
                                 : GL_FALSE;
}

void RasterizerOpenGL::SyncStencilTest() {
    const auto& regs = Pica::g_state.regs;
    state.stencil.test_enabled =
        regs.framebuffer.output_merger.stencil_test.enable &&
        regs.framebuffer.framebuffer.depth_format == Pica::FramebufferRegs::DepthFormat::D24S8;
    state.stencil.test_func =
        PicaToGL::CompareFunc(regs.framebuffer.output_merger.stencil_test.func);
    state.stencil.test_ref = regs.framebuffer.output_merger.stencil_test.reference_value;
    state.stencil.test_mask = regs.framebuffer.output_merger.stencil_test.input_mask;
    state.stencil.action_stencil_fail =
        PicaToGL::StencilOp(regs.framebuffer.output_merger.stencil_test.action_stencil_fail);
    state.stencil.action_depth_fail =
        PicaToGL::StencilOp(regs.framebuffer.output_merger.stencil_test.action_depth_fail);
    state.stencil.action_depth_pass =
        PicaToGL::StencilOp(regs.framebuffer.output_merger.stencil_test.action_depth_pass);
}

void RasterizerOpenGL::SyncDepthTest() {
    const auto& regs = Pica::g_state.regs;
    state.depth.test_enabled = regs.framebuffer.output_merger.depth_test_enable == 1 ||
                               regs.framebuffer.output_merger.depth_write_enable == 1;
    state.depth.test_func =
        regs.framebuffer.output_merger.depth_test_enable == 1
            ? PicaToGL::CompareFunc(regs.framebuffer.output_merger.depth_test_func)
            : GL_ALWAYS;
}

void RasterizerOpenGL::SyncCombinerColor() {
    const auto combiner_color =
        PicaToGL::ColorRGBA8(Pica::g_state.regs.texturing.tev_combiner_buffer_color.raw);
    if (combiner_color != uniform_block_data.data.tev_combiner_buffer_color) {
        uniform_block_data.data.tev_combiner_buffer_color = combiner_color;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncTevConstColor(int stage_index,
                                         const Pica::TexturingRegs::TevStageConfig& tev_stage) {
    const auto const_color = PicaToGL::ColorRGBA8(tev_stage.const_color);
    if (const_color != uniform_block_data.data.const_color[stage_index]) {
        uniform_block_data.data.const_color[stage_index] = const_color;
        uniform_block_data.dirty = true;
    }
}

void RasterizerOpenGL::SyncGlobalAmbient() {
    const auto ambient_color = PicaToGL::LightColor(Pica::g_state.regs.lighting.global_ambient);
    if (ambient_color != uniform_block_data.light_data.lighting_global_ambient) {
        uniform_block_data.light_data.lighting_global_ambient = ambient_color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightingLutScale() {
    const auto& regs = Pica::g_state.regs;
    GLfloat scales[7];
    scales[0] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.d0);
    scales[1] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.d1);
    scales[2] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.sp);
    scales[3] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.fr);
    scales[4] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.rb);
    scales[5] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.rg);
    scales[6] = regs.lighting.lut_scale.GetScale(regs.lighting.lut_scale.rr);
    for (int i = 0; i < 7; ++i) {
        if (scales[i] != uniform_block_data.light_data.lighting_lut_scales[i]) {
            uniform_block_data.light_data.lighting_lut_scales[i] = scales[i];
            uniform_block_data.light_dirty = true;
        }
    }
}

void RasterizerOpenGL::SyncLightingLutData() {
    const auto& regs = Pica::g_state.regs;
    auto& lut_config = regs.lighting.lut_config;
    uniform_block_data.lighting_lut_dirty[lut_config.type] = true;
    uniform_block_data.lighting_lut_dirty_any = true;
}

void RasterizerOpenGL::SyncLightSpecular0(int light_index) {
    auto color = PicaToGL::LightColor(Pica::g_state.regs.lighting.light[light_index].specular_0);
    if (color != uniform_block_data.light_data.light_src[light_index].specular_0) {
        uniform_block_data.light_data.light_src[light_index].specular_0 = color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightSpecular1(int light_index) {
    auto color = PicaToGL::LightColor(Pica::g_state.regs.lighting.light[light_index].specular_1);
    if (color != uniform_block_data.light_data.light_src[light_index].specular_1) {
        uniform_block_data.light_data.light_src[light_index].specular_1 = color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightDiffuse(int light_index) {
    auto color = PicaToGL::LightColor(Pica::g_state.regs.lighting.light[light_index].diffuse);
    if (color != uniform_block_data.light_data.light_src[light_index].diffuse) {
        uniform_block_data.light_data.light_src[light_index].diffuse = color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightAmbient(int light_index) {
    auto color = PicaToGL::LightColor(Pica::g_state.regs.lighting.light[light_index].ambient);
    if (color != uniform_block_data.light_data.light_src[light_index].ambient) {
        uniform_block_data.light_data.light_src[light_index].ambient = color;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightPosition(int light_index) {
    GLvec3 position = {
        Pica::float16::FromRaw(Pica::g_state.regs.lighting.light[light_index].x).ToFloat32(),
        Pica::float16::FromRaw(Pica::g_state.regs.lighting.light[light_index].y).ToFloat32(),
        Pica::float16::FromRaw(Pica::g_state.regs.lighting.light[light_index].z).ToFloat32()};

    if (position != uniform_block_data.light_data.light_src[light_index].position) {
        uniform_block_data.light_data.light_src[light_index].position = position;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightSpotDirection(int light_index) {
    const auto& light = Pica::g_state.regs.lighting.light[light_index];
    GLvec3 spot_direction = {light.spot_x / 2047.0f, light.spot_y / 2047.0f,
                             light.spot_z / 2047.0f};

    if (spot_direction != uniform_block_data.light_data.light_src[light_index].spot_direction) {
        uniform_block_data.light_data.light_src[light_index].spot_direction = spot_direction;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightDistanceAttenuationBias(int light_index) {
    GLfloat dist_atten_bias =
        Pica::float20::FromRaw(Pica::g_state.regs.lighting.light[light_index].dist_atten_bias)
            .ToFloat32();

    if (dist_atten_bias != uniform_block_data.light_data.light_src[light_index].dist_atten_bias) {
        uniform_block_data.light_data.light_src[light_index].dist_atten_bias = dist_atten_bias;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncLightDistanceAttenuationScale(int light_index) {
    GLfloat dist_atten_scale =
        Pica::float20::FromRaw(Pica::g_state.regs.lighting.light[light_index].dist_atten_scale)
            .ToFloat32();

    if (dist_atten_scale != uniform_block_data.light_data.light_src[light_index].dist_atten_scale) {
        uniform_block_data.light_data.light_src[light_index].dist_atten_scale = dist_atten_scale;
        uniform_block_data.light_dirty = true;
    }
}

void RasterizerOpenGL::SyncShadowBias() {
    const auto& shadow = Pica::g_state.regs.framebuffer.shadow;
    GLfloat constant = Pica::float16::FromRaw(shadow.constant).ToFloat32();
    GLfloat linear = Pica::float16::FromRaw(shadow.linear).ToFloat32();

    if (constant != uniform_block_data.data.shadow_bias_constant ||
        linear != uniform_block_data.data.shadow_bias_linear) {
        uniform_block_data.data.shadow_bias_constant = constant;
        uniform_block_data.data.shadow_bias_linear = linear;
        uniform_block_data.dirty = AllowShadow;
    }
}

void RasterizerOpenGL::SyncShadowTextureBias() {
    GLint bias = Pica::g_state.regs.texturing.shadow.bias << 1;
    if (bias != uniform_block_data.light_data.shadow_texture_bias) {
        uniform_block_data.light_data.shadow_texture_bias = bias;
        uniform_block_data.light_dirty = AllowShadow;
    }
}

void RasterizerOpenGL::SyncAndUploadLUTsLF() {
    constexpr std::size_t max_size =
        sizeof(GLvec2) * 256 * Pica::LightingRegs::NumLightingSampler + sizeof(GLvec2) * 128; // fog

    if (!uniform_block_data.lighting_lut_dirty_any && !uniform_block_data.fog_lut_dirty) {
        return;
    }

    u8* buffer;
    GLintptr offset;
    bool invalidate;
    std::size_t bytes_used = 0;
#ifdef CARA_IOS_GLES
    if (!UseNativeLUTBuffers()) {
        std::tie(buffer, offset, invalidate) = texture_lf_buffer.MapStaging(max_size, sizeof(GLvec4));
    } else
#endif
    {
        glBindBuffer(StreamBufferTarget(GL_TEXTURE_BUFFER), texture_lf_buffer.GetHandle());
        std::tie(buffer, offset, invalidate) = texture_lf_buffer.Map(max_size, sizeof(GLvec4));
    }

    // Sync the lighting luts
    if (uniform_block_data.lighting_lut_dirty_any || invalidate) {
        for (unsigned index = 0; index < uniform_block_data.lighting_lut_dirty.size(); index++) {
            if (uniform_block_data.lighting_lut_dirty[index] || invalidate) {
                bool is_changed = false;
                for (std::size_t i = 0; i < lighting_lut_data[index].size(); ++i) {
                    const auto& entry = Pica::g_state.lighting.luts[index][i];
                    GLvec2 new_data{entry.ToFloat(), entry.DiffToFloat()};
                    const auto& prev_data = lighting_lut_data[index][i];
                    if (new_data != prev_data) {
                        lighting_lut_data[index][i] = new_data;
                        is_changed = true;
                    }
                }
                auto& lut_offset =
                    uniform_block_data.light_data.lighting_lut_offset[index / 4][index % 4];
                if (is_changed || invalidate || lut_offset < 0) {
                    std::memcpy(buffer + bytes_used, lighting_lut_data[index].data(),
                                lighting_lut_data[index].size() * sizeof(GLvec2));
                    lut_offset = (offset + bytes_used) / sizeof(GLvec2);
                    uniform_block_data.light_dirty = true;
                    bytes_used += lighting_lut_data[index].size() * sizeof(GLvec2);
                }
                uniform_block_data.lighting_lut_dirty[index] = false;
            }
        }
        uniform_block_data.lighting_lut_dirty_any = false;
    }

    // Sync the fog lut
    if (uniform_block_data.fog_lut_dirty || invalidate) {
        bool is_changed = false;
        for (std::size_t i = 0; i < fog_lut_data.size(); ++i) {
            const auto& entry = Pica::g_state.fog.lut[i];
            GLvec2 new_data{entry.ToFloat(), entry.DiffToFloat()};
            const auto& prev_data = fog_lut_data[i];
            if (new_data != prev_data) {
                fog_lut_data[i] = new_data;
                is_changed = true;
            }
        }
        if (is_changed || invalidate || uniform_block_data.data.fog_lut_offset < 0) {
            std::memcpy(buffer + bytes_used, fog_lut_data.data(),
                        fog_lut_data.size() * sizeof(GLvec2));
            uniform_block_data.data.fog_lut_offset = (offset + bytes_used) / sizeof(GLvec2);
            uniform_block_data.dirty = true;
            bytes_used += fog_lut_data.size() * sizeof(GLvec2);
        }
        uniform_block_data.fog_lut_dirty = false;
    }

#ifdef CARA_IOS_GLES
    if (!UseNativeLUTBuffers()) {
        UploadIOSLUT(texture_buffer_lut_lf.handle, TextureUnits::TextureBufferLUT_LF.id, GL_RG,
                     offset, buffer, bytes_used);
        texture_lf_buffer.FinishStaging(bytes_used);
    } else
#endif
        texture_lf_buffer.Unmap(bytes_used);
}

void RasterizerOpenGL::SyncAndUploadLUTs() {
    constexpr std::size_t max_size = sizeof(GLvec2) * 128 * 3 + // proctex: noise + color + alpha
                                     sizeof(GLvec4) * 256 +     // proctex
                                     sizeof(GLvec4) * 256;      // proctex diff

    if (!uniform_block_data.proctex_noise_lut_dirty &&
        !uniform_block_data.proctex_color_map_dirty &&
        !uniform_block_data.proctex_alpha_map_dirty && !uniform_block_data.proctex_lut_dirty &&
        !uniform_block_data.proctex_diff_lut_dirty) {
        return;
    }

    u8* buffer;
    GLintptr offset;
    bool invalidate;
    std::size_t bytes_used = 0;
#ifdef CARA_IOS_GLES
    if (!UseNativeLUTBuffers()) {
        std::tie(buffer, offset, invalidate) = texture_buffer.MapStaging(max_size, sizeof(GLvec4));
    } else
#endif
    {
        glBindBuffer(StreamBufferTarget(GL_TEXTURE_BUFFER), texture_buffer.GetHandle());
        std::tie(buffer, offset, invalidate) = texture_buffer.Map(max_size, sizeof(GLvec4));
    }

    // helper function for SyncProcTexNoiseLUT/ColorMap/AlphaMap
    auto SyncProcTexValueLUT = [this, buffer, offset, invalidate, &bytes_used](
                                   const std::array<Pica::State::ProcTex::ValueEntry, 128>& lut,
                                   std::array<GLvec2, 128>& lut_data, GLint& lut_offset) {
        bool is_changed = false;
        for (std::size_t i = 0; i < lut_data.size(); ++i) {
            const auto& entry = lut[i];
            GLvec2 new_data{entry.ToFloat(), entry.DiffToFloat()};
            const auto& prev_data = lut_data[i];
            if (new_data != prev_data) {
                lut_data[i] = new_data;
                is_changed = true;
            }
        }
        if (is_changed || invalidate || lut_offset < 0) {
            std::memcpy(buffer + bytes_used, lut_data.data(), lut_data.size() * sizeof(GLvec2));
            lut_offset = (offset + bytes_used) / sizeof(GLvec2);
            uniform_block_data.dirty = true;
            bytes_used += lut_data.size() * sizeof(GLvec2);
        }
    };

    // Sync the proctex noise lut
    if (uniform_block_data.proctex_noise_lut_dirty || invalidate) {
        SyncProcTexValueLUT(Pica::g_state.proctex.noise_table, proctex_noise_lut_data,
                            uniform_block_data.data.proctex_noise_lut_offset);
        uniform_block_data.proctex_noise_lut_dirty = false;
    }

    // Sync the proctex color map
    if (uniform_block_data.proctex_color_map_dirty || invalidate) {
        SyncProcTexValueLUT(Pica::g_state.proctex.color_map_table, proctex_color_map_data,
                            uniform_block_data.data.proctex_color_map_offset);
        uniform_block_data.proctex_color_map_dirty = false;
    }

    // Sync the proctex alpha map
    if (uniform_block_data.proctex_alpha_map_dirty || invalidate) {
        SyncProcTexValueLUT(Pica::g_state.proctex.alpha_map_table, proctex_alpha_map_data,
                            uniform_block_data.data.proctex_alpha_map_offset);
        uniform_block_data.proctex_alpha_map_dirty = false;
    }

    // Sync the proctex lut
    if (uniform_block_data.proctex_lut_dirty || invalidate) {
        bool is_changed = false;
        for (std::size_t i = 0; i < proctex_lut_data.size(); ++i) {
            const auto& entry = Pica::g_state.proctex.color_table[i];
            GLvec4 new_color{static_cast<u8>(entry.r) / 255.0f, static_cast<u8>(entry.g) / 255.0f,
                             static_cast<u8>(entry.b) / 255.0f, static_cast<u8>(entry.a) / 255.0f};
            const auto& prev_color = proctex_lut_data[i];
            if (new_color != prev_color) {
                proctex_lut_data[i] = new_color;
                is_changed = true;
            }
        }
        if (is_changed || invalidate || uniform_block_data.data.proctex_lut_offset < 0) {
            std::memcpy(buffer + bytes_used, proctex_lut_data.data(),
                        proctex_lut_data.size() * sizeof(GLvec4));
            uniform_block_data.data.proctex_lut_offset = (offset + bytes_used) / sizeof(GLvec4);
            uniform_block_data.dirty = true;
            bytes_used += proctex_lut_data.size() * sizeof(GLvec4);
        }
        uniform_block_data.proctex_lut_dirty = false;
    }

    // Sync the proctex difference lut
    if (uniform_block_data.proctex_diff_lut_dirty || invalidate) {
        bool is_changed = false;
        for (std::size_t i = 0; i < proctex_diff_lut_data.size(); ++i) {
            const auto& entry = Pica::g_state.proctex.color_diff_table[i];
            GLvec4 new_color{static_cast<u8>(entry.r) / 255.0f, static_cast<u8>(entry.g) / 255.0f,
                             static_cast<u8>(entry.b) / 255.0f, static_cast<u8>(entry.a) / 255.0f};
            const auto& prev_color = proctex_diff_lut_data[i];
            if (new_color != prev_color) {
                proctex_diff_lut_data[i] = new_color;
                is_changed = true;
            }
        }
        if (is_changed || invalidate || uniform_block_data.data.proctex_diff_lut_offset < 0) {
            std::memcpy(buffer + bytes_used, proctex_diff_lut_data.data(),
                        proctex_diff_lut_data.size() * sizeof(GLvec4));
            uniform_block_data.data.proctex_diff_lut_offset =
                (offset + bytes_used) / sizeof(GLvec4);
            uniform_block_data.dirty = true;
            bytes_used += proctex_diff_lut_data.size() * sizeof(GLvec4);
        }
        uniform_block_data.proctex_diff_lut_dirty = false;
    }

#ifdef CARA_IOS_GLES
    if (!UseNativeLUTBuffers()) {
        UploadIOSLUT(texture_buffer_lut_rg.handle, TextureUnits::TextureBufferLUT_RG.id, GL_RG,
                     offset, buffer, bytes_used);
        UploadIOSLUT(texture_buffer_lut_rgba.handle, TextureUnits::TextureBufferLUT_RGBA.id, GL_RGBA,
                     offset, buffer, bytes_used);
        texture_buffer.FinishStaging(bytes_used);
    } else
#endif
        texture_buffer.Unmap(bytes_used);
}

// Cara GPU oracle: exercise the production upload/cache path before guest execution.
bool RasterizerOpenGL::RunUniformReuseSelfTest(std::string& error) {
    const char* reuse = std::getenv("CARA_VS_UNIFORM_REUSE");
    if (!reuse || std::strcmp(reuse, "1") != 0) {
        error = "uniform reuse oracle requires CARA_VS_UNIFORM_REUSE=1 before first upload";
        return false;
    }
    struct Range {
        GLuint buffer;
        GLint64 offset;
        GLint64 size;
        bool operator==(const Range& other) const {
            return buffer == other.buffer && offset == other.offset && size == other.size;
        }
    };
    const auto range = [](UniformBindings binding) {
        const GLuint index = static_cast<GLuint>(binding);
        GLint buffer = 0;
        GLint64 offset = 0, size = 0;
        glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, index, &buffer);
        glGetInteger64i_v(GL_UNIFORM_BUFFER_START, index, &offset);
        glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, index, &size);
        return Range{static_cast<GLuint>(buffer), offset, size};
    };
    constexpr std::array<UniformBindings, 3> bindings{
        UniformBindings::VS, UniformBindings::Common, UniformBindings::Light};
    const std::array<Range, 3> saved_ranges{range(bindings[0]), range(bindings[1]), range(bindings[2])};
    GLint saved_generic = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &saved_generic);
    const GLuint saved_tracked = OpenGLState::GetCurState().draw.uniform_buffer;
    const auto saved_regs = Pica::g_state.regs.vs;
    const auto saved_uniforms = Pica::g_state.vs.uniforms;
    const auto saved_blocks = uniform_block_data;
    SCOPE_EXIT({
        Pica::g_state.regs.vs = saved_regs;
        Pica::g_state.vs.uniforms = saved_uniforms;
        uniform_block_data = saved_blocks;
        // Ring contents were deliberately invalidated; restored CPU state must
        // be uploaded before the first real draw, including a nonaccelerated one.
        previous_vs_uniforms_valid = false;
        uniform_block_data.dirty = true;
        uniform_block_data.light_dirty = true;
        for (size_t n = 0; n < bindings.size(); ++n) {
            const auto& saved = saved_ranges[n];
            const GLuint index = static_cast<GLuint>(bindings[n]);
            if (saved.buffer != 0 && saved.size > 0) {
                glBindBufferRange(GL_UNIFORM_BUFFER, index, saved.buffer,
                                  static_cast<GLintptr>(saved.offset), static_cast<GLsizeiptr>(saved.size));
            } else {
                glBindBufferBase(GL_UNIFORM_BUFFER, index, saved.buffer);
            }
        }
        OpenGLState::BindUniformBuffer(saved_tracked);
        glBindBuffer(GL_UNIFORM_BUFFER, static_cast<GLuint>(saved_generic));
    });
    const auto fail = [&](const char* stage) {
        error = std::string("uniform reuse oracle: ") + stage;
        return false;
    };
    if (glGetError() != GL_NO_ERROR) return fail("preexisting GL error");
    const auto read = [&](UniformBindings binding, void* destination, size_t size) {
        const auto indexed = range(binding);
        if (indexed.buffer != uniform_buffer.GetHandle() || indexed.offset < 0 ||
            indexed.size != static_cast<GLint64>(size) ||
            indexed.offset + indexed.size > static_cast<GLint64>(uniform_buffer.GetSize())) return false;
        GLint native = 0;
        glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &native);
        const GLuint tracked = OpenGLState::GetCurState().draw.uniform_buffer;
        OpenGLState::BindUniformBuffer(indexed.buffer);
        SCOPE_EXIT({
            OpenGLState::BindUniformBuffer(tracked);
            glBindBuffer(GL_UNIFORM_BUFFER, static_cast<GLuint>(native));
        });
        const void* mapped = glMapBufferRange(GL_UNIFORM_BUFFER, static_cast<GLintptr>(indexed.offset),
                                              static_cast<GLsizeiptr>(size), GL_MAP_READ_BIT);
        if (!mapped) return false;
        std::memcpy(destination, mapped, size);
        const bool unmapped = glUnmapBuffer(GL_UNIFORM_BUFFER) == GL_TRUE;
        return unmapped && glGetError() == GL_NO_ERROR;
    };
    const auto check_vs = [&] {
        VSUniformData expected{}, actual{};
        expected.uniforms.SetFromRegs(Pica::g_state.regs.vs, Pica::g_state.vs);
        if (!read(UniformBindings::VS, &actual, sizeof(actual))) return false;
        // BoolAligned padding is deliberately excluded, exactly as in the cache key.
        const auto& a = actual.uniforms;
        const auto& b = expected.uniforms;
        if (std::memcmp(a.i.data(), b.i.data(), sizeof(a.i)) != 0 ||
            std::memcmp(a.f.data(), b.f.data(), sizeof(a.f)) != 0) return false;
        for (size_t n = 0; n < a.bools.size(); ++n) {
            if (a.bools[n].b != b.bools[n].b) return false;
        }
        return true;
    };
    const auto check_blocks = [&] {
        UniformData fs{};
        UniformLightData light{};
        return read(UniformBindings::Common, &fs, sizeof(fs)) &&
               read(UniformBindings::Light, &light, sizeof(light)) &&
               std::memcmp(&fs, &uniform_block_data.data, sizeof(fs)) == 0 &&
               std::memcmp(&light, &uniform_block_data.light_data, sizeof(light)) == 0;
    };
    previous_vs_uniforms_valid = false;
    uniform_block_data.dirty = uniform_block_data.light_dirty = true;
    UploadUniforms(true);
    if (!check_vs() || !check_blocks()) return fail("initial upload/readback");
    auto vs = range(UniformBindings::VS);
    auto fs = range(UniformBindings::Common);
    auto light = range(UniformBindings::Light);
    UploadUniforms(true);
    if (!(range(UniformBindings::VS) == vs) || !(range(UniformBindings::Common) == fs) ||
        !(range(UniformBindings::Light) == light) || !check_vs()) return fail("unchanged VS reuse");
    const auto changed_vs = [&] {
        const auto before = range(UniformBindings::VS);
        UploadUniforms(true);
        const auto after = range(UniformBindings::VS);
        if (before == after || !check_vs()) return false;
        UploadUniforms(true);
        return range(UniformBindings::VS) == after && check_vs();
    };
    Pica::g_state.vs.uniforms.b[0] = !Pica::g_state.vs.uniforms.b[0];
    if (!changed_vs()) return fail("boolean mutation/reuse");
    auto& integer = Pica::g_state.regs.vs.int_uniforms[0].x;
    integer.Assign(integer.Value() ^ 0x5Au);
    if (!changed_vs()) return fail("integer register mutation/reuse");
    Pica::g_state.vs.uniforms.f[0].x = Pica::float24::FromFloat32(
        Pica::g_state.vs.uniforms.f[0].x.ToFloat32() == 1.25f ? 2.5f : 1.25f);
    if (!changed_vs()) return fail("float mutation/reuse");
    // Float24 currently retains a float32 value. Verify byte-sensitive cases
    // through SetFromRegs rather than relying on numeric float comparisons.
    for (const u32 bits : {0x00000000u, 0x80000000u, 0x7FC12345u, 0x7FC54321u}) {
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        Pica::g_state.vs.uniforms.f[0].x = Pica::float24::FromFloat32(value);
        VSUniformData expected{};
        expected.uniforms.SetFromRegs(Pica::g_state.regs.vs, Pica::g_state.vs);
        u32 represented = 0;
        std::memcpy(&represented, &expected.uniforms.f[0][0], sizeof(represented));
        if (represented != bits) return fail("float special value not representable");
        if (!changed_vs()) return fail("signed-zero/NaN payload mutation/reuse");
    }
    vs = range(UniformBindings::VS);
    fs = range(UniformBindings::Common);
    light = range(UniformBindings::Light);
    uniform_block_data.data.alphatest_ref ^= 1;
    uniform_block_data.dirty = true;
    UploadUniforms(true);
    if (!(range(UniformBindings::VS) == vs) || range(UniformBindings::Common) == fs ||
        !(range(UniformBindings::Light) == light) || !check_vs() || !check_blocks())
        return fail("fragment dirty independence");
    fs = range(UniformBindings::Common);
    uniform_block_data.light_data.shadow_texture_bias ^= 1;
    uniform_block_data.light_dirty = true;
    UploadUniforms(true);
    if (!(range(UniformBindings::VS) == vs) || !(range(UniformBindings::Common) == fs) ||
        range(UniformBindings::Light) == light || !check_vs() || !check_blocks())
        return fail("light dirty independence");
    const auto exhaust_ring = [&] {
        OpenGLState::BindUniformBuffer(uniform_buffer.GetHandle());
        auto [data, offset, invalidated] = uniform_buffer.Map(uniform_buffer.GetSize(), uniform_buffer_alignment);
        if (!data || offset != 0) return false;
        // Uses the existing 4 MiB GPU ring / bounded staging, no new test buffer.
        // Poison also prevents a stale range from accidentally passing readback.
        std::memset(data, 0xC7, static_cast<size_t>(uniform_buffer.GetSize()));
        uniform_buffer.Unmap(uniform_buffer.GetSize());
        return glGetError() == GL_NO_ERROR;
    };
    if (!exhaust_ring()) return fail("accelerated wrap setup");
    uniform_block_data.dirty = true; // Actual UploadUniforms must observe the next wrap.
    UploadUniforms(true);
    if (!previous_vs_uniforms_valid || !check_vs() || !check_blocks() ||
        range(UniformBindings::VS).offset != 0) return fail("wrap reupload of cached VS");
    if (!exhaust_ring()) return fail("nonaccelerated wrap setup");
    uniform_block_data.dirty = true;
    UploadUniforms(false);
    if (previous_vs_uniforms_valid || !check_blocks()) return fail("nonaccelerated wrap invalidation");
    UploadUniforms(true);
    if (!previous_vs_uniforms_valid || !check_vs() || !check_blocks())
        return fail("accelerated restore after nonaccelerated wrap");
    if (glGetError() != GL_NO_ERROR) return fail("final GL error");
    return true;
}

void RasterizerOpenGL::UploadUniforms(bool accelerate_draw) {
    Common::FrameTiming::Scope timing(Common::FrameTiming::Phase::UniformUpload);
    static const bool reuse_vs=[] {
        const char* value=std::getenv("CARA_VS_UNIFORM_REUSE");
        return value && std::strcmp(value,"1")==0;
    }();
    VSUniformData vs_uniforms{};
    bool sync_vs=accelerate_draw;
    if (accelerate_draw) {
        vs_uniforms.uniforms.SetFromRegs(Pica::g_state.regs.vs,Pica::g_state.vs);
        if (reuse_vs && previous_vs_uniforms_valid) {
            const auto& a=vs_uniforms.uniforms;
            const auto& b=previous_vs_uniforms.uniforms;
            bool same=std::memcmp(a.i.data(),b.i.data(),sizeof(a.i))==0 &&
                      std::memcmp(a.f.data(),b.f.data(),sizeof(a.f))==0;
            for (unsigned i=0;i<a.bools.size() && same;++i) same=a.bools[i].b==b.bools[i].b;
            // Compare meaningful fields only: std140 BoolAligned has padding.
            sync_vs=!same;
        }
    }
    const bool sync_fs = uniform_block_data.dirty;
    const bool sync_light = uniform_block_data.light_dirty;

    std::size_t uniform_size = 0;
    if (sync_vs) {
        uniform_size += uniform_size_aligned_vs;
    }
    if (sync_fs) {
        uniform_size += uniform_size_aligned_fs;
    }
    if (sync_light) {
        uniform_size += uniform_size_aligned_light;
    }
    if (uniform_size == 0) {
        return;
    }
    // Bind only when there is work; BindBufferRange also changes this generic binding.
    OpenGLState::BindUniformBuffer(uniform_buffer.GetHandle());

    std::size_t used_bytes = 0;
    u8* uniforms;
    GLintptr offset;
    bool invalidate;
    // A wrap invalidates the entire buffer, so even clean fragment/light
    // blocks must be copied again. Reserve their space before Map can wrap;
    // Unmap advances by only the bytes actually written.
    const std::size_t reservation=uniform_size_aligned_vs+uniform_size_aligned_fs+uniform_size_aligned_light;
    std::tie(uniforms, offset, invalidate) =
        uniform_buffer.Map(reservation, uniform_buffer_alignment);

    if (invalidate) previous_vs_uniforms_valid=false;
    if (sync_vs || (accelerate_draw && invalidate)) {
        std::memcpy(uniforms + used_bytes, &vs_uniforms, sizeof(vs_uniforms));
        previous_vs_uniforms=vs_uniforms;
        previous_vs_uniforms_valid=true;
        glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(UniformBindings::VS),
                          uniform_buffer.GetHandle(), offset + used_bytes, sizeof(VSUniformData));
        used_bytes += uniform_size_aligned_vs;
    }

    if (sync_fs || invalidate) {
        std::memcpy(uniforms + used_bytes, &uniform_block_data.data, sizeof(UniformData));
        glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(UniformBindings::Common),
                          uniform_buffer.GetHandle(), offset + used_bytes, sizeof(UniformData));
        uniform_block_data.dirty = false;
        used_bytes += uniform_size_aligned_fs;
    }

    if (sync_light || invalidate) {
        std::memcpy(uniforms + used_bytes, &uniform_block_data.light_data, sizeof(UniformLightData));
#ifdef CARA_IOS_GLES
        // Mutate only this uploaded copy. The CPU's live light LUT offsets
        // remain valid when the next draw switches back to exact lighting.
        if (runtime_tev_words) {
            static_assert(sizeof(*runtime_tev_words) == sizeof(UniformLightData::lighting_lut_offset));
            std::memcpy(uniforms + used_bytes + offsetof(UniformLightData, lighting_lut_offset),
                        runtime_tev_words->data(), sizeof(*runtime_tev_words));
        }
#endif
        glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(UniformBindings::Light),
                          uniform_buffer.GetHandle(), offset + used_bytes, sizeof(UniformLightData));
        uniform_block_data.light_dirty = false;
        used_bytes += uniform_size_aligned_light;
    }

    uniform_buffer.Unmap(used_bytes);
}


// Diagnostic only: read actual framebuffer attachment state without reconciling
// cache metadata or modifying attachments. Consumed errors remain sticky so this
// instrumentation cannot turn an oracle failure into a pass.
void RasterizerOpenGL::TraceSoftwareBatchVisibilityFBO(const char* stage,GLuint selected_color,GLuint selected_depth) {
    static const bool enabled=[] {const char* value=std::getenv("CARA_SW_BATCH_VISIBILITY_TEST");return value&&std::strcmp(value,"1")==0;}();
    if(!enabled)return;
    const auto errors=[] {GLenum first=GL_NO_ERROR;for(unsigned n=0;n<32;++n){const auto error=glGetError();if(error==GL_NO_ERROR)break;if(first==GL_NO_ERROR)first=error;}return first;};
    const auto entry_error=errors();
    if(!software_visibility_first_gl_error&&entry_error)software_visibility_first_gl_error=entry_error;
    struct Snapshot {GLenum status=0;GLint color_type=0,color_name=0,depth_type=0,depth_name=0,stencil_type=0,stencil_name=0;};
    const auto snapshot=[](GLenum target,GLuint object) {
        Snapshot result;result.status=glCheckFramebufferStatus(target);
        if(!object)return result; // default-FBO attachment enums differ; do not probe them
        const auto attachment=[target](GLenum point,GLint& type,GLint& name){
            glGetFramebufferAttachmentParameteriv(target,point,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&type);
            if(type!=GL_NONE)glGetFramebufferAttachmentParameteriv(target,point,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&name);
        };
        attachment(GL_COLOR_ATTACHMENT0,result.color_type,result.color_name);
        attachment(GL_DEPTH_ATTACHMENT,result.depth_type,result.depth_name);
        attachment(GL_STENCIL_ATTACHMENT,result.stencil_type,result.stencil_name);
        return result;
    };
    GLint actual_draw=0,actual_read=0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&actual_draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&actual_read);
    const auto draw_state=snapshot(GL_DRAW_FRAMEBUFFER,GLuint(actual_draw));
    const auto read_state=snapshot(GL_READ_FRAMEBUFFER,GLuint(actual_read));
    // Query the rasterizer-owned FBO as well if an export helper temporarily
    // selected another one. Restore the actual binding; tracker state is untouched.
    if(GLuint(actual_draw)!=framebuffer.handle)glBindFramebuffer(GL_DRAW_FRAMEBUFFER,framebuffer.handle);
    const auto owned_state=snapshot(GL_DRAW_FRAMEBUFFER,framebuffer.handle);
    if(GLuint(actual_draw)!=framebuffer.handle)glBindFramebuffer(GL_DRAW_FRAMEBUFFER,GLuint(actual_draw));
    const bool selected_color_live=selected_color&&glIsTexture(selected_color);
    const bool cached_color_live=framebuffer_info.color_attachment&&glIsTexture(framebuffer_info.color_attachment);
    const auto query_error=errors();
    if(!software_visibility_first_query_error&&query_error)software_visibility_first_query_error=query_error;
    LOG_INFO(Render_OpenGL,"SW_VIS_FBO seq={} stage={} owner_fbo={} tracked_fbo={} actual_draw={} actual_read={} cached_color={} cached_depth={} selected_color={} selected_depth={} cached_color_live={} selected_color_live={} owned_status=0x{:x} owned_color_type=0x{:x} owned_color={} owned_depth_type=0x{:x} owned_depth={} owned_stencil_type=0x{:x} owned_stencil={} draw_status=0x{:x} draw_color_type=0x{:x} draw_color={} read_status=0x{:x} read_color_type=0x{:x} read_color={} entry_error={} query_error={} sticky_error={} sticky_query_error={} vertices={} visibility_pending={}",
        ++software_visibility_oracle_trace_sequence,stage,framebuffer.handle,state.draw.draw_framebuffer,actual_draw,actual_read,
        framebuffer_info.color_attachment,framebuffer_info.depth_attachment,selected_color,selected_depth,cached_color_live,selected_color_live,
        unsigned(owned_state.status),unsigned(owned_state.color_type),owned_state.color_name,unsigned(owned_state.depth_type),owned_state.depth_name,unsigned(owned_state.stencil_type),owned_state.stencil_name,
        unsigned(draw_state.status),unsigned(draw_state.color_type),draw_state.color_name,unsigned(read_state.status),unsigned(read_state.color_type),read_state.color_name,entry_error,query_error,
        software_visibility_first_gl_error,software_visibility_first_query_error,vertex_batch.size(),software_framebuffer_visibility.pending);
}

} // namespace OpenGL
