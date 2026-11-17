// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#include "../emulator/src/video_core/renderer_opengl/gl_packed_pool_observer.h"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace OpenGL;
using namespace PackedPoolObserver;
static std::size_t cases=0,checks=0;
#define CHECK(x) do {++checks; if (!(x)) throw std::runtime_error(#x);} while(false)
struct Handle {std::uint32_t handle=0;};
struct Entry {
    Handle texture,indices;
    unsigned rows=0;
    std::vector<std::uint8_t> input_shadow,index_shadow;
};
// Independent expression of the current real selector's frame reset, class
// partition, guard order and existing-entry branch. Mutates only test locals.
static Token RealPolicy(unsigned frame,unsigned previous,std::size_t upload,std::size_t used,
                        unsigned input_rows,std::size_t indices,unsigned max_draws,unsigned max_rows) {
    Token t;t.frame=frame;
    if (frame!=previous) {upload=0;used=0;}
    const unsigned index_rows=indices/128;
    unsigned size_class=0,rows=1;
    while(rows<std::max(input_rows,index_rows)) {rows*=2;++size_class;}
    const bool supported=size_class<IOSVertexUploadBank::HeightClasses;
    size_class=IOSVertexUploadBank::ClassIndex(2,size_class);
    const auto before=upload;
    if (supported && upload++<max_draws && size_class<IOSVertexUploadBank::ClassCount && rows<=max_rows) {
        t.state=State::Pending;t.rows=rows;t.size_class=size_class;t.ordinal=used;t.upload_ordinal=before;
    } else t.state=(!supported || rows>max_rows)?State::Shape:State::Storage;
    return t;
}
int main() {
    std::mt19937 rng(0x504f4f4c);
    for(unsigned n=0;n<30000;++n) {
        const unsigned frame=rng()%8, previous=rng()%8, rows=1+rng()%20000;
        const std::size_t upload=rng()%8200,used=rng()%100,index_count=(1+rng()%20000)*128;
        const unsigned limit=1+rng()%8192,max_rows=1+rng()%20000;
        const auto got=Virtual(frame,previous,upload,used,rows,index_count,128,limit,max_rows);
        const auto expected=RealPolicy(frame,previous,upload,used,rows,index_count,limit,max_rows);
        CHECK(got.state==expected.state);
        if(got.state==State::Pending) {
            CHECK(got.rows==expected.rows && got.size_class==expected.size_class);
            CHECK(got.ordinal==expected.ordinal && got.upload_ordinal==expected.upload_ordinal);
        }
        ++cases;
    }
    std::vector<std::uint8_t> constants(256,0),source(320,0x7d);
    const std::vector<PackedSnapshotCompare::Segment> spans{
        {constants.data(),constants.size(),0,constants.size()},
        {source.data()+1,257,263,257}};
    // Independent old materialization, including alignment and tail padding.
    std::vector<std::uint8_t> materialized(2048,0);
    for(const auto& s:spans) std::copy_n(s.source,s.size,materialized.begin()+s.destination);
    std::vector<std::uint16_t> index(128,3);
    for(unsigned trial=0;trial<512;++trial) {
        std::vector<Entry> pool(40);
        for(std::size_t i=0;i<pool.size();++i) {
            auto& e=pool[i];e.texture.handle=i+1;e.indices.handle=i+100;e.rows=1;
            e.input_shadow=materialized;
            e.index_shadow.resize(index.size()*2);std::memcpy(e.index_shadow.data(),index.data(),e.index_shadow.size());
            if(rng()%2) e.input_shadow[300]^=1; // equal ends, unequal middle
            if(rng()%3==0) e.input_shadow[260]=1; // poisoned zero padding
            if(rng()%5==0) e.index_shadow[70]^=1;
            if(rng()%7==0) e.input_shadow.pop_back();
            if(rng()%11==0) e.rows=2;
            if(rng()%13==0) e.texture.handle=0;
            if(rng()%17==0) e.indices.handle=0;
        }
        for(std::size_t first=0;first<=pool.size();++first) {
            auto token=Virtual(5,5,19,first,1,index.size(),128,4096,8192);
            token.logical=materialized.size();token.merged=257;
            const auto expected=IOSVertexUploadOrderReuse::Find(pool,first,32,
                materialized.data(),materialized.size(),index.data(),index.size()*2,
                [](const Entry& e){return e.rows==1 && e.texture.handle && e.indices.handle;});
            Probe(token,pool,spans.data(),spans.size(),index.data(),index.size());
            if(first==pool.size()) CHECK(token.state==State::Storage);
            else {
                CHECK(token.match.selected==expected.selected);
                CHECK(token.match.input_equal==expected.input_equal && token.match.index_equal==expected.index_equal);
                const auto& actual=pool[expected.selected];
                CHECK(Actual(token,5,token.size_class,first,19,expected,actual.texture.handle,actual.indices.handle)==State::Concordant);
                CHECK(Actual(token,6,token.size_class,first,19,expected,actual.texture.handle,actual.indices.handle)==State::OwnerChanged);
                CHECK(Actual(token,5,token.size_class+1,first,19,expected,actual.texture.handle,actual.indices.handle)==State::OwnerChanged);
                CHECK(Actual(token,5,token.size_class,first+1,19,expected,actual.texture.handle,actual.indices.handle)==State::OwnerChanged);
                CHECK(Actual(token,5,token.size_class,first,20,expected,actual.texture.handle,actual.indices.handle)==State::OwnerChanged);
                CHECK(Actual(token,5,token.size_class,first,19,expected,actual.texture.handle+1,actual.indices.handle)==State::OwnerChanged);
                auto changed=expected;changed.input_equal=!changed.input_equal;
                CHECK(Actual(token,5,token.size_class,first,19,changed,actual.texture.handle,actual.indices.handle)==State::DecisionChanged);
                changed=expected;changed.index_equal=!changed.index_equal;
                CHECK(Actual(token,5,token.size_class,first,19,changed,actual.texture.handle,actual.indices.handle)==State::DecisionChanged);
            }
            ++cases;
        }
    }
    // Changed source at the original boundary cannot be accepted by stale bytes.
    Entry e{{1},{2},1,materialized,std::vector<std::uint8_t>(256)};
    source[130]^=1;
    Token t=Virtual(1,0,9999,9999,1,128,128,4096,8192);t.logical=2048;
    CHECK(t.ordinal==0 && t.upload_ordinal==0);
    Probe(t,std::vector<Entry>{e},spans.data(),spans.size(),index.data(),index.size());
    CHECK(!t.match.input_equal);
    CHECK(Virtual(1,1,4096,0,1,128,128,4096,8192).state==State::Storage);
    CHECK(Virtual(1,1,0,0,1,0,128,4096,8192).state==State::Shape);
    CHECK(Virtual(1,1,0,0,1,129,128,4096,8192).state==State::Shape);
    std::cout<<"PASS cases="<<cases<<" checks="<<checks<<"\n";
}
