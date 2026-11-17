// Copyright 2026 Cara contributors
// Licensed under GPLv2 or any later version
// CPU codec/mapping contract, not proof of actual shader execution or GPU ownership.
#include "../emulator/src/video_core/renderer_opengl/gl_native_rgba8_remap.h"
#include "../emulator/src/video_core/renderer_opengl/gl_surface_remap_layout.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
using Byte = std::uint8_t;
using Pixel = std::array<Byte, 4>;
struct Layout { unsigned width, height; bool tiled; };
unsigned Morton(unsigned x, unsigned y) { // Independent bit-by-bit reference encoder.
    unsigned result = 0;
    for (unsigned bit = 0; bit != 3; ++bit) {
        result |= ((x >> bit) & 1) << (bit * 2);
        result |= ((y >> bit) & 1) << (bit * 2 + 1);
    }
    return result;
}
unsigned GuestPixel(Layout l, unsigned x, unsigned y) {
    if (!l.tiled) return y * l.width + x;
    const unsigned gy = l.height - 1 - y;
    return (gy / 8) * l.width * 8 + (x / 8) * 64 + Morton(x % 8, gy % 8);
}
Pixel PackedNativeUpload(Pixel bytes) { // GL_UNSIGNED_INT_8_8_8_8 on this little-endian host contract.
    const std::uint32_t word = bytes[0] | (std::uint32_t(bytes[1]) << 8) |
        (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
    return {Byte(word >> 24), Byte(word >> 16), Byte(word >> 8), Byte(word)};
}
std::vector<Pixel> Upload(const std::vector<Byte>& guest, Layout l) {
    std::vector<Pixel> texture(l.width * l.height);
    for (unsigned y = 0; y != l.height; ++y) for (unsigned x = 0; x != l.width; ++x) {
        const auto offset = GuestPixel(l, x, y) * 4;
        Pixel staging{guest[offset],guest[offset+1],guest[offset+2],guest[offset+3]};
        if (!l.tiled) std::reverse(staging.begin(), staging.end()); // Actual linear staging codec.
        texture[y*l.width+x] = PackedNativeUpload(staging);
    }
    return texture;
}
std::vector<Byte> Export(const std::vector<Pixel>& texture, Layout l) {
    std::vector<Byte> guest(l.width*l.height*4);
    for (unsigned y=0;y!=l.height;++y) for(unsigned x=0;x!=l.width;++x) {
        Pixel staging=texture[y*l.width+x];
        std::reverse(staging.begin(),staging.end()); // Native packed-u32 download.
        if (!l.tiled) std::reverse(staging.begin(),staging.end()); // Linear guest flush codec.
        auto off=GuestPixel(l,x,y)*4;
        std::copy(staging.begin(),staging.end(),guest.begin()+off);
    }
    return guest;
}
unsigned ShaderDeinterleave(unsigned value) {
    return (value & 1) | ((value >> 1) & 2) | ((value >> 2) & 4);
}
Pixel ShaderFetch(const std::vector<Pixel>& texture, Layout l, unsigned guestPixel) {
    unsigned x,y;
    if(l.tiled) {
        const unsigned tile=guestPixel/64,within=guestPixel&63;
        x=(tile%(l.width/8))*8+ShaderDeinterleave(within);
        y=l.height-1-((tile/(l.width/8))*8+ShaderDeinterleave(within>>1));
    } else {x=guestPixel%l.width;y=guestPixel/l.width;}
    assert(x<l.width && y<l.height);
    return texture[y*l.width+x];
}
int main() {
    using OpenGL::SurfaceRemapLayout::RectangleSpan;
    // Partial pixels/tiles must not round outwards across CPU-owned neighbors.
    for (unsigned unit : {4u,256u}) {
        assert(!RectangleSpan(0x1000,unit*8,unit,{0x1001,0x1000+unit}));
        assert(!RectangleSpan(0x1000,unit*8,unit,{0x1000,0x1000+unit-1}));
        const auto exact=RectangleSpan(0x1000,unit*8,unit,{0x1001,0x1000+unit*2});
        assert(exact && exact.begin==0x1000+unit && exact.end==0x1000+unit*2);
    }
    assert(!RectangleSpan(0x1000,0,4,{0x1000,0x1004}));
    assert(!RectangleSpan(0x1000,6,4,{0x1000,0x1004}));
    assert(!RectangleSpan(0x1000,16,4,{0xfff,0x1004}));
    assert(!RectangleSpan(0x1000,16,4,{0x1004,0x1004}));
    assert(!RectangleSpan(0x1000,16,4,{0x1008,0x1004}));
    std::uint64_t checked=0;
    for(bool a:{false,true}) for(bool b:{false,true}) {
        const auto p=OpenGL::PlanNativeRGBA8RemapEncoding(true,true,0x18000000,0x18000000,a,b);
        assert(p.supported && p.reverse_components==(a!=b));
        for(unsigned offset=1;offset!=4;++offset) {
            assert(!OpenGL::PlanNativeRGBA8RemapEncoding(true,true,0x18000000+offset,0x18000000,a,b).supported);
            assert(!OpenGL::PlanNativeRGBA8RemapEncoding(true,true,0x18000000,0x18000000+offset,a,b).supported);
        }
        assert(!OpenGL::PlanNativeRGBA8RemapEncoding(false,true,0,0,a,b).supported);
        assert(!OpenGL::PlanNativeRGBA8RemapEncoding(true,false,0,0,a,b).supported);
        Layout source{a?256u:128u,416,a},destination{b?256u:128u,416,b};
        for(unsigned pattern=0;pattern!=4;++pattern) {
            std::vector<Byte> guest(source.width*source.height*4);
            std::uint32_t random=0x13579bdf;
            for(unsigned i=0;i!=guest.size();++i) {
                random^=random<<13;random^=random>>17;random^=random<<5;
                guest[i]=pattern==0?Byte(i):pattern==1?Byte(random):pattern==2?Byte((i%4)*73):Byte((i/4)^((i/1024)*29));
            }
            auto texture=Upload(guest,source);assert(Export(texture,source)==guest);
            std::vector<Byte> original(destination.width*destination.height*4,0xA5);
            constexpr std::int64_t sourceBase=0x18000000;
            for (std::int64_t delta : {-16384,-512,-256,-4,0,4,256,512,16384}) {
                const auto encoding=OpenGL::PlanNativeRGBA8RemapEncoding(
                    true,true,sourceBase,sourceBase+delta,a,b);
                assert(encoding.supported && encoding.reverse_components==(a!=b));
                // Source coverage clipped in independent signed address arithmetic.
                const std::int64_t first=std::max<std::int64_t>(0,-delta);
                const std::int64_t last=std::min<std::int64_t>(original.size(),std::int64_t(guest.size())-delta);
                const std::array<std::array<std::int64_t,2>,7> requested{{
                    {{0x4000,0x34000}}, {{0,4}}, {{4,8}}, {{0,256}},
                    {{4,512}}, {{0x3ff0,0x4204}}, {{first,last}}
                }};
                for (const auto& request : requested) {
                    const auto lo=std::max(first,request[0]),hi=std::min(last,request[1]);
                    if(lo>=hi) continue;
                    const auto span=OpenGL::SurfaceRemapLayout::RectangleSpan(
                        sourceBase+delta,destination.width*4*(b?8:1),b?256:4,
                        {std::uint64_t(sourceBase+delta+lo),std::uint64_t(sourceBase+delta+hi)});
                    if (!span) { assert(hi-lo < (b?512:8)); continue; }
                    const unsigned begin=unsigned(span.begin-(sourceBase+delta));
                    const unsigned end=unsigned(span.end-(sourceBase+delta));
                    assert(begin>=lo && end<=hi && begin<end);
                    assert(begin%(b?256:4)==0 && end%(b?256:4)==0);
                    auto output=Upload(original,destination),wrong=output;
                    auto expected=original;
                    for(unsigned y=0;y!=destination.height;++y) for(unsigned x=0;x!=destination.width;++x) {
                        const unsigned offset=GuestPixel(destination,x,y)*4;
                        if(offset<begin || offset>=end) continue;
                        const auto sourceOffset=std::int64_t(offset)+delta;
                        assert(sourceOffset>=0 && sourceOffset+4<=std::int64_t(guest.size()));
                        Pixel value=ShaderFetch(texture,source,unsigned(sourceOffset/4));
                        wrong[y*destination.width+x]=value;
                        if(encoding.reverse_components) std::reverse(value.begin(),value.end());
                        output[y*destination.width+x]=value;
                    }
                    std::copy(guest.begin()+begin+delta,guest.begin()+end+delta,expected.begin()+begin);
                    const auto result=Export(output,destination);assert(result==expected);
                    if(a!=b && pattern==2) assert(Export(wrong,destination)!=expected);
                    checked+=result.size();
                }
            }
        }
    }
    std::cout << "Native RGBA8 CPU codec/mapping PASS: " << checked << " whole-destination bytes; no GPU proof\n";
}
