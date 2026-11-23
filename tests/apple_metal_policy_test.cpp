// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/apple_metal_policy.h"
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace {
constexpr const char* common[]{
    "CARA_METAL_REDUCE_COPY_QUEUE_WORK", "CARA_METAL_FUSE_DEPTH_CLEAR",
    "CARA_METAL_DIRECT_COLOR_EXPORT", "CARA_METAL_SETUP_REUSE",
    "CARA_METAL_TEXTURE_RESIDENCY", "CARA_METAL_GROUPED_PASS",
    "CARA_METAL_DRAW_INPUT_REUSE", "CARA_METAL_DEFER_UNRELATED_PUBLICATION",
    "CARA_METAL_REUSE_SELECTION", "CARA_METAL_EXACT_DEPTH_CLEAR",
    "CARA_METAL_QUEUE_COLOR_EXPORT", "CARA_METAL_OVERLAP_TRANSFERS",
    "CARA_METAL_RAW_INDICES", "CARA_METAL_RAW_INDEX_CONSTANTS",
    "CARA_METAL_REUSE_RENDER_PIPELINES",
    "CARA_METAL_CACHE_KEYED_TEXTURE_SNAPSHOTS",
    "CARA_METAL_REUSE_SHADER_IDENTITY",
    "CARA_METAL_CACHE_LINEAR_TEXTURE_ALIGNMENT",
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
constexpr const char* experiments[]{
    "CARA_METAL_PC230_SPECIALIZATION",
    "CARA_METAL_STAGEIN_RUNTIME_TEV",
    "CARA_METAL_REUSE_UNIFORM_UPLOADS", "CARA_METAL_UNIQUE_VERTEX_COMPUTE",
    "CARA_METAL_BATCH_VERTEX_COMPUTE",
    "CARA_METAL_UNTRACKED_UPLOAD_SOURCES",
    "CARA_METAL_DEFER_RASTER_FALLBACKS",
    "CARA_METAL_UNIQUE_TRANSFORM_ADMISSION",
    "CARA_METAL_HOST_LATENCY_CRITICAL",
    "CARA_METAL_CACHE_COMPLETED_WRITERS",
};
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool Is(const char* name, const char* expected) {
    const auto* value = std::getenv(name);
    return value && std::strcmp(value, expected) == 0;
}
void Clear() {
    for (const auto* name : common) unsetenv(name);
    for (const auto* name : experiments) unsetenv(name);
    unsetenv("CARA_METAL_EIGHT_UPLOAD_PAGES");
    unsetenv("CARA_METAL_CACHE_LARGE_TEXTURE_SNAPSHOTS");
}
void CheckCommon() {
    for (const auto* name : common) Require(Is(name, "1"), name);
    for (const auto* name : experiments) Require(!std::getenv(name), name);
}
}
int main() { try {
    constexpr std::uint64_t gib = 1024ULL * 1024 * 1024;
    for (const std::uint64_t memory_gib : {3, 4, 6, 8}) {
        for (const bool ipad : {false, true}) {
            Clear();
            Require(Common::ApplyAppleMetalDefaults(ipad, memory_gib * gib), "Policy failed");
            CheckCommon();
            Require((ipad || memory_gib >= 6) ? Is("CARA_METAL_EIGHT_UPLOAD_PAGES", "1")
                         : !std::getenv("CARA_METAL_EIGHT_UPLOAD_PAGES"), "Paging policy changed");
            Require(memory_gib >= 6 ? Is("CARA_METAL_CACHE_LARGE_TEXTURE_SNAPSHOTS", "1")
                                   : !std::getenv("CARA_METAL_CACHE_LARGE_TEXTURE_SNAPSHOTS"),
                    "Large snapshot memory threshold changed");
        }
    }
    // Every explicit common override survives, including presence-only empty values.
    for (const auto* value : {"0", "", "custom"}) {
        Clear();
        for (const auto* name : common) setenv(name, value, 1);
        setenv("CARA_METAL_EIGHT_UPLOAD_PAGES", value, 1);
        setenv("CARA_METAL_CACHE_LARGE_TEXTURE_SNAPSHOTS", value, 1);
        Require(Common::ApplyAppleMetalDefaults(false, 4 * gib), "Mac override policy failed");
        Require(Common::ApplyAppleMetalDefaults(true, 8 * gib), "iPad override policy failed");
        for (const auto* name : common) Require(Is(name, value), name);
        Require(Is("CARA_METAL_EIGHT_UPLOAD_PAGES", value), "Paging override changed");
        Require(Is("CARA_METAL_CACHE_LARGE_TEXTURE_SNAPSHOTS", value), "Large snapshot override changed");
        for (const auto* name : experiments) Require(!std::getenv(name), name);
    }
    Clear();
    setenv("CARA_METAL_DRAW_GROUP_64", "0", 1);
    Require(Common::ApplyAppleMetalDefaults(true, 6 * gib), "Explicit experiment policy failed");
    Require(Is("CARA_METAL_DRAW_GROUP_64", "0"), "Explicit experiment changed");
    unsetenv("CARA_METAL_PIPELINE_LEARNING");
    unsetenv("CARA_MAC_METAL_LEARN");
    Require(Common::AppleMetalPipelineLearning(), "Normal play must learn pipelines");
    Require(!Common::AppleMetalPipelineLearning(true), "Quiet benchmark must not learn implicitly");
    setenv("CARA_MAC_METAL_LEARN", "1", 1);
    Require(Common::AppleMetalPipelineLearning(true), "Legacy explicit learning lost");
    setenv("CARA_METAL_PIPELINE_LEARNING", "0", 1);
    Require(!Common::AppleMetalPipelineLearning(), "Shared off override lost");
    setenv("CARA_MAC_METAL_LEARN", "0", 1);
    setenv("CARA_METAL_PIPELINE_LEARNING", "1", 1);
    Require(Common::AppleMetalPipelineLearning(true), "Shared on override lost");
    std::cout << "Shared Apple Metal defaults and explicit overrides passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
