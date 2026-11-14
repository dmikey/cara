// Cara exact-selection admission oracle; SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_texture_lookup_key.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#define REQUIRE(condition) do { if (!(condition)) { std::cerr << "Failed: " << #condition << " at " << __LINE__ << "\n"; std::abort(); } } while (false)
#include <iostream>
struct Params {unsigned addr=1,end=2,size=3,width=4,height=5,stride=6,res_scale=1;bool is_tiled=true;unsigned pixel_format=17,type=2;};
struct Candidate {unsigned scale;bool valid;bool exact=true;};
// Simulate the same exact+invalid selection order: validity is inspected before
// exact layout on the reference path. Invalid candidates remain eligible;
// higher scale wins, then validity, then the original first-identity tie order.
const Candidate* SelectExact(const std::array<Candidate,3>& candidates,
                            const std::array<unsigned,3>& order, unsigned scale_mode,
                            bool prefilter, unsigned& validity_lookups) {
 const Candidate* best=nullptr;
 for(auto i:order) {
  const auto& c=candidates[i];
  if(prefilter&&!c.exact)continue;
  ++validity_lookups;
  const bool valid=c.valid;
  if(!c.exact)continue;
  if((scale_mode==1&&c.scale!=2)||(scale_mode==2&&c.scale<2))continue;
  if(!best||c.scale>best->scale||(c.scale==best->scale&&valid&&!best->valid))best=&c;
 }
 return best;
}
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
 unsigned prefilter_cases=0,avoided_validity=0;
 for(unsigned x=1;x<=3;++x)for(unsigned y=1;y<=3;++y)for(unsigned z=1;z<=3;++z)
  for(unsigned validity=0;validity<8;++validity) {
   std::array<unsigned,3> order{0,1,2};
   do {
    std::array<Candidate,3> candidates{{{x,bool(validity&1)},{y,bool(validity&2)},{z,bool(validity&4)}}};
    for(unsigned exact=0;exact<8;++exact)for(unsigned mode=0;mode<3;++mode) {
     auto layouts=candidates;
     for(unsigned i=0;i<3;++i)layouts[i].exact=bool(exact&(1u<<i));
     unsigned reference_reads=0,filtered_reads=0;
     REQUIRE(SelectExact(layouts,order,mode,false,reference_reads)==
             SelectExact(layouts,order,mode,true,filtered_reads));
     REQUIRE(filtered_reads<=reference_reads);
     avoided_validity+=reference_reads-filtered_reads;++prefilter_cases;
    }
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
          <<" ambiguous rejections, "<<reties<<" tie counterexample and10 fieldwise key mutations passed; "
          <<prefilter_cases<<" exact prefilter order/scale/validity cases, "<<avoided_validity
          <<" avoided validity lookups.\n";
}
