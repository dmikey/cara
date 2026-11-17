// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_host_scissor.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
using OpenGL::HostScissor::ApplyInclude;
using OpenGL::HostScissor::State;
using OpenGL::ScissorDamage::Rect;
static bool Same(State a, State b) {
  return a.enabled == b.enabled && a.x == b.x && a.y == b.y &&
         a.width == b.width && a.height == b.height;
}
int main() {
  std::uint64_t checks = 0;
  for (unsigned scale : {1u, 2u, 3u, 4u})
    for (int ox : {0, 3, 11})
      for (int oy : {0, 5, 13}) {
        const int s = scale;
        const State original{true, ox * s, oy * s, 9 * s, 7 * s};
        for (int x1 : {-3, 0, 1, 8, 10})
          for (int x2 : {-2, 0, 1, 5, 9, 12})
            for (int y1 : {-3, 0, 3, 6, 8})
              for (int y2 : {-2, 0, 1, 4, 7, 10}) {
                const Rect bounds{(ox + x1) * s, (oy + y1) * s, (ox + x2) * s,
                                  (oy + y2) * s};
                auto candidate = original;
                assert(ApplyInclude(candidate, bounds, 3, scale, false));
                for (int y = original.y; y < original.y + original.height; ++y)
                  for (int x = original.x; x < original.x + original.width;
                       ++x) {
                    const float fx = (float(x) + 0.5f) / s - ox,
                                fy = (float(y) + 0.5f) / s - oy;
                    const bool expected =
                        fx >= x1 && fx < x2 && fy >= y1 && fy < y2;
                    const bool actual =
                        x >= candidate.x && x < candidate.x + candidate.width &&
                        y >= candidate.y && y < candidate.y + candidate.height;
                    assert(actual == expected);
                    ++checks;
                  }
                for (unsigned mode : {0u, 1u, 2u, 4u, UINT32_MAX}) {
                  candidate = original;
                  assert(!ApplyInclude(candidate, bounds, mode, scale, false) &&
                         Same(candidate, original));
                }
                candidate = original;
                assert(!ApplyInclude(candidate, bounds, 3, scale, true) &&
                       Same(candidate, original));
                // Damage narrowing still rejects potential stencil writes; host
                // clipping does not.
                auto damage = OpenGL::ScissorDamage::NarrowInclude(
                    {original.x, original.y, original.x + original.width,
                     original.y + original.height},
                    bounds, 3, scale, false, true);
                assert(!damage.eligible);
              }
      }
  for (State original : {State{false, 0, 0, 16, 16}, State{true, -1, 0, 16, 16},
                         State{true, 0, 0, -1, 16}, State{true, 0, 0, 16, -1},
                         State{true, INT32_MAX, 0, INT32_MAX, 16},
                         State{true, 0, 0, (1 << 23) + 1, 16}}) {
    auto candidate = original;
    assert(!ApplyInclude(candidate, {0, 0, 16, 16}, 3, 1, false) &&
           Same(candidate, original));
  }
  const State original{true, 0, 0, 16, 16};
  for (Rect bounds : {Rect{INT64_MIN, 0, INT64_MAX, 16}, Rect{1, 0, 16, 16}}) {
    auto candidate = original;
    assert(!ApplyInclude(candidate, bounds, 3, 2, false) &&
           Same(candidate, original));
  }
  auto candidate = original;
  assert(!ApplyInclude(candidate, {0, 0, 16, 16}, 3, 0, false) &&
         Same(candidate, original));
  candidate = {true, (1 << 23) - 1, 0, 1, 1};
  assert(ApplyInclude(candidate, {(1 << 23) - 1, 0, 1 << 23, 1}, 3, 1, false));
  std::cout << "Host INCLUDE checks=" << checks << " PASS\n";
}
