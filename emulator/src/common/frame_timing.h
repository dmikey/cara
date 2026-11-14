// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace Common::FrameTiming {

struct PhaseCPUSamples {
    double wall_seconds = 0;
    double cpu_seconds = 0;
    std::uint64_t samples = 0;
    std::uint64_t clock_failures = 0;
};

enum class TextureAcquisitionPhase { Acquire, Selection, Validation, Reinterpretation, Allocation,
                                     Load, Upload, Mipmap, Count };
struct TextureAcquisitionSamples {
    std::uint64_t calls = 0;
    PhaseCPUSamples timing;
};

// Opt-in diagnostics on the emulation thread. No clocks are read during normal play.
struct Counters {
    bool enabled = false;
    // Requests, not unique frames: repeated command lists/draws/swaps count again.
    std::uint64_t pica_command_lists = 0;
    std::uint64_t pica_draw_requests = 0;
    std::uint64_t pica_post_irq_submissions = 0;
    std::uint64_t pica_post_irq_writes = 0;
    std::uint64_t pica_post_irq_draw_requests = 0;
    std::uint64_t pica_post_irq_jumps = 0;
    std::uint64_t pica_first_irq_remaining_words = 0;
    std::uint64_t experimental_ppf_scheduled = 0;
    std::uint64_t experimental_ppf_delivered = 0;
    std::uint64_t experimental_ppf_modeled_ns = 0;
    std::uint64_t experimental_ppf_queued_ticks = 0;
    std::uint64_t experimental_p3d_scheduled = 0;
    std::uint64_t experimental_p3d_delivered = 0;
    std::uint64_t experimental_p3d_retired = 0;
    std::uint64_t experimental_p3d_modeled_ns = 0;
    std::uint64_t experimental_p3d_queued_ticks = 0;
    std::uint64_t top_buffer_swaps = 0;
    // Top swaps with nonzero, distinct physical left/right addresses. This does
    // not establish that both views were rendered or presented.
    std::uint64_t stereo_top_buffer_swaps = 0;
    std::uint64_t texture_lookup_checks = 0;
    std::uint64_t texture_lookup_hits = 0;
    std::uint64_t texture_lookup_misses = 0;
    std::uint64_t texture_lookup_stores = 0;
    std::uint64_t texture_lookup_ambiguous_rejects = 0;
    std::uint64_t texture_lookup_epoch_rejects = 0;
    std::uint64_t texture_lookup_verified_hits = 0;
    std::uint64_t texture_lookup_verify_mismatches = 0;

    double draw_framebuffer_setup_seconds = 0;
    double draw_lut_upload_seconds = 0;
    double draw_vertex_execution_seconds = 0;
    double draw_framebuffer_acquire_seconds = 0;
    std::uint64_t draw_framebuffer_acquire_calls = 0;
    double draw_texture_acquire_seconds = 0;
    std::uint64_t draw_texture_acquire_calls = 0;
    double draw_shader_setup_seconds = 0;
    std::uint64_t draw_shader_setup_calls = 0;
    double draw_dirty_invalidate_seconds = 0;
    std::uint64_t draw_dirty_invalidate_calls = 0;
    double render_reinterpret_check_seconds = 0;
    std::uint64_t render_reinterpret_check_calls = 0;
    std::uint64_t render_reinterpret_invocations = 0;
    std::uint64_t render_reinterpret_find_calls = 0;
    std::uint64_t render_reinterpret_avoided_calls = 0;
    std::uint64_t render_reinterpret_repeat_opportunities = 0;
    std::uint64_t contiguous_transfer_loads = 0;
    std::uint64_t contiguous_transfer_stores = 0;
    std::uint64_t contiguous_transfer_bytes = 0;

    PhaseCPUSamples draw_submit_cpu_samples;
    PhaseCPUSamples vertex_prepare_cpu_samples;
    PhaseCPUSamples vertex_reuse_cpu_samples;
    PhaseCPUSamples state_apply_cpu_samples;
    PhaseCPUSamples vertex_execution_cpu_samples;
    PhaseCPUSamples framebuffer_setup_cpu_samples;
    PhaseCPUSamples present_cpu_samples;
    PhaseCPUSamples gpu_wait_cpu_samples;

    PhaseCPUSamples vertex_orphan_cpu_samples;
    PhaseCPUSamples stream_map_cpu_samples;
    PhaseCPUSamples stream_unmap_cpu_samples;
    PhaseCPUSamples uniform_upload_cpu_samples;
    PhaseCPUSamples vertex_subimage_cpu_samples;
    PhaseCPUSamples surface_download_cpu_samples;
    PhaseCPUSamples surface_read_pixels_cpu_samples;
    std::array<TextureAcquisitionSamples, static_cast<std::size_t>(TextureAcquisitionPhase::Count)>
        texture_acquisition_samples{};
    std::uint64_t texture_exact_candidates = 0;
    std::uint64_t texture_exact_wrong_layout = 0;
    std::uint64_t texture_exact_early_rejects = 0;
    std::uint64_t texture_exact_validity_lookups = 0;
    double shader_seconds = 0;
    double guest_cpu_seconds = 0;
    double draw_seconds = 0;
    double vertex_prepare_seconds = 0;
    double vertex_upload_seconds = 0;
    std::uint64_t vertex_index_cache_hits = 0;
    std::uint64_t vertex_index_cache_misses = 0;
    std::uint64_t vertex_index_cache_bytes = 0;
    std::uint64_t lit_pair_probe_draws = 0;
    std::uint64_t lit_pair_probe_unavailable_draws = 0;
    std::uint64_t lit_pair_probe_vertices = 0;
    std::uint64_t lit_pair_probe_unique_pairs = 0;
    std::uint64_t lit_pair_probe_baseline_vs_runs = 0;
    std::uint64_t lit_pair_probe_lru16_misses = 0;
    std::uint64_t lit_pair_probe_lru32_misses = 0;
    std::uint64_t lit_pair_probe_lru16_vs_runs = 0;
    std::uint64_t lit_pair_probe_lru32_vs_runs = 0;
    std::uint64_t lit_pair_probe_pair_table_bytes = 0;
    std::uint64_t lit_pair_probe_selected_draws = 0;
    std::uint64_t lit_pair_probe_selected_baseline_vs_runs = 0;
    std::uint64_t lit_pair_probe_selected_lru32_vs_runs = 0;

    double vertex_reuse_check_seconds = 0;
    std::uint64_t vertex_upload_reused_bytes = 0;
    std::uint64_t vertex_upload_reused_textures = 0;
    double vertex_orphan_upload_seconds = 0;
    double vertex_storage_allocate_seconds = 0;
    double vertex_subimage_upload_seconds = 0;
    std::uint64_t vertex_upload_bytes = 0;
    std::uint64_t vertex_index_upload_bytes = 0;
    std::uint64_t gpu_input_records = 0;
    std::uint64_t gpu_indexed_input_draws = 0;
    std::uint64_t gpu_packed_input_draws = 0;
    std::uint64_t packed_validation_failures = 0;
    std::uint64_t packed_verified_draws = 0;
    std::uint64_t vertex_orphan_upload_bytes = 0;
    std::uint64_t vertex_gl_image_calls = 0;
    std::uint64_t vertex_gl_subimage_calls = 0;
    std::uint64_t vertex_upload_bank_budget_bytes = 0;
    std::uint64_t vertex_reuse_expanded = 0;
    std::uint64_t vertex_reuse_unique = 0;
    std::uint64_t vertex_reuse_attribute_bytes = 0;
    std::uint64_t vertex_reuse_unique_attribute_bytes = 0;
    std::uint64_t vertex_reuse_invalid_draws = 0;
    double stream_map_seconds = 0;
    double stream_unmap_seconds = 0;
    std::uint64_t stream_map_calls = 0;
    std::uint64_t stream_unmap_calls = 0;
    std::uint64_t stream_mapped_bytes = 0;
    double uniform_upload_seconds = 0;
    double state_apply_seconds = 0;
    double draw_submit_seconds = 0;
    double surface_remap_seconds = 0;
    std::uint64_t surface_remap_calls = 0;
    std::uint64_t surface_remap_bytes = 0;
    double surface_pack_seconds = 0;
    double surface_download_seconds = 0;
    double surface_read_pixels_seconds = 0;
    std::uint64_t surface_read_pixels_calls = 0;
    double shader_compile_seconds = 0;
    double gpu_wait_seconds = 0;
    std::uint64_t surface_download_calls = 0;
    std::uint64_t surface_download_bytes = 0;
    std::uint64_t cpu_surface_invalidations = 0;
    std::uint64_t shader_compile_calls = 0;
    std::uint64_t lit_draws = 0;
    std::uint64_t active_light_sum = 0;
    std::uint64_t guest_ticks = 0;
    std::array<std::uint32_t, 4> last_guest_pc{};
    std::array<std::uint64_t, 4> guest_core_ticks{};
    std::uint64_t vertex_upload_fallbacks = 0;
    std::uint64_t vertex_storage_allocations = 0;
    std::uint64_t vertex_pool_peak_bytes = 0;
    double readback_seconds = 0;
    double present_seconds = 0;
    std::uint64_t shader_calls = 0;
    std::uint64_t gpu_feedback_vertex_draws = 0;
    std::uint64_t gpu_feedback_source_vertices = 0;
    std::uint64_t gpu_lit_pair_draws = 0;
    std::uint64_t gpu_lit_pair_vertices = 0;
    std::uint64_t gpu_lit_pair_ids = 0;
    std::uint64_t gpu_direct_vertex_draws = 0;
    std::uint64_t gpu_direct_vertices = 0;
    std::uint64_t gpu_direct_unique_inputs = 0;
    // GPU input staging attempts after topology validation; a later shader
    // rejection may still send the draw through the existing CPU fallback.
    std::uint64_t vertex_input_stage_draws = 0;
    std::uint64_t vertex_configured_input_registers = 0;
    std::uint64_t vertex_selected_input_registers = 0;
    std::uint64_t vertex_pruned_input_draws = 0;
    std::uint64_t gpu_vertex_draws = 0;
    std::uint64_t gpu_vertices = 0;
    std::uint64_t cpu_vertex_draws = 0;
};
inline thread_local Counters counters;

inline bool TextureAcquisitionProfileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_TEXTURE_ACQUISITION_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
inline thread_local unsigned texture_acquisition_depth = 0;
inline bool TextureAcquisitionTracingActive() {
    return counters.enabled && texture_acquisition_depth != 0;
}
class TextureAcquisitionContext {
public:
    TextureAcquisitionContext() : active(counters.enabled && TextureAcquisitionProfileEnabled()) {
        if (active) ++texture_acquisition_depth;
    }
    ~TextureAcquisitionContext() { if (active) --texture_acquisition_depth; }
    TextureAcquisitionContext(const TextureAcquisitionContext&) = delete;
    TextureAcquisitionContext& operator=(const TextureAcquisitionContext&) = delete;
private:
    bool active;
};

// Calls are exact; wall/thread CPU totals contain only selected calls. Nested
// phases overlap, and must not be summed or extrapolated to whole-run totals.
// Unlike Scope, unsampled calls do not read either clock.
class TextureAcquisitionScope {
public:
    explicit TextureAcquisitionScope(TextureAcquisitionPhase phase) {
        if (!TextureAcquisitionTracingActive()) return;
        const auto index = static_cast<std::size_t>(phase);
        auto& value = counters.texture_acquisition_samples[index];
        ++value.calls;
        static thread_local std::array<std::uint64_t,
            static_cast<std::size_t>(TextureAcquisitionPhase::Count)> sequence{};
        const auto mixed = std::uint32_t(++sequence[index]) * 0x9E3779B9u;
        if (sample_phase < 13 && (mixed >> 26) != 0) return;
        sample = &value.timing;
        started = std::chrono::steady_clock::now();
        cpu_started = ReadCPU();
        if (cpu_started < 0) {
            ++sample->clock_failures;
            sample = nullptr;
        }
    }
    ~TextureAcquisitionScope() {
        if (!sample) return;
        const double ended = ReadCPU();
        const double wall = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (ended < cpu_started) { ++sample->clock_failures; return; }
        sample->wall_seconds += wall;
        sample->cpu_seconds += ended - cpu_started;
        ++sample->samples;
    }
    TextureAcquisitionScope(const TextureAcquisitionScope&) = delete;
    TextureAcquisitionScope& operator=(const TextureAcquisitionScope&) = delete;
private:
    static double ReadCPU() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
        timespec value{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
            return double(value.tv_sec) + double(value.tv_nsec) * 1e-9;
#endif
        return -1;
    }
    PhaseCPUSamples* sample = nullptr;
    double cpu_started = 0;
    std::chrono::steady_clock::time_point started;
};

inline bool RenderBookkeepingProfileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_RENDER_BOOKKEEPING_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

enum class Phase { SurfaceRemap, SurfacePack, UniformUpload, StreamMap, StreamUnmap, StateApply, Shader, Readback, Present, GuestCPU, Draw, VertexPrepare, VertexUpload, VertexReuseCheck, VertexOrphanUpload, VertexStorageAllocate, VertexSubImageUpload, DrawSubmit, SurfaceDownload, SurfaceReadPixels, ShaderCompile, GPUWait, DrawFramebufferAcquire, DrawTextureAcquire, DrawShaderSetup, DrawDirtyInvalidate, ReinterpretCheck, DrawFramebufferSetup, DrawLUTUpload, DrawVertexExecution };
class Scope {
public:
    explicit Scope(Phase phase_, bool enabled = true) : phase(phase_), active(counters.enabled && enabled) {
        if (!active) return;
        started = std::chrono::steady_clock::now();
        int sample_phase = -1;
        if (phase == Phase::DrawSubmit) sample_phase = 0;
        else if (phase == Phase::VertexOrphanUpload) sample_phase = 1;
        else if (phase == Phase::StreamMap) sample_phase = 2;
        else if (phase == Phase::StreamUnmap) sample_phase = 3;
        else if (phase == Phase::UniformUpload) sample_phase = 4;
        else if (phase == Phase::VertexSubImageUpload) sample_phase = 5;
        else if (phase == Phase::SurfaceDownload) sample_phase = 6;
        else if (phase == Phase::SurfaceReadPixels) sample_phase = 7;
        else if (phase == Phase::VertexPrepare) sample_phase = 8;
        else if (phase == Phase::VertexReuseCheck) sample_phase = 9;
        else if (phase == Phase::StateApply) sample_phase = 10;
        else if (phase == Phase::DrawVertexExecution) sample_phase = 11;
        else if (phase == Phase::DrawFramebufferSetup) sample_phase = 12;
        else if (phase == Phase::Present) sample_phase = 13;
        else if (phase == Phase::GPUWait) sample_phase = 14;
        if (sample_phase < 0 || !CPUSamplingEnabled()) return;
        static thread_local std::array<std::uint64_t, 15> sample_sequence{};
        // Use high bits of a multiplicative hash, avoiding a fixed draw-slot
        // pattern when a frame repeatedly submits a multiple of 64 draws.
        const auto mixed = std::uint32_t(++sample_sequence[sample_phase]) * 0x9E3779B9u;
        if (sample_phase < 13 && (mixed >> 26) != 0) return;
        const std::array<PhaseCPUSamples*, 15> samples{{
            &counters.draw_submit_cpu_samples,
            &counters.vertex_orphan_cpu_samples,
            &counters.stream_map_cpu_samples,
            &counters.stream_unmap_cpu_samples,
            &counters.uniform_upload_cpu_samples,
            &counters.vertex_subimage_cpu_samples,
            &counters.surface_download_cpu_samples,
            &counters.surface_read_pixels_cpu_samples,
            &counters.vertex_prepare_cpu_samples,
            &counters.vertex_reuse_cpu_samples,
            &counters.state_apply_cpu_samples,
            &counters.vertex_execution_cpu_samples,
            &counters.framebuffer_setup_cpu_samples,
            &counters.present_cpu_samples,
            &counters.gpu_wait_cpu_samples}};
        cpu_sample = samples[sample_phase];
        cpu_started = ReadThreadCPUTime();
        if (cpu_started < 0) {
            ++cpu_sample->clock_failures;
            cpu_sample = nullptr;
        }
    }
    ~Scope() {
        if (!active) return;
        const double cpu_ended = cpu_sample ? ReadThreadCPUTime() : -1;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (cpu_sample) {
            if (cpu_ended >= cpu_started) {
                cpu_sample->wall_seconds += seconds;
                cpu_sample->cpu_seconds += cpu_ended - cpu_started;
                ++cpu_sample->samples;
            } else ++cpu_sample->clock_failures;
        }
        if (phase == Phase::SurfaceRemap) {
            counters.surface_remap_seconds += seconds;
        } else if (phase == Phase::SurfacePack) {
            counters.surface_pack_seconds += seconds;
        } else if (phase == Phase::UniformUpload) {
            counters.uniform_upload_seconds += seconds;
        } else if (phase == Phase::StreamMap) {
            counters.stream_map_seconds += seconds;
            ++counters.stream_map_calls;
        } else if (phase == Phase::StreamUnmap) {
            counters.stream_unmap_seconds += seconds;
            ++counters.stream_unmap_calls;
        } else if (phase == Phase::StateApply) {
            counters.state_apply_seconds += seconds;
        } else if (phase == Phase::Shader) {
            counters.shader_seconds += seconds;
            ++counters.shader_calls;
        } else if (phase == Phase::Readback) {
            counters.readback_seconds += seconds;
        } else if (phase == Phase::GuestCPU) {
            counters.guest_cpu_seconds += seconds;
        } else if (phase == Phase::Draw) {
            counters.draw_seconds += seconds;
        } else if (phase == Phase::VertexPrepare) {
            counters.vertex_prepare_seconds += seconds;
        } else if (phase == Phase::VertexReuseCheck) {
            counters.vertex_reuse_check_seconds += seconds;
        } else if (phase == Phase::VertexUpload) {
            counters.vertex_upload_seconds += seconds;
        } else if (phase == Phase::VertexOrphanUpload) {
            counters.vertex_orphan_upload_seconds += seconds;
        } else if (phase == Phase::VertexStorageAllocate) {
            counters.vertex_storage_allocate_seconds += seconds;
        } else if (phase == Phase::VertexSubImageUpload) {
            counters.vertex_subimage_upload_seconds += seconds;
        } else if (phase == Phase::DrawSubmit) {
            counters.draw_submit_seconds += seconds;
        } else if (phase == Phase::SurfaceDownload) {
            counters.surface_download_seconds += seconds;
        } else if (phase == Phase::SurfaceReadPixels) {
            counters.surface_read_pixels_seconds += seconds;
            ++counters.surface_read_pixels_calls;
        } else if (phase == Phase::ShaderCompile) {
            counters.shader_compile_seconds += seconds;
            ++counters.shader_compile_calls;
        } else if (phase == Phase::DrawFramebufferSetup) {
            counters.draw_framebuffer_setup_seconds += seconds;
        } else if (phase == Phase::DrawLUTUpload) {
            counters.draw_lut_upload_seconds += seconds;
        } else if (phase == Phase::DrawVertexExecution) {
            counters.draw_vertex_execution_seconds += seconds;
        } else if (phase == Phase::DrawFramebufferAcquire) {
            counters.draw_framebuffer_acquire_seconds += seconds;
            ++counters.draw_framebuffer_acquire_calls;
        } else if (phase == Phase::DrawTextureAcquire) {
            counters.draw_texture_acquire_seconds += seconds;
            ++counters.draw_texture_acquire_calls;
        } else if (phase == Phase::DrawShaderSetup) {
            counters.draw_shader_setup_seconds += seconds;
            ++counters.draw_shader_setup_calls;
        } else if (phase == Phase::DrawDirtyInvalidate) {
            counters.draw_dirty_invalidate_seconds += seconds;
            ++counters.draw_dirty_invalidate_calls;
        } else if (phase == Phase::ReinterpretCheck) {
            counters.render_reinterpret_check_seconds += seconds;
            ++counters.render_reinterpret_check_calls;
        } else if (phase == Phase::GPUWait) {
            counters.gpu_wait_seconds += seconds;
        } else {
            counters.present_seconds += seconds;
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    static bool CPUSamplingEnabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("CARA_PHASE_CPU_TIMING");
            return value && std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }
    static double ReadThreadCPUTime() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
        timespec value{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0)
            return double(value.tv_sec) + double(value.tv_nsec) * 1e-9;
#endif
        return -1;
    }
    // Approximately one in 64 calls, reported as samples rather than a phase
    // total: deterministic selection and clock overhead can still bias results.
    PhaseCPUSamples* cpu_sample = nullptr;
    double cpu_started = 0;
    Phase phase;
    bool active;
    std::chrono::steady_clock::time_point started;
};
} // namespace Common::FrameTiming
