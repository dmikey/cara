// Copyright 2026 Cara contributors. Licensed under GPL-2.0-or-later.
#pragma once
#include <array>
#include <cstdint>
#include <limits>

namespace OpenGL::NearestBorrow {
struct Rect {
    std::uint32_t left=0, bottom=0, right=0, top=0;
    bool operator==(const Rect&) const = default;
};
inline bool Nonempty(Rect r) { return r.left<r.right && r.bottom<r.top; }
inline bool Contains(Rect outer, Rect inner) {
    return Nonempty(inner) && outer.left<=inner.left && outer.bottom<=inner.bottom &&
           outer.right>=inner.right && outer.top>=inner.top;
}
inline bool Disjoint(Rect a, Rect b) {
    return a.right<=b.left || b.right<=a.left || a.top<=b.bottom || b.top<=a.bottom;
}
inline bool PowerOfTwo(std::uint32_t x) { return x && !(x&(x-1)); }
struct VertexBits { std::array<std::uint32_t,4> position{}; std::array<std::uint32_t,2> uv{}; };
struct Inputs {
    // Current prepared semantic outputs only; caller proves bounded MOV provenance.
    std::array<VertexBits,4> vertices{};
    std::array<std::uint32_t,6> triangles{};
    std::array<std::uint32_t,4> viewport_bits{}; // native cornerX,Y, halfX,Y
    Rect viewport{}, conservative_write{}; // owner GL coordinates
    std::uint32_t logical_width=0, logical_height=0;
    bool native_snap16=false, no_geometry_shader=false;
};
enum class Reason { None, Mode, Dimensions, Viewport, Strip, Position, UV, Overflow };
struct Plan { Rect logical_read{}, conservative_write{}; };
struct Result { Reason reason=Reason::Mode; Plan plan{}; };

// Exact finite float32 dyadic * integer -> signed integer; no FP interpreter/epsilon.
inline bool ScaledInteger(std::uint32_t bits, std::uint32_t scale, std::int64_t& out) {
    const unsigned exponent=(bits>>23)&255;
    if (exponent==255) return false;
    const std::uint64_t mantissa=(bits&0x7fffff) | (exponent ? 0x800000 : 0);
    if (!mantissa) { out=0; return true; }
    std::uint64_t value=mantissa*std::uint64_t(scale);
    const int shift=exponent ? int(exponent)-150 : -149;
    constexpr auto limit=std::uint64_t(std::numeric_limits<std::int64_t>::max());
    if (shift>=0) {
        if (shift>=63 || value>(limit>>shift)) return false;
        value<<=shift;
    } else {
        const unsigned right=unsigned(-shift);
        if (right>=64 || (value&((std::uint64_t(1)<<right)-1))) return false;
        value>>=right;
    }
    if (value>limit) return false;
    out=(bits>>31) ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
    return true;
}
inline Result PlanQuad(const Inputs& in) {
    const auto reject=[](Reason r) { return Result{r,{}}; };
    if (!in.native_snap16 || !in.no_geometry_shader) return reject(Reason::Mode);
    if (!PowerOfTwo(in.logical_width) || !PowerOfTwo(in.logical_height) ||
        in.logical_width>65536 || in.logical_height>65536) return reject(Reason::Dimensions);
    if (in.triangles!=std::array<std::uint32_t,6>{0,1,2,2,1,3}) return reject(Reason::Strip);
    std::array<std::int64_t,4> vp{};
    for (unsigned i=0;i<4;++i)
        if (!ScaledInteger(in.viewport_bits[i],1,vp[i])) return reject(Reason::Viewport);
    if (vp[2]<=0 || vp[3]<=0 || vp[2]>32768 || vp[3]>32768 ||
        vp[0]<-65536 || vp[0]>65536 || vp[1]<-65536 || vp[1]>65536 ||
        !Nonempty(in.viewport) || !Contains(in.conservative_write,in.viewport) ||
        in.viewport.right-in.viewport.left!=std::uint64_t(vp[2])*2 ||
        in.viewport.top-in.viewport.bottom!=std::uint64_t(vp[3])*2)
        return reject(Reason::Viewport);
    // Exact emission order of the restricted strip: TL, BL, TR, BR. No flips.
    constexpr std::array<std::uint32_t,4> x{0xbf800000,0xbf800000,0x3f800000,0x3f800000};
    constexpr std::array<std::uint32_t,4> y{0x3f800000,0xbf800000,0x3f800000,0xbf800000};
    std::array<std::int64_t,4> u{},v{};
    for (unsigned i=0;i<4;++i) {
        const auto& p=in.vertices[i].position;
        if (p[0]!=x[i] || p[1]!=y[i] || (p[2]&0x7fffffff)!=0 || p[3]!=0x3f800000)
            return reject(Reason::Position);
        if (!ScaledInteger(in.vertices[i].uv[0],in.logical_width,u[i]) ||
            !ScaledInteger(in.vertices[i].uv[1],in.logical_height,v[i])) return reject(Reason::UV);
        if (u[i]<0 || v[i]<0 || u[i]>in.logical_width || v[i]>in.logical_height)
            return reject(Reason::UV);
    }
    if (u[0]!=u[1] || u[2]!=u[3] || v[0]!=v[2] || v[1]!=v[3] ||
        u[2]<=u[0] || v[0]<=v[1] ||
        u[2]-u[0]!=in.viewport.right-in.viewport.left ||
        v[0]-v[1]!=in.viewport.top-in.viewport.bottom) return reject(Reason::UV);
    return {Reason::None,{{std::uint32_t(u[0]),std::uint32_t(v[1]),
                          std::uint32_t(u[2]),std::uint32_t(v[0])},in.conservative_write}};
}
// This is a restricted affine experimental plan, never a universal GPU proof.
} // namespace OpenGL::NearestBorrow
