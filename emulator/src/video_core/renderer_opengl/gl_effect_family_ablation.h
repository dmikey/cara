// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
namespace OpenGL::EffectFamilyAblation {
// Narrow retained-checkpoint diagnostic signatures; no title detection.
// Indexed identity is intentionally omitted: final software draws lose it.
struct Snapshot {
    std::uint32_t use_gs, topology, num_vertices, vs_entry, gs_entry;
    std::uint32_t texture_config, texture0, texture1, texture2, format0, format1, format2;
    std::uint32_t color_address, width, height, depth_color_mask, fragment_operation, alpha_blending;
};
inline bool Common(const Snapshot& s) {
    return s.color_address==0x180d4800 && s.width==256 && s.height==512 &&
        ((s.depth_color_mask>>8)&15)==15 && !(s.depth_color_mask&0x1000) &&
        s.fragment_operation==0xe40100;
}
inline bool ParticleFamily(const Snapshot& s) {
    return Common(s) && s.use_gs==2 && s.topology==3 && s.vs_entry==0 && s.gs_entry==88 &&
        s.texture_config==0x11001 && s.texture0==0x184abe00 && s.format0==13 &&
        (s.depth_color_mask==0xf51 || s.depth_color_mask==0xf10) && s.alpha_blending==0x76760000;
}
inline bool CompositePass(const Snapshot& s) {
    return Common(s) && s.use_gs==0 && s.topology==1 && s.num_vertices==4 &&
        s.depth_color_mask==0xf10 && s.alpha_blending==0x01110000 && s.texture_config==0x11007 &&
        s.texture0==0x18070800 && s.texture1==0x18080800 && s.texture2==0x18080800 &&
        s.format0==0 && s.format1==0 && s.format2==0;
}
// Additive regular-geometry candidate. No artistic identity/coverage inferred.
// num_vertices and inactive texture2/GS entry are deliberately not identity.
inline bool RayCandidate(const Snapshot& s) {
    return Common(s) && s.use_gs==0 && s.topology==1 && s.vs_entry==0 &&
        s.texture_config==0x13203 && s.texture0==0x18160400 && s.format0==0 &&
        s.texture1==0x22e00400 && s.format1==13 && s.depth_color_mask==0xf51 &&
        s.alpha_blending==0x16160000;
}
inline bool RayEnabled() {
    static const bool value=[] {const char* p=std::getenv("CARA_GPU_ABLATE_RAY_CANDIDATE");return p && std::strcmp(p,"1")==0;}();
    return value;
}
inline bool ParticleEnabled() {
    static const bool value=[] {const char* p=std::getenv("CARA_GPU_ABLATE_PARTICLE_FAMILY");return p && std::strcmp(p,"1")==0;}();
    return value;
}
inline bool CompositeEnabled() {
    static const bool value=[] {const char* p=std::getenv("CARA_GPU_ABLATE_COMPOSITE_PASS");return p && std::strcmp(p,"1")==0;}();
    return value;
}
struct Counts {
    std::uint64_t particle_host_candidates=0, composite_host_candidates=0, ray_host_candidates=0;
    std::uint64_t particle_host_skips=0, composite_host_skips=0, ray_host_skips=0;
    std::uint64_t particle_host_vertices_skipped=0, composite_host_vertices_skipped=0, ray_host_vertices_skipped=0;
};
inline Counts counts;
inline void Add(std::uint64_t& n,std::uint64_t add=1) {
    const auto max=std::numeric_limits<std::uint64_t>::max();n=add>max-n?max:n+add;
}
inline bool Suppress(const Snapshot& s,std::uint64_t vertices) {
    const bool particle=ParticleFamily(s), composite=CompositePass(s), ray=RayCandidate(s);
    if(particle)Add(counts.particle_host_candidates);
    if(composite)Add(counts.composite_host_candidates);
    if(ray)Add(counts.ray_host_candidates);
    if(particle && ParticleEnabled()) {Add(counts.particle_host_skips);Add(counts.particle_host_vertices_skipped,vertices);return true;}
    if(composite && CompositeEnabled()) {Add(counts.composite_host_skips);Add(counts.composite_host_vertices_skipped,vertices);return true;}
    if(ray && RayEnabled()) {Add(counts.ray_host_skips);Add(counts.ray_host_vertices_skipped,vertices);return true;}
    return false;
}
} // namespace OpenGL::EffectFamilyAblation
