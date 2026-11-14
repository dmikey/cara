// SPDX-License-Identifier: GPL-2.0-or-later
#include "../emulator/src/video_core/renderer_opengl/gl_vertex_upload_ranges.h"
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>

int main() {
    std::mt19937 rng(0x43415241);
    std::vector<unsigned char> source(65536);
    for (auto& value : source) value = static_cast<unsigned char>(rng());
    for (unsigned test=0;test<20000;++test) {
        OpenGL::BoundedVertexUploadRanges<16> ranges;
        for (unsigned i=0,n=rng()%17;i<n;++i) {
            const auto begin=rng()%32768;
            assert(ranges.Add(begin,begin+1+rng()%1024));
        }
        ranges.SortAndMerge();
        const std::size_t prefix=256;
        std::size_t bytes=prefix;
        for (auto& range:ranges) {
            bytes=(bytes+3)&~std::size_t(3); range.destination=bytes;
            bytes+=range.end-range.begin;
        }
        bytes=((bytes+2047)/2048)*2048;
        std::vector<unsigned char> reference(bytes,0), actual(bytes,0xa5);
        assert(ranges.ClearPadding(actual.data(),actual.size(),prefix));
        std::memcpy(reference.data(),source.data(),prefix);
        std::memcpy(actual.data(),source.data(),prefix);
        for (const auto& range:ranges) {
            std::memcpy(reference.data()+range.destination,source.data()+range.begin,range.end-range.begin);
            std::memcpy(actual.data()+range.destination,source.data()+range.begin,range.end-range.begin);
        }
        assert(actual==reference);
    }
    OpenGL::BoundedVertexUploadRanges<16> invalid;
    assert(invalid.Add(0,17)); invalid.begin()->destination=0;
    unsigned char bytes[16]{};
    assert(!invalid.ClearPadding(bytes,sizeof(bytes),0));
    assert(!invalid.ClearPadding(bytes,sizeof(bytes),17));
    invalid.begin()->destination=2;
    assert(!invalid.ClearPadding(bytes,sizeof(bytes),4));
    std::puts("vertex padding: 20000 exact-byte cases and invalid ranges passed");
}
