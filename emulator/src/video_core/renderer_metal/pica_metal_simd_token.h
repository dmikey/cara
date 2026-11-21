// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
namespace Cara {
class SimdDrawTokens {
public:
 explicit SimdDrawTokens(std::uint64_t initial=1) : next(initial) {}
 std::optional<std::uint64_t> Next() {
  auto value=next.load(std::memory_order_relaxed);
  while(value && value!=std::numeric_limits<std::uint64_t>::max())
   if(next.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value;
  return std::nullopt;
 }
private:
 std::atomic<std::uint64_t> next;
};
inline SimdDrawTokens simd_draw_tokens;
}
