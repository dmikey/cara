// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <array>
#include <cstdint>

namespace Common::FrameTiming {

// Opt-in diagnostics on the emulation thread. No clocks are read during normal play.
struct Counters {
    bool enabled = false;
    double shader_seconds = 0;
    double guest_cpu_seconds = 0;
    double draw_seconds = 0;
    double vertex_prepare_seconds = 0;
    double vertex_upload_seconds = 0;
    std::uint64_t vertex_index_cache_hits = 0;
    std::uint64_t vertex_index_cache_misses = 0;
    std::uint64_t vertex_index_cache_bytes = 0;
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
    std::uint64_t gpu_direct_vertex_draws = 0;
    std::uint64_t gpu_direct_vertices = 0;
    std::uint64_t gpu_direct_unique_inputs = 0;
    std::uint64_t gpu_vertex_draws = 0;
    std::uint64_t gpu_vertices = 0;
    std::uint64_t cpu_vertex_draws = 0;
};
inline thread_local Counters counters;

enum class Phase { SurfaceRemap, SurfacePack, UniformUpload, StreamMap, StreamUnmap, StateApply, Shader, Readback, Present, GuestCPU, Draw, VertexPrepare, VertexUpload, VertexReuseCheck, VertexOrphanUpload, VertexStorageAllocate, VertexSubImageUpload, DrawSubmit, SurfaceDownload, ShaderCompile, GPUWait };
class Scope {
public:
    explicit Scope(Phase phase_) : phase(phase_), active(counters.enabled) {
        if (active) started = std::chrono::steady_clock::now();
    }
    ~Scope() {
        if (!active) return;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
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
        } else if (phase == Phase::GPUWait) {
            counters.gpu_wait_seconds += seconds;
        } else {
            counters.present_seconds += seconds;
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Phase phase;
    bool active;
    std::chrono::steady_clock::time_point started;
};
} // namespace Common::FrameTiming
