#include "cara/native_mac/profile_gpu_boundary.h"
#include "video_core/command_processor.h"
#include "cara/native_mac/diagnostic_clock.h"
#include "video_core/lut_effective_change.h"
#include "video_core/renderer_opengl/gl_software_batch_policy.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/video_core.h"
#include "video_core/renderer_opengl/gl_effect_family_ablation.h"
// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
// Opt-in developer snapshots. No GPU readback, RAM payload, counter reset or timing mutation.
#pragma once
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include "common/frame_timing.h"
#include "cara/native_mac/script_only_input.h"
#include "core/arm/fastinterp/fastinterp.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/hid/hid.h"
#include "core/savestate.h"
#include "core/settings.h"
#include "video_core/renderer_opengl/gl_packed_vertex_padding.h"
#include "video_core/renderer_opengl/gl_fragment_lighting_ablation.h"
#include "video_core/renderer_opengl/gl_command_queue.h"
#include "video_core/renderer_opengl/gl_index_payload_reuse.h"
#include "common/host_render_queue.h"

inline void WriteNativeProfileSnapshot(const std::string& stem, u32 frame, bool terminal_benchmark_boundary=false) {
    const char* enabled = std::getenv("CARA_NATIVE_PROFILE_SNAPSHOT");
    const char* diagnostics = std::getenv("CARA_NATIVE_DIAGNOSTICS");
    if (!diagnostics || std::strcmp(diagnostics, "1") != 0 ||
        !enabled || std::strcmp(enabled, "1") != 0)
        return;
    // Benchmark boundaries include every preceding host command, so a growing
    // queue cannot masquerade as faster rendering. This is not glFinish.
    OpenGL::CommandQueue::Drain();
    const auto exact_flag=[](const char* name){const char* v=std::getenv(name);return v&&std::strcmp(v,"1")==0;};
    const bool gpu_requested=exact_flag("CARA_NATIVE_PROFILE_GPU_COMPLETE");
    const bool gpu_complete=Cara::NativeProfile::CompleteGPU(gpu_requested,true,
        exact_flag("CARA_NATIVE_BENCHMARK"),terminal_benchmark_boundary,
        {glad_glFenceSync,glad_glClientWaitSync,glad_glDeleteSync,&OpenGL::CommandQueue::Drain});
    const auto worker_cpu=OpenGL::CommandQueue::ReadWorkerCPUForProfile();
    timespec process_cpu{};
    if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&process_cpu))throw std::runtime_error("Process CPU profile clock failed");
    const auto process_cpu_ns=std::uint64_t(process_cpu.tv_sec)*1000000000ull+std::uint64_t(process_cpu.tv_nsec);
    auto& system = Core::System::GetInstance();
    std::ofstream out(stem + "-profile.json");
    timespec cpu{};
    const bool cpu_ok = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) == 0;
    const auto wall = std::chrono::steady_clock::now().time_since_epoch();
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wall).count();
    out << std::setprecision(17) << std::boolalpha;
    out << "{\"kind\":\"native_capture_time_cumulative_counters_not_gpu_timing\",\"frame\":"
        << frame << ",\"steady_wall_ns\":" << wall_ns << ",\"thread_cpu_ns\":";
    if (cpu_ok)
        out << u64(cpu.tv_sec) * 1000000000ULL + u64(cpu.tv_nsec);
    else
        out << "null";
    out << ",\"process_cpu_ns\":" << process_cpu_ns
        << ",\"worker_thread_cpu_ns\":" << worker_cpu
        << ",\"gpu_complete_requested\":" << gpu_requested
        << ",\"gpu_complete_boundary\":" << gpu_complete;
    const auto hid = Service::HID::GetModule(system);
    out << ",\"hid_pad_state_hex\":";
    if (hid)
        out << hid->GetState().hex;
    else
        out << "null";
    out << ",\"source_fingerprint\":\"" << Core::GetSaveStateBuildFingerprint() << "\""
        << ",\"timing_counter_mode\":\""
        << (Common::FrameTiming::counters.enabled ? "enabled" : "disabled") << "\"";
    out << ",\"gl_command_queue_enabled\":" << OpenGL::CommandQueue::Enabled()
        << ",\"gl_queue_native_map_upload_requested\":" << OpenGL::CommandQueue::NativeMapUploadRequested()
        << ",\"gl_queue_native_map_upload_active\":" << OpenGL::CommandQueue::NativeMapUploadActive();
    const auto unchecked_blit=OpenGL::CommandQueue::GetUncheckedBlitProducerStats();
    out << ",\"unchecked_blit_producer\":{\"requested\":" << unchecked_blit.requested
        << ",\"gate_active\":" << unchecked_blit.active
        << ",\"blit_commands_recorded\":" << unchecked_blit.blit_commands_recorded << "}";
    if (auto* queue = OpenGL::CommandQueue::Queue()) {
        const auto q=queue->GetStats();const auto r=OpenGL::CommandQueue::SnapshotStats();
        out << ",\"gl_command_queue\":{\"immutable_query_memoization\":false,\"worker_thread_cpu_ns\":" << worker_cpu
            << ",\"epoch\":" << q.epoch
            << ",\"submissions\":" << q.submissions << ",\"completions\":" << q.completions
            << ",\"errors\":" << q.errors << ",\"blocked_submissions\":" << q.blocked_submissions
            << ",\"high_water_batches\":" << q.high_water_jobs
            << ",\"high_water_payload_bytes\":" << q.high_water_bytes
            << ",\"live_batches\":" << q.live_jobs << ",\"live_bytes\":" << q.live_bytes
            << ",\"recorded_gl_commands\":" << r.recorded_commands
            << ",\"submitted_gl_batches\":" << r.submitted_batches
            << ",\"synchronous_calls\":" << r.synchronous_calls
            << ",\"texture_owned_uploads\":" << r.texture_owned_uploads
            << ",\"texture_pointer_rpcs\":" << r.texture_pointer_rpcs
            << ",\"owned_payload_bytes\":" << r.owned_payload_bytes
            << ",\"uniform_location_rpcs\":" << r.uniform_location_rpcs
            << ",\"uniform_location_cache_hits\":" << r.uniform_location_cache_hits
            << ",\"attrib_location_cache_hits\":" << r.attrib_location_cache_hits
            << ",\"uniform_block_index_cache_hits\":" << r.uniform_block_index_cache_hits
            << ",\"worker_rejected_calls\":" << r.worker_rejected_calls
            << ",\"transaction_sync_calls\":" << r.transaction_sync_calls
            << ",\"transaction_state_fallbacks\":" << r.transaction_state_fallbacks
            << ",\"transaction_lut_calls\":" << r.transaction_lut_calls
            << ",\"transaction_inline_calls\":" << r.transaction_inline_calls
            << ",\"owned_unmap_error_queries\":" << r.owned_unmap_error_queries
            << ",\"owned_native_map_uploads\":" << r.owned_native_map_uploads
            << ",\"owned_budget_live\":" << r.owned_budget_live
            << ",\"owned_budget_high\":" << r.owned_budget_high
            << ",\"rpc_by_api\":{";
        for (std::size_t i=0;i<OpenGL::CommandQueue::RpcApiCount;++i) {
            out << (i ? "," : "") << '\"'
                << OpenGL::CommandQueue::RpcApiName(static_cast<OpenGL::CommandQueue::RpcApi>(i))
                << "\":" << r.rpc_by_api[i];
        }
        out << "}}";
        out << ",\"gl_transaction_attribution\":";
        OpenGL::TransactionAttribution::WriteJSON(out,OpenGL::CommandQueue::GetTransactionAttribution());
    }
    out << ",\"packed_padding_only_zero\":" << OpenGL::PackedVertexPaddingOnlyEnabled();
    out << ",\"fragment_lighting_ablation\":" << OpenGL::FragmentLightingAblationEnabled();
    out << ",\"is_new_3ds\":" << Settings::values.is_new_3ds
        << ",\"core_downcount_hack_setting\":" << Settings::values.core_downcount_hack
        << ",\"guest_cpu_requested_percent\":" << Cara::NativeDiagnosticClock::RequestedPercent()
        << ",\"guest_cpu_effective_percent\":" << Cara::NativeDiagnosticClock::EffectivePercent(system.CoreTiming())
        << ",\"diagnostic_clock\":";
    Cara::NativeDiagnosticClock::WriteJSON(out,system.CoreTiming(),system.GetNumCores());
    out << ",\"timers\":[";
    for (u32 i = 0; i < system.GetNumCores(); ++i) {
        const auto timer = system.CoreTiming().GetTimer(i);
        out << (i ? "," : "") << "{\"core\":" << i
            << ",\"cpu_clock_scale\":" << timer->GetClockScaleForDiagnostics()
            << ",\"effective_guest_clock_percent\":" << 100.0 / timer->GetClockScaleForDiagnostics()
            << ",\"downcount_hack_shift\":" << timer->GetDowncountHackForDiagnostics()
            << ",\"ticks_divisor_before_clock_scale\":"
            << (u64(1) << timer->GetDowncountHackForDiagnostics()) << '}';
    }
    out << ']';
    namespace E=OpenGL::EffectFamilyAblation;
    out << ",\"particle_family_ablation\":" << E::ParticleEnabled()
        << ",\"composite_pass_ablation\":" << E::CompositeEnabled()
        << ",\"ray_candidate_ablation\":" << E::RayEnabled()
        << ",\"effect_family_host_counts\":{\"particle_candidates\":" << E::counts.particle_host_candidates
        << ",\"composite_candidates\":" << E::counts.composite_host_candidates
        << ",\"particle_skips\":" << E::counts.particle_host_skips
        << ",\"composite_skips\":" << E::counts.composite_host_skips
        << ",\"particle_vertices_skipped\":" << E::counts.particle_host_vertices_skipped
        << ",\"composite_vertices_skipped\":" << E::counts.composite_host_vertices_skipped
        << ",\"ray_candidates\":" << E::counts.ray_host_candidates
        << ",\"ray_skips\":" << E::counts.ray_host_skips
        << ",\"ray_vertices_skipped\":" << E::counts.ray_host_vertices_skipped << '}';
    const auto& c = Common::FrameTiming::counters;
    out << ",\"current_timer_ticks\":" << system.CoreTiming().GetTicks()
        << ",\"global_ticks\":" << system.CoreTiming().GetGlobalTicks()
        << ",\"candidate_requests\":{\"vldr_pairs\":";
    const auto flag = [](const char* name) {
        const char* value = std::getenv(name);
        return value && std::strcmp(value, "1") == 0;
    };
    out << flag("CARA_FASTINTERP_VLDR_PAIRS")
        << ",\"mac_depth_reinterpret\":" << flag("CARA_MAC_GPU_DEPTH_REINTERPRET")
        << ",\"tight_framebuffer_backing\":" << flag("CARA_TIGHT_FRAMEBUFFER_BACKING")
        << "},\"diagnostic_controls\":{\"enabled\":true,\"snapshot\":true,\"capture_readback\":false"
        << ",\"benchmark_requested\":" << flag("CARA_NATIVE_BENCHMARK")
        << ",\"script_only_input\":" << Cara::NativeScriptOnlyInput::Enabled()
        << ",\"ignored_physical_event_count\":"
        << Cara::NativeScriptOnlyInput::ignored_physical_event_count
        << ",\"circle_script_requested\":" << (std::getenv("CARA_NATIVE_CIRCLE_SEGMENTS") != nullptr)
        << ",\"a_script_requested\":" << (std::getenv("CARA_NATIVE_PRESS_A_FRAMES") != nullptr)
        << ",\"start_script_requested\":" << (std::getenv("CARA_NATIVE_PRESS_START_FRAMES") != nullptr)
        << "},\"execution_state\":[";
    for (u32 i = 0; i < system.GetNumCores(); ++i) {
        const auto& core = system.GetCore(i);
        out << (i ? "," : "") << "{\"core\":" << i << ",\"cpsr\":" << core.GetCPSR()
            << ",\"registers\":[";
        for (u32 j = 0; j < 16; ++j)
            out << (j ? "," : "") << core.GetReg(j);
        out << "],\"fpscr\":" << core.GetVFPSystemReg(VFP_FPSCR) << ",\"vfp_registers\":[";
        for (u32 j = 0; j < 64; ++j)
            out << (j ? "," : "") << core.GetVFPReg(j);
        out << "]}";
    }
    out << "],\"counters\":{\"enabled\":" << c.enabled;
    out << ",\"pica_command_lists\":" << c.pica_command_lists;
    out << ",\"pica_draw_requests\":" << c.pica_draw_requests;
    out << ",\"pica_post_irq_submissions\":" << c.pica_post_irq_submissions;
    out << ",\"pica_post_irq_writes\":" << c.pica_post_irq_writes;
    out << ",\"pica_post_irq_draw_requests\":" << c.pica_post_irq_draw_requests;
    out << ",\"pica_post_irq_jumps\":" << c.pica_post_irq_jumps;
    out << ",\"pica_first_irq_remaining_words\":" << c.pica_first_irq_remaining_words;
    out << ",\"experimental_ppf_scheduled\":" << c.experimental_ppf_scheduled;
    out << ",\"experimental_ppf_delivered\":" << c.experimental_ppf_delivered;
    out << ",\"experimental_ppf_modeled_ns\":" << c.experimental_ppf_modeled_ns;
    out << ",\"experimental_ppf_queued_ticks\":" << c.experimental_ppf_queued_ticks;
    out << ",\"experimental_p3d_scheduled\":" << c.experimental_p3d_scheduled;
    out << ",\"experimental_p3d_delivered\":" << c.experimental_p3d_delivered;
    out << ",\"experimental_p3d_retired\":" << c.experimental_p3d_retired;
    out << ",\"experimental_p3d_modeled_ns\":" << c.experimental_p3d_modeled_ns;
    out << ",\"experimental_p3d_queued_ticks\":" << c.experimental_p3d_queued_ticks;
    out << ",\"top_buffer_swaps\":" << c.top_buffer_swaps;
    out << ",\"stereo_top_buffer_swaps\":" << c.stereo_top_buffer_swaps;
    out << ",\"texture_lookup_checks\":" << c.texture_lookup_checks;
    out << ",\"texture_lookup_hits\":" << c.texture_lookup_hits;
    out << ",\"texture_lookup_misses\":" << c.texture_lookup_misses;
    out << ",\"texture_lookup_stores\":" << c.texture_lookup_stores;
    out << ",\"texture_lookup_ambiguous_rejects\":" << c.texture_lookup_ambiguous_rejects;
    out << ",\"texture_lookup_epoch_rejects\":" << c.texture_lookup_epoch_rejects;
    out << ",\"texture_lookup_verified_hits\":" << c.texture_lookup_verified_hits;
    out << ",\"texture_lookup_verify_mismatches\":" << c.texture_lookup_verify_mismatches;
    out << ",\"draw_framebuffer_acquire_seconds\":" << c.draw_framebuffer_acquire_seconds;
    out << ",\"draw_framebuffer_acquire_calls\":" << c.draw_framebuffer_acquire_calls;
    out << ",\"draw_texture_acquire_seconds\":" << c.draw_texture_acquire_seconds;
    out << ",\"draw_texture_acquire_calls\":" << c.draw_texture_acquire_calls;
    out << ",\"draw_shader_setup_seconds\":" << c.draw_shader_setup_seconds;
    out << ",\"draw_shader_setup_calls\":" << c.draw_shader_setup_calls;
    out << ",\"draw_dirty_invalidate_seconds\":" << c.draw_dirty_invalidate_seconds;
    out << ",\"draw_dirty_invalidate_calls\":" << c.draw_dirty_invalidate_calls;
    out << ",\"render_reinterpret_check_seconds\":" << c.render_reinterpret_check_seconds;
    out << ",\"render_reinterpret_check_calls\":" << c.render_reinterpret_check_calls;
    out << ",\"render_reinterpret_invocations\":" << c.render_reinterpret_invocations;
    out << ",\"render_reinterpret_find_calls\":" << c.render_reinterpret_find_calls;
    out << ",\"render_reinterpret_avoided_calls\":" << c.render_reinterpret_avoided_calls;
    out << ",\"render_reinterpret_repeat_opportunities\":"
        << c.render_reinterpret_repeat_opportunities;
    out << ",\"contiguous_transfer_loads\":" << c.contiguous_transfer_loads;
    out << ",\"contiguous_transfer_stores\":" << c.contiguous_transfer_stores;
    out << ",\"contiguous_transfer_bytes\":" << c.contiguous_transfer_bytes;
    out << ",\"texture_exact_candidates\":" << c.texture_exact_candidates;
    out << ",\"texture_exact_wrong_layout\":" << c.texture_exact_wrong_layout;
    out << ",\"texture_exact_early_rejects\":" << c.texture_exact_early_rejects;
    out << ",\"texture_exact_validity_lookups\":" << c.texture_exact_validity_lookups;
    out << ",\"shader_seconds\":" << c.shader_seconds;
    out << ",\"guest_cpu_seconds\":" << c.guest_cpu_seconds;
    out << ",\"draw_seconds\":" << c.draw_seconds;
    out << ",\"vertex_prepare_seconds\":" << c.vertex_prepare_seconds;
    out << ",\"vertex_upload_seconds\":" << c.vertex_upload_seconds;
    out << ",\"vertex_index_cache_hits\":" << c.vertex_index_cache_hits;
    out << ",\"vertex_index_cache_misses\":" << c.vertex_index_cache_misses;
    out << ",\"vertex_index_cache_bytes\":" << c.vertex_index_cache_bytes;
    out << ",\"lit_pair_probe_draws\":" << c.lit_pair_probe_draws;
    out << ",\"lit_pair_probe_unavailable_draws\":" << c.lit_pair_probe_unavailable_draws;
    out << ",\"lit_pair_probe_vertices\":" << c.lit_pair_probe_vertices;
    out << ",\"lit_pair_probe_unique_pairs\":" << c.lit_pair_probe_unique_pairs;
    out << ",\"lit_pair_probe_baseline_vs_runs\":" << c.lit_pair_probe_baseline_vs_runs;
    out << ",\"lit_pair_probe_lru16_misses\":" << c.lit_pair_probe_lru16_misses;
    out << ",\"lit_pair_probe_lru32_misses\":" << c.lit_pair_probe_lru32_misses;
    out << ",\"lit_pair_probe_lru16_vs_runs\":" << c.lit_pair_probe_lru16_vs_runs;
    out << ",\"lit_pair_probe_lru32_vs_runs\":" << c.lit_pair_probe_lru32_vs_runs;
    out << ",\"lit_pair_probe_pair_table_bytes\":" << c.lit_pair_probe_pair_table_bytes;
    out << ",\"lit_pair_probe_selected_draws\":" << c.lit_pair_probe_selected_draws;
    out << ",\"lit_pair_probe_selected_baseline_vs_runs\":"
        << c.lit_pair_probe_selected_baseline_vs_runs;
    out << ",\"lit_pair_probe_selected_lru32_vs_runs\":" << c.lit_pair_probe_selected_lru32_vs_runs;
    out << ",\"vertex_reuse_check_seconds\":" << c.vertex_reuse_check_seconds;
    out << ",\"vertex_upload_reused_bytes\":" << c.vertex_upload_reused_bytes;
    out << ",\"vertex_upload_reused_textures\":" << c.vertex_upload_reused_textures;
    out << ",\"vertex_orphan_upload_seconds\":" << c.vertex_orphan_upload_seconds;
    out << ",\"vertex_storage_allocate_seconds\":" << c.vertex_storage_allocate_seconds;
    out << ",\"vertex_subimage_upload_seconds\":" << c.vertex_subimage_upload_seconds;
    out << ",\"vertex_upload_bytes\":" << c.vertex_upload_bytes;
    out << ",\"vertex_index_upload_bytes\":" << c.vertex_index_upload_bytes;
    out << ",\"gpu_input_records\":" << c.gpu_input_records;
    out << ",\"gpu_indexed_input_draws\":" << c.gpu_indexed_input_draws;
    out << ",\"gpu_packed_input_draws\":" << c.gpu_packed_input_draws;
    out << ",\"packed_validation_failures\":" << c.packed_validation_failures;
    out << ",\"packed_verified_draws\":" << c.packed_verified_draws;
    out << ",\"vertex_orphan_upload_bytes\":" << c.vertex_orphan_upload_bytes;
    out << ",\"vertex_gl_image_calls\":" << c.vertex_gl_image_calls;
    out << ",\"vertex_gl_subimage_calls\":" << c.vertex_gl_subimage_calls;
    out << ",\"vertex_upload_bank_budget_bytes\":" << c.vertex_upload_bank_budget_bytes;
    out << ",\"vertex_reuse_expanded\":" << c.vertex_reuse_expanded;
    out << ",\"vertex_reuse_unique\":" << c.vertex_reuse_unique;
    out << ",\"vertex_reuse_attribute_bytes\":" << c.vertex_reuse_attribute_bytes;
    out << ",\"vertex_reuse_unique_attribute_bytes\":" << c.vertex_reuse_unique_attribute_bytes;
    out << ",\"vertex_reuse_invalid_draws\":" << c.vertex_reuse_invalid_draws;
    out << ",\"stream_map_seconds\":" << c.stream_map_seconds;
    out << ",\"stream_unmap_seconds\":" << c.stream_unmap_seconds;
    out << ",\"stream_map_calls\":" << c.stream_map_calls;
    out << ",\"stream_unmap_calls\":" << c.stream_unmap_calls;
    out << ",\"stream_mapped_bytes\":" << c.stream_mapped_bytes;
    out << ",\"uniform_upload_seconds\":" << c.uniform_upload_seconds;
    out << ",\"state_apply_seconds\":" << c.state_apply_seconds;
    out << ",\"draw_submit_seconds\":" << c.draw_submit_seconds;
    out << ",\"surface_remap_seconds\":" << c.surface_remap_seconds;
    out << ",\"surface_remap_calls\":" << c.surface_remap_calls;
    out << ",\"surface_remap_bytes\":" << c.surface_remap_bytes;
    out << ",\"surface_pack_seconds\":" << c.surface_pack_seconds;
    out << ",\"surface_download_seconds\":" << c.surface_download_seconds;
    out << ",\"surface_read_pixels_seconds\":" << c.surface_read_pixels_seconds;
    out << ",\"surface_read_pixels_calls\":" << c.surface_read_pixels_calls;
    out << ",\"shader_compile_seconds\":" << c.shader_compile_seconds;
    out << ",\"gpu_wait_seconds\":" << c.gpu_wait_seconds;
    out << ",\"surface_download_calls\":" << c.surface_download_calls;
    out << ",\"surface_download_bytes\":" << c.surface_download_bytes;
    out << ",\"cpu_surface_invalidations\":" << c.cpu_surface_invalidations;
    out << ",\"shader_compile_calls\":" << c.shader_compile_calls;
    out << ",\"lit_draws\":" << c.lit_draws;
    out << ",\"active_light_sum\":" << c.active_light_sum;
    out << ",\"guest_ticks\":" << c.guest_ticks;
    out << ",\"vertex_upload_fallbacks\":" << c.vertex_upload_fallbacks;
    out << ",\"vertex_storage_allocations\":" << c.vertex_storage_allocations;
    out << ",\"vertex_pool_peak_bytes\":" << c.vertex_pool_peak_bytes;
    out << ",\"readback_seconds\":" << c.readback_seconds;
    out << ",\"present_seconds\":" << c.present_seconds;
    out << ",\"shader_calls\":" << c.shader_calls;
    out << ",\"gpu_feedback_vertex_draws\":" << c.gpu_feedback_vertex_draws;
    out << ",\"gpu_feedback_source_vertices\":" << c.gpu_feedback_source_vertices;
    out << ",\"gpu_lit_pair_draws\":" << c.gpu_lit_pair_draws;
    out << ",\"gpu_lit_pair_vertices\":" << c.gpu_lit_pair_vertices;
    out << ",\"gpu_lit_pair_ids\":" << c.gpu_lit_pair_ids;
    out << ",\"gpu_direct_vertex_draws\":" << c.gpu_direct_vertex_draws;
    out << ",\"gpu_direct_vertices\":" << c.gpu_direct_vertices;
    out << ",\"gpu_direct_unique_inputs\":" << c.gpu_direct_unique_inputs;
    out << ",\"vertex_input_stage_draws\":" << c.vertex_input_stage_draws;
    out << ",\"vertex_configured_input_registers\":" << c.vertex_configured_input_registers;
    out << ",\"vertex_selected_input_registers\":" << c.vertex_selected_input_registers;
    out << ",\"vertex_pruned_input_draws\":" << c.vertex_pruned_input_draws;
    out << ",\"gpu_vertex_draws\":" << c.gpu_vertex_draws;
    out << ",\"gpu_vertices\":" << c.gpu_vertices;
    out << ",\"cpu_vertex_draws\":" << c.cpu_vertex_draws;
    const auto phase = [&](const char* name, const Common::FrameTiming::PhaseCPUSamples& p) {
        out << ",\"" << name << "\":{\"wall_seconds\":" << p.wall_seconds
            << ",\"cpu_seconds\":" << p.cpu_seconds << ",\"samples\":" << p.samples
            << ",\"clock_failures\":" << p.clock_failures << '}';
    };
    phase("draw_submit_cpu_samples", c.draw_submit_cpu_samples);
    phase("vertex_orphan_cpu_samples", c.vertex_orphan_cpu_samples);
    phase("stream_map_cpu_samples", c.stream_map_cpu_samples);
    phase("stream_unmap_cpu_samples", c.stream_unmap_cpu_samples);
    phase("uniform_upload_cpu_samples", c.uniform_upload_cpu_samples);
    phase("vertex_subimage_cpu_samples", c.vertex_subimage_cpu_samples);
    phase("surface_download_cpu_samples", c.surface_download_cpu_samples);
    phase("surface_read_pixels_cpu_samples", c.surface_read_pixels_cpu_samples);
    out << "},\"texture_acquisition_sampled_phases\":[";
    for (std::size_t i = 0; i < c.texture_acquisition_samples.size(); ++i) {
        const auto& p = c.texture_acquisition_samples[i];
        out << (i ? "," : "") << "{\"phase_index\":" << i << ",\"calls\":" << p.calls
            << ",\"wall_seconds\":" << p.timing.wall_seconds
            << ",\"cpu_seconds\":" << p.timing.cpu_seconds << ",\"samples\":" << p.timing.samples
            << ",\"clock_failures\":" << p.timing.clock_failures << '}';
    }
    out << "],\"cores\":[";
    for (u32 i = 0; i < system.GetNumCores(); ++i) {
        if (i)
            out << ',';
        if (auto* cpu = dynamic_cast<Core::FastInterp::ARM_FastInterp*>(&system.GetCore(i)))
            cpu->WriteBlockProfile(out);
        else
            out << "{\"core\":" << i << ",\"backend\":\"non-FastInterp\"}";
    }
    out << "],\"software_batching\":";
    OpenGL::SoftwareBatching::WriteJSON(out);
    out << ",\"vertex_index_identity_reuse\":";
    OpenGL::IndexPayloadReuse::WriteJSON(out);
    out << ",\"pica_shader_stream_batch\":";
    Pica::CommandProcessor::WriteShaderStreamBatchJSON(out);
    if (auto* rasterizer=dynamic_cast<OpenGL::RasterizerOpenGL*>(VideoCore::Rasterizer())) {
        const auto& nb=rasterizer->NativeNearestBorrowCounters();
        out << ",\"native_nearest_borrow\":{\"enabled\":"
            << (rasterizer->NativeNearestBorrowEnabled()?"true":"false")
            << ",\"extension_and_entrypoint_available\":" << (rasterizer->NativeNearestBorrowAvailable()?"true":"false")
            << ",\"counters_available\":" << (rasterizer->NativeNearestBorrowEnabled()?"true":"false")
            << ",\"attempts\":" << nb.attempts << ",\"prepared_rejections\":" << nb.prepared_rejections
            << ",\"state_rejections\":" << nb.state_rejections << ",\"plan_rejections\":" << nb.plan_rejections
            << ",\"cache_rejections\":" << nb.cache_rejections << ",\"late_rejections\":" << nb.late_rejections
            << ",\"baseline_restarts\":" << nb.baseline_restarts << ",\"borrowed_draws\":" << nb.borrowed_draws
            << ",\"barriers\":" << nb.barriers << '}';
        out << ",\"native_copytex_surface_copy_calls\":" << rasterizer->NativeCopyTexSurfaceCopyCalls()
            << ",\"native_shader_surface_copy_calls\":" << rasterizer->NativeShaderSurfaceCopyCalls();
    }
    out << ",\"old3ds_syscore\":";
    system.Kernel().WriteOld3dsSyscoreJSON(out);
    out << ",\"lut_effective_change\":{\"enabled_requested\":"
        << (std::getenv("CARA_LUT_EFFECTIVE_CHANGE") && std::strcmp(std::getenv("CARA_LUT_EFFECTIVE_CHANGE"),"1")==0 ? "true":"false")
        << ",\"profile_requested\":"
        << (std::getenv("CARA_LUT_EFFECTIVE_CHANGE_PROFILE") && std::strcmp(std::getenv("CARA_LUT_EFFECTIVE_CHANGE_PROFILE"),"1")==0 ? "true":"false")
        << ",\"tables\":[";
    const auto lut_counts=Cara::LutEffectiveChange::ReadCounters();
    for(std::size_t i=0;i<lut_counts.size();++i){const auto& c=lut_counts[i];
        if(i)out<<',';out<<"{\"kind\":"<<i<<",\"writes\":"<<c.writes<<",\"changed\":"<<c.changed
            <<",\"notifications\":"<<c.notifications<<",\"invalid_targets\":"<<c.invalid_targets<<'}';}
    out<<"]}";
    timespec end_cpu{};
    const bool end_cpu_ok = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end_cpu) == 0;
    const auto end_wall = std::chrono::steady_clock::now().time_since_epoch();
    out << ",\"snapshot_complete_steady_wall_ns\":"
        << std::chrono::duration_cast<std::chrono::nanoseconds>(end_wall).count()
        << ",\"snapshot_complete_thread_cpu_ns\":";
    if (end_cpu_ok)
        out << u64(end_cpu.tv_sec) * 1000000000ULL + u64(end_cpu.tv_nsec);
    else
        out << "null";
    out << "}\n";
}
