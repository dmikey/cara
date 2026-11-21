// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 Cara contributors.
#pragma once
#if defined(__APPLE__)
#include <cstdlib>
#include <string_view>
#include "common/apple_hardware.h"

namespace Common {
// Shared production renderer policy for the Apple Metal frontends. Invoke
// before initializing the renderer, only when Metal is selected. Explicit
// environment values, including "0" and the empty string, always take priority.
// Wider upload paging preserves the existing iPad policy and is shared on
// devices with at least 6GiB; explicit overrides always remain authoritative.
inline constexpr const char* AppleMetalDefaultOptions[]{
        "CARA_METAL_REDUCE_COPY_QUEUE_WORK",
        "CARA_METAL_FUSE_DEPTH_CLEAR",
        "CARA_METAL_DIRECT_COLOR_EXPORT",
        "CARA_METAL_SETUP_REUSE",
        "CARA_METAL_TEXTURE_RESIDENCY",
        "CARA_METAL_GROUPED_PASS",
        "CARA_METAL_DRAW_INPUT_REUSE",
        "CARA_METAL_DEFER_UNRELATED_PUBLICATION",
        "CARA_METAL_REUSE_SELECTION",
        "CARA_METAL_EXACT_DEPTH_CLEAR",
        "CARA_METAL_QUEUE_COLOR_EXPORT",
        "CARA_METAL_OVERLAP_TRANSFERS",
        "CARA_METAL_RAW_INDICES",
        "CARA_METAL_RAW_INDEX_CONSTANTS",
        "CARA_METAL_REUSE_RENDER_PIPELINES",
        "CARA_METAL_CACHE_KEYED_TEXTURE_SNAPSHOTS",
        "CARA_METAL_REUSE_SHADER_IDENTITY",
        "CARA_METAL_CACHE_LINEAR_TEXTURE_ALIGNMENT",
        // Exact-byte index metadata is bounded to 5MiB; watch retirement
        // preserves whole-page ownership and adjacent-page dependencies.
        "CARA_METAL_INDEX_SCAN_MEMO",
        "CARA_METAL_INDEX_SCAN_SPECIALIZE",
        "CARA_METAL_INDEX_SCAN_ASSOCIATIVE",
        "CARA_METAL_RELEASE_INVALIDATED_FCRAM_PAGES",
        "CARA_METAL_RELEASE_OWNERLESS_FCRAM_PAGES",
        "CARA_METAL_CACHE_COMPLETED_PREDECESSORS",
        "CARA_METAL_DRAW_GROUP_64",
        "CARA_METAL_REUSE_OWNERSHIP_PUBLICATIONS",
        "CARA_METAL_REUSE_COMPLETED_FILLS",
        "CARA_METAL_BATCH_TEXTURE_COPIES",
        "CARA_METAL_FILL_LEASE_NO_RESERVE",
        "CARA_METAL_COLOR_LEASE_NO_RESERVE",
        "CARA_METAL_LATE_DRAW_RESERVE",
        "CARA_METAL_REUSE_VERTEX_READ_PLAN",
        "CARA_METAL_REUSE_VERTEX_STAGING",
        "CARA_METAL_REUSE_VERTEX_SOURCE_LOOKUP",
        "CARA_METAL_CACHE_SHADER_POLICY",
        "CARA_METAL_VERTEX_TEXTURE_RECENT_LOOKUP",
        "CARA_METAL_JOIN_CHECKED_DEPTH_IMPORT",
        "CARA_METAL_FUSE_CHECKED_DEPTH_IMPORT",
        "CARA_METAL_FUSE_JOINED_DEPTH_EXPORT",
        "CARA_METAL_ASYNC_CHECKED_DEPTH_IMPORT",
        "CARA_METAL_BORROW_RAM_SNAPSHOT_COMPARE",
        "CARA_METAL_VERTEX_TEXTURE_ORDINAL_LOOKUP",
};
// Start with the device model on which the wider queue and queued depth
// were measured together. Both modes remain available through overrides.
inline bool AppleMetalWideSubmissionDevice() {
    char model[32]{};
    std::size_t size = sizeof(model);
    constexpr std::string_view verified_model = "iPad16,3";
    return sysctlbyname("hw.machine", model, &size, nullptr, 0) == 0 &&
           size == verified_model.size() + 1 &&
           std::string_view(model, size - 1) == verified_model;
}

inline bool ApplyAppleMetalDefaults(bool wider_upload_paging = false,
                                    std::uint64_t memory_bytes = ApplePhysicalMemoryBytes()) {
    for (const auto* option : AppleMetalDefaultOptions)
        if (setenv(option, "1", 0) != 0)
            return false;
    // The additional large-source cache is bounded separately. Retain the
    // original exact-byte fallback on lower-memory devices and honor overrides.
    if (memory_bytes >= 6ULL * 1024 * 1024 * 1024 &&
        setenv("CARA_METAL_CACHE_LARGE_TEXTURE_SNAPSHOTS", "1", 0) != 0)
        return false;
    if (AppleMetalWideSubmissionDevice() &&
        (setenv("CARA_METAL_SUBMISSION_WINDOW", "32", 0) != 0 ||
         setenv("CARA_METAL_QUEUE_PENDING_DEPTH_EXPORT", "1", 0) != 0))
        return false;
    const bool wider = wider_upload_paging || memory_bytes >= 6ULL * 1024 * 1024 * 1024;
    return !wider || setenv("CARA_METAL_EIGHT_UPLOAD_PAGES", "1", 0) == 0;
}

// The same learned pipeline path serves both frontends. Quiet comparisons stay
// cold unless explicitly selected; ordinary play learns and reuses local work.
inline bool AppleMetalPipelineLearning(bool benchmark = false) {
    if (const auto* value = std::getenv("CARA_METAL_PIPELINE_LEARNING"))
        return std::string_view(value) == "1";
    if (const auto* value = std::getenv("CARA_MAC_METAL_LEARN"))
        return std::string_view(value) == "1";
    return !benchmark;
}
} // namespace Common
#endif
