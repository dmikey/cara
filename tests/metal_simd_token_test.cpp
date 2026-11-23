// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_metal/pica_metal_simd_token.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
void Check(bool ok) {if(!ok)throw std::runtime_error("SIMD draw-token invariant failed");}
int main() {
 Cara::SimdDrawTokens exhausted(UINT64_MAX-1);Check(exhausted.Next()==UINT64_MAX-1);
 Check(!exhausted.Next() && !exhausted.Next());Cara::SimdDrawTokens zero(0);Check(!zero.Next());
 Cara::SimdDrawTokens tokens;std::array<std::vector<std::uint64_t>,4> values;
 std::array<std::thread,4> threads;
 for(unsigned i=0;i<threads.size();++i)threads[i]=std::thread([&,i]{for(unsigned n=0;n<1000;++n)values[i].push_back(*tokens.Next());});
 for(auto& thread:threads)thread.join();std::vector<std::uint64_t> all;
 for(const auto& group:values)all.insert(all.end(),group.begin(),group.end());std::sort(all.begin(),all.end());
 for(unsigned i=0;i<all.size();++i)Check(all[i]==i+1);
 std::cout<<"PASS:4000 cross-thread unique tokens; exhausted/zero counters fail closed without wrap\n";
}
