// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>

// Intentionally isolated test translation unit: never link this target against
// the core library or compile arm_tick_counts.cpp separately into this target.
// Including the implementation exposes its private canonical matcher tables
// and bucket helper without adding a production testing API.
#include "core/arm/fastinterp/arm_tick_counts.cpp"

namespace {

u64 LinearOracle(bool thumb, u32 instruction) {
    const auto scan = [instruction](const auto& table) -> u64 {
        for (const auto& matcher : table) {
            if ((instruction & matcher.mask) == matcher.expect)
                return matcher.fn(instruction);
        }
        return 1;
    };
    if (!thumb)
        return scan(arm_matchers);
    return instruction >> 16 ? scan(thumb32_matchers) : scan(thumb16_matchers);
}

[[noreturn]] void Fail(const char* reason, u32 instruction = 0) {
    std::cerr << "ARM tick oracle failure: " << reason << " instruction=0x" << std::hex
              << instruction << '\n';
    std::exit(1);
}

} // namespace

int main() {
    const ArmTickBuckets buckets;
    std::mt19937 random(0x736163);
    u64 checked = 0;
    u64 trace = 14695981039346656037ULL;
    const auto mix = [&trace](u64 value) {
        trace ^= value;
        trace *= 1099511628211ULL;
    };

    // Exhaustively prove candidate membership and original ordering for every
    // prefix/table pair. Any full-mask match necessarily passes this prefix
    // restriction, so filtering cannot change first-match priority for any word.
    for (u32 prefix = 0; prefix < 256; ++prefix) {
        size_t next = buckets.offsets[prefix];
        for (size_t index = 0; index < arm_matchers.size(); ++index) {
            const auto& matcher = arm_matchers[index];
            const bool possible = ((prefix << 20) & matcher.mask & 0x0FF00000) ==
                                  (matcher.expect & 0x0FF00000);
            const bool present = next < buckets.offsets[prefix + 1] &&
                                 buckets.indices[next] == index;
            if (possible != present)
                Fail("candidate membership/order", prefix << 20);
            next += present;
        }
        if (next != buckets.offsets[prefix + 1])
            Fail("extra bucket candidates", prefix << 20);
    }

    const auto check = [&](bool thumb, u32 instruction) {
        const u64 expected = LinearOracle(thumb, instruction);
        if (Core::TicksForInstruction(thumb, instruction) != expected)
            Fail("public tick API differs from ordered linear oracle", instruction);
        if (!thumb && BucketedArmTicks(instruction) != expected)
            Fail("bucket helper differs from ordered linear oracle", instruction);
        mix(instruction);
        mix(thumb);
        mix(expected);
        ++checked;
    };

    // Every Thumb16 word; Thumb32/random cases below exercise the unchanged
    // original path under both external CARA_FASTINTERP_TICK_BUCKETS=0/1 runs.
    for (u32 instruction = 0; instruction < 65536; ++instruction)
        check(true, instruction);

    for (const auto& matcher : arm_matchers) {
        check(false, matcher.expect);
        check(false, matcher.expect | ~matcher.mask);
        // Fixed-bit violations and operand extremes include PC, shift amounts,
        // register lists and all condition codes, including overlapping matches.
        for (unsigned bit = 0; bit < 32; ++bit) {
            check(false, matcher.expect ^ (1U << bit));
            check(false, (matcher.expect | ~matcher.mask) ^ (1U << bit));
        }
        for (unsigned index = 0; index < 10000; ++index)
            check(false, matcher.expect | (random() & ~matcher.mask));
    }

    for (u32 prefix : {0U, 1U, 0x12U, 0x30U, 0x35U, 0x49U, 0x59U, 0x71U,
                      0x81U, 0x91U, 0xA0U, 0xB0U, 0xC4U, 0xE0U, 0xEFU, 0xFFU}) {
        for (u32 low = 0; low < 65536; ++low)
            check(false, 0xE0000000U | (prefix << 20) | low);
    }
    for (unsigned index = 0; index < 1000000; ++index) {
        const u32 instruction = random();
        check(false, instruction);
        if (index < 100000)
            check(true, instruction);
    }

    const char* mode = std::getenv("CARA_FASTINTERP_TICK_BUCKETS");
    std::cout << "ARM tick oracle passed: cases=" << checked << " trace=" << trace
              << " tick_buckets=" << (mode && std::strcmp(mode, "1") == 0 ? 1 : 0)
              << " matchers=" << arm_matchers.size()
              << " bucket_indices=" << buckets.indices.size() << '\n';
}
