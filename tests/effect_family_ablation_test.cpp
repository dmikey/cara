#include "video_core/renderer_opengl/gl_effect_family_ablation.h"
#include <cassert>
#include <initializer_list>
#include <cstdint>
using namespace OpenGL::EffectFamilyAblation;
int main(int argc,char** argv) {
    const bool particle_mode=argc>1 && argv[1][0]=='1';
    const bool composite_mode=argc>2 && argv[2][0]=='1';
    assert(ParticleEnabled()==particle_mode && CompositeEnabled()==composite_mode);
    Snapshot p{2,3,32,0,88,0x11001,0x184abe00,123,456,13,2,3,0x180d4800,256,512,0xf51,0xe40100,0x76760000};
    Snapshot c{0,1,4,77,88,0x11007,0x18070800,0x18080800,0x18080800,0,0,0,0x180d4800,256,512,0xf10,0xe40100,0x01110000};
    assert(ParticleFamily(p) && !CompositePass(p));assert(CompositePass(c) && !ParticleFamily(c));
    auto particle_fields={&Snapshot::use_gs,&Snapshot::topology,&Snapshot::vs_entry,&Snapshot::gs_entry,&Snapshot::texture_config,&Snapshot::texture0,&Snapshot::format0,&Snapshot::color_address,&Snapshot::width,&Snapshot::height,&Snapshot::depth_color_mask,&Snapshot::fragment_operation,&Snapshot::alpha_blending};
    for(auto field:particle_fields){auto n=p;n.*field^=0x80000000u;assert(!ParticleFamily(n));}
    auto composite_fields={&Snapshot::use_gs,&Snapshot::topology,&Snapshot::num_vertices,&Snapshot::texture_config,&Snapshot::texture0,&Snapshot::texture1,&Snapshot::texture2,&Snapshot::format0,&Snapshot::format1,&Snapshot::format2,&Snapshot::color_address,&Snapshot::width,&Snapshot::height,&Snapshot::depth_color_mask,&Snapshot::fragment_operation,&Snapshot::alpha_blending};
    for(auto field:composite_fields){auto n=c;n.*field^=0x80000000u;assert(!CompositePass(n));}
    auto n=p;n.num_vertices=900;n.texture1=0;n.texture2=0;n.format1=15;n.format2=15;assert(ParticleFamily(n));
    n=p;n.depth_color_mask=0xf10;assert(ParticleFamily(n));
    n=c;n.vs_entry=0;n.gs_entry=1234;assert(CompositePass(n));
    assert(Suppress(p,30)==particle_mode);assert(Suppress(c,4)==composite_mode);
    assert(counts.particle_host_candidates==1 && counts.composite_host_candidates==1);
    assert(counts.particle_host_skips==unsigned(particle_mode) && counts.composite_host_skips==unsigned(composite_mode));
    assert(counts.particle_host_vertices_skipped==(particle_mode?30u:0u));
    assert(counts.composite_host_vertices_skipped==(composite_mode?4u:0u));
    std::uint64_t count=~std::uint64_t(0)-1;Add(count,5);assert(count==~std::uint64_t(0));Add(count);assert(count==~std::uint64_t(0));
}
