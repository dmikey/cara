// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <string_view>

namespace Cara::NativeMetalCost {
enum Phase : std::size_t {
    Color, Depth, Lut, Record, Encode, Upload, EncoderSetup, EncoderStarts, EncoderDraw,
    RecordRetire, RecordAppend, RecordInvalidate, RecordNotify, PageHits,
    ReserveWait, ArenaWait, VertexUploadHits, VertexUploadMisses, PairHits, FragmentHits, GpuCompleted, GpuTimeMissing, DepthClearHits, UnitMemoHits, SelectionMemoHits, SnapshotHits, SnapshotReadNoPublish, InvalidationNoPublish, FlushNoPublish,
    Publish, Submits, PublishDraws, VertexRead, Present, Transfer, Flush,
    FillHost, TextureCopyHost, DisplayHost, RetileHost, RawDetileHost, NativeColorHost, GenericDisplayHost, PublishBytesHost, ResolveBytesHost, ExportSpanHost, ClassifyHost, SamplerPlanHost, UniformsHost, FootprintHost, QueuedColorExportHost,
    CopyPublishPending, CopyReserve, CopyPlan, CopyAllocate, CopyResolve, CopyEncode,
    CopyPublish, CopyDense, QueueReserveFull, QueueTrackFull, QueueDrain,
    SnapshotReserveAvoided, DepthClearFused, DirectTiledColorExport, QueuedMipBuild, LateTransferReserve,
    RenderPipelineCreate, RenderPipelineCreateValidated, RenderPipelineReuse,
    RenderPipelineArchiveOpen, RenderPipelineArchiveOpenSuccess, RenderPipelineArchiveRecord,
    RenderPipelineArchiveRecordSuccess, RenderPipelineArchiveHit, RenderPipelineArchiveMiss,
    RenderPipelineArchiveSerialize, RenderPipelineArchiveSerializeSuccess,
    UniformCommonUploadRequests, UniformCommonUploadHits, UniformCommonRequestedBytes, UniformCommonCopiedBytes,
    UniformLightUploadRequests, UniformLightUploadHits, UniformLightRequestedBytes, UniformLightCopiedBytes,
    UniformVsUploadRequests, UniformVsUploadHits, UniformVsRequestedBytes, UniformVsCopiedBytes,
    QueueWaitSiteOther, QueueWaitSiteDraw, QueueWaitSiteCopy, QueueWaitSiteFill,
    QueueWaitSiteDisplay, QueueWaitSiteSample, QueueWaitSitePresent, QueueWaitSiteDrain,
    QueueWaitOldestOther, QueueWaitOldestDraw, QueueWaitOldestCopy, QueueWaitOldestFill,
    QueueWaitOldestDisplay, QueueWaitOldestSample, QueueWaitOldestPresent,
    TextureCopyCommands, TextureCopyPayloadBytes, TextureCopyBatchQueued, TextureCopyBatchCommits,
    TextureCopyBatchCopiesCommitted, TextureCopyBatchCommandBuffersSaved, TextureCopyBatchHazards,
    TextureCopyBatchFull, TextureCopyBatchAbandoned, IsolatedBlendDraws,
    BatchedVertexDispatches, BatchedVertexDraws, BatchedVertexCorners, BatchedUniqueVertexTransforms,
    FlushPublish, FlushFindOwners, FlushExportGuest, FlushWriteRam,
    FlushNativeBytes, FlushExportBytes, FlushRamReuseBytes, DisplayScaledCropRgb565,
    FlushReadbackHit, FlushReadbackFill, FlushReadbackBytes,
    DisplayPackedExport, DisplayRgbaExport, DisplayDepthExport, DisplayOtherExport,
    DisplayQueuedPackedExport, DisplayQueuedPackedBytes,
    RuntimeShaderTranslate, RuntimeVertexLibrary, RuntimeFragmentLibrary,
    LargeSnapshotHits, LargeSnapshotMisses, SnapshotNativeOwners, SnapshotOutsideLimits,
    SnapshotComparedBytes,
    KeyedSnapshotHits, KeyedSnapshotKeyMisses, KeyedSnapshotBytesChanged, KeyedSnapshotEvictions,
    FillBackingHits, FillBackingMisses,
    DirectQueuedExport, DirectQueuedExportBytes, ExportFallbackDepth, ExportFallbackColor,
    ExportFallbackRawBuffer, ExportFallbackOther, RuntimeMslDiskHits, IndexScanEightBit, IndexScanSixteenBit, IndexMemoHits, IndexMemoMisses, RuntimeMslLoadHost,
    DrawWritesElided, DrawGroupsAtCapacity, DrawPublishOther, DrawPublishFromDraw,
    DrawPublishFromCopy, DrawPublishFromFill, DrawPublishFromDisplay, DrawPublishFromSample,
    DrawPublishFromPresent, DrawPublishFromDrain, DrawPublishAuthority, DrawEncodeWhileQueueFull, DrawBoundaryCapacity, DrawBoundaryBacking, FillLeaseFullQueue, ExactWriteNodeReuse, TextureCopyStripedSourceReuse, TextureCopyStagingBytesAvoided,
    VertexCacheHash, VertexCacheHashBytes, VertexCacheCompare, VertexCacheCompareBytes,
    VertexCacheTransport, VertexCacheTransportBytes, VertexCacheEvictions, VertexCacheEvictedBytes,
    VertexArenaTransport, VertexArenaTransportBytes, VertexCacheRecentProbes, VertexCacheRecentHits,
    VertexTransformDedupCandidates, VertexTransformDedupHits, VertexTransformsAvoided,
    RuntimeSourceLogicalBytes, RuntimeSourceUniqueBytes, RuntimeSourceInternHits,
    SimdReferenceRequests, SimdReferenceCompiled, SimdReferenceRejected, SimdReferenceFallbacks, SimdReferenceDraws, DisplayCropRgb8Candidates, DisplayCrop565Candidates, DisplayCropOwnerHits, DisplayCropOwnerMisses, DisplayCropFused, DisplayCropSourceBytesAvoided, DisplayRgb8RetileCandidates, DisplayRgb8RetileOwnerHits, DisplayLinearBufferOnly, DisplayLinearTextureBytesAvoided, LinearTextureAlignmentQueries, LinearTextureAlignmentHits, ShaderIdentityFullChecks, ShaderIdentityComparedBytes, ShaderIdentityFastHits, ShaderIdentityValidationFailures, AsyncDepthImports, AsyncDepthChecksPassed, AsyncDepthChecksFailed, QueuedPendingDepthExports,
    PicaListToFirstShaderTranslation, PicaFirstShaderWithin100us, PicaFirstShaderWithin1ms,
    PicaFirstShaderWithin5ms, PicaFirstShaderAfter5ms,
    RuntimeVertexSpirvMslHit, GeneralFragmentDraws, VertexUploadToDraw, VertexAnyUploadToDraw, VertexUploadUnobserved, VertexFallbackDraws, RuntimeLinkedVertexHit, UniformPreviousVsHit, PhaseCount
};
inline constexpr std::array<const char*, PhaseCount> names{
    "color_acquire", "depth_acquire", "lut_refresh", "record_total", "encode_total",
    "draw_upload", "encoder_open", "encoder_starts", "encode_state_and_draw",
    "record_retire", "record_append", "record_invalidate", "record_write_notice", "page_range_hits",
    "submission_pressure_wait", "upload_arena_wait", "vertex_upload_hits", "vertex_upload_misses",
    "catalog_pair_hits", "fragment_source_hits",
    "retired_metal_command_gpu_span", "retired_metal_command_gpu_span_missing",
    "exact_depth_clear_hits", "material_recipe_hits", "pipeline_selection_hits", "untracked_snapshot_hits", "snapshot_hit_without_publish",
    "unrelated_invalidate_without_publish", "unrelated_flush_without_publish", "publish_pending", "draw_group_submits", "published_draws",
    "vertex_guest_read", "native_present", "native_transfer", "guest_flush",
    "fill_host", "texture_copy_host", "display_transfer_host", "retile_host", "raw_detile_host", "native_color_transfer_host", "generic_display_host", "publish_bytes_host", "resolve_bytes_host", "export_span_host", "draw_register_classification", "draw_sampler_planning", "draw_uniform_snapshot", "draw_write_footprint", "queued_color_export_host",
    "copy_publish_pending", "copy_reserve", "copy_plan", "copy_allocate", "copy_resolve",
    "copy_encode", "copy_publish", "copy_dense",
    "queue_reserve_full", "queue_track_full", "queue_drain",
    "snapshot_reserve_avoided", "depth_clear_fused", "direct_tiled_color_export", "queued_mip_build", "late_transfer_reserve",
    "render_pipeline_create", "render_pipeline_create_validated", "render_pipeline_reuse",
    "render_pipeline_archive_open", "render_pipeline_archive_open_success",
    "render_pipeline_archive_record", "render_pipeline_archive_record_success",
    "render_pipeline_archive_hit", "render_pipeline_archive_miss",
    "render_pipeline_archive_serialize", "render_pipeline_archive_serialize_success",
    "uniform_common_upload_requests", "uniform_common_upload_hits",
    "uniform_common_requested_bytes", "uniform_common_copied_bytes",
    "uniform_light_upload_requests", "uniform_light_upload_hits",
    "uniform_light_requested_bytes", "uniform_light_copied_bytes",
    "uniform_vs_upload_requests", "uniform_vs_upload_hits",
    "uniform_vs_requested_bytes", "uniform_vs_copied_bytes",
    "queue_wait_site_other", "queue_wait_site_draw", "queue_wait_site_copy", "queue_wait_site_fill",
    "queue_wait_site_display", "queue_wait_site_sample", "queue_wait_site_present", "queue_wait_site_drain",
    "queue_wait_oldest_other", "queue_wait_oldest_draw", "queue_wait_oldest_copy", "queue_wait_oldest_fill",
    "queue_wait_oldest_display", "queue_wait_oldest_sample", "queue_wait_oldest_present",
    "texture_copy_commands", "texture_copy_payload_bytes", "texture_copy_batch_queued", "texture_copy_batch_commits",
    "texture_copy_batch_copies_committed", "texture_copy_batch_command_buffers_saved", "texture_copy_batch_hazards",
    "texture_copy_batch_full", "texture_copy_batch_abandoned", "isolated_blend_draws",
    "batched_vertex_dispatches", "batched_vertex_draws", "batched_vertex_corners",
    "batched_unique_vertex_transforms",
    "flush_publish", "flush_find_owners", "flush_export_guest", "flush_write_ram",
    "flush_native_bytes", "flush_export_bytes", "flush_ram_reuse_bytes", "display_scaled_crop_rgb565",
    "flush_readback_hit", "flush_readback_fill", "flush_readback_bytes",
    "display_packed_export", "display_rgba_export", "display_depth_export", "display_other_export",
    "display_queued_packed_export", "display_queued_packed_bytes",
    "runtime_shader_translate", "runtime_vertex_library", "runtime_fragment_library",
    "large_snapshot_hits", "large_snapshot_misses", "snapshot_native_owners",
    "snapshot_outside_limits", "snapshot_compared_bytes",
    "keyed_snapshot_hits", "keyed_snapshot_key_misses", "keyed_snapshot_bytes_changed",
    "keyed_snapshot_evictions", "fill_backing_hits", "fill_backing_misses",
    "direct_queued_export", "direct_queued_export_bytes", "export_fallback_depth",
    "export_fallback_color", "export_fallback_raw_buffer", "export_fallback_other", "runtime_msl_disk_hits", "index_scan_eight_bit", "index_scan_sixteen_bit", "index_memo_hits", "index_memo_misses", "runtime_msl_load_host",
    "draw_writes_elided", "draw_groups_at_capacity", "draw_publish_other", "draw_publish_from_draw",
    "draw_publish_from_copy", "draw_publish_from_fill", "draw_publish_from_display",
    "draw_publish_from_sample", "draw_publish_from_present", "draw_publish_from_drain",
    "draw_publish_authority", "draw_encode_while_queue_full",
    "draw_boundary_capacity", "draw_boundary_backing", "fill_lease_full_queue", "exact_write_node_reuse", "texture_copy_striped_source_reuse", "texture_copy_staging_bytes_avoided",
    "vertex_cache_hash", "vertex_cache_hash_bytes", "vertex_cache_compare", "vertex_cache_compare_bytes",
    "vertex_cache_transport", "vertex_cache_transport_bytes", "vertex_cache_evictions", "vertex_cache_evicted_bytes",
    "vertex_arena_transport", "vertex_arena_transport_bytes", "vertex_cache_recent_probes", "vertex_cache_recent_hits",
    "vertex_transform_dedup_candidates", "vertex_transform_dedup_hits", "vertex_transforms_avoided",
    "runtime_source_logical_bytes", "runtime_source_unique_bytes", "runtime_source_intern_hits",
    "simd_reference_requests", "simd_reference_compiled", "simd_reference_rejected", "simd_reference_fallbacks", "simd_reference_draws", "display_crop_rgb8_candidates", "display_crop_rgb565_candidates", "display_crop_owner_hits", "display_crop_owner_misses", "display_crop_fused", "display_crop_source_bytes_avoided", "display_rgb8_retile_candidates", "display_rgb8_retile_owner_hits", "display_linear_buffer_only", "display_linear_texture_bytes_avoided", "linear_texture_alignment_queries", "linear_texture_alignment_hits", "shader_identity_full_checks", "shader_identity_compared_bytes", "shader_identity_fast_hits", "shader_identity_validation_failures", "async_depth_imports", "async_depth_checks_passed", "async_depth_checks_failed", "queued_pending_depth_exports",
    "pica_list_to_first_shader_translation", "pica_first_shader_within_100us",
    "pica_first_shader_within_1ms", "pica_first_shader_within_5ms", "pica_first_shader_after_5ms",
    "runtime_vertex_spirv_msl_hit", "general_fragment_draws",
    "vertex_upload_to_draw", "vertex_any_upload_to_draw", "vertex_upload_unobserved", "vertex_fallback_draws", "runtime_linked_vertex_hit",
    "uniform_previous_vs_hit"};
struct Span { std::uint64_t calls{}, ns{}; };
inline thread_local std::array<Span, PhaseCount> spans{};
inline bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_FRAME_COST");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool AvoidRedundantSetup() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_SETUP_REUSE");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool GroupDrawPasses() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_GROUPED_PASS");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool ReuseDrawInputs() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_DRAW_INPUT_REUSE");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool QueuePackedDisplayExports() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_QUEUE_PACKED_DISPLAY_EXPORTS");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool CoalesceFlushReadbacks() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_COALESCE_FLUSH_EXPORTS");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool ReuseMaterializedGuestFlush() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_REUSE_MATERIALIZED_FLUSH");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool DeferUnrelatedPublication() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_DEFER_UNRELATED_PUBLICATION");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool ReusePipelineSelection() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_REUSE_SELECTION");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool ClearUniformDepth() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_EXACT_DEPTH_CLEAR");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool QueueColorExports() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_QUEUE_COLOR_EXPORT");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline std::size_t DrawGroupLimit() {
    static const std::size_t limit = []() -> std::size_t {
        // Bounded experiments in tile-pass lifetime; explicit numeric selection
        // takes precedence over the existing 32/64 switch. Defaults stay intact.
        if (const auto* numeric = std::getenv("CARA_METAL_DRAW_GROUP_LIMIT")) {
            const std::string_view value(numeric);
            if (value == "32") return 32;
            if (value == "64") return 64;
            if (value == "128") return 128;
            if (value == "256") return 256;
        }
        const auto* value = std::getenv("CARA_METAL_DRAW_GROUP_64");
        return value && std::string_view(value) == "1" ? 64 : 32;
    }();
    return limit;
}
inline std::size_t DrawComputeGroupLimit() {
    // Wider raster groups must not expand the existing deferred output budget.
    const auto limit = DrawGroupLimit();
    return limit > 64 ? 64 : limit;
}
inline bool ReduceCopyQueueWork() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_REDUCE_COPY_QUEUE_WORK");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool FuseDepthClear() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_FUSE_DEPTH_CLEAR");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool DirectTiledColorExportEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("CARA_METAL_DIRECT_COLOR_EXPORT");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
inline bool OverlapTransfers() {
    static const bool enabled = [] {
        const auto* env = std::getenv("CARA_METAL_OVERLAP_TRANSFERS");
        return env && std::string_view(env) == "1";
    }();
    return enabled;
}
// The request is the operation trying to admit a command. The oldest
// producer is the tracked command whose completion the caller waited for.
enum class QueueWork : std::size_t { Other, Draw, Copy, Fill, Display, Sample, Present, Drain };
inline thread_local QueueWork queue_request = QueueWork::Other;
struct ScopeQueueWork {
    QueueWork previous = queue_request;
    explicit ScopeQueueWork(QueueWork work) {
        if (Enabled())
            queue_request = work;
    }
    ~ScopeQueueWork() { queue_request = previous; }
};
inline constexpr Phase WaitSite(QueueWork work) {
    return static_cast<Phase>(QueueWaitSiteOther + static_cast<std::size_t>(work));
}
inline constexpr Phase WaitOldest(QueueWork work) {
    if (work == QueueWork::Drain)
        work = QueueWork::Other;
    return static_cast<Phase>(QueueWaitOldestOther + static_cast<std::size_t>(work));
}
static_assert(WaitSite(QueueWork::Drain) == QueueWaitSiteDrain);
static_assert(WaitOldest(QueueWork::Present) == QueueWaitOldestPresent);

inline void Count(Phase phase, std::uint64_t count) {
    if (Enabled()) spans[phase].calls += count;
}
inline void RecordNs(Phase phase, std::uint64_t ns) {
    auto& span = spans[phase];
    ++span.calls;
    span.ns += ns;
}
struct Scope {
    using Clock = std::chrono::steady_clock;
    const Phase phase;
    Clock::time_point start;
    explicit Scope(Phase phase_) :
        phase(phase_), start(Enabled() ? Clock::now() : Clock::time_point{}) {}
    ~Scope() { Stop(); }
    void Stop() {
        if (start == Clock::time_point{}) return;
        auto& span = spans[phase];
        ++span.calls;
        span.ns += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
        start = {};
    }
};
// Only the first translation miss is timed. Later misses in the same list could
// include waits for the earlier compiler work and overstate available lead time.
inline thread_local Scope::Clock::time_point pica_list_started{};
struct PicaListScope {
    Scope::Clock::time_point previous{};
    PicaListScope() {
        if (Enabled()) {
            previous = pica_list_started;
            pica_list_started = Scope::Clock::now();
        }
    }
    ~PicaListScope() {
        if (Enabled())
            pica_list_started = previous;
    }
};
inline void RecordFirstShaderTranslation() {
    if (pica_list_started == Scope::Clock::time_point{})
        return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Scope::Clock::now() - pica_list_started).count();
    pica_list_started = {};
    RecordNs(PicaListToFirstShaderTranslation, elapsed);
    const Phase bucket = elapsed < 100'000 ? PicaFirstShaderWithin100us
        : elapsed < 1'000'000 ? PicaFirstShaderWithin1ms
        : elapsed < 5'000'000 ? PicaFirstShaderWithin5ms : PicaFirstShaderAfter5ms;
    Count(bucket, 1);
}
inline thread_local Scope::Clock::time_point last_vertex_upload{}, last_vertex_change{},
                                            native_draw_start{};
inline void RecordVertexUpload(bool changed) {
    if (Enabled()) {
        last_vertex_upload = Scope::Clock::now();
        if (changed)
            last_vertex_change = last_vertex_upload;
    }
}
struct NativeDrawScope {
    NativeDrawScope() {
        if (Enabled())
            native_draw_start = Scope::Clock::now();
    }
    ~NativeDrawScope() { native_draw_start = {}; }
};
inline void RecordColdVertexDraw() {
    if (native_draw_start == Scope::Clock::time_point{})
        return;
    if (last_vertex_upload == Scope::Clock::time_point{}) {
        Count(VertexUploadUnobserved, 1);
        return;
    }
    RecordNs(VertexAnyUploadToDraw, std::chrono::duration_cast<std::chrono::nanoseconds>(
        native_draw_start - last_vertex_upload).count());
    if (last_vertex_change != Scope::Clock::time_point{})
        RecordNs(VertexUploadToDraw, std::chrono::duration_cast<std::chrono::nanoseconds>(
            native_draw_start - last_vertex_change).count());
}
inline void Append(std::ostream& out) {
    if (!Enabled()) return;
    for (std::size_t i = 0; i < names.size(); ++i)
        out << names[i] << ',' << spans[i].calls << ',' << spans[i].ns << '\n';
}
} // namespace Cara::NativeMetalCost
