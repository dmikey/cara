// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include "video_core/renderer_opengl/gl_validation_copy_profile.h"

int main() {
    using namespace OpenGL::ValidationCopyProfile;
    static_assert(!InWindow(299, 300, 1200));
    static_assert(InWindow(300, 300, 1200));
    static_assert(InWindow(1199, 300, 1200));
    static_assert(!InWindow(1200, 300, 1200));
    static_assert(!InWindow(300, 1200, 300));
    static_assert(!InWindow(300, 300, 300));
    static_assert(InWindow(UINT32_MAX - 1, 0, UINT32_MAX));
    static_assert(!InWindow(UINT32_MAX, 0, UINT32_MAX));
    static_assert(Delta(true, 10, true, 20).ns == 10);
    static_assert(Delta(true, 10, true, 10).valid);
    static_assert(!Delta(true, 20, true, 10).valid);
    static_assert(!Delta(false, 10, true, 20).valid);
    static_assert(!Delta(true, 10, false, 20).valid);
    SlotBudget<512> slots;
    SlotBudget<0> empty;
    assert(!empty.Take() && empty.used == 0);
    Totals all, recorded, overflow;
    // Detail-cap exhaustion cannot truncate exact totals. Exercise the same
    // disjoint totals accounting used by the production recorder.
    for (unsigned n = 0; n < 1000; ++n) {
        const Elapsed wall{true, n}, cpu{n % 2 == 0, n / 2};
        all.Add(4096, wall, cpu);
        (slots.Take() ? recorded : overflow).Add(4096, wall, cpu);
    }
    assert(slots.used == 512 && !slots.Take());
    assert(recorded.calls == 512 && overflow.calls == 488);
    assert(all.calls == 1000 && all.guest_bytes == 4096000);
    assert(all.wall_samples == 1000 && all.cpu_samples == 500 && all.cpu_failures == 500);
    assert(all.calls == recorded.calls + overflow.calls);
    assert(all.guest_bytes == recorded.guest_bytes + overflow.guest_bytes);
    assert(all.wall_ns == recorded.wall_ns + overflow.wall_ns);
    assert(all.thread_cpu_ns == recorded.thread_cpu_ns + overflow.thread_cpu_ns);
    Totals failed;
    failed.Add(1, Delta(true, 20, true, 10), Delta(false, 0, false, 0));
    assert(failed.wall_failures == 1 && failed.cpu_failures == 1);
    assert(failed.wall_ns == 0 && failed.thread_cpu_ns == 0);
    Totals saturated;
    saturated.guest_bytes = UINT64_MAX - 1;
    saturated.Add(2, {}, {});
    assert(saturated.guest_bytes == UINT64_MAX && saturated.saturations == 1);
    saturated.Add(0, {}, {});
    assert(saturated.guest_bytes == UINT64_MAX && saturated.saturations == 1);
    saturated.saturations = UINT64_MAX;
    saturated.Add(1, {}, {});
    assert(saturated.saturations == UINT64_MAX);
    std::cout << "validation copy window/clock/overflow totals PASS\n";
}
