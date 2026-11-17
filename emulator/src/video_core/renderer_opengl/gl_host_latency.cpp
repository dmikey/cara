#include "gl_host_latency.h"
#include "core/cara_state_version.h"
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#include <pthread.h>
#endif
namespace OpenGL::HostLatency {
namespace {
constexpr auto Max=std::numeric_limits<std::uint64_t>::max();
std::uint64_t Load(std::uint64_t& x) noexcept {return std::atomic_ref<std::uint64_t>(x).load(std::memory_order_relaxed);}
bool Sat(std::uint64_t& x,std::uint64_t n=1) noexcept {auto a=std::atomic_ref<std::uint64_t>(x);const auto old=a.load(std::memory_order_relaxed);const bool overflow=n>Max-old;a.store(overflow?Max:old+n,std::memory_order_release);return overflow;}
void StorePayload(Payload& p,const Record& r) noexcept {
 std::array<std::uint64_t,sizeof(Record)/8> words{};std::memcpy(words.data(),&r,sizeof(r));
 for(std::size_t i=0;i<words.size();++i)std::atomic_ref<std::uint64_t>(p.words[i]).store(words[i],std::memory_order_release);
}
void AtomicSat(Journal& j,std::uint64_t& x) noexcept {auto a=std::atomic_ref<std::uint64_t>(x);auto old=a.load(std::memory_order_relaxed);while(old!=Max&&!a.compare_exchange_weak(old,old+1,std::memory_order_relaxed)) {}if(old==Max)std::atomic_ref<std::uint64_t>(j.flags).fetch_or(CounterSaturated,std::memory_order_release);}
void Flagged(Journal& j,std::uint64_t flag) noexcept {std::atomic_ref<std::uint64_t>(j.flags).fetch_or(flag,std::memory_order_release);}
std::uint64_t Tick() noexcept {
#ifdef CARA_MAC_ATLAS_PIPELINE
 return mach_absolute_time();
#else
 return 0;
#endif
}
bool IsDraw(Api api) noexcept {return api==Api::DrawArrays||api==Api::DrawElements||api==Api::DrawRange;}
std::size_t Category(const Record& r) noexcept {
 if(r.flags&FirstObservedDraw)return 1;
 if(r.pname==0)return 0;
 if(r.pname==0x8b81)return 2; // GL_COMPILE_STATUS
 if(r.pname==0x8b82)return 3; // GL_LINK_STATUS
 if(r.pname==0x8b84)return 4; // GL_INFO_LOG_LENGTH
 return 5;
}
}
std::uint64_t ReadHostTick() noexcept {return Tick();}
bool Requested() noexcept {
#ifdef CARA_MAC_ATLAS_PIPELINE
 const char* p=std::getenv("CARA_GL_HOST_LATENCY");return p&&std::strcmp(p,"1")==0;
#else
 return false;
#endif
}
struct Recorder::Impl {
 struct Shader {std::uint32_t id=0,type=0;std::uint64_t generation=0,revision=0;bool deleted=false;};
 struct Program {std::uint32_t id=0;std::uint64_t generation=0,link=0,draws=0;bool deleted=false,link_known=false,linked=false;};
 Journal* journal=nullptr;std::thread::id owner{};std::atomic<unsigned> owner_state{0};bool active=false,stopped=false,metadata_lost=false;
 std::array<Shader,MaxShaders> shaders{};std::array<Program,MaxPrograms> programs{};
 std::size_t shader_count=0,program_count=0;Program* current=nullptr;
 std::uint32_t requested_program=0,vao=0,framebuffer=0;
 std::uint64_t generation=0,link=0,token=0,publication=0,stats_publication=0;
 explicit Impl(const std::string& directory) {
  const std::filesystem::path path(directory);
  if(directory.empty()||!path.is_absolute()||!std::filesystem::create_directory(path))throw std::runtime_error("host latency requires a fresh absolute directory");
  ::chmod(directory.c_str(),0700);
  const int fd=::open((path/"host-latency.bin").c_str(),O_CREAT|O_EXCL|O_RDWR,0600);
  if(fd<0)throw std::runtime_error("host latency file unavailable");
  if(ftruncate(fd,sizeof(Journal))!=0){::close(fd);throw std::runtime_error("host latency sizing failed");}
  void* map=mmap(nullptr,sizeof(Journal),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);::close(fd);
  if(map==MAP_FAILED)throw std::runtime_error("host latency mapping failed");
  journal=new(map) Journal{};std::memcpy(journal->magic.data(),"CARAHLT1",8);journal->pid=getpid();
  static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
  static_assert(std::atomic<unsigned>::is_always_lock_free);
  static_assert(sizeof(CARA_STATE_FINGERPRINT)==65);
  std::memcpy(journal->build_identity.data(),CARA_STATE_FINGERPRINT,65);
  const char* cap=std::getenv("CARA_GL_PIPELINE_CAPTURE");journal->capture_requested=cap&&std::strcmp(cap,"1")==0;
 }
 ~Impl(){if(journal){msync(journal,sizeof(Journal),MS_SYNC);munmap(journal,sizeof(Journal));}}
 bool Owner() noexcept {
  if(owner_state.load(std::memory_order_acquire)!=2 || owner!=std::this_thread::get_id()) {
   AtomicSat(*journal,journal->wrong_owner);Flagged(*journal,ObserverInvalidated);return false;
  }
  return !stopped && !(std::atomic_ref<std::uint64_t>(journal->flags).load(std::memory_order_relaxed)&ObserverInvalidated);
 }
 Shader* FindShader(std::uint32_t id) noexcept {if(metadata_lost)return nullptr;for(auto n=shader_count;n;--n)if(shaders[n-1].id==id)return &shaders[n-1];return nullptr;}
 Program* FindProgram(std::uint32_t id) noexcept {if(metadata_lost)return nullptr;for(auto n=program_count;n;--n)if(programs[n-1].id==id)return &programs[n-1];return nullptr;}
 void Unknown() noexcept {if(Sat(journal->metadata_unknown))Flagged(*journal,CounterSaturated);}
 void LoseMetadata() noexcept {metadata_lost=true;current=nullptr;Flagged(*journal,MetadataOverflow);Unknown();}
 void Stop() noexcept {stopped=true;Flagged(*journal,SequenceStopped);}
 void Publish(const Record& r) noexcept {
  if(publication==Max){Stop();return;}
  const auto n=++publication;StorePayload(journal->latest[n&1],r);
  std::atomic_ref<std::uint64_t>(journal->committed).store(n,std::memory_order_release);
 }
};
Recorder::Recorder(const std::string& directory) noexcept {try{impl=std::make_unique<Impl>(directory);}catch(const std::exception& e){std::fprintf(stderr,"CARA host latency unavailable: %s\n",e.what());}catch(...){std::fprintf(stderr,"CARA host latency unavailable\n");}}
Recorder::~Recorder()=default;
void Recorder::BindOwnerThread() noexcept {
 if(!impl)return;auto& x=*impl;
 unsigned expected=0;
 if(!x.owner_state.compare_exchange_strong(expected,1,std::memory_order_acq_rel,std::memory_order_acquire)) {
  if(expected!=2 || x.owner!=std::this_thread::get_id()){AtomicSat(*x.journal,x.journal->wrong_owner);Flagged(*x.journal,ObserverInvalidated);}
  return;
 }
#ifdef CARA_MAC_ATLAS_PIPELINE
 mach_timebase_info_data_t t{};std::uint64_t tid=0;
 if(mach_timebase_info(&t)!=KERN_SUCCESS||!t.numer||!t.denom||pthread_threadid_np(nullptr,&tid)!=0){Flagged(*x.journal,ClockUnavailable);x.stopped=true;x.owner_state.store(3,std::memory_order_release);std::atomic_ref<std::uint64_t>(x.journal->owner_ready).store(2,std::memory_order_release);return;}
 x.journal->timebase_numer=t.numer;x.journal->timebase_denom=t.denom;x.journal->owner_tid=tid;
 const unsigned __int128 ticks=(static_cast<unsigned __int128>(20000000)*t.denom+t.numer-1)/t.numer;
 x.journal->slow_threshold_ticks=static_cast<std::uint64_t>(ticks);
 x.owner=std::this_thread::get_id();std::atomic_ref<std::uint64_t>(x.journal->owner_ready).store(1,std::memory_order_release);x.owner_state.store(2,std::memory_order_release);
#else
 Flagged(*x.journal,ClockUnavailable);x.stopped=true;x.owner_state.store(3,std::memory_order_release);std::atomic_ref<std::uint64_t>(x.journal->owner_ready).store(2,std::memory_order_release);
#endif
}
Token Recorder::Begin(Api api,std::uint32_t object,std::uint32_t pname,std::uint32_t mode,std::int32_t count) noexcept {
 Token t;if(!impl)return t;auto& x=*impl;if(!x.Owner())return t;
 if(x.active){AtomicSat(*x.journal,x.journal->reentry);Flagged(*x.journal,ObserverInvalidated);return t;}
 if(std::uint32_t(api)>=std::uint32_t(Api::Count)||x.token==Max||x.publication>Max-2){x.Stop();return t;}
 t.record.token=++x.token;t.record.api=std::uint32_t(api);t.record.object=object;t.record.pname=pname;t.record.mode=mode;t.record.count=count;
 if(x.metadata_lost)t.record.flags|=UnknownObject;
 t.record.program=x.requested_program;t.record.vao=x.vao;t.record.framebuffer=x.framebuffer;
 if(x.current){t.record.program_generation=x.current->generation;t.record.link_serial=x.current->link;if(x.current->deleted)t.record.flags|=DeletedRequested;}
 else if(x.requested_program)t.record.flags|=UnknownObject;
 if(api==Api::Compile||api==Api::ShaderQuery){if(auto* s=x.FindShader(object)){t.record.object_generation=s->generation;t.record.object_revision=s->revision;t.record.mode=s->type;}else t.record.flags|=UnknownObject;}
 if(api==Api::Link||api==Api::ProgramQuery){if(auto* p=x.FindProgram(object)){t.record.object_generation=p->generation;t.record.link_serial=p->link;}else t.record.flags|=UnknownObject;}
 if(IsDraw(api)) {
  if(x.current){if(!x.current->link_known)t.record.flags|=LinkStatusUnobserved;else if(!x.current->linked)t.record.flags|=LinkStatusFalse;if(!x.current->link)t.record.flags|=UnknownObject;if(!x.current->draws)t.record.flags|=FirstObservedDraw;if(Sat(x.current->draws))Flagged(*x.journal,CounterSaturated);t.record.draw_ordinal=x.current->draws;}
  else t.record.flags|=UnknownObject;
 }
 t.record.begin_tick=Tick();if(!t.record.begin_tick){Flagged(*x.journal,ClockUnavailable);x.stopped=true;return {};}
 t.record.phase=1;x.active=true;t.active=true;x.Publish(t.record);return t;
}
void Recorder::End(Token t,std::uint64_t end,std::int32_t status,bool status_present) noexcept {
 if(!t.active||!impl)return;auto& x=*impl;if(!x.Owner())return;
 if(!x.active || t.record.token!=x.token || t.record.phase!=1){Flagged(*x.journal,ObserverInvalidated);return;}
 x.active=false;
 if(end<t.record.begin_tick||!end){Flagged(*x.journal,ClockUnavailable);x.stopped=true;return;}
 t.record.end_tick=end;t.record.phase=2;if(status_present){t.record.flags|=StatusPresent;t.record.status=status;}
 if(status_present && t.record.api==std::uint32_t(Api::ProgramQuery) && t.record.pname==0x8b82) {
  if(auto* p=x.FindProgram(t.record.object)){p->link_known=true;p->linked=status!=0;}
 }
 x.Publish(t.record);if(x.stopped)return;
 auto& j=*x.journal;if(x.stats_publication==Max){x.Stop();return;}
 std::atomic_ref<std::uint64_t>(j.stats_committed).store(0,std::memory_order_release);
 const auto elapsed=end-t.record.begin_tick;auto& b=j.buckets[std::size_t(t.record.api)*6+Category(t.record)];bool saturated=Sat(b.calls);saturated|=Sat(b.ticks,elapsed);if(elapsed>Load(b.max_ticks))std::atomic_ref<std::uint64_t>(b.max_ticks).store(elapsed,std::memory_order_release);
 // Raw-tick log2 bins: bin0 contains0/1 ticks; binN contains[2^N,2^(N+1)-1].
 const unsigned bucket=elapsed?unsigned(std::bit_width(elapsed)-1):0;
 saturated|=Sat(b.histogram[bucket]);saturated|=Sat(j.completed_calls);
 if(elapsed>=j.slow_threshold_ticks){
  if(Load(j.slow_returns)==Max){x.Stop();return;}
  Sat(j.slow_returns);const auto slow=Load(j.slow_returns);if(slow>MaxSlow)saturated|=Sat(j.overwritten_slow);
  auto& slot=j.slow[(slow-1)%MaxSlow];
  std::atomic_ref<std::uint64_t>(slot.committed).store(0,std::memory_order_release);StorePayload(slot.payload,t.record);
  std::atomic_ref<std::uint64_t>(slot.committed).store(slow,std::memory_order_release);
 }
 if(saturated)Flagged(j,CounterSaturated);
 std::atomic_ref<std::uint64_t>(j.stats_committed).store(++x.stats_publication,std::memory_order_release);
}
bool Recorder::CanCopyStatus(const Token& t) noexcept {
 if(!t.active||!impl)return false;auto& x=*impl;if(!x.Owner())return false;
 // Only valid status pnames for a tracked, not-deleted object. No new GL error queries.
 // Result bytes are copied from the original output, not independently GL-error validated.
 if(t.record.api==std::uint32_t(Api::ShaderQuery) && (t.record.pname==0x8b81||t.record.pname==0x8b84)) {
  auto* p=x.FindShader(t.record.object);return p&&!p->deleted&&p->generation==t.record.object_generation;
 }
 if(t.record.api==std::uint32_t(Api::ProgramQuery) && (t.record.pname==0x8b82||t.record.pname==0x8b83||t.record.pname==0x8b84)) {
  auto* p=x.FindProgram(t.record.object);return p&&!p->deleted&&p->generation==t.record.object_generation;
 }
 return false;
}
#define OWNER_OR_RETURN if(!impl||!impl->Owner())return;auto& x=*impl
void Recorder::CreateShader(std::uint32_t id,std::uint32_t type) noexcept {OWNER_OR_RETURN;if(!id)return;if(x.shader_count==MaxShaders){x.LoseMetadata();return;}if(x.generation==Max){x.Stop();return;}x.shaders[x.shader_count++]={id,type,++x.generation,0,false};}
void Recorder::Source(std::uint32_t id) noexcept {OWNER_OR_RETURN;if(auto* s=x.FindShader(id)){if(s->revision==Max){x.LoseMetadata();return;}++s->revision;}else x.Unknown();}
void Recorder::DeleteShader(std::uint32_t id) noexcept {OWNER_OR_RETURN;if(auto* s=x.FindShader(id))s->deleted=true;else x.Unknown();}
void Recorder::CreateProgram(std::uint32_t id) noexcept {OWNER_OR_RETURN;if(!id)return;if(x.program_count==MaxPrograms){x.LoseMetadata();return;}if(x.generation==Max){x.Stop();return;}x.programs[x.program_count++]={id,++x.generation,0,0,false,false,false};}
void Recorder::Link(std::uint32_t id) noexcept {OWNER_OR_RETURN;if(auto* p=x.FindProgram(id)){if(x.link==Max){x.Stop();return;}p->link=++x.link;p->draws=0;p->link_known=false;p->linked=false;}else x.Unknown();}

void Recorder::Use(std::uint32_t id) noexcept {OWNER_OR_RETURN;x.requested_program=id;x.current=id?x.FindProgram(id):nullptr;if(id&&!x.current)x.Unknown();}
void Recorder::DeleteProgram(std::uint32_t id) noexcept {OWNER_OR_RETURN;if(auto* p=x.FindProgram(id))p->deleted=true;else if(id)x.Unknown();}
void Recorder::BindVAO(std::uint32_t id) noexcept {OWNER_OR_RETURN;x.vao=id;}
void Recorder::BindFramebuffer(std::uint32_t target,std::uint32_t id) noexcept {OWNER_OR_RETURN;if(target==0x8d40||target==0x8ca9)x.framebuffer=id;}
#undef OWNER_OR_RETURN
} // namespace OpenGL::HostLatency
