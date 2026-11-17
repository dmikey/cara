// Copyright 2026 Cara contributors. GPLv2 or later; see license.txt.
#include "../emulator/src/video_core/renderer_opengl/gl_shared_vertex_shadow.h"
#include "../emulator/src/video_core/renderer_opengl/gl_packed_pool_observer.h"
#include <iostream>
#include <random>
#include <stdexcept>
using namespace OpenGL;
static std::size_t cases=0,checks=0,actual_full_memcmps_saved=0;
#define CHECK(x) do {++checks;if(!(x)) throw std::runtime_error(#x);} while(false)
struct Handle {unsigned handle=0;};
struct Entry {
    Handle texture,indices;
    unsigned rows=1;
    SharedVertexShadow input_shadow;
    std::vector<std::uint8_t> index_shadow;
};
static Entry Make(const std::vector<std::uint8_t>& bytes,const std::vector<std::uint16_t>& indices,unsigned id) {
    Entry e;e.texture.handle=id+1;e.indices.handle=id+100;
    e.input_shadow.assign(bytes.data(),bytes.data()+bytes.size());
    const auto* index=reinterpret_cast<const std::uint8_t*>(indices.data());
    e.index_shadow.assign(index,index+indices.size()*2);return e;
}
static std::vector<std::uint8_t> Materialize(const std::vector<PackedSnapshotCompare::Segment>& spans,std::size_t size) {
    std::vector<std::uint8_t> bytes(size,0);
    for(const auto& span:spans) std::copy_n(span.source,span.size,bytes.begin()+span.destination);
    return bytes;
}
static auto Find(const std::vector<Entry>& pool,std::size_t first,const std::uint8_t* data,std::size_t bytes,
                 const std::vector<std::uint16_t>& indices, bool self_equal=false) {
    const auto eligible=[](const Entry& e){return e.rows==1 && e.texture.handle && e.indices.handle;};
    return self_equal ? IOSVertexUploadOrderReuse::FindWithInputSelfEquality(pool,first,32,data,bytes,indices.data(),indices.size()*2,eligible) :
        IOSVertexUploadOrderReuse::Find(pool,first,32,data,bytes,indices.data(),indices.size()*2,eligible);
}
// CPU integration contract: exact original-boundary probe, then immutable owned
// bytes. No GPU owner is reserved; later selection must use the captured view.
static auto Capture(std::vector<Entry>& pool,std::size_t first,
                    const std::vector<PackedSnapshotCompare::Segment>& spans,std::size_t size,
                    const std::vector<std::uint16_t>& indices) {
    auto token=PackedPoolObserver::Virtual(7,7,0,first,1,indices.size(),128,4096,8192);
    token.logical=size;
    PackedPoolObserver::Probe(token,pool,spans.data(),spans.size(),indices.data(),indices.size());
    std::shared_ptr<const std::vector<std::uint8_t>> lease;
    if(token.state==PackedPoolObserver::State::Pending && token.match.input_equal)
        lease=pool[token.match.selected].input_shadow.CaptureLease();
    return lease;
}
static void SameSelection(const std::vector<Entry>& pool,std::size_t first,
                          const std::shared_ptr<const std::vector<std::uint8_t>>& lease,
                          const std::vector<std::uint8_t>& captured,const std::vector<std::uint16_t>& indices) {
    CHECK(lease && *lease==captured);
    const auto legacy=Find(pool,first,lease->data(),lease->size(),indices,false);
    const auto actual=Find(pool,first,lease->data(),lease->size(),indices,true);
    const auto expected=Find(pool,first,captured.data(),captured.size(),indices,false);
    CHECK(actual.selected==expected.selected && actual.selected==legacy.selected);
    CHECK(actual.input_equal==expected.input_equal && actual.index_equal==expected.index_equal);
    CHECK(actual.input_equal==legacy.input_equal && actual.index_equal==legacy.index_equal);
    CHECK(actual.candidates==expected.candidates && actual.candidates==legacy.candidates);
    CHECK(legacy.full_comparisons==expected.full_comparisons);
    CHECK(actual.full_comparisons<=legacy.full_comparisons);
    CHECK(legacy.full_comparisons-actual.full_comparisons<=1); // Unique live input owner.
    actual_full_memcmps_saved+=legacy.full_comparisons-actual.full_comparisons;
}
int main() {
    std::mt19937 rng(0x4c454153);
    std::vector<std::uint8_t> constants(256,0),guest(400);
    for(auto& byte:guest) byte=rng();
    const std::vector<PackedSnapshotCompare::Segment> plan{
        {constants.data(),256,0,256},{guest.data()+1,257,263,257}};
    const auto captured=Materialize(plan,2048);
    std::vector<std::uint16_t> indices(128,3);
    for(unsigned trial=0;trial<512;++trial) {
        std::vector<Entry> pool;
        for(unsigned id=0;id<40;++id) {
            auto bytes=captured;
            if(rng()%2) bytes[300]^=1; // equal ends, changed middle
            if(rng()%5==0) bytes[260]=1; // poisoned padding
            pool.push_back(Make(bytes,indices,id));
            if(rng()%7==0) pool.back().index_shadow[90]^=1;
            if(rng()%11==0) pool.back().rows=2;
            if(rng()%13==0) pool.back().texture.handle=0;
        }
        const auto first=trial%41;
        const auto expected=Find(pool,first,captured.data(),captured.size(),indices);
        std::vector<unsigned> names;
        for(const auto& e:pool) names.push_back(e.texture.handle);
        auto lease=Capture(pool,first,plan,captured.size(),indices);
        CHECK(bool(lease)==(first<pool.size() && expected.input_equal));
        for(std::size_t i=0;i<pool.size();++i) CHECK(pool[i].texture.handle==names[i]);
        if(lease) {
            SameSelection(pool,first,lease,captured,indices);
            // Future guest writes must not change the already captured input.
            guest[130]^=1;constants[33]^=1;
            CHECK(Materialize(plan,2048)!=captured);
            SameSelection(pool,first,lease,captured,indices);
            guest[130]^=1;constants[33]^=1;
            // Overwrite all owners (COW) and force pool relocation/reordering.
            auto poison=captured;poison[300]^=1;
            for(auto& e:pool) e.input_shadow.assign(poison.data(),poison.data()+poison.size());
            pool.reserve(200);std::reverse(pool.begin(),pool.end());
            SameSelection(pool,first,lease,captured,indices);
            // Reclaim every GPU-owner analogue. CPU snapshot survives even when
            // later normal selection must miss and upload the captured bytes.
            pool.clear();SameSelection(pool,0,lease,captured,indices);
            pool.push_back(Make(poison,indices,999));
            const auto miss=Find(pool,0,lease->data(),lease->size(),indices);
            CHECK(!miss.input_equal && miss.index_equal);
            // The ordinary upload fallback copies the captured view, not guest RAM.
            pool[0].input_shadow.assign(lease->data(),lease->data()+lease->size());
            CHECK(Find(pool,0,lease->data(),lease->size(),indices).input_equal);
            SameSelection(pool,0,lease,captured,indices);
            // Drop/cancel before the next decoded preparation; no lease retained.
            lease.reset();CHECK(!lease);
        }
        ++cases;
    }
    // Real bank reclamation, unused-prefix ownership and lease survival.
    IOSVertexUploadBank::Bank<Entry> bank;
    const auto cls=IOSVertexUploadBank::ClassIndex(2,0);
    auto& pool=bank.sizes[cls];
    pool.push_back(Make(captured,indices,1));pool.push_back(Make(captured,indices,2));
    bank.bytes=2*IOSVertexUploadBank::AllocationBytes(cls,128);bank.textures=2;bank.used[cls]=1;
    auto lease=Capture(pool,1,plan,2048,indices);CHECK(lease);
    IOSVertexUploadBank::Reclaim(bank,bank.bytes,1,1,128);
    CHECK(pool.size()==1 && bank.used[cls]==1);CHECK(*lease==captured);
    SameSelection(pool,1,lease,captured,indices);
    bank.used[cls]=0;IOSVertexUploadBank::Reclaim(bank,bank.bytes+1,0,0,128);
    CHECK(pool.empty() && bank.bytes==0 && bank.textures==0 && *lease==captured);
    // Materialization fallback: changed live middle or padding never leases.
    std::vector<Entry> one;auto changed=captured;changed[300]^=1;
    one.push_back(Make(changed,indices,7));CHECK(!Capture(one,0,plan,2048,indices));
    changed=captured;changed[260]=1;one[0].input_shadow.assign(changed.data(),changed.data()+changed.size());
    CHECK(!Capture(one,0,plan,2048,indices));
    CHECK(Materialize(plan,2048)==captured);
    CHECK(actual_full_memcmps_saved>0);
    std::cout<<"actual_full_memcmps_saved="<<actual_full_memcmps_saved<<" ";
    std::cout<<"PASS cases="<<cases<<" checks="<<checks<<"\n";
}
