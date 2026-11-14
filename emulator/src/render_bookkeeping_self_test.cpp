// Cara original observable-equivalence oracle; SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_opengl/gl_reinterpret_lookup_cache.h"
#include "common/frame_timing.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#define REQUIRE(condition) do { if (!(condition)) { std::cerr << "Failed: " << #condition << " at " << __LINE__ << "\n"; std::abort(); } } while (false)
#include <memory>
#include <vector>
#include <iostream>
using Format=unsigned;
constexpr Format RGBA8=0,RGB8=1,RGB5A1=2,RGB565=3,RGBA4=4,D16=14,D24=16,D24S8=17;
constexpr std::array<Format,17> formats{RGBA8,RGB8,RGB5A1,RGB565,RGBA4,D16,D24,D24S8};
constexpr unsigned Bpp(Format f) { return f==RGBA8||f==D24S8?32:f==RGB8||f==D24?24:16; }
struct Params { Format format; unsigned address=0x12345,size=4096,stride=64; bool operator==(const Params&)const=default; };
struct Result { bool implemented=true; Params params; std::vector<Format> warnings; unsigned lookups=0,avoided=0,repeats=0,sibling_checks=0; };
Result Run(bool reuse,unsigned bpp,unsigned matches,Format initial,bool sibling_invalid) {
    Result r;r.params.format=initial;
    OpenGL::ReinterpretLookupCache<Format,std::shared_ptr<unsigned>,1> cache;
    bool seen=false;
    for(auto format:formats) if(Bpp(format)==bpp) {
        r.params.format=format;
        if(format==RGBA8) { if(seen)++r.repeats; seen=true; }
        const auto found=cache.Get(reuse,format,[&] {
            ++r.lookups;
            REQUIRE(r.params.address==0x12345 && r.params.size==4096 && r.params.stride==64);
            const auto it=std::find(formats.begin(),formats.begin()+8,format);
            const auto bit=std::size_t(it-formats.begin());
            return matches&(1u<<bit)?std::make_shared<unsigned>(format):std::shared_ptr<unsigned>{};
        });
        r.avoided+=found.reused;
        if(found.value) { REQUIRE(*found.value==format);r.warnings.push_back(format);r.implemented=false; }
    }
    // The real sibling call mutates params only when short-circuit permits it.
    if(r.implemented) { ++r.sibling_checks;r.params.format=255;(void)sibling_invalid; }
    return r;
}
int main() {
    unsigned fixtures=0;
    for(unsigned bpp:{4u,8u,16u,24u,32u}) for(unsigned matches=0;matches<256;++matches)
      for(Format initial:{RGBA8,D24S8,255u}) for(bool invalid:{false,true}) {
        const auto baseline=Run(false,bpp,matches,initial,invalid);
        const auto reused=Run(true,bpp,matches,initial,invalid);
        REQUIRE(baseline.implemented==reused.implemented && baseline.params==reused.params);
        REQUIRE(baseline.warnings==reused.warnings && baseline.sibling_checks==reused.sibling_checks);
        REQUIRE(baseline.repeats==reused.repeats && baseline.avoided==0);
        if(bpp==32) {
            REQUIRE(baseline.lookups==11 && reused.lookups==2 && reused.avoided==9 && reused.repeats==9);
            if(!reused.implemented) REQUIRE(reused.params.format==RGBA8 && reused.sibling_checks==0);
        } else REQUIRE(baseline.lookups==reused.lookups && reused.avoided==0);
        ++fixtures;
      }
    using namespace Common::FrameTiming;
    counters={};counters.enabled=true;
    { Scope s(Phase::DrawFramebufferAcquire,false); }
    REQUIRE(counters.draw_framebuffer_acquire_calls==0);
    for(auto phase:{Phase::DrawFramebufferAcquire,Phase::DrawTextureAcquire,Phase::DrawShaderSetup,
                    Phase::DrawDirtyInvalidate,Phase::ReinterpretCheck}) { Scope s(phase,true); }
    REQUIRE(counters.draw_framebuffer_acquire_calls==1 && counters.draw_texture_acquire_calls==1 &&
           counters.draw_shader_setup_calls==1 && counters.draw_dirty_invalidate_calls==1 &&
           counters.render_reinterpret_check_calls==1);
    counters.enabled=false;{ Scope s(Phase::ReinterpretCheck,true); }
    REQUIRE(counters.render_reinterpret_check_calls==1);
    std::cout<<fixtures<<" mutable-params/return/warning/null-cache/sibling-short-circuit equivalence fixtures and narrow phase gating passed.\n";
}
