// Acquire-load reader for the separate host journal; no GL dependencies.
// Host API spans are not GPU time; use the strict post-exit Python validator for complete gates.
#include "video_core/renderer_opengl/gl_host_latency.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
using namespace OpenGL::HostLatency;
static std::uint64_t Load(const std::uint64_t& x,std::memory_order order=std::memory_order_acquire){return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(x)).load(order);}
static Record Copy(const Payload& p){Record r;std::array<std::uint64_t,sizeof(r)/8> words{};for(size_t i=0;i<words.size();++i)words[i]=Load(p.words[i],std::memory_order_acquire);std::memcpy(&r,words.data(),sizeof(r));return r;}
static bool Available(const Journal& j){return Load(j.owner_ready)==1 && !(Load(j.flags)&(ClockUnavailable|SequenceStopped|ObserverInvalidated));}
static void Aggregate(const Journal& j){
 for(unsigned retry=0;retry<8;++retry){
  const auto before=Load(j.stats_committed);if(!before)throw std::runtime_error("no completed aggregate publication");
  const auto completed=Load(j.completed_calls),slow=Load(j.slow_returns),overwritten=Load(j.overwritten_slow);
  std::array<Bucket,BucketCount> copy{};
  for(size_t i=0;i<copy.size();++i){const auto& source=j.buckets[i];auto& b=copy[i];b.calls=Load(source.calls);b.ticks=Load(source.ticks);b.max_ticks=Load(source.max_ticks);for(size_t n=0;n<64;++n)b.histogram[n]=Load(source.histogram[n]);}
  const auto after=Load(j.stats_committed);if(before!=after)continue;if(!Available(j))throw std::runtime_error("aggregate observer censored");
  std::printf("{\"aggregate\":true,\"publication\":%llu,\"completed_calls\":%llu,\"slow_returns\":%llu,\"overwritten_slow\":%llu,\"flags\":%llu,\"timebase_numer\":%u,\"timebase_denom\":%u,\"buckets\":[",(unsigned long long)after,(unsigned long long)completed,(unsigned long long)slow,(unsigned long long)overwritten,(unsigned long long)Load(j.flags),j.timebase_numer,j.timebase_denom);
  bool comma=false;for(size_t i=0;i<copy.size();++i){const auto& b=copy[i];if(!b.calls)continue;if(comma)std::printf(",");comma=true;std::printf("{\"api\":%zu,\"category\":%zu,\"calls\":%llu,\"ticks\":%llu,\"max_ticks\":%llu,\"hist_raw_log2\":[",i/6,i%6,(unsigned long long)b.calls,(unsigned long long)b.ticks,(unsigned long long)b.max_ticks);for(size_t n=0;n<64;++n)std::printf("%s%llu",n?",":"",(unsigned long long)b.histogram[n]);std::printf("]}");}
  std::printf("],\"scope\":\"host call spans with entry publication overhead; not GPU time\"}\n");return;
 }
 throw std::runtime_error("unstable aggregate publication");
}
int main(int argc,char** argv){int fd=-1;void* memory=MAP_FAILED;int result=2;try{
 if(argc<3||argc>5)throw std::runtime_error("usage: reader journal expectedPID [expectedFP [--summary]]");char* end=nullptr;auto pid=std::strtoull(argv[2],&end,10);if(!pid||!end||*end)throw std::runtime_error("expected PID required");
 fd=open(argv[1],O_RDONLY);struct stat st{};if(fd<0||fstat(fd,&st)||st.st_size!=sizeof(Journal)||sizeof(Journal)>256*1024)throw std::runtime_error("journal size/open mismatch");
 memory=mmap(nullptr,sizeof(Journal),PROT_READ,MAP_SHARED,fd,0);if(memory==MAP_FAILED)throw std::runtime_error("map failed");auto& j=*static_cast<const Journal*>(memory);
 if(std::memcmp(j.magic.data(),"CARAHLT1",8)||j.schema!=1||j.bytes!=sizeof(Journal)||j.record_bytes!=sizeof(Record)||j.pid!=pid||!Available(j)||!j.timebase_numer||!j.timebase_denom)throw std::runtime_error("wrong schema/PID or observer unavailable/censored");
 if(argc>=4 && (std::strlen(argv[3])!=64||std::memcmp(j.build_identity.data(),argv[3],64)))throw std::runtime_error("build fingerprint mismatch");
 if(argc==5 && std::strcmp(argv[4],"--summary"))throw std::runtime_error("unknown reader option");
 for(unsigned retry=0;retry<8;++retry){const auto a=Load(j.committed);if(!a)throw std::runtime_error("no call published");auto r=Copy(j.latest[a&1]);const auto b=Load(j.committed);
  if(a!=b)continue;if(!Available(j))throw std::runtime_error("observer invalidated during snapshot");
  if(!r.token||r.api>=std::uint32_t(Api::Count)||(r.phase!=1&&r.phase!=2)||!r.begin_tick||(r.phase==2&&r.end_tick<r.begin_tick))throw std::runtime_error("invalid record");
  std::printf("{\"publication\":%llu,\"token\":%llu,\"api\":%u,\"pname\":%u,\"phase\":%u,\"object\":%u,\"object_generation\":%llu,\"source_revision\":%llu,\"program\":%u,\"program_generation\":%llu,\"link_serial\":%llu,\"draw_ordinal\":%llu,\"mode\":%u,\"count\":%d,\"status\":%d,\"record_flags\":%u,\"observer_flags\":%llu,\"begin_tick\":%llu,\"end_tick\":%llu,\"timebase_numer\":%u,\"timebase_denom\":%u,\"scope\":\"host call span includes entry publication overhead; not GPU time\"}\n",(unsigned long long)a,(unsigned long long)r.token,r.api,r.pname,r.phase,r.object,(unsigned long long)r.object_generation,(unsigned long long)r.object_revision,r.program,(unsigned long long)r.program_generation,(unsigned long long)r.link_serial,(unsigned long long)r.draw_ordinal,r.mode,r.count,r.status,r.flags,(unsigned long long)Load(j.flags),(unsigned long long)r.begin_tick,(unsigned long long)r.end_tick,j.timebase_numer,j.timebase_denom);for(const auto& slot:j.slow){const auto first=Load(slot.committed);if(!first)continue;auto slow=Copy(slot.payload);const auto last=Load(slot.committed);if(first!=last)continue;if(!Available(j))throw std::runtime_error("observer invalidated while reading slow ring");if(slow.phase!=2||!slow.token||slow.end_tick<slow.begin_tick)throw std::runtime_error("invalid slow record");std::printf("{\"slow_sequence\":%llu,\"token\":%llu,\"api\":%u,\"pname\":%u,\"program\":%u,\"program_generation\":%llu,\"link_serial\":%llu,\"begin_tick\":%llu,\"end_tick\":%llu,\"flags\":%u}\n",(unsigned long long)last,(unsigned long long)slow.token,slow.api,slow.pname,slow.program,(unsigned long long)slow.program_generation,(unsigned long long)slow.link_serial,(unsigned long long)slow.begin_tick,(unsigned long long)slow.end_tick,slow.flags);}
  if(argc==5)Aggregate(j);result=0;break;
 }
 }catch(const std::exception& e){std::fprintf(stderr,"host latency unavailable: %s\n",e.what());}
 if(memory!=MAP_FAILED)munmap(memory,sizeof(Journal));if(fd>=0)close(fd);return result;
}
