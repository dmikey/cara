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
    auto too_large = expected;
    too_large[0].binary.resize(MaxBinary + 1);
    assert(Encode(identity, too_large).empty());
    too_large = expected;
    too_large[0].vertex.resize(MaxSource + 1, 'v');
    assert(Encode(identity, too_large).empty());
    assert(decoded.size() == 2); // Invalid files never partially replace accepted records.
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
