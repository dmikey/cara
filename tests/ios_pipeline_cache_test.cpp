// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#include "../emulator/src/video_core/renderer_opengl/ios_pipeline_cache.h"
#include <cassert>
#include <iostream>
using namespace OpenGL::IOSPipelineCache;
int main() {
    const std::string identity = "generator/layout+Apple/M4+OS27.2+formats:1";
    std::vector<Entry> expected{{"vertex-source", "fragment-source", 1, {1, 2, 3}},
                                {"source-only-vs", "source-only-fs", 0, {}}};
    const auto bytes = Encode(identity, expected);
    std::vector<Entry> decoded;
    assert(Decode(bytes, identity, decoded));
    assert(decoded.size() == 2 && decoded[0].binary == expected[0].binary);
    assert(decoded[1].binary.empty());
    assert(!Decode(bytes, identity + "changed-layout", decoded));
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + length);
        assert(!Decode(truncated, identity, decoded));
    }
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        auto corrupt = bytes;
        corrupt[index] ^= 0x80;
        assert(!Decode(corrupt, identity, decoded));
    }
    auto malicious = bytes;
    for (std::size_t i = 8; i < 12; ++i) malicious[i] = 0xff;
    assert(!Decode(malicious, identity, decoded));
    auto too_many = std::vector<Entry>(MaxEntries + 1, expected[0]);
    assert(Encode(identity, too_many).empty());
    const auto extended = Encode(identity, too_many, ExtendedMaliLimits);
    assert(!extended.empty());
    std::vector<Entry> extended_decoded;
    assert(!Decode(extended, identity, extended_decoded));
    assert(Decode(extended, identity, extended_decoded, ExtendedMaliLimits));
    assert(extended_decoded.size() == MaxEntries + 1);
    assert(Encode(identity, expected, Limits{bytes.size() - 1, MaxEntries}).empty());
    assert(!Decode(bytes, identity, extended_decoded, Limits{bytes.size() - 1, MaxEntries}));
    auto too_large = expected;
    too_large[0].binary.resize(MaxBinary + 1);
    assert(Encode(identity, too_large).empty());
    too_large = expected;
    too_large[0].vertex.resize(MaxSource + 1, 'v');
    assert(Encode(identity, too_large).empty());
    assert(decoded.size() == 2); // Invalid files never partially replace accepted records.
    const auto stage_archive = ArchiveKind::VertexStages;
    const std::vector<Entry> vertices{{"vertex-source", {}, 1, {1, 2, 3}}};
    const auto stage_bytes = Encode(identity, vertices, DefaultLimits, stage_archive);
    assert(Decode(stage_bytes, identity, decoded, DefaultLimits, stage_archive));
    assert(decoded.size() == 1 && decoded[0].vertex == vertices[0].vertex &&
           decoded[0].binary == vertices[0].binary);
    assert(!Decode(stage_bytes, identity, decoded));
    assert(!Decode(bytes, identity, decoded, DefaultLimits, stage_archive));
    assert(!Decode(stage_bytes, identity + "new-driver", decoded, DefaultLimits, stage_archive));
    assert(Encode(identity, expected, DefaultLimits, stage_archive).empty());
    assert(Encode(identity, {{"vertex-source", {}, 1, {}}}, DefaultLimits, stage_archive).empty());
    assert(Encode(identity, {{"vertex-source", {}, 0, {1}}}, DefaultLimits, stage_archive).empty());
    auto corrupt_stage = stage_bytes;
    corrupt_stage[corrupt_stage.size() - 8] ^= 1;
    assert(!Decode(corrupt_stage, identity, decoded, DefaultLimits, stage_archive));
    assert(decoded.size() == 1 && decoded[0].binary == vertices[0].binary);
    const auto separable_archive = ArchiveKind::SeparableStages;
    const std::vector<Entry> stages{vertices[0], {{}, "fragment-source", 1, {4, 5}}};
    const auto separable_bytes = Encode(identity, stages, DefaultLimits, separable_archive);
    assert(Decode(separable_bytes, identity, decoded, DefaultLimits, separable_archive));
    assert(decoded.size() == 2 && decoded[0].vertex == "vertex-source" &&
           decoded[1].vertex.empty() && decoded[1].fragment == "fragment-source" &&
           decoded[1].binary == stages[1].binary);
    assert(!Decode(stage_bytes, identity, decoded, DefaultLimits, separable_archive));
    assert(!Decode(separable_bytes, identity, decoded, DefaultLimits, stage_archive));
    assert(Encode(identity, expected, DefaultLimits, separable_archive).empty());
    assert(Encode(identity, {{{}, {}, 1, {4}}}, DefaultLimits, separable_archive).empty());
    assert(Encode(identity, {{{}, "fragment-source", 1, {}}}, DefaultLimits, separable_archive).empty());
    assert(Key("a", "bc") != Key("ab", "c"));
    const auto vs_digest = Digest(expected[0].vertex.data(), expected[0].vertex.size());
    const auto fs_digest = Digest(expected[0].fragment.data(), expected[0].fragment.size());
    assert(KeyFromDigests(vs_digest, fs_digest) == Key(expected[0].vertex, expected[0].fragment));
    int vs_a = 0, vs_b = 0, fs_a = 0, fs_b = 0, factories = 0;
    StagePairMemo<std::uint64_t, 1> memo; // Force every distinct pair to collide.
    const auto get = [&](const void* vs, const void* fs, std::uint64_t value) {
        return memo.Get(vs, fs, [&] { ++factories; return value; });
    };
    for (int i = 0; i < 100000; ++i) assert(get(&vs_a, &fs_a, 17) == 17);
    assert(factories == 1); // Repeated draws never recompute source/pair identity.
    assert(get(&vs_b, &fs_a, 23) == 23 && factories == 2);
    assert(get(&vs_b, &fs_b, 29) == 29 && factories == 3);
    assert(get(&vs_a, &fs_a, 17) == 17 && factories == 4);
    CheckpointRetry retry;
    const auto start = CheckpointRetry::Clock::time_point{} + std::chrono::seconds(100);
    unsigned attempts = 0;
    for (unsigned second = 0; second < 10000; ++second) {
        const auto now = start + std::chrono::seconds(second);
        if (retry.CanAttempt(now)) { ++attempts; retry.Failed(now); }
        if (second < 30) assert(attempts == 1);
        else if (second < 60) assert(attempts == 2);
        else assert(attempts == 3);
    }
    retry.Succeeded();
    assert(retry.CanAttempt(start));
    std::cout << "PASS: source-only/binary roundtrip, all truncations and byte corruptions, "
                 "identity/length/count limits, transactional fallback, bounded hot-path memo, checkpoint retry budget\n";
}
