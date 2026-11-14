// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "video_core/renderer_opengl/gl_vertex_index_range.h"

namespace {
std::uint64_t checks{},cases{},guarded_cases{};
void Require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<typename Index>
OpenGL::VertexIndexRange Reference(const Index* data,std::uint32_t count) {
    std::uint32_t minimum=0xFFFF,maximum=0;
    for(std::uint32_t i=0;i<count;++i){if(data[i]<minimum)minimum=data[i];if(data[i]>maximum)maximum=data[i];}
    return {minimum,maximum};
}
template<typename Index>
void Check(const Index* data,std::uint32_t count) {
    const auto expected=Reference(data,count);
    const std::vector<Index> before=count?std::vector<Index>(data,data+count):std::vector<Index>{};
    const auto actual=OpenGL::AnalyzeVertexIndexRange(data,count);
    Require(actual.minimum==expected.minimum && actual.maximum==expected.maximum,"range differs from independent u32 reference");
    Require(!count || std::equal(before.begin(),before.end(),data),"range modified live source bytes");
    ++cases;
}
template<typename Index>
void TailsAndMutation() {
    constexpr std::array<unsigned,27> counts{0,1,2,3,4,5,7,8,9,15,16,17,23,31,32,33,47,63,64,65,127,128,129,255,256,257,4095};
    std::vector<Index> storage(4096+64,Index(0xA5));
    for(unsigned shift=0;shift<64/sizeof(Index);++shift)for(auto count:counts) {
        auto* data=storage.data()+shift;
        std::uint32_t random=0x3C521B79;
        for(unsigned i=0;i<count;++i){random^=random<<13;random^=random>>17;random^=random<<5;data[i]=Index(random);}
        const auto original=storage;
        Check(data,count);Require(storage==original,"alignment canaries changed");
        if(count){data[0]=0;data[count-1]=std::numeric_limits<Index>::max();Check(data,count);}
        if(count>2) {
            std::fill(data,data+count,Index(17));Check(data,count);
            // Same pointer/count, no page-flag or generation notification.
            data[count/2]=Index(3);Check(data,count);
            data[count/2]=Index(29);Check(data,count);
        }
    }
}
template<typename Index>
void GuardedEnds() {
#if defined(__unix__) || defined(__APPLE__)
    const auto page=std::size_t(sysconf(_SC_PAGESIZE));
    auto* mapping=static_cast<std::uint8_t*>(mmap(nullptr,page*3,PROT_NONE,MAP_ANON|MAP_PRIVATE,-1,0));
    Require(mapping!=MAP_FAILED,"guard mmap failed");
    try {
        auto* writable=mapping+page;Require(mprotect(writable,page,PROT_READ|PROT_WRITE)==0,"guard mprotect failed");
        for(unsigned count:{0u,1u,2u,3u,7u,8u,15u,16u,17u,31u,32u,33u,63u,64u,65u,unsigned(page/sizeof(Index))}) {
            std::fill(writable,writable+page,0xA5);
            auto* data=reinterpret_cast<Index*>(writable+page-count*sizeof(Index));
            for(unsigned i=0;i<count;++i)data[i]=Index(i*73+29);
            const std::vector<std::uint8_t> before(writable,writable+page);
            Check(data,count);Require(std::equal(before.begin(),before.end(),writable),"guarded-page canaries changed");++guarded_cases;
        }
        // An empty call must not touch even a PROT_NONE pointer.
        Check(reinterpret_cast<Index*>(mapping),0);++guarded_cases;
    } catch(...){munmap(mapping,page*3);throw;}
    munmap(mapping,page*3);
#else
    std::cout<<"GUARDS unavailable on this platform\n";
#endif
}
void Correctness() {
    Check<std::uint8_t>(nullptr,0);Check<std::uint16_t>(nullptr,0);
    std::array<std::uint8_t,2> bytes{};
    for(unsigned a=0;a<256;++a){bytes[0]=std::uint8_t(a);Check(bytes.data(),1);for(unsigned b=0;b<256;++b){bytes[1]=std::uint8_t(b);Check(bytes.data(),2);}}
    constexpr std::array<std::uint16_t,9> boundaries{0,1,255,256,257,32767,32768,65534,65535};
    std::array<std::uint16_t,2> words{};
    for(auto a:boundaries){words[0]=a;Check(words.data(),1);for(auto b:boundaries){words[1]=b;Check(words.data(),2);}}
    std::vector<std::uint16_t> domain(65536);
    for(unsigned i=0;i<domain.size();++i)domain[i]=std::uint16_t(i);
    Check(domain.data(),unsigned(domain.size()));std::reverse(domain.begin(),domain.end());Check(domain.data(),unsigned(domain.size()));
    for(unsigned i=0;i<domain.size();++i)domain[i]=std::uint16_t(i*40503u+97u);
    Check(domain.data(),unsigned(domain.size()));
    std::vector<std::uint16_t> large(262145);
    for(unsigned i=0;i<large.size();++i)large[i]=std::uint16_t(i*73u+31u);
    for(auto count:{65535u,65536u,65537u,262144u,262145u})Check(large.data(),count);
    TailsAndMutation<std::uint8_t>();TailsAndMutation<std::uint16_t>();
    GuardedEnds<std::uint8_t>();GuardedEnds<std::uint16_t>();
    std::cout<<"INDEX_RANGE cases="<<cases<<" checks="<<checks<<" guarded="<<guarded_cases
             <<" full_u8_pairs / full_u16_domain / natural_alignment_offsets / zero1tails / live_mutations / source_unchanged PASS\n";
}
#if defined(__clang__) || defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif
NOINLINE OpenGL::VertexIndexRange Baseline8(const std::uint8_t* p,std::uint32_t n){return Reference(p,n);}
NOINLINE OpenGL::VertexIndexRange Baseline16(const std::uint16_t* p,std::uint32_t n){return Reference(p,n);}
NOINLINE OpenGL::VertexIndexRange Candidate8(const std::uint8_t* p,std::uint32_t n){return OpenGL::AnalyzeVertexIndexRange(p,n);}
NOINLINE OpenGL::VertexIndexRange Candidate16(const std::uint16_t* p,std::uint32_t n){return OpenGL::AnalyzeVertexIndexRange(p,n);}
std::uint64_t ThreadCPU() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec value{};
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value))throw std::runtime_error("thread CPU clock failed");
    return std::uint64_t(value.tv_sec)*1000000000ULL+value.tv_nsec;
#else
    throw std::runtime_error("thread CPU benchmark clock unavailable on this platform");
#endif
}
template<typename Index>
void Benchmark(const char* format,OpenGL::VertexIndexRange(*baseline)(const Index*,std::uint32_t),
               OpenGL::VertexIndexRange(*candidate)(const Index*,std::uint32_t)) {
    for(unsigned count:{1u,2u,3u,4u,7u,8u,15u,16u,17u,31u,32u,33u,63u,64u,65u,127u,128u,129u,1024u,8192u,65536u,262144u}) {
        const unsigned total_bytes=(count<128?16u:64u)*1024u*1024u;
        const unsigned iterations=std::max<unsigned>(64u,unsigned(total_bytes/(count*sizeof(Index))));
        std::vector<Index> data(count);for(unsigned i=0;i<count;++i)data[i]=Index(i*73u+31u);
        const auto initial=data;
        for(unsigned order=0;order<4;++order) {
            const bool use_candidate=order==1||order==2;auto function=use_candidate?candidate:baseline;
            data=initial;std::uint64_t checksum=0;
            for(unsigned i=0;i<32;++i){const auto r=function(data.data(),count);checksum+=r.minimum+r.maximum;}
            const auto wall_start=std::chrono::steady_clock::now();const auto cpu_start=ThreadCPU();
            for(unsigned i=0;i<iterations;++i) {
                // Live mutations also prevent cross-iteration pure-call hoisting.
                data[(i*37u)%count]^=Index(1);
                const auto result=function(data.data(),count);checksum+=result.minimum+result.maximum;
            }
            const auto cpu=ThreadCPU()-cpu_start;const auto wall=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-wall_start).count();
            std::cout<<"SCAN_BENCH {\"format\":\""<<format<<"\",\"count\":"<<count<<",\"order\":"<<order
                     <<",\"candidate\":"<<(use_candidate?"true":"false")<<",\"iterations\":"<<iterations
                     <<",\"thread_cpu_ns\":"<<cpu<<",\"wall_ns\":"<<wall<<",\"checksum\":"<<checksum<<"}\n";
        }
    }
}
} // namespace
int main(int argc,char** argv) {
    try {
        Correctness();
        if(argc==2 && std::string(argv[1])=="--bench") {
            Benchmark<std::uint8_t>("u8",Baseline8,Candidate8);
            Benchmark<std::uint16_t>("u16",Baseline16,Candidate16);
        }
    } catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
