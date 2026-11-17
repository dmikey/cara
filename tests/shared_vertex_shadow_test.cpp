// SPDX-License-Identifier: GPL-2.0-or-later
// Original independent deep-copy oracle for Cara immutable CPU shadow leases.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <new>
#include <thread>
#include <type_traits>
#include "video_core/renderer_opengl/gl_shared_vertex_shadow.h"

// Allocation elision/allocator-side-effect assumptions are legitimate in an
// optimized caller. Isolate fault/count observations, not semantic tests, at O0.
#if defined(__clang__)
#define ALLOCATION_OBSERVER __attribute__((optnone, noinline))
#elif defined(__GNUC__)
#define ALLOCATION_OBSERVER __attribute__((optimize("O0"), noinline))
#else
#define ALLOCATION_OBSERVER
#endif
namespace {
volatile std::ptrdiff_t fail_after = -1;
volatile std::size_t allocations = 0, allocated_bytes = 0;
unsigned checks = 0;
void Check(bool condition) {
    ++checks;
    if (!condition) { std::cerr << "shared shadow check failed: " << checks << '\n'; std::abort(); }
}
}
ALLOCATION_OBSERVER void* operator new(std::size_t n) {
    if (fail_after == 0) throw std::bad_alloc{};
    if (fail_after > 0) --fail_after;
    ++allocations; allocated_bytes += n;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
using Shadow = OpenGL::SharedVertexShadow;
using Bytes = Shadow::Buffer;
static Bytes Copy(const Shadow& s) { return s.empty() ? Bytes{} : Bytes(s.data(), s.data()+s.size()); }
static void Equal(const Shadow& s, const Bytes& expected) {
    Check(s.size()==expected.size()); Check(s.empty()==expected.empty());
    Check(expected.empty() || std::equal(expected.begin(),expected.end(),s.data()));
}
static void Equal(const Shadow::Lease& lease, const Bytes& expected) { Check(bool(lease)); Check(*lease==expected); }
ALLOCATION_OBSERVER static void FailAssign(Shadow& s,const u8* first,const u8* last) {
    const Bytes before=Copy(s); const u8* pointer=s.data(); bool caught=false;
    fail_after=0;try{s.assign(first,last);}catch(const std::bad_alloc&){caught=true;}fail_after=-1;
    Check(caught); Equal(s,before); Check(s.data()==pointer);
}
struct IterationFailure {};
struct ThrowingIterator {
    using iterator_category=std::input_iterator_tag;
    using value_type=u8; using difference_type=std::ptrdiff_t; using pointer=const u8*; using reference=u8;
    const u8* current; const u8* throw_at;
    u8 operator*() const { if(current==throw_at)throw IterationFailure{};return *current; }
    ThrowingIterator& operator++(){++current;return *this;}
    ThrowingIterator operator++(int){auto old=*this;++*this;return old;}
    friend bool operator==(ThrowingIterator a,ThrowingIterator b){return a.current==b.current;}
    friend bool operator!=(ThrowingIterator a,ThrowingIterator b){return !(a==b);}
};
static std::size_t promotion_bytes;
ALLOCATION_OBSERVER static void AllocationOracle() {
    u8 source[256];for(unsigned i=0;i<256;++i)source[i]=u8(i);
    auto initial=allocations;Shadow s;Check(allocations==initial);Check(!s.CaptureLease());Check(allocations==initial);
    const auto vector_start=allocations,vector_bytes=allocated_bytes;
    Bytes ordinary;ordinary.assign(source,source+16);ordinary.assign(source,source+256);ordinary.assign(source,source+32);ordinary.clear();
    const auto vector_count=allocations-vector_start,vector_total_bytes=allocated_bytes-vector_bytes;
    const auto wrapper_start=allocations,wrapper_bytes=allocated_bytes;
    s.assign(source,source+16);s.assign(source,source+256);s.assign(source,source+32);s.clear();
    Check(allocations-wrapper_start==vector_count);Check(allocated_bytes-wrapper_bytes==vector_total_bytes);
    s.assign(source,source+128);const Bytes before=Copy(s);const u8* pointer=s.data();auto n=allocations;
    fail_after=0;auto failed=s.CaptureLease();fail_after=-1;Check(!failed);Check(allocations==n);Equal(s,before);Check(s.data()==pointer);
    n=allocations;const auto bytes=allocated_bytes;auto lease=s.CaptureLease();promotion_bytes=allocated_bytes-bytes;
    Check(allocations==n+1);Check(lease->data()==pointer);Equal(lease,before);
    n=allocations;fail_after=0;auto second=s.CaptureLease();fail_after=-1;Check(allocations==n);Check(second.get()==lease.get());
    FailAssign(s,source,source+16);FailAssign(s,s.data()+3,s.data()+20);Equal(lease,before);Equal(second,before);
    lease.reset();second.reset(); // Even sole-owner publication must replace, not mutate.
    FailAssign(s,source,source+16);Equal(s,before);
    n=allocations;fail_after=0;s.assign(s.data(),s.data()+s.size());fail_after=-1;Check(allocations==n);Equal(s,before);
    n=allocations;fail_after=0;s.clear();fail_after=-1;Check(allocations==n);Check(s.empty());
    s.assign(source,source+32);FailAssign(s,s.data()+3,s.data()+20); // Partial inline alias uses a strong temporary.
    auto snapshot=s.CaptureLease();const Bytes old=*snapshot;const Bytes replacement(source,source+64);
    const Bytes current=Copy(s);bool caught=false;
    fail_after=0;try{s.assign(replacement.begin(),replacement.end());}catch(const std::bad_alloc&){caught=true;}fail_after=-1;
    Check(caught);Equal(s,current);Equal(snapshot,old);
}
static void SemanticOracle() {
    u8 source[256];for(unsigned i=0;i<256;++i)source[i]=u8(i);
    Shadow s;s.assign(source,source+128);const Bytes original=Copy(s);const u8* pointer=s.data();
    auto lease=s.CaptureLease();auto second=s.CaptureLease();Check(lease->data()==pointer);Check(lease.get()==second.get());
    s.assign(s.data()+3,s.data()+20);Equal(s,Bytes(original.begin()+3,original.begin()+20));Equal(lease,original);Equal(second,original);
    source[3]^=0xff;Equal(lease,original); // No naked source/guest pointer retained.
    const Bytes old=Copy(s);s.assign(s.data()+2,s.data()+s.size()-1);Equal(s,Bytes(old.begin()+2,old.end()-1));
    const Bytes self=Copy(s);s.assign(s.data(),s.data()+s.size());Equal(s,self);
    auto held=s.CaptureLease();Bytes snapshot=*held;Shadow moved(std::move(s));Check(s.empty());Equal(moved,snapshot);
    moved=std::move(moved);Equal(moved,snapshot);Equal(held,snapshot);
    Shadow other;other.assign(source,source+4);const Bytes other_copy=Copy(other);
    swap(other,moved);Equal(other,snapshot);Equal(moved,other_copy);Equal(held,snapshot);
    moved=std::move(other);Check(other.empty());Equal(moved,snapshot);moved.clear();Check(moved.empty());Equal(held,snapshot);
    s.assign(source,source+32);auto generic=s.CaptureLease();snapshot=*generic;
    s.assign(generic->begin()+1,generic->end()-1);Equal(s,Bytes(snapshot.begin()+1,snapshot.end()-1));Equal(generic,snapshot);
    const Bytes before=Copy(s);pointer=s.data();bool caught=false;
    try{s.assign(ThrowingIterator{source,source+3},ThrowingIterator{source+16,source+3});}catch(const IterationFailure&){caught=true;}
    Check(caught);Equal(s,before);Check(s.data()==pointer);Equal(generic,snapshot);
    // Deterministically distinguish permanent publication from use_count()==1.
    Shadow published;published.assign(source,source+32);auto published_lease=published.CaptureLease();
    std::weak_ptr<const Bytes> weak=published_lease;published_lease.reset();
    published.assign(source,source+16);Check(weak.expired());Equal(published,Bytes(source,source+16));
    published_lease=published.CaptureLease();weak=published_lease;snapshot=*published_lease;published_lease.reset();
    auto locked=weak.lock();Check(bool(locked));published.assign(source+2,source+12);Equal(locked,snapshot);
    auto locked_again=weak.lock();Equal(locked_again,snapshot);locked.reset();locked_again.reset();Check(weak.expired());
    // Destruction, vector relocation and pool reclamation cannot revoke leases.
    std::vector<Shadow> pool;pool.emplace_back();pool[0].assign(source,source+32);auto reclaimed=pool[0].CaptureLease();snapshot=*reclaimed;
    for(unsigned i=0;i<100;++i)pool.emplace_back();Equal(reclaimed,snapshot);pool.clear();pool.shrink_to_fit();Equal(reclaimed,snapshot);
    Shadow threaded;threaded.assign(source,source+128);auto shared=threaded.CaptureLease();const Bytes expected=*shared;
    std::atomic<bool> okay{true};auto reader=[shared,&expected,&okay]{for(unsigned i=0;i<10000;++i)if(*shared!=expected)okay=false;};
    std::thread one(reader),two(reader);for(unsigned i=0;i<1000;++i){source[0]=u8(i);threaded.assign(source,source+128);}one.join();two.join();Check(okay.load());Equal(shared,expected);
    unsigned random=0x91a43;
    for(unsigned i=0;i<5000;++i){
        random=random*1664525u+1013904223u;const unsigned count=1+(random%256);
        Shadow a;a.assign(source,source+count);const Bytes saved=Copy(a);auto keep=a.CaptureLease();auto keep2=keep;
        const unsigned offset=random%count;const Bytes tail(saved.begin()+offset,saved.end());
        a.assign(a.data()+offset,a.data()+a.size());Equal(a,tail);Equal(keep,saved);
        Shadow b(std::move(a));a.clear();Equal(b,tail);Equal(keep2,saved);swap(a,b);b.clear();Equal(a,tail);a.clear();Equal(keep,saved);
    }
    // Retained capacity, not only size, obeys both the caller and hard bounds.
    Shadow retained;retained.assign(source,source+256);pointer=retained.data();retained.assign(source,source+16);
    Check(retained.data()==pointer);Check(!retained.CaptureLease(128));Equal(retained,Bytes(source,source+16));
    Bytes bounded(Shadow::MaxLeaseBytes,0x6b);Shadow large;large.assign(bounded.data(),bounded.data()+bounded.size());
    auto bound=large.CaptureLease();Check(bool(bound));Check(bound->size()==Shadow::MaxLeaseBytes);
    bounded.push_back(0x7a);large.assign(bounded.data(),bounded.data()+bounded.size());Check(!large.CaptureLease(Shadow::MaxLeaseBytes+1));
    pointer=large.data();large.assign(source,source+16);Check(large.data()==pointer);Check(!large.CaptureLease());Equal(large,Bytes(source,source+16));
    Check((*bound)[0]==0x6b&&bound->back()==0x6b);large.clear();Check(bound->size()==Shadow::MaxLeaseBytes);
    s.assign(static_cast<const u8*>(nullptr),static_cast<const u8*>(nullptr));Check(s.empty());
}
int main() {
    static_assert(!std::is_copy_constructible_v<Shadow>);
    static_assert(std::is_nothrow_move_constructible_v<Shadow>);
    static_assert(std::is_nothrow_move_assignable_v<Shadow>);
    static_assert(std::is_same_v<decltype(std::declval<Shadow>().data()),const u8*>);
    AllocationOracle();SemanticOracle();
    std::cout<<checks<<" shared-shadow deep-copy/lease/failure/weak-lifetime checks passed; vector="<<sizeof(Bytes)
             <<" wrapper="<<sizeof(Shadow)<<" metadata_extra="<<sizeof(Shadow)-sizeof(Bytes)
             <<" promotion_control_allocation_bytes="<<promotion_bytes
             <<"; allocation/fault observer=O0; semantic/lifetime suite=build optimization; default_no_shared_allocation=verified\n";
}
