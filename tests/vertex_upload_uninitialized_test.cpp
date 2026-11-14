// SPDX-License-Identifier: GPL-2.0-or-later
#include "../emulator/src/video_core/renderer_opengl/gl_vertex_upload_ranges.h"
#include <boost/container/vector.hpp>
#include <array>
#include <cassert>
#include <cstdio>
#include <random>
#include <vector>
int main() {
    std::mt19937 random(0x43415241);
    std::vector<unsigned char> source(65536);
    for (auto& b : source) b = static_cast<unsigned char>(random());
    using Word = std::array<unsigned,4>;
    static_assert(sizeof(Word)==16);
    boost::container::vector<Word> staging;
    for (unsigned test=0; test<20000; ++test) {
        OpenGL::BoundedVertexUploadRanges<16> ranges;
        for (unsigned i=0,n=random()%17; i<n; ++i) {
            const auto begin=random()%32768;
            assert(ranges.Add(begin,begin+1+random()%1024));
        }
        ranges.SortAndMerge();
        const size_t prefix=256;
        size_t bytes=prefix;
        for (auto& range:ranges) {
            bytes=(bytes+3)&~size_t(3);range.destination=bytes;
            bytes+=range.end-range.begin;
        }
        bytes=(bytes+2047)/2048*2048;
        staging.resize(bytes/sizeof(Word), boost::container::default_init);
        // Poison every allocation and retained extent. Missing writes must fail.
        auto* actual=reinterpret_cast<unsigned char*>(staging.data());
        std::memset(actual,0xA5,bytes);
        std::vector<unsigned char> reference(bytes,0);
        assert(ranges.ClearPadding(actual,bytes,prefix));
        std::memcpy(actual,source.data(),prefix);
        std::memcpy(reference.data(),source.data(),prefix);
        for (const auto& range:ranges) {
            std::memcpy(actual+range.destination,source.data()+range.begin,range.end-range.begin);
            std::memcpy(reference.data()+range.destination,source.data()+range.begin,range.end-range.begin);
        }
        assert(std::equal(reference.begin(),reference.end(),actual));
    }
    staging.resize(1);staging.resize(300);
    for (size_t i=1;i<staging.size();++i) for (auto value:staging[i]) assert(value==0);
    std::puts("uninitialized staging: 20000 exact-byte growth/shrink cases passed");
}
