// Cara exact-selection admission oracle; SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_texture_lookup_key.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#define REQUIRE(condition) do { if (!(condition)) { std::cerr << "Failed: " << #condition << " at " << __LINE__ << "\n"; std::abort(); } } while (false)
#include <iostream>
struct Params {unsigned addr=1,end=2,size=3,width=4,height=5,stride=6,res_scale=1;bool is_tiled=true;unsigned pixel_format=17,type=2;};
struct Candidate {unsigned scale;bool valid;};
const Candidate* OriginalSelect(const std::array<Candidate,3>& candidates,const std::array<unsigned,3>& order) {
 const Candidate* best=nullptr;
 for(auto i:order) {
  const auto& c=candidates[i];
  if(!best || c.scale>best->scale || (c.scale==best->scale&&c.valid&&!best->valid)) best=&c;
 }
 return best;
}
int main() {
 Params a,b=a;REQUIRE(OpenGL::SameTextureLookupParams(a,b));
#define CHANGE(field,value) b=a;b.field=value;REQUIRE(!OpenGL::SameTextureLookupParams(a,b))
 CHANGE(addr,99);CHANGE(end,99);CHANGE(size,99);CHANGE(width,99);CHANGE(height,99);CHANGE(stride,99);
 CHANGE(res_scale,99);CHANGE(is_tiled,false);CHANGE(pixel_format,99);CHANGE(type,99);
#undef CHANGE
 unsigned admission_cases=0,ambiguities=0,reties=0;
 for(unsigned x=1;x<=3;++x)for(unsigned y=1;y<=3;++y)for(unsigned z=1;z<=3;++z)
  for(unsigned validity=0;validity<8;++validity) {
   std::array<unsigned,3> order{0,1,2};
   do {
    std::array<Candidate,3> candidates{{{x,bool(validity&1)},{y,bool(validity&2)},{z,bool(validity&4)}}};
    OpenGL::ExactLookupSelection<const Candidate*> proof;
    for(auto i:order) { proof.Observe(&candidates[i],candidates[i].scale);proof.Observe(&candidates[i],candidates[i].scale); }
    const auto* selected=OriginalSelect(candidates,order);
    if(proof.IsUnique(selected)) {
     const auto selected_index=std::size_t(selected-candidates.data());
     for(unsigned changed=0;changed<8;++changed) {
      auto after=candidates;
      for(unsigned i=0;i<3;++i)after[i].valid=bool(changed&(1u<<i));
      after[selected_index].valid=true; // Required full-region-valid hit guard.
      REQUIRE(OriginalSelect(after,order)==&after[selected_index]);++admission_cases;
     }
    } else { REQUIRE(proof.ambiguous);++ambiguities; }
   }while(std::next_permutation(order.begin(),order.end()));
  }
 // Explicit equal-scale invalid-earlier/valid-later changes selection without
 // registration. Such a query must never be admitted, even if bytes coincide.
 std::array<Candidate,3> ties{{{2,false},{2,true},{1,true}}};
 const std::array<unsigned,3> order{0,1,2};REQUIRE(OriginalSelect(ties,order)==&ties[1]);
 OpenGL::ExactLookupSelection<const Candidate*> proof;
 for(auto i:order)proof.Observe(&ties[i],ties[i].scale);
 REQUIRE(!proof.IsUnique(&ties[1]));ties[0].valid=true;
 REQUIRE(OriginalSelect(ties,order)==&ties[0]);++reties;
 std::cout<<admission_cases<<" unique-highest validity-change cases, "<<ambiguities
          <<" ambiguous rejections, "<<reties<<" tie counterexample and10 fieldwise key mutations passed.\n";
}
