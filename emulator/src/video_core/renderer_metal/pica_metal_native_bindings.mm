// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_native_bindings.h"
#include "simd_vertex_source.h"
#include "video_core/renderer_metal/pica_metal_cube_build.hh"
#include "video_core/renderer_metal/pica_metal_draw_stage_profile.hh"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include "video_core/renderer_metal/pica_metal_capture_raw_indices.hh"
#include "video_core/renderer_metal/pica_metal_pair_indices.h"

#include <CommonCrypto/CommonDigest.h>

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <bit>
#include <tuple>
#include <cstring>
#include <fstream>
#include "common/file_util.h"
#include "common/logging/log.h"
#include <limits>
#include <memory>
#include <fmt/format.h>
#include "core/settings.h"
#include "video_core/pica_state.h"
#include "video_core/renderer_metal/pica_metal_lut.h"
#include "video_core/renderer_metal/pica_metal_native_uniforms.h"
#include "video_core/renderer_opengl/gl_surface_params.h"
#include "video_core/renderer_opengl/gl_vertex_input_layout.h"
#include "video_core/renderer_opengl/pica_to_gl.h"
#include "video_core/texture/texture_decode.h"
#include "video_core/video_core.h"

namespace Cara {
namespace {

using HostClock = std::chrono::steady_clock;
struct HostPhase {
    u64 count = 0;
    u64 ns = 0;
};
constexpr std::array<const char*, 10> phase_names{
    "bind_total", "fragment_source", "vertex_source", "vertex_inputs",
    "pipeline_and_catalog", "attachments_and_lut", "sampled_textures",
    "mip_level_lookup_and_decode", "mip_chain_build", "mip_chain_wait"};
thread_local std::array<HostPhase, phase_names.size()> phases{};
thread_local u64 fog_bindings = 0;
thread_local u64 raw_index_draws = 0, raw_index_vertices = 0;
thread_local u64 mip_chain_hits = 0, mip_chain_misses = 0;
thread_local u64 vertex_source_probes = 0, vertex_source_recent_hits = 0;
thread_local u64 vertex_source_fallback_hits = 0, vertex_source_generations = 0;
thread_local u64 packed_layout_admissions = 0, packed_layout_hits = 0, packed_layout_declines = 0;

bool NativeHostProfileEnabled() {
    static const bool value = [] {
        const auto* env = std::getenv("CARA_METAL_EARLY_PROFILE");
        return env && std::string_view(env) == "1";
    }();
    return value;
}

class ScopedNativeHostPhase {
public:
    explicit ScopedNativeHostPhase(std::size_t index_)
        : index(index_), start(NativeHostProfileEnabled() ? HostClock::now() : HostClock::time_point{}) {}
    ~ScopedNativeHostPhase() {
        if (start != HostClock::time_point{}) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(HostClock::now() - start);
            phases[index].ns += ns.count();
            ++phases[index].count;
        }
    }
private:
    std::size_t index;
    HostClock::time_point start;
};

template<typename Work>
auto Measure(std::size_t phase, Work&& work) {
    ScopedNativeHostPhase timer(phase);
    return work();
}

struct NativeShaderSource {
    std::string_view bytes;
    u64 revision = 0;
    bool pc230_specialized = false;
};

u64 NextSourceRevision() {
    // Revision 1 is reserved for the immutable stage-in vertex source.
    thread_local u64 revision = 1;
    if (revision == std::numeric_limits<u64>::max())
        throw std::runtime_error("Native Metal shader source identity exhausted");
    return ++revision;
}

constexpr OpenGL::PicaFragmentShaderTarget NativeFragmentTarget() {
    return {
        .gles = true,
        .allow_shadow = false,
#ifdef CARA_RUNTIME_METAL_SHADERS
        .honor_material_enable = true,
        .emit_explicit_material_lod_bias = true,
        .emulate_material_border = true,
#endif
    };
}

bool UseRuntimeTevFallback() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_METAL_RUNTIME_TEV");
        return !value || std::string_view(value) != "0";
    }();
    return enabled;
}

NativeShaderSource NativeFragmentSource(const OpenGL::PicaFSConfig& config, bool general = false,
                                        unsigned tev_units = 0) {
    struct Entry {
        OpenGL::PicaFSConfig config;
        std::string source;
        u64 revision;
    };
    auto target = NativeFragmentTarget();
    target.metal_general_fragment = general;
    target.metal_runtime_tev_units = tev_units;
    target.elide_dead_combiner_buffer = UseMetalDeadCombinerElision();
    struct Cache {
        std::array<std::optional<Entry>, 128> entries{};
        std::size_t recent = 0, next = 0;
        bool elide_dead_combiner_buffer = false;
    };
    thread_local std::array<Cache, 4> caches;
    auto& [entries, recent, next, elide_dead_combiner_buffer] = caches[tev_units ? (tev_units == 1 ? 2 : 3) : unsigned(general)];
    if (elide_dead_combiner_buffer != target.elide_dead_combiner_buffer) {
        for (auto& entry : entries)
            entry.reset();
        recent = next = 0;
        elide_dead_combiner_buffer = target.elide_dead_combiner_buffer;
    }
    const auto matches = [&](std::size_t i) {
        return entries[i] &&
               std::memcmp(&entries[i]->config.state, &config.state, sizeof(config.state)) == 0;
    };
    if (matches(recent)) {
        NativeMetalCost::Count(NativeMetalCost::FragmentHits, 1);
        return {entries[recent]->source, entries[recent]->revision};
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (matches(i)) {
            recent = i;
            NativeMetalCost::Count(NativeMetalCost::FragmentHits, 1);
            return {entries[i]->source, entries[i]->revision};
        }
    }
    auto& slot = entries[next];
    slot.emplace(Entry{config, OpenGL::GenerateFragmentShader(config, false, target), NextSourceRevision()});
    recent = next;
    next = (next + 1) % entries.size();
    return {slot->source, slot->revision};
}

std::optional<PicaMetalPipelineSelection> SelectGeneralFragment(
    PicaMetalPipelines& pipelines, const PicaPipelineInputs& draw, OpenGL::PicaFSConfig canonical,
    const PicaSampleInputs& sampler, const std::optional<PicaSampleInputs>& second,
    const std::optional<PicaSampleInputs>& third) {
    canonical.state.alpha_test_func = Pica::FramebufferRegs::CompareFunc::Always;
    canonical.state.scissor_test_mode = Pica::RasterizerRegs::ScissorMode::Disabled;
    if (UseRuntimeTevFallback() && canonical.SupportsRuntimeTev()) {
        auto dynamic = canonical;
        dynamic.state.tev_stages = {};
        dynamic.state.combiner_buffer_input = 0;
        const auto fs = NativeFragmentSource(dynamic, true, second ? 3 : 1);
        if (auto choice = pipelines.SelectWithGeneral(draw, fs.bytes, sampler, second, third)) {
            choice->runtime_tev = choice->general_fragment;
            return choice;
        }
    }
    const auto fs = NativeFragmentSource(canonical, true);
    return pipelines.SelectWithGeneral(draw, fs.bytes, sampler, second, third);
}

// The audited PC 230 rewrite selects only byte ranges from the runtime shader.
// Both hashes gate the transformation and no captured shader is bundled.
bool SpecializePc230(std::string& source) {
    using Digest = std::array<unsigned char, CC_SHA256_DIGEST_LENGTH>;
    constexpr Digest original {0x0f, 0x53, 0xca, 0x69, 0x86, 0x7b, 0x61, 0xcc, 0xf5, 0xea, 0x8a, 0x6b, 0x24, 0xf4, 0x81, 0x6e, 0x29, 0xac, 0x2c, 0x05, 0x7c, 0x36, 0x18, 0x3c, 0xfc, 0x3e, 0x61, 0x29, 0x73, 0x7a, 0xef, 0x97};
    constexpr Digest specialized {0x56, 0x1a, 0xfb, 0xf1, 0x51, 0x87, 0x01, 0x52, 0xd8, 0xfb, 0x04, 0x6f, 0x0f, 0x75, 0x24, 0x97, 0xce, 0x8c, 0x28, 0xdb, 0x15, 0x6b, 0x15, 0x83, 0xa1, 0x13, 0x57, 0xa8, 0x21, 0x0a, 0x21, 0x01};
    const auto matches = [](std::string_view text, const Digest& expected) {
        Digest actual{};
        return CC_SHA256(text.data(), static_cast<CC_LONG>(text.size()), actual.data()) &&
               actual == expected;
    };
    if (source.size() != 75804 || !matches(source, original))
        return false;

    std::string result = source.substr(0, 4210);
    const auto append_instructions = [&](std::size_t first, std::size_t end,
                                         std::size_t indentation) {
        while (first < end) {
            const auto next = source.find('\n', first) + 1;
            result.append(indentation, ' ');
            result.append(source, first + 12, next - first - 12);
            first = next;
        }
    };
    result += "bool sub_105_134();\nbool sub_45_55();\nbool sub_51_53();\nbool sub_53_54();\n"
              "\nbool exec_shader() {\n";
    append_instructions(47510, 48032, 4); // PCs 230–235
    append_instructions(72341, 72448, 8); // PC 499
    append_instructions(72543, 72865, 4); // PCs 500–502
    result += "    return true;\n}\n\nbool sub_105_134() {\n";
    append_instructions(15865, 16789, 4); // PCs 105–112
    append_instructions(17390, 18898, 12); // PCs 118–133
    result += "    return false;\n}\n\n";
    result.append(source, 10891, 582);
    result += '\n';
    result.append(source, 11473, 177);
    result += '\n';
    result.append(source, 11650, 104);
    result += '\n';
    result.append(source, 72997);
    if (!matches(result, specialized))
        return false;
    source = std::move(result);
    return true;
}

// Match the generator configuration and the complete input program. A hash match
// alone cannot admit a cached source, including after a save-state load.
NativeShaderSource NativeVertexSource(const Pica::Shader::ShaderSetup& setup,
                                    const OpenGL::PicaVSConfig& config,
                                    const OpenGL::PicaFixedGSConfig& output,
                                    u16 input_mask, unsigned indexed_mode, bool user_clip,
                                    const OpenGL::PackedVertexLayout* captured_layout = nullptr,
                                    bool simd_reference = false) {
    struct Entry {
        OpenGL::PicaVSConfig config;
        OpenGL::PicaFixedGSConfig output;
        decltype(setup.program_code) code;
        decltype(setup.swizzle_data) swizzle;
        u16 input_mask;
        unsigned indexed_mode;
        unsigned policy;
        std::string source;
        u64 revision;
        bool pc230_specialized;
        OpenGL::PackedVertexLayout layout{};
        mutable u64 validated_code_identity = 0, validated_swizzle_identity = 0;
    };
    const auto enabled = [](const char* name) {
        const auto* value = std::getenv(name);
        return value && std::string_view(value) == "1";
    };
    static const bool pc230_enabled = enabled("CARA_METAL_PC230_SPECIALIZATION");
    static const bool reuse_recent = enabled("CARA_METAL_REUSE_VERTEX_SOURCE_LOOKUP");
    static const bool cache_source_policy = enabled("CARA_METAL_CACHE_SHADER_POLICY");
    static const bool specialize_layout = enabled("CARA_METAL_SPECIALIZE_PACKED_LAYOUT");
    static const bool reuse_identity = enabled("CARA_METAL_REUSE_SHADER_IDENTITY");
    static const bool validate_identity = enabled("CARA_METAL_VALIDATE_SHADER_IDENTITY");
    // Two identities identify bytes, not source configuration or a hash. Every
    // existing small key check remains in the caller before this byte check.
    const auto code_identity = reuse_identity ? setup.GetProgramCodeIdentity() : 0;
    const auto swizzle_identity = reuse_identity ? setup.GetSwizzleDataIdentity() : 0;
    const bool identities_available = reuse_identity && code_identity && swizzle_identity &&
        Pica::Shader::ShaderIdentityAvailable();
    const auto same_program = [&](const Entry& entry) {
        const auto complete = [&] {
            NativeMetalCost::Count(NativeMetalCost::ShaderIdentityFullChecks, 1);
            NativeMetalCost::Count(NativeMetalCost::ShaderIdentityComparedBytes,
                sizeof(entry.code) + sizeof(entry.swizzle));
            return entry.code == setup.program_code && entry.swizzle == setup.swizzle_data;
        };
        if (identities_available && entry.validated_code_identity == code_identity &&
            entry.validated_swizzle_identity == swizzle_identity) {
            if (validate_identity && !complete()) {
                NativeMetalCost::Count(NativeMetalCost::ShaderIdentityValidationFailures, 1);
                throw std::runtime_error("Shader arrays changed without MarkDirty");
            }
            NativeMetalCost::Count(NativeMetalCost::ShaderIdentityFastHits, 1);
            return true;
        }
        if (!complete()) {
            return false;
        }
        if (identities_available) {
            entry.validated_code_identity = code_identity;
            entry.validated_swizzle_identity = swizzle_identity;
        }
        return true;
    };
    const auto read_source_policy = [&] {
        // These diagnostic generator options are configured before emulation.
        // Preserve the two presence-based options, including empty/"0" values,
        // separately from the two options requiring the exact value "1".
        return (std::getenv("CARA_RAW_DEPTH") ? 1u : 0u) |
               (std::getenv("CARA_UNSNAPPED_VERTICES") ? 2u : 0u) |
               (enabled("CARA_GPU_QUATERNION_SLICE") ? 4u : 0u) |
               (enabled("CARA_GPU_PACKED_FETCH_REUSE") ? 8u : 0u);
    };
    // Opt-in startup contract: source-generator environment options stay fixed
    // for this process. Avoid four libc environment-lock scans on every draw.
    // The control retains its original live environment reads.
    static const unsigned cached_source_policy = cache_source_policy ? read_source_policy() : 0u;
    const unsigned source_policy = cache_source_policy ? cached_source_policy : read_source_policy();
    const bool pc230_branch = pc230_enabled && indexed_mode == 2 && UseMetalRawIndices() &&
                              UseMetalRawIndexConstants() && setup.uniforms.b[14];
    const unsigned policy = source_policy | (pc230_branch ? 64u : 0u) | (simd_reference ? 1024u : 0u) |
        (Settings::values.disable_clip_coef ? 16u : 0u) | (user_clip ? 32u : 0u);

    // Permanent bounded admission prevents layout churn from continually minting
    // new source revisions and unbounded downstream compiled pipeline variants.
    // PC230's byte-audited source rewrite retains priority over this experiment.
    struct SpecializedCache {
        std::array<std::optional<Entry>, OpenGL::MaxSpecializedPackedLayouts> entries;
        std::size_t count = 0;
        std::size_t recent = entries.size();
    };
    thread_local std::unique_ptr<SpecializedCache> specialized;

    std::optional<OpenGL::PackedVertexLayout> known_layout;
    const auto same_identity = [&](const Entry& entry) {
        return entry.input_mask == input_mask && entry.indexed_mode == indexed_mode &&
            entry.policy == policy &&
            std::memcmp(&entry.config.state, &config.state, sizeof(config.state)) == 0 &&
            std::memcmp(&entry.output.state, &output.state, sizeof(output.state)) == 0 &&
            same_program(entry);
    };
    if (specialize_layout && captured_layout && indexed_mode == 2 && !pc230_branch) {
        if (!specialized) specialized = std::make_unique<SpecializedCache>();
        auto& specialized_entries = specialized->entries;
        auto& specialized_count = specialized->count;
        auto& specialized_recent = specialized->recent;
        known_layout = OpenGL::CanonicalPackedVertexLayout(*captured_layout);
        const auto candidate = [&](std::size_t index) {
            if (NativeHostProfileEnabled()) ++vertex_source_probes;
            const auto& entry = *specialized_entries[index];
            return entry.layout == *known_layout && same_identity(entry);
        };
        if (specialized_recent < specialized_count && candidate(specialized_recent)) {
            if (NativeHostProfileEnabled()) { ++vertex_source_recent_hits; ++packed_layout_hits; }
            const auto& entry = *specialized_entries[specialized_recent];
            return {entry.source, entry.revision, entry.pc230_specialized};
        }
        std::size_t family_count = 0;
        for (std::size_t i = 0; i < specialized_count; ++i) {
            if (i != specialized_recent && candidate(i)) {
                if (NativeHostProfileEnabled()) { ++vertex_source_fallback_hits; ++packed_layout_hits; }
                specialized_recent = i;
                const auto& entry = *specialized_entries[i];
                return {entry.source, entry.revision, entry.pc230_specialized};
            }
            if (same_identity(*specialized_entries[i])) ++family_count;
        }
        if (!OpenGL::CanAdmitPackedVertexLayout(specialized_count, family_count)) {
            known_layout.reset(); // Reuse the ordinary generic source cache.
            if (NativeHostProfileEnabled()) ++packed_layout_declines;
        }
    }

    thread_local std::array<std::optional<Entry>, 64> entries;
    thread_local std::size_t next = 0;
    thread_local std::size_t recent = entries.size();
    const bool profile = NativeHostProfileEnabled();
    const auto matches = [&](std::size_t i) {
        if (profile)
            ++vertex_source_probes;
        const auto& entry = entries[i];
        return entry && entry->input_mask == input_mask && entry->indexed_mode == indexed_mode &&
            entry->policy == policy &&
            std::memcmp(&entry->config.state, &config.state, sizeof(config.state)) == 0 &&
            std::memcmp(&entry->output.state, &output.state, sizeof(output.state)) == 0 &&
            same_program(*entry);
    };
    // This is only a candidate-order hint, never a source identity. Recheck the
    // complete current program, swizzles and configuration even after a load or
    // replacement of this slot. The bounded source storage and revisions retain
    // their original ownership and replacement policy.
    if (!known_layout && reuse_recent && recent < entries.size() && matches(recent)) {
        if (profile)
            ++vertex_source_recent_hits;
        const auto& entry = entries[recent];
        return {entry->source, entry->revision, entry->pc230_specialized};
    }
    for (std::size_t i = 0; !known_layout && i < entries.size(); ++i) {
        if (reuse_recent && i == recent)
            continue; // The same exact comparison already failed above.
        if (matches(i)) {
            if (profile)
                ++vertex_source_fallback_hits;
            recent = i;
            const auto& entry = entries[i];
            return {entry->source, entry->revision, entry->pc230_specialized};
        }
    }

    if (profile)
        ++vertex_source_generations;
    auto source = OpenGL::GenerateVertexShader(setup, config, false, &output, input_mask,
                                               indexed_mode, true, false, false, false,
                                               known_layout ? &*known_layout : nullptr);
    if (user_clip)
        source.insert(0, RuntimeMetalUserClip);
    const bool pc230_specialized = pc230_branch && SpecializePc230(source);
    if (simd_reference && !TagSimdVertexSource(source)) {
        NativeMetalCost::Count(NativeMetalCost::SimdReferenceRejected, 1);
        LOG_INFO(Render_OpenGL, "SIMD vertex source admission rejected; retaining full shader");
    }
    const auto destination = known_layout ? specialized->count : next;
    auto& entry = known_layout ? specialized->entries[destination] : entries[destination];
    entry.emplace(Entry{config, output, setup.program_code, setup.swizzle_data,
                        input_mask, indexed_mode, policy, std::move(source), NextSourceRevision(),
                        pc230_specialized, known_layout.value_or(OpenGL::PackedVertexLayout{})});
    if (known_layout) {
        specialized->recent = specialized->count++;
        if (profile) ++packed_layout_admissions;
    } else {
        recent = next;
        next = (next + 1) % entries.size();
    }
    return {entry->source, entry->revision, entry->pc230_specialized};
}

// The shader revisions carry source identity. Compare the remaining recipe
// fields exactly, without treating alignment padding as guest state.
auto PipelineRecipeKey(const PicaPipelineInputs& input) {
    const auto& [vertex, fragment, sampled_unit, secondary_unit, packed_arrays, vertex_mode,
                 color_format, dither, dither_width, dither_height, dither_scissor, shadow, polygon_offset,
                 discard, sample_control, active_logic_op, clip0, clip1, depth_near, depth_far,
                 blend, depth_test, depth_write, stencil, rgb_op, alpha_op, rgb_src, rgb_dst,
                 alpha_src, alpha_dst, color_mask, depth_compare, stencil_compare, stencil_read,
                 stencil_write, stencil_fail, stencil_depth_fail, stencil_pass, stencil_reference,
                 cull, cull_face, winding, blend_color, tertiary_unit] = input;
    return std::tuple{
        sampled_unit, secondary_unit, tertiary_unit, packed_arrays, vertex_mode, color_format, dither, dither_width,
        dither_height, dither_scissor, shadow, polygon_offset, discard, sample_control,
        active_logic_op, clip0, clip1, std::bit_cast<u64>(depth_near), std::bit_cast<u64>(depth_far),
        blend, depth_test, depth_write, stencil, rgb_op, alpha_op, rgb_src, rgb_dst, alpha_src,
        alpha_dst, color_mask, depth_compare, stencil_compare, stencil_read, stencil_write,
        stencil_fail, stencil_depth_fail, stencil_pass, stencil_reference, cull, cull_face, winding,
        std::bit_cast<std::array<u32, 4>>(blend_color),
    };
}

auto SamplerRecipeKey(const PicaSampleInputs& input) {
    const auto& [min_filter, mag_filter, wrap_s, wrap_t, compare, base_level, max_level, levels,
                 min_lod, max_lod, lod_bias, anisotropy, rgba8, normalized, identity_swizzle,
                 opaque_black_border] = input;
    return std::tuple{
        min_filter, mag_filter, wrap_s, wrap_t, compare, base_level, max_level, levels,
        std::bit_cast<u32>(min_lod), std::bit_cast<u32>(max_lod), std::bit_cast<u32>(lod_bias),
        std::bit_cast<u32>(anisotropy), rgba8, normalized, identity_swizzle, opaque_black_border,
    };
}

struct NativePipelineMemo {
    struct Stages {
        u64 vertex, fragment;
        PicaMetalVertexMode mode;
        bool operator==(const Stages&) const = default;
    };
    struct Material {
        Stages stages;
        std::array<unsigned, 3> units;
    };
    struct Selection {
        Stages stages;
        decltype(PipelineRecipeKey(PicaPipelineInputs{})) recipe;
        std::array<decltype(SamplerRecipeKey(PicaSampleInputs{})), 3> samplers;
        bool dual;
        bool triple;
        PicaMetalPipelineSelection result;
    };
    id<MTLCommandQueue> queue;
    const PicaMetalPipelines* pipeline_owner;
    std::array<std::optional<Material>, 128> units{};
    std::array<std::optional<Selection>, 128> selections{};
    std::size_t next_units = 0, last_units = 0, next_selection = 0, last_selection = 0;

    template<class Entry, class Matches>
    static const Entry* Find(const std::array<std::optional<Entry>, 128>& items,
                             std::size_t& last, const Matches& matches) {
        if (items[last] && matches(*items[last]))
            return &*items[last];
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i] && matches(*items[i])) {
                last = i;
                return &*items[i];
            }
        }
        return nullptr;
    }

    std::optional<std::array<unsigned, 3>> MaterialUnits(
        const PicaMetalPipelines& pipelines, NativeShaderSource vs, NativeShaderSource fs,
        PicaMetalVertexMode mode) {
        const Stages stages{vs.revision, fs.revision, mode};
        if (const auto* found = Find(units, last_units,
                                    [&](const auto& entry) { return entry.stages == stages; })) {
            NativeMetalCost::Count(NativeMetalCost::UnitMemoHits, 1);
            return found->units;
        }
        const auto result = pipelines.GateMaterialUnits(vs.bytes, fs.bytes, mode);
        if (result) {
            units[next_units] = Material{stages, *result};
            last_units = next_units;
            next_units = (next_units + 1) % units.size();
        }
        return result;
    }

    std::optional<PicaMetalPipelineSelection> Resolve(
        PicaMetalPipelines& pipelines, NativeShaderSource vs, NativeShaderSource fs,
        const PicaPipelineInputs& draw, const std::array<PicaSampleInputs, 3>& samplers,
        bool dual, bool triple, const OpenGL::PicaFSConfig* general = nullptr) {
        const Stages stages{vs.revision, fs.revision, draw.vertex_mode};
        const auto recipe = PipelineRecipeKey(draw);
        const std::array sampler_keys{SamplerRecipeKey(samplers[0]), SamplerRecipeKey(samplers[1]),
                                      SamplerRecipeKey(samplers[2])};
        if (const auto* found = Find(selections, last_selection, [&](const auto& entry) {
                return entry.stages == stages && entry.dual == dual && entry.triple == triple &&
                       entry.recipe == recipe && entry.samplers == sampler_keys;
            })) {
            NativeMetalCost::Count(NativeMetalCost::SelectionMemoHits, 1);
            return found->result;
        }
        const auto second = dual ? std::optional{samplers[1]} : std::nullopt;
        const auto third = triple ? std::optional{samplers[2]} : std::nullopt;
        auto result = pipelines.Select(draw, samplers[0], second, third);
        if (!result && general) {
            result = SelectGeneralFragment(pipelines, draw, *general, samplers[0], second, third);
        }
        if (!result && pipelines.Prepare(draw, samplers[0], second, third))
            result = pipelines.Select(draw, samplers[0], second, third);
        if (result && !result->general_fragment) {
            selections[next_selection] = Selection{stages, recipe, sampler_keys, dual, triple, *result};
            last_selection = next_selection;
            next_selection = (next_selection + 1) % selections.size();
        }
        return result;
    }
};

NativePipelineMemo* ActivePipelineMemo(id<MTLCommandQueue> queue, const PicaMetalPipelines& owner) {
    if (!NativeMetalCost::ReusePipelineSelection())
        return nullptr;
    thread_local std::unique_ptr<NativePipelineMemo> memo;
    if (!memo || memo->queue != queue || memo->pipeline_owner != &owner) {
        memo = std::make_unique<NativePipelineMemo>();
        memo->queue = queue;
        memo->pipeline_owner = &owner;
    }
    return memo.get();
}

using Params = OpenGL::SurfaceParams;
using TextureConfig = Pica::TexturingRegs::TextureConfig;

struct BindingStorage {
    PicaMetalNativeUniforms uniforms;
    std::vector<PicaMetalStageVertex> stage_in;
    std::optional<PicaMetalPackedInputs> vertex_inputs;
};

std::optional<MetalTransfer::GuestInterval> TiledRange(PAddr address, u32 width, u32 height,
                                                       u32 tile_bytes) {
    if (!address || !width || !height || !tile_bytes || ((width | height) & 7))
        return std::nullopt;
    return MetalTransfer::AlignedGuestInterval(address, u64{width / 8} * (height / 8) * tile_bytes);
}

std::optional<PicaSampleInputs> ReadSampler(const TextureConfig& t, unsigned unit,
                                                bool cube = false) {
    using Mode = TextureConfig::WrapMode;
    const auto supported_wrap = [&](Mode mode) {
        return mode == Mode::Repeat || mode == Mode::MirroredRepeat || mode == Mode::ClampToEdge ||
               (mode == Mode::ClampToBorder &&
                (t.border_color.raw == 0 || t.border_color.raw == 0xff000000 ||
                 (!cube && (unit != 0 || t.type == TextureConfig::Texture2D))));
    };
    // Runtime shaders apply the full signed bias in explicit Texture2D LOD.
    if (!NativeFragmentTarget().emit_explicit_material_lod_bias && t.lod.bias != 0)
        return std::nullopt;
    // Only PICA texture unit 0 interprets the texture type field.
    const auto unit0_type = cube ? TextureConfig::TextureCube : TextureConfig::Texture2D;
    if ((unit == 0 && t.type != unit0_type) || t.lod.max_level > 7 ||
        t.lod.min_level > t.lod.max_level ||
        !supported_wrap(t.wrap_s) || !supported_wrap(t.wrap_t))
        return std::nullopt;
    PicaSampleInputs out{};
    out.min_filter = PicaToGL::TextureMinFilterMode(t.min_filter, t.mip_filter);
    out.mag_filter = PicaToGL::TextureMagFilterMode(t.mag_filter);
    // The colored-border shader selects individual mips and snaps nearest
    // samples to texel centers. Keep the hardware taps bilinear for both paths.
    if (t.border_color.raw != 0 && t.border_color.raw != 0xff000000 &&
        (t.wrap_s == Mode::ClampToBorder || t.wrap_t == Mode::ClampToBorder)) {
        out.min_filter = PicaToGL::TextureMinFilterMode(TextureConfig::Linear, TextureConfig::Nearest);
        out.mag_filter = PicaToGL::TextureMagFilterMode(TextureConfig::Linear);
    }
    out.wrap_s = PicaToGL::WrapMode(t.wrap_s);
    out.wrap_t = PicaToGL::WrapMode(t.wrap_t);
    out.opaque_black_border = t.border_color.raw == 0xff000000 &&
                              (t.wrap_s == Mode::ClampToBorder || t.wrap_t == Mode::ClampToBorder);
    out.min_lod = t.lod.min_level.Value();
    out.max_lod = t.lod.max_level.Value();
    out.max_level = t.lod.max_level;
    out.levels = out.max_level + 1;
    out.rgba8 = out.normalized = out.identity_swizzle = true;
    return out;
}

struct TextureMipPlan {
    std::array<MetalTransfer::GuestInterval, 8> levels;
    MetalTransfer::GuestInterval bytes;
};

std::optional<TextureMipPlan> PlanTextureMips(const TextureConfig& t, u32 tile_bytes) {
    TextureMipPlan out{};
    PAddr cursor = t.GetPhysicalAddress();
    const u32 last = t.lod.max_level;
    const u32 width = t.width;
    const u32 height = t.height;
    if (last >= out.levels.size() || ((width >> last) << last) != width ||
        ((height >> last) << last) != height)
        return std::nullopt;
    for (u32 level = 0; level <= last; ++level) {
        const auto bytes = TiledRange(cursor, width >> level, height >> level, tile_bytes);
        if (!bytes)
            return std::nullopt;
        out.levels[level] = *bytes;
        cursor = bytes->end;
    }
    out.bytes = {out.levels.front().begin, cursor};
    return out;
}

id<MTLTexture> DecodeTextureMips(PicaMetalNativeDrawLane& lane, id<MTLCommandQueue> queue,
                                const PicaMetalTextureDecoder& decoder,
                                const Pica::TexturingRegs::FullTextureConfig& info,
                                const TextureMipPlan& plan,
                                const MetalGuestAuthority::GuestReader& ram) {
    const auto& t = info.config;
    const u32 last = t.lod.max_level;
    const auto decode = [&](u32 level) -> id<MTLTexture> {
        const u32 width = u32(t.width) >> level;
        const u32 height = u32(t.height) >> level;
        const u32 row = (width / 8) * Pica::Texture::CalculateTileSize(info.format);
        return lane.DecodeTexture(decoder, plan.levels[level], row, width, height,
                                  unsigned(info.format), false, ram);
    };
    if (last == 0)
        return Measure(7, [&] { return decode(0); });

    // A mip chain is keyed by immutable Metal level identities. A write to any
    // source range replaces that level's identity after precise invalidation.
    const auto levels = Measure(7, [&] {
        std::array<id<MTLTexture>, 8> result{};
        for (u32 i = 0; i <= last; ++i)
            result[i] = decode(i);
        return result;
    });
    struct MipChain {
        std::array<id<MTLTexture>, 8> levels;
        id<MTLTexture> combined;
        u64 bytes = 0, last_use = 0;
    };
    struct MipCache {
        id<MTLCommandQueue> queue;
        std::array<MipChain, 128> chains{};
        u64 bytes = 0, time = 0;
    };
    thread_local std::unique_ptr<MipCache> cache;
    if (!cache || cache->queue != queue) {
        cache = std::make_unique<MipCache>();
        cache->queue = queue;
    }

    constexpr u64 budget = 64 * 1024 * 1024;
    u64 bytes = 0;
    for (unsigned i = 0; i <= last; ++i)
        bytes += u64{levels[i].width} * levels[i].height * 4;
    const bool cacheable = bytes > 0 && 2 * bytes <= budget;
    if (cacheable) {
        for (auto& chain : cache->chains) {
            if (chain.combined && chain.levels == levels) {
                chain.last_use = ++cache->time;
                ++mip_chain_hits;
                return chain.combined;
            }
        }
    }

    ++mip_chain_misses;
    ScopedNativeHostPhase build(8);
    const auto desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:t.width height:t.height
        mipmapped:YES];
    desc.mipmapLevelCount = last + 1;
    desc.storageMode = MTLStorageModePrivate;
    desc.hazardTrackingMode = MTLHazardTrackingModeTracked;
    desc.usage = MTLTextureUsageShaderRead;
    const auto texture = [queue.device newTextureWithDescriptor:desc];
    if (!texture)
        throw std::runtime_error("Could not allocate native sampled mip chain");
    const auto command = [queue commandBuffer];
    const auto blit = [command blitCommandEncoder];
    if (!blit)
        throw std::runtime_error("Could not encode native sampled mip chain");
    for (u32 i = 0; i <= last; ++i) {
        const auto source = levels[i];
        [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                   sourceSize:MTLSizeMake(source.width,source.height,1)
                    toTexture:texture destinationSlice:0 destinationLevel:i
             destinationOrigin:MTLOriginMake(0,0,0)];
    }
    [blit endEncoding];
    if (NativeMetalCost::OverlapTransfers()) {
        // The resulting private, hazard-tracked texture is consumed only by
        // this same queue. No CPU code reads mip contents before use.
        lane.SubmitSampledTextureBuild(command);
    } else {
        [command commit];
        {
            ScopedNativeHostPhase wait(9);
            [command waitUntilCompleted];
        }
        if (command.status != MTLCommandBufferStatusCompleted || command.error)
            throw std::runtime_error("Native sampled mip chain blit failed");
    }
    if (cacheable) {
        // Account for both the chain and the decoded levels it retains.
        const u64 cost = 2 * bytes;
        const auto oldest = [&] {
            return std::min_element(cache->chains.begin(), cache->chains.end(),
                                    [](const auto& a, const auto& b) {
                                        return a.last_use < b.last_use;
                                    });
        };
        auto slot = oldest();
        if (slot->combined) {
            cache->bytes -= slot->bytes;
            *slot = {};
        }
        while (cost > budget - cache->bytes) {
            const auto victim = std::min_element(cache->chains.begin(), cache->chains.end(),
                [](const auto& a, const auto& b) {
                    if (!a.combined || !b.combined)
                        return bool(a.combined) && !b.combined;
                    return a.last_use < b.last_use;
                });
            cache->bytes -= victim->bytes;
            *victim = {};
        }
        *slot = {levels, texture, cost, ++cache->time};
        cache->bytes += cost;
    }
    return texture;
}

PicaPipelineInputs ReadPipeline(const Pica::Regs& regs) {
    const auto& fb = regs.framebuffer.framebuffer;
    const auto& merger = regs.framebuffer.output_merger;
    const auto& blend = merger.alpha_blending;
    const auto& stencil = merger.stencil_test;
    PicaPipelineInputs p{};
    p.dither = false; // Native Metal writes use the attachment UNORM conversion.
    p.clip0 = true;
    p.clip1 = regs.rasterizer.clip_enable != 0;
    p.blend = merger.alphablend_enable;
    p.active_logic_op = !p.blend && merger.logic_op != Pica::FramebufferRegs::LogicOp::Copy &&
                         merger.logic_op != Pica::FramebufferRegs::LogicOp::NoOp;
    p.color_mask = fb.allow_color_write &&
                   (p.blend || merger.logic_op != Pica::FramebufferRegs::LogicOp::NoOp)
        ? unsigned(merger.red_enable) | (unsigned(merger.green_enable) << 1) |
          (unsigned(merger.blue_enable) << 2) | (unsigned(merger.alpha_enable) << 3)
        : 0;
    p.rgb_op = PicaToGL::BlendEquation(blend.blend_equation_rgb);
    p.alpha_op = PicaToGL::BlendEquation(blend.blend_equation_a);
    p.rgb_src = PicaToGL::BlendFunc(blend.factor_source_rgb);
    p.rgb_dst = PicaToGL::BlendFunc(blend.factor_dest_rgb);
    p.alpha_src = PicaToGL::BlendFunc(blend.factor_source_a);
    p.alpha_dst = PicaToGL::BlendFunc(blend.factor_dest_a);
    p.blend_color = PicaToGL::ColorRGBA8(merger.blend_const.raw);
    p.depth_test = merger.depth_test_enable || merger.depth_write_enable;
    p.depth_write = fb.allow_depth_stencil_write && merger.depth_write_enable;
    p.depth_compare = merger.depth_test_enable ? PicaToGL::CompareFunc(merger.depth_test_func)
                                                : GL_ALWAYS;
    p.stencil = regs.framebuffer.HasStencil() && stencil.enable;
    p.stencil_compare = PicaToGL::CompareFunc(stencil.func);
    p.stencil_read = stencil.input_mask;
    p.stencil_write = regs.framebuffer.HasStencil() && fb.allow_depth_stencil_write
                          ? u32(stencil.write_mask) : 0;
    p.stencil_reference = stencil.reference_value;
    p.stencil_fail = PicaToGL::StencilOp(stencil.action_stencil_fail);
    p.stencil_depth_fail = PicaToGL::StencilOp(stencil.action_depth_fail);
    p.stencil_pass = PicaToGL::StencilOp(stencil.action_depth_pass);
    using Cull = Pica::RasterizerRegs::CullMode;
    p.cull = regs.rasterizer.cull_mode != Cull::KeepAll;
    p.cull_face = GL_BACK;
    p.winding = regs.rasterizer.cull_mode == Cull::KeepClockWise ? GL_CW : GL_CCW;
    return p;
}

PicaGuestRange WriteFootprint(MetalTransfer::GuestInterval bytes, u32 width, u32 height,
                             Params::PixelFormat format, const Common::Rectangle<u32>& rect) {
    Params layout{};
    layout.addr = bytes.begin;
    layout.width = layout.stride = width;
    layout.height = height;
    layout.is_tiled = true;
    layout.pixel_format = format;
    layout.UpdateParams();
    const auto interval = layout.GetSubRectInterval(rect);
    return {boost::icl::first(interval), boost::icl::last_next(interval)};
}

} // namespace

void ReportNativeMetalTextureResidency(const std::string& path);
void AppendNativeMetalDeferredAdmissionProfile(std::ostream& out, bool json);

std::array<std::uint64_t, 5> ReadNativeMetalBindingOpeningProfile() noexcept {
    return {phases[1].ns, phases[2].ns, phases[3].ns, phases[4].ns, fog_bindings};
}

void AppendNativeMetalBindingWindow(std::ostream& stream,
                                    std::array<std::uint64_t, 20>& previous) {
    stream << ",\"metal_binding_cost\":{";
    for (std::size_t i = 0; i < phases.size(); ++i) {
        if (i)
            stream << ',';
        stream << '\"' << phase_names[i] << "\":["
               << phases[i].count - previous[2*i] << ','
               << phases[i].ns - previous[2*i+1] << ']';
        previous[2*i] = phases[i].count;
        previous[2*i+1] = phases[i].ns;
    }
    stream << '}';
    MetalDrawStageProfile::AppendWindow(stream);
    AppendNativeMetalDeferredAdmissionProfile(stream, true);
    stream << ",\"metal_raw_work_totals\":{\"draws\":" << raw_index_draws
           << ",\"vertices\":" << raw_index_vertices
           << ",\"source_generations\":" << vertex_source_generations << '}';
}

void ReportNativeMetalBindings(const std::string& path) {
    ReportNativeMetalTextureResidency(path);
    ReportNativeMetalCommandTimeline(path);
    if (!NativeHostProfileEnabled())
        return;
    std::ofstream out(path + "metal-binding-profile.csv");
    out << "phase,calls,elapsed_ns\n";
    for (std::size_t i = 0; i < phases.size(); ++i)
        out << phase_names[i] << ',' << phases[i].count << ',' << phases[i].ns << '\n';
    NativeMetalCost::Append(out);
    AppendNativeMetalDeferredAdmissionProfile(out, false);
    ReportMetalPairIndices(out);
    MetalDrawStageProfile::Append(out);
    out << "fog_bindings," << fog_bindings << ",0\n"
        << "mip_chain_hits," << mip_chain_hits << ",0\n"
        << "mip_chain_misses," << mip_chain_misses << ",0\n"
        << "raw_index_draws," << raw_index_draws << ",0\n"
        << "raw_index_vertices," << raw_index_vertices << ",0\n"
        << "raw_read_plan_hits," << raw_read_plan_hits << ",0\n"
        << "raw_read_plan_misses," << raw_read_plan_misses << ",0\n"
        << "vertex_source_lookup_probes," << vertex_source_probes << ",0\n"
        << "vertex_source_recent_hits," << vertex_source_recent_hits << ",0\n"
        << "vertex_source_fallback_hits," << vertex_source_fallback_hits << ",0\n"
        << "vertex_source_generations," << vertex_source_generations << ",0\n"
        << "packed_layout_admissions," << packed_layout_admissions << ",0\n"
        << "packed_layout_hits," << packed_layout_hits << ",0\n"
        << "packed_layout_declines," << packed_layout_declines << ",0\n";
    if (NativeMetalCost::Enabled()) {
        const auto raster = ReadNativeMetalRasterStateProfile();
        for (std::size_t i = 0; i < raster.size(); ++i) {
            out << "raster_state_" << NativeMetalRasterStateNames[i] << "_requests," << raster[i][0] << ",0\n";
            out << "raster_state_" << NativeMetalRasterStateNames[i] << "_skips," << raster[i][1] << ",0\n";
        }
    }
}

namespace {
NativeShaderSource StageInVertexSource() {
    static const auto source = OpenGL::GenerateTrivialVertexShader(false);
    return {source, 1};
}

bool AllowVertexFallbackOnThisRefresh() {
    static const auto override_enabled = []() -> std::optional<bool> {
        if (const char* value = std::getenv("CARA_METAL_VERTEX_FALLBACK"))
            return std::string_view(value) == "1";
        return std::nullopt;
    }();
    static const auto frame = [] {
        const char* value = std::getenv("CARA_METAL_VERTEX_FALLBACK_FRAME");
        return value ? std::strtoul(value, nullptr, 10) : 0;
    }();
    return override_enabled.value_or(UseMetalGeneralFragment()) &&
           (!frame || frame == VideoCore::GetCurrentFrame() + 1);
}
} // namespace

PicaMetalNativeBindings::PicaMetalNativeBindings(const PicaMetalResources& resources_)
    : resources(resources_), pipelines(resources.queue.device), lighting(resources.queue) {}

std::optional<NativePicaDrawBindings> PicaMetalNativeBindings::Prepare(
    PicaMetalNativeDrawLane& lane, const Pica::State& state, bool indexed,
    const MetalGuestAuthority::GuestReader& ram, std::span<const PicaMetalStageVertex> stage_in,
    bool* interpret_vertices) {
    const ScopedNativeHostPhase total(0);
    const auto& r = state.regs;
    const auto& fb = r.framebuffer.framebuffer;
    const auto decline = [&](const char* stage) -> std::optional<NativePicaDrawBindings> {
        const auto& tr = r.texturing;
        const auto viewport = r.rasterizer.GetViewportRect();
        throw std::runtime_error(fmt::format(
            "Native PICA binding {} frame={} mode={} indexed={} vertices={} color=0x{:08x}/{} "
            "depth=0x{:08x}/{} fb={}x{} viewport={},{},{},{} shadow={} procedural={} "
            "texture_flags=0x{:08x} blend={} logic={} custom={} linear={}",
            stage, VideoCore::GetCurrentFrame(), stage_in.empty() ? "hardware" : "stage_in",
            indexed, u32(r.pipeline.num_vertices), fb.GetColorBufferPhysicalAddress(),
            unsigned(fb.color_format.Value()), fb.GetDepthBufferPhysicalAddress(),
            unsigned(fb.depth_format.Value()), fb.GetWidth(), fb.GetHeight(), viewport.left,
            viewport.bottom, viewport.right, viewport.top, r.framebuffer.IsShadowRendering(),
            bool(tr.main_config.texture3_enable), r.reg_array[PICA_REG_INDEX(texturing.main_config)],
            bool(r.framebuffer.output_merger.alphablend_enable),
            unsigned(r.framebuffer.output_merger.logic_op.Value()), Settings::values.custom_textures,
            Settings::values.use_linear_filter));
    };
    using ColorFormat = Pica::FramebufferRegs::ColorFormat;
    using DepthFormat = Pica::FramebufferRegs::DepthFormat;
    const auto color_format = fb.color_format.Value();
    const auto depth_format = fb.depth_format.Value();
    const bool has_stencil = depth_format == DepthFormat::D24S8;
    const bool packed_color = color_format == ColorFormat::RGB565 ||
                              color_format == ColorFormat::RGBA4 ||
                              color_format == ColorFormat::RGB5A1;
    if (r.framebuffer.IsShadowRendering() ||
        (!packed_color && color_format != ColorFormat::RGBA8) ||
        (packed_color && ![resources.queue.device supportsFamily:MTLGPUFamilyApple2]) ||
        (!has_stencil && depth_format != DepthFormat::D24) ||
        Settings::values.custom_textures || Settings::values.use_linear_filter)
        return decline("framebuffer or configuration");

    const u32 width = fb.GetWidth();
    const u32 height = fb.GetHeight();
    const auto color_range = TiledRange(fb.GetColorBufferPhysicalAddress(), width, height,
                                        packed_color ? 128 : 256);
    const auto depth_range = TiledRange(fb.GetDepthBufferPhysicalAddress(), width, height,
                                       has_stencil ? 256 : 192);
    const auto view = r.rasterizer.GetViewportRect();
    if (!color_range || !depth_range || color_range->Overlaps(*depth_range) ||
        view.left < 0 || view.bottom < 0 || view.right <= view.left || view.top <= view.bottom ||
        u32(view.right) > width || u32(view.top) > height)
        return decline("framebuffer address or viewport");

    NativeMetalCost::Scope classify(NativeMetalCost::ClassifyHost);
    auto pica = ReadPipeline(r);
    if (color_format == ColorFormat::RGB565) {
        pica.color_format = MTLPixelFormatB5G6R5Unorm;
        pica.color_mask &= 7;
    } else if (color_format == ColorFormat::RGBA4) {
        pica.color_format = MTLPixelFormatABGR4Unorm;
    } else if (color_format == ColorFormat::RGB5A1) {
        pica.color_format = MTLPixelFormatA1BGR5Unorm;
    }
    constexpr auto target = NativeFragmentTarget();
    const auto fcfg = OpenGL::PicaFSConfig::BuildFromRegs(r, false, target);
    if (NativeHostProfileEnabled() && r.texturing.fog_mode == Pica::TexturingRegs::FogMode::Fog)
        ++fog_bindings;
    classify.Stop();
    const auto fs = Measure(1, [&] { return NativeFragmentSource(fcfg); });
    const u16 mask = OpenGL::IOSVertexInputLayout::MaskFromRegs(r.vs);
    NativeShaderSource vs;
    auto* memo = ActivePipelineMemo(resources.queue, pipelines);
    const auto material_units = [&](PicaMetalVertexMode mode) {
        return memo ? memo->MaterialUnits(pipelines, vs, fs, mode)
                    : pipelines.GateMaterialUnits(vs.bytes, fs.bytes, mode);
    };
    std::optional<std::array<unsigned, 3>> units;
    std::optional<PicaMetalPackedInputs> vertex_inputs;
    bool raw_indices = false;
    std::shared_ptr<const MetalPairIndexPayload> pair_indices;
    std::shared_ptr<BindingStorage> storage;
    using Mode = PicaMetalVertexMode;
    if (stage_in.empty()) {
        if (r.pipeline.use_gs != Pica::PipelineRegs::UseGS::No)
            return decline("geometry");
        // ShaderSetup memoizes its identity hash, so construct the generator key
        // from the active render lane's state.
        auto& setup = const_cast<Pica::Shader::ShaderSetup&>(state.vs);
        const OpenGL::PicaVSConfig vcfg(r, setup);
        const OpenGL::PicaFixedGSConfig gs(r);
        const auto topology = state.primitive_assembler.GetTopology();
        if (!indexed && topology == Pica::PipelineRegs::TriangleTopology::Shader)
            return decline("programmable primitive topology");
        const auto visible = [&](PAddr address, std::span<u8> bytes) {
            return lane.ReadGuest(address, bytes, ram);
        };
        const bool use_raw_indices = UseMetalRawIndices();
        static const bool reuse_staging = [] {
            const char* value = std::getenv("CARA_METAL_REUSE_VERTEX_STAGING");
            return value && std::string_view(value) == "1";
        }();
        if (reuse_staging && use_raw_indices) {
            // An outstanding draw owns its binding storage. Reentrant or
            // deferred consumers get a distinct object until that lease ends.
            thread_local std::shared_ptr<BindingStorage> retained;
            if (!retained)
                retained = std::make_shared<BindingStorage>();
            if (retained.use_count() == 1) {
                storage = retained;
                if (!storage->vertex_inputs)
                    storage->vertex_inputs.emplace();
                static thread_local bool reported = false;
                if (!reported && !storage->vertex_inputs->vertices.empty() &&
                    !storage->vertex_inputs->indices.empty()) {
                    LOG_INFO(Render_OpenGL, "Metal reused owned vertex staging");
                    reported = true;
                }
            }
        }
        vertex_inputs = Measure(3, [&] {
            auto* staging = storage ? &*storage->vertex_inputs : nullptr;
            return use_raw_indices ? CaptureMetalRawIndices(state, indexed, mask, visible, staging)
                                   : CapturePicaMetalPackedInputs(state, indexed, mask, visible);
        });
        if (vertex_inputs) {
            raw_indices = UseMetalRawIndices();
            if (raw_indices && UseMetalPairIndices())
                pair_indices = Measure(3, [&] {
                    return CaptureMetalPairIndices(resources.queue.device, state, indexed, mask, *vertex_inputs);
                });
            const auto source_mode = pair_indices ? 6u : 2u;
            const auto simd_flag = [](const char* name) { const auto* value=std::getenv(name); return value && std::string_view(value)=="1"; };
            static const bool simd_requested=simd_flag("CARA_METAL_VERTEX_SIMD_REFERENCE");
            static const bool compute_requested=simd_flag("CARA_METAL_BATCH_VERTEX_COMPUTE") || simd_flag("CARA_METAL_UNIQUE_VERTEX_COMPUTE");
            const bool simd_reference=simd_requested && !compute_requested && source_mode==2 &&
                UseMetalRawIndices() && UseMetalRawIndexConstants() && [resources.queue.device supportsFamily:MTLGPUFamilyApple7];
            pica.vertex_mode = pair_indices ? Mode::PairIndexed : Mode::Packed;
            vs = Measure(2, [&]{
                return NativeVertexSource(setup, vcfg, gs, mask, source_mode, pica.clip1,
                                          &vertex_inputs->layout, simd_reference);
            });
            units = Measure(4, [&] { return material_units(pica.vertex_mode); });
            if (pair_indices && !units) {
                pair_indices.reset(); pica.vertex_mode = Mode::Packed;
                vs = Measure(2, [&] {
                    return NativeVertexSource(setup, vcfg, gs, mask, 2, pica.clip1,
                                              &vertex_inputs->layout);
                });
                units = Measure(4, [&] { return material_units(Mode::Packed); });
            }
            raw_indices = raw_indices && !pair_indices;
        } else {
            vs = Measure(2, [&]{
                return NativeVertexSource(setup, vcfg, gs, mask, 1, pica.clip1);
            });
            units = Measure(4, [&] { return material_units(Mode::DecodedIndexed); });
            pica.vertex_mode = Mode::DecodedIndexed;
            if (units) {
                vertex_inputs = Measure(3, [&] { return CapturePicaMetalDecodedInputs(state, indexed, mask, visible); });
                if (!vertex_inputs)
                    return decline("decoded vertex input");
            }
        }
    } else {
        pica.vertex_mode = Mode::StageIn;
        vs = Measure(2, StageInVertexSource);
        units = Measure(4, [&] { return material_units(Mode::StageIn); });
    }
    if (!units) {
        const auto* capture = std::getenv("CARA_METAL_CAPTURE_CATALOG_MISS");
        if (capture && std::string_view(capture) == "1") {
            const auto home = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
            std::ofstream(home + "metal-miss.vert", std::ios::binary) << vs.bytes;
            std::ofstream(home + "metal-miss.frag", std::ios::binary) << fs.bytes;
            std::ofstream(home + "metal-miss-mode.txt") << unsigned(pica.vertex_mode) << '\n';
        }
        return decline("catalog identity");
    }
    pica.vertex = vs.bytes;
    pica.fragment = fs.bytes;
    pica.packed_arrays = pica.vertex_mode == Mode::Packed || pica.vertex_mode == Mode::PairIndexed;
    pica.sampled_unit = (*units)[0];
    pica.secondary_unit = (*units)[1];
    pica.tertiary_unit = (*units)[2];

    NativeMetalCost::Scope sampler_plan(NativeMetalCost::SamplerPlanHost);
    const auto textures = r.texturing.GetTextures();
    std::array<PicaSampleInputs, 3> sampler{};
    std::array<std::optional<TextureMipPlan>, 3> ranges;
    std::optional<MetalTransfer::CubePlan> primary_cube;
    bool primary_color_snapshot = false;
    unsigned depth_snapshot_slots = 0;
    for (unsigned i = 0; i < units->size(); ++i) {
        const auto unit = (*units)[i];
        if (unit == NoMaterialUnit)
            continue;
        const auto& info = textures[unit];
        const auto& t = info.config;
        if (!info.enabled || unsigned(info.format) >= 14)
            return decline("texture format or enable");
        const auto tile_size = Pica::Texture::CalculateTileSize(info.format);
        if (unit == 0 && t.type == TextureConfig::TextureCube) {
            // Each face is decoded with its guest format before assembling
            // the private RGBA8 cube.
            const auto recipe = ReadSampler(t, unit, true);
            if (i != 0 || !recipe)
                throw std::runtime_error(fmt::format(
                    "Native PICA cube sampler unit={} format={} wrap_s={} wrap_t={} "
                    "lod_min={} lod_max={} lod_bias={}",
                    unit, unsigned(info.format), unsigned(t.wrap_s.Value()), unsigned(t.wrap_t.Value()),
                    u32(t.lod.min_level), u32(t.lod.max_level), s32(t.lod.bias)));
            using Face = Pica::TexturingRegs::CubeFace;
            const auto address = [&](Face face) { return r.texturing.GetCubePhysicalAddress(face); };
            const MetalTransfer::CubeRegisters cube{
                {address(Face::PositiveX), address(Face::NegativeX),
                 address(Face::PositiveY), address(Face::NegativeY),
                 address(Face::PositiveZ), address(Face::NegativeZ)},
                u32(t.width), u32(t.height), static_cast<u32>(tile_size), u32(t.lod.min_level),
                u32(t.lod.max_level), s32(t.lod.bias)};
            primary_cube = MetalTransfer::PlanCube(cube, *color_range, *depth_range);
            if (!primary_cube)
                return decline("cube mip geometry or ownership");
            sampler[i] = *recipe;
            continue;
        }
        const auto recipe = ReadSampler(t, unit);
        ranges[i] = PlanTextureMips(t, tile_size);
        const bool color_alias = ranges[i] && ranges[i]->bytes.Overlaps(*color_range);
        if (i == 0 && color_alias && !packed_color && t.lod.max_level == 0 &&
            info.format == Pica::TexturingRegs::TextureFormat::RGBA8)
            primary_color_snapshot = true;
        const bool depth_alias = ranges[i] && ranges[i]->bytes.Overlaps(*depth_range);
        if (depth_alias && !color_alias && t.type == TextureConfig::Texture2D &&
            t.lod.max_level == 0 && info.format == Pica::TexturingRegs::TextureFormat::RGBA8)
            depth_snapshot_slots |= 1u << i;
        if (!recipe || !ranges[i] || (color_alias && !(i == 0 && primary_color_snapshot)) ||
            (depth_alias && !(depth_snapshot_slots & (1u << i)))) {
            throw std::runtime_error(fmt::format(
                "Native PICA sampler unit={} format={} type={} addr=0x{:08x} dims={}x{} "
                "min_filter={} mag_filter={} mip_filter={} wrap_s={} wrap_t={} "
                "lod_min={} lod_max={} lod_bias={} border=0x{:08x} "
                "valid_range={} color_alias={} depth_alias={}",
                unit, unsigned(info.format), unsigned(t.type.Value()), t.GetPhysicalAddress(),
                u32(t.width), u32(t.height), unsigned(t.min_filter.Value()),
                unsigned(t.mag_filter.Value()), unsigned(t.mip_filter.Value()),
                unsigned(t.wrap_s.Value()), unsigned(t.wrap_t.Value()),
                u32(t.lod.min_level), u32(t.lod.max_level), s32(t.lod.bias), t.border_color.raw,
                ranges[i].has_value(), color_alias, depth_alias));
        }
        sampler[i] = *recipe;
    }
    if (primary_cube) {
        for (const auto& resource : ranges) {
            if (!resource)
                continue;
            for (const auto& face : primary_cube->faces) {
                const auto range = resource->bytes;
                if (range.Overlaps(face.bytes) &&
                    (range.begin != face.bytes.begin || range.end != face.bytes.end))
                    return decline("cube face aliases another sampler");
            }
        }
    }
    sampler_plan.Stop();
    const auto second = ranges[1] ? std::optional{sampler[1]} : std::nullopt;
    const auto third = ranges[2] ? std::optional{sampler[2]} : std::nullopt;
    const bool probe_fallback = interpret_vertices && stage_in.empty() && AllowVertexFallbackOnThisRefresh();
    if (probe_fallback && std::getenv("CARA_METAL_VERTEX_FALLBACK_TRACE")) {
        static thread_local u64 last_revision = 0;
        if (last_revision != vs.revision) {
            last_revision = vs.revision;
            LOG_INFO(Render_OpenGL, "Vertex fallback admission frame={} topology={} clip1={} pair={} gs={} vertices={}",
                     VideoCore::GetCurrentFrame() + 1, unsigned(state.primitive_assembler.GetTopology()),
                     pica.clip1, bool(pair_indices), unsigned(r.pipeline.use_gs.Value()),
                     u32(r.pipeline.num_vertices));
        }
    }
    using Topology = Pica::PipelineRegs::TriangleTopology;
    const auto& assembler = state.primitive_assembler;
    const auto topology = assembler.GetTopology();
    // With geometry shaders disabled, Shader topology assembles independent triangles
    // unless an earlier geometry emission left a winding change pending.
    const bool independent_triangles = topology == Topology::List ||
        (topology == Topology::Shader && !assembler.HasPendingWinding());
    using Compare = Pica::FramebufferRegs::CompareFunc;
    using Scissor = Pica::RasterizerRegs::ScissorMode;
    const auto& f = fcfg.state;
    const bool can_use_general = UseMetalGeneralFragment() && !pair_indices && !f.lighting.enable &&
        !f.proctex.enable && f.fog_mode == Pica::TexturingRegs::FogMode::None &&
        !f.native_nearest_borrow_tex0 && !f.shadow_rendering &&
        f.alpha_test_func != Compare::Never && pica.sampled_unit == 0 &&
        !third && (!second || pica.secondary_unit == 1) &&
        f.texture0_type == TextureConfig::Texture2D &&
        (f.scissor_test_mode == Scissor::Disabled ||
         f.scissor_test_mode == Scissor::Include ||
         f.scissor_test_mode == Scissor::Exclude);
    if (probe_fallback && !pica.clip1 && !pair_indices && !Settings::values.skip_slow_draw &&
        r.pipeline.use_gs == Pica::PipelineRegs::UseGS::No && independent_triangles &&
        assembler.IsEmpty() && r.pipeline.num_vertices % 3 == 0 &&
        r.pipeline.num_vertices <= 16384 &&
        pipelines.TryVertexFallback(pica, StageInVertexSource().bytes, sampler[0], second, third,
                                    can_use_general && UseRuntimeTevFallback() && fcfg.SupportsRuntimeTev())) {
        *interpret_vertices = true;
        return std::nullopt;
    }
    // The existing Metal shaders do not use the GL atlas crop fields.
    // Only the opted-in general fragment interprets two of those fields as
    // modes. Its resource envelope and all fixed draw state must still match.
    const auto resolve_pipeline = [&]() -> std::optional<PicaMetalPipelineSelection> {
        if (memo)
            return memo->Resolve(pipelines, vs, fs, pica, sampler, second.has_value(),
                                 third.has_value(), can_use_general ? &fcfg : nullptr);
        if (can_use_general) {
            if (auto choice = SelectGeneralFragment(pipelines, pica, fcfg, sampler[0], second, third))
                return choice;
        }
        auto result = pipelines.Select(pica, sampler[0], second, third);
        if (!result && pipelines.Prepare(pica, sampler[0], second, third))
            result = pipelines.Select(pica, sampler[0], second, third);
        return result;
    };
    auto selection = Measure(4, resolve_pipeline);
    if (pair_indices && (!selection || selection->vertex_mode != Mode::PairIndexed ||
                         selection->primary_material_cube != primary_cube.has_value())) {
        // An unsupported/failed experiment must retain the established raw
        // renderer, including current attributes, uniforms, clip and ordering.
        pair_indices.reset(); raw_indices = true; pica.vertex_mode = Mode::Packed;
        auto& setup = const_cast<Pica::Shader::ShaderSetup&>(state.vs);
        const OpenGL::PicaVSConfig vcfg(r, setup);
        const OpenGL::PicaFixedGSConfig gs(r);
        vs = Measure(2, [&] { return NativeVertexSource(setup, vcfg, gs, mask, 2, pica.clip1,
                                                       &vertex_inputs->layout); });
        const auto raw_units = Measure(4, [&] { return material_units(Mode::Packed); });
        if (!raw_units || *raw_units != *units) return decline("raw pair fallback material identity");
        pica.vertex = vs.bytes;
        selection = Measure(4, resolve_pipeline);
    }
    if (!selection)
        return decline("pipeline state or reflection");
    if (selection->primary_material_cube != primary_cube.has_value())
        return decline("reflected primary material texture type");
    if (raw_indices && selection->vertex_mode != MetalRawIndexMode) {
        ScopedNativeHostPhase timer(3);
        ExpandCapturedMetalRawIndices(*vertex_inputs);
        raw_indices = false;
    }

    const auto& parts = resources.draw;
    auto color = Measure(5, [&] {
        NativeMetalCost::Scope cost(NativeMetalCost::Color);
        return lane.AcquireTiledColor(*color_range, width, height, pica.color_format, *parts.textures,
                                      packed_color ? parts.tiled_packed_color_export : parts.tiled_export,
                                      parts.detile_packed_color, ram); });
    auto depth = Measure(5, [&] {
        NativeMetalCost::Scope cost(NativeMetalCost::Depth);
        return lane.AcquireTiledDepthStencil(*depth_range, width, height,
        {has_stencil ? parts.detile_depth : parts.detile_depth24, parts.unpack_depth},
        has_stencil ? parts.tiled_export : parts.tiled_depth24_export, parts.pack_depth, ram,
        has_stencil); });

    const bool procedural = r.texturing.main_config.texture3_enable;
    if (selection->slots[0] != NSNotFound &&
        (lut_dirty || (procedural && (!lut_has_procedural || procedural_lut_dirty)))) {
        lut = Measure(5, [&] {
            NativeMetalCost::Scope cost(NativeMetalCost::Lut);
            const auto values = SnapshotPicaMetalLuts(state, ++generation, procedural);
            const auto rows = procedural ? values->procedural_rows : values->rows;
            const auto active = std::span(values->texels).first(rows * values->width);
            const auto bytes = std::as_bytes(active);
            return lighting.Resolve(resources.queue,
                {{reinterpret_cast<const u8*>(bytes.data()), bytes.size()}, rows, values->generation});
        });
        if (!lut)
            return decline("lighting LUT");
        lut_dirty = false;
        lut_has_procedural = procedural;
        if (procedural)
            procedural_lut_dirty = false;
    }

    NativePicaDrawBindings out{};
    out.selected_inputs = mask;
    selection->Apply(out.draw);
    if (pica.blend && pica.color_mask) {
        using Equation = Pica::FramebufferRegs::BlendEquation;
        using Factor = Pica::FramebufferRegs::BlendFactor;
        const auto& blend = r.framebuffer.output_merger.alpha_blending;
        // ADD with coefficients one and zero replaces the written channels.
        // Blend modes that can read the destination keep their own pass.
        const bool rgb_replace = !(pica.color_mask & 7) ||
            (blend.blend_equation_rgb == Equation::Add &&
             blend.factor_source_rgb == Factor::One && blend.factor_dest_rgb == Factor::Zero);
        const bool alpha_replace = !(pica.color_mask & 8) ||
            (blend.blend_equation_a == Equation::Add &&
             blend.factor_source_a == Factor::One && blend.factor_dest_a == Factor::Zero);
        out.draw.isolate_color_blend = !rgb_replace || !alpha_replace;
    }
    out.color = std::move(color);
    out.depth = std::move(depth);
    out.views.color = out.color.view;
    out.views.depth_stencil = out.depth.view;
    out.draw.color = out.color.view.texture;
    out.draw.depth_stencil = out.depth.view.texture;
    out.draw.lighting_lut = selection->slots[0] == NSNotFound ? nil : lut;
    // Depth feedback misses publish through DecodeTexture before resolving
    // authoritative bytes. Recorded depth writes invalidate cached samples,
    // so a cache hit can keep the current render group open.
    if (primary_color_snapshot)
        lane.PublishPending();
    out.views.primary_color_snapshot = primary_color_snapshot;
    out.views.depth_snapshot_slots = depth_snapshot_slots;
    for (unsigned i = 0; i < ranges.size(); ++i) {
        if (i == 0 && primary_cube) {
            const auto cube = Measure(6, [&] {
                return BuildPicaMetalCube(lane, resources.queue, *parts.textures, *primary_cube,
                                          unsigned(textures[0].format), ram);
            });
            out.draw.texture0 = cube.texture;
            out.views.sampled = {cube.texture, {}, ++generation};
            auto& faces = out.views.primary_cube_faces.emplace();
            for (std::size_t face = 0; face < cube.faces.size(); ++face)
                faces[face] = {cube.faces[face].begin, cube.faces[face].end};
            continue;
        }
        if (!ranges[i])
            continue;
        const auto& info = textures[(*units)[i]];
        auto texture = Measure(6, [&] {
            return DecodeTextureMips(lane, resources.queue, *parts.textures, info, *ranges[i], ram);
        });
        const auto bytes = ranges[i]->bytes;
        PicaMetalSurfaceView sampled{texture, {bytes.begin, bytes.end}, ++generation};
        if (i == 0) {
            out.draw.texture0 = texture;
            out.views.sampled = sampled;
        } else if (i == 1) {
            out.draw.secondary_material->texture = texture;
            out.views.secondary_sampled = sampled;
        } else {
            out.draw.tertiary_material->texture = texture;
            out.views.tertiary_sampled = sampled;
        }
    }

    NativeMetalCost::Scope footprint(NativeMetalCost::FootprintHost);
    const Common::Rectangle<u32> rect{u32(view.left), u32(view.top), u32(view.right), u32(view.bottom)};
    if (pica.color_mask) {
        auto footprint_format = Params::PixelFormat::RGBA8;
        if (color_format == ColorFormat::RGB565)
            footprint_format = Params::PixelFormat::RGB565;
        else if (color_format == ColorFormat::RGBA4)
            footprint_format = Params::PixelFormat::RGBA4;
        else if (color_format == ColorFormat::RGB5A1)
            footprint_format = Params::PixelFormat::RGB5A1;
        out.views.color_dirty = WriteFootprint(*color_range, width, height, footprint_format, rect);
    }
    if (pica.depth_write || pica.stencil_write)
        out.views.depth_stencil_dirty = WriteFootprint(*depth_range, width, height,
            has_stencil ? Params::PixelFormat::D24S8 : Params::PixelFormat::D24, rect);
    out.draw.viewport = {double(rect.left), double(rect.bottom), double(rect.GetWidth()),
                         double(rect.GetHeight()), 0, 1};
    out.draw.scissor = {rect.left, rect.bottom, rect.GetWidth(), rect.GetHeight()};

    footprint.Stop();
    NativeMetalCost::Scope uniforms(NativeMetalCost::UniformsHost);
    if (!storage)
        storage = std::make_shared<BindingStorage>();
    storage->uniforms = CapturePicaMetalUniforms(
        state, Settings::values.disable_clip_coef && !pica.clip1);
    if (selection->runtime_tev) {
        // Lighting is disabled for this path. Its six ivec4 slots carry the six
        // TEV register groups without changing either uniform block's layout.
        auto& words = storage->uniforms.light.lighting_lut_offset;
        static_assert(std::size(OpenGL::PicaFSConfigState{}.tev_stages) ==
                      std::extent_v<std::remove_reference_t<decltype(words)>>);
        for (unsigned i = 0; i < fcfg.state.tev_stages.size(); ++i)
            words[i] = fcfg.RuntimeTevWords(i);
    }
    if (selection->general_fragment) {
        storage->uniforms.common.cara_tex0_crop[0] = s32(fcfg.state.alpha_test_func);
        storage->uniforms.common.cara_tex0_crop[1] = s32(fcfg.state.scissor_test_mode);
        NativeMetalCost::Count(NativeMetalCost::GeneralFragmentDraws, 1);
    }
    if (vs.pc230_specialized && !storage->uniforms.vertex.uniforms.bools[14].b)
        throw std::runtime_error("Metal PC 230 draw uniforms changed after shader selection");
    out.draw.common_uniform = std::as_bytes(std::span(&storage->uniforms.common, 1));
    out.draw.light_uniform = std::as_bytes(std::span(&storage->uniforms.light, 1));
    if (pica.vertex_mode != Mode::StageIn)
        out.draw.vs_uniform = std::as_bytes(std::span(&storage->uniforms.vertex, 1));
    if (!stage_in.empty()) {
        storage->stage_in.assign(stage_in.begin(), stage_in.end());
        out.draw.packed_vertices = std::as_bytes(std::span(storage->stage_in));
        out.draw.vertex_count = stage_in.size();
    } else if (vertex_inputs) {
        storage->vertex_inputs = std::move(vertex_inputs);
        auto& vertices = *storage->vertex_inputs;
        if (raw_indices && UseMetalRawIndexConstants()) {
            const auto mode = static_cast<std::uint32_t>(vertices.indices[0]);
            const auto first = static_cast<std::uint32_t>(vertices.indices[1]);
            auto& elements = vertices.layout[0][3];
            // The original header remains in the index texture. Only this
            // alternate shader reads an additional copy in the layout uniform.
            elements |= static_cast<std::int32_t>((mode << 3) | (first << 7));
            out.draw.unique_source_vertices = vertices.unique_dense_vertices;
        }
        out.draw.packed_vertices = std::as_bytes(std::span(vertices.vertices));
        out.draw.expanded_indices = std::as_bytes(std::span(vertices.indices));
        if (pica.vertex_mode == Mode::PairIndexed)
            vertices.layout = OpenGL::CanonicalPackedVertexLayout(vertices.layout);
        if (pica.vertex_mode == Mode::Packed || pica.vertex_mode == Mode::PairIndexed)
            out.draw.layout_uniform = std::as_bytes(std::span(vertices.layout));
        out.draw.vertex_count = vertices.vertex_count;
        out.draw.pair_indices = std::move(pair_indices);
        if (raw_indices) {
            out.draw.vertex_mode = MetalRawIndexMode;
            if (NativeHostProfileEnabled()) {
                ++raw_index_draws;
                raw_index_vertices += vertices.vertex_count;
            }
        }
    }
    out.storage = std::move(storage);
    return out;
}

} // namespace Cara
