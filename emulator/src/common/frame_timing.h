// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <array>
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
    PhaseCPUSamples vertex_orphan_cpu_samples;
    PhaseCPUSamples stream_map_cpu_samples;
    PhaseCPUSamples stream_unmap_cpu_samples;
    PhaseCPUSamples uniform_upload_cpu_samples;
    PhaseCPUSamples vertex_subimage_cpu_samples;
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
    std::uint64_t gpu_vertex_draws = 0;
    std::uint64_t gpu_vertices = 0;
    std::uint64_t cpu_vertex_draws = 0;
};
inline thread_local Counters counters;

inline bool RenderBookkeepingProfileEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CARA_RENDER_BOOKKEEPING_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

enum class Phase { SurfaceRemap, SurfacePack, UniformUpload, StreamMap, StreamUnmap, StateApply, Shader, Readback, Present, GuestCPU, Draw, VertexPrepare, VertexUpload, VertexReuseCheck, VertexOrphanUpload, VertexStorageAllocate, VertexSubImageUpload, DrawSubmit, SurfaceDownload, ShaderCompile, GPUWait, DrawFramebufferAcquire, DrawTextureAcquire, DrawShaderSetup, DrawDirtyInvalidate, ReinterpretCheck };
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
        if (sample_phase < 0 || !CPUSamplingEnabled()) return;
        static thread_local std::array<std::uint64_t, 6> sample_sequence{};
        // Use high bits of a multiplicative hash, avoiding a fixed draw-slot
        // pattern when a frame repeatedly submits a multiple of 64 draws.
        const auto mixed = std::uint32_t(++sample_sequence[sample_phase]) * 0x9E3779B9u;
        if ((mixed >> 26) != 0) return;
        cpu_sample = sample_phase == 0 ? &counters.draw_submit_cpu_samples
                   : sample_phase == 1 ? &counters.vertex_orphan_cpu_samples
                                       : sample_phase == 2 ? &counters.stream_map_cpu_samples
                   : sample_phase == 3 ? &counters.stream_unmap_cpu_samples
                   : sample_phase == 4 ? &counters.uniform_upload_cpu_samples
                                       : &counters.vertex_subimage_cpu_samples;
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
        } else if (phase == Phase::ShaderCompile) {
            counters.shader_compile_seconds += seconds;
            ++counters.shader_compile_calls;
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
