#include "video_core/renderer_opengl/gl_backlog_profile.h"
#include "video_core/renderer_opengl/gl_shared_vertex_shadow.h"
#include "gl_command_queue.h"
#include "gl_pipeline_capture.h"
#include "gl_host_latency.h"
#include "gl_driver_transactions.h"
#include "video_core/renderer_opengl/gl_state.h"
#include "common/frame_timing.h"
#include "common/logging/log.h"
#include <condition_variable>
#include <mutex>
#include <ctime>
#include <chrono>
#include <cstdlib>
#include "common/host_render_queue.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
namespace OpenGL::CommandQueue {
namespace {
static_assert(unsigned(DriverTransactions::Kind::Count)==TransactionAttribution::Kinds);
TransactionAttribution::Recorder transaction_attribution;
constexpr std::size_t MaxBatchCommands=64, MaxBatchBytes=256*1024;
std::atomic<Common::HostRenderQueue*> owner{nullptr};
std::thread::id producer;
std::vector<std::function<void()>> pending;
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
std::array<Backlog::Tag, MaxBatchCommands> pending_tags{};
std::atomic<std::int64_t> wait_start_ns{};
std::atomic<unsigned> wait_tag{};
std::unordered_map<std::uint64_t, Backlog::Row> worker_backlog;

std::int64_t ProfileNowNs() {
 return std::chrono::duration_cast<std::chrono::nanoseconds>(
     std::chrono::steady_clock::now().time_since_epoch()).count();
}
unsigned TagIndex(Backlog::Tag tag) {
 return ((static_cast<unsigned>(tag.source) * static_cast<unsigned>(RpcApiCount) + static_cast<unsigned>(tag.api)) << 16) | tag.entry;
}
void RunProfiled(Backlog::Tag command, std::function<void()>& job) {
 const auto pending_at_entry = wait_start_ns.load(std::memory_order_acquire);
 const auto begin = pending_at_entry ? ProfileNowNs() : 0;
 job();
 const auto end = ProfileNowNs();
 const auto wait = wait_start_ns.load(std::memory_order_acquire);
 if (!wait || end <= wait) return;
 const auto request = wait_tag.load(std::memory_order_relaxed);
 const std::uint64_t key = (std::uint64_t(request) << 32) | TagIndex(command);
 auto& row = worker_backlog[key];
 row.request = {static_cast<Backlog::Source>((request >> 16) / RpcApiCount), static_cast<RpcApi>((request >> 16) % RpcApiCount), Backlog::NoEntry};
 row.command = command;
 ++row.calls;
 row.ns += end - std::max(wait, begin);
}
#endif
struct PayloadLease;std::shared_ptr<PayloadLease> pending_slots;
std::size_t pending_bytes=0;
struct AtomicStats {
 std::atomic<std::uint64_t> recorded_commands{0},submitted_batches{0},synchronous_calls{0};
 std::atomic<std::uint64_t> owned_payload_bytes{0},conservative_pointer_rpcs{0},texture_pointer_rpcs{0};
 std::atomic<std::uint64_t> texture_owned_uploads{0},texture_pbo_offsets{0},client_array_rpcs{0};
 std::atomic<std::uint64_t> pending_commands{0},pending_bytes{0},worker_inline_calls{0};
 std::atomic<std::uint64_t> owned_native_map_uploads{0},transaction_inline_calls{0},transaction_sync_calls{0},transaction_state_fallbacks{0},transaction_lut_calls{0},owned_unmap_error_queries{0},owned_budget_waits{0};
 std::atomic<std::uint64_t> worker_rejected_calls{0},uniform_location_cache_hits{0},attrib_location_cache_hits{0},uniform_block_index_cache_hits{0};
} stats;
std::array<std::atomic<std::uint64_t>,RpcApiCount> rpc_counts{};
struct StringHash {using is_transparent=void;std::size_t operator()(std::string_view s) const noexcept{return std::hash<std::string_view>{}(s);}};
template<class T> using QueryNames=std::unordered_map<std::string,T,StringHash,std::equal_to<>>;
struct ProgramQueries {bool linked=false;QueryNames<GLint> uniforms,attributes;QueryNames<GLuint> blocks;};
std::unordered_map<GLuint,ProgramQueries> programs;
void InvalidateProgram(GLuint id){const auto p=programs.find(id);if(p!=programs.end())p->second=ProgramQueries{};}
// Successor deliberately disables immutable-query memoization. A synchronous
// helper can create/delete/relink private program names through raw GL, so never
// return an old numeric-name location. Every original query reaches the driver.
ProgramQueries* LinkedProgram(GLuint,const GLchar*) {return nullptr;}

void CountRpc(RpcApi api){++rpc_counts[std::size_t(api)];}

// Global owned-payload budget includes producer copies/reservations plus pending,
// queued and executing payloads. Command metadata is separately bounded64x128.
constexpr std::size_t OwnedBudgetLimit=8*1024*1024, OwnedCopyReserve=512*1024;
std::mutex budget_mutex;std::condition_variable budget_cv;std::size_t budget_live=0,budget_high=0,budget_waitable=0;
void ReserveBudget(std::size_t n) {
 if(n>OwnedBudgetLimit)throw std::invalid_argument("Oversized owned payload reservation");
 // Flush local commands before a capacity wait, so their worker can release bytes.
 {std::lock_guard lock(budget_mutex);if(n<=OwnedBudgetLimit-budget_live){budget_live+=n;budget_high=std::max(budget_high,budget_live);return;}}
 ++stats.owned_budget_waits;Flush();std::unique_lock lock(budget_mutex);
 // Producer-only active reservations cannot retire while this producer waits.
 // Reject rather than deadlock if queued/executing work cannot free enough space.
 budget_cv.wait(lock,[&]{return n<=OwnedBudgetLimit-budget_live || budget_waitable==0;});
 if(n>OwnedBudgetLimit-budget_live)throw std::length_error("Owned reservation capacity requires producer progress");
 budget_live+=n;budget_high=std::max(budget_high,budget_live);
}
void ReleaseBudget(std::size_t n,bool queued=false){std::lock_guard lock(budget_mutex);budget_live-=n;if(queued)budget_waitable-=n;budget_cv.notify_all();}
struct PayloadLease {
 std::size_t bytes;bool queued=false;explicit PayloadLease(std::size_t n):bytes(n){ReserveBudget(n);}
 void Shrink(std::size_t n){if(n>bytes)throw std::length_error("Owned capacity exceeded prior reservation");ReleaseBudget(bytes-n,queued);bytes=n;}
 void MarkQueued(){std::lock_guard lock(budget_mutex);queued=true;budget_waitable+=bytes;}
 ~PayloadLease(){ReleaseBudget(bytes,queued);}
};
thread_local std::shared_ptr<PayloadLease> copy_reservation;
struct OwnedCopyScope {
 OwnedCopyScope(){if(copy_reservation)throw std::logic_error("Nested owned copy");copy_reservation=std::make_shared<PayloadLease>(OwnedCopyReserve);}
 ~OwnedCopyScope(){copy_reservation.reset();}
};
// Destruction clears owned closures BEFORE returning their budget on failure/cancel.
struct BudgetedJob {
 std::shared_ptr<PayloadLease> lease;std::function<void()> job;
 ~BudgetedJob(){job={};lease.reset();}
};
struct Attribute { bool enabled=false,client=true; };
struct VAO { GLuint element=0; std::unordered_map<GLuint,Attribute> attributes; };
std::unordered_map<GLuint,VAO> vaos;
std::unordered_set<GLuint> known_vaos,known_buffers;
GLuint current_vao=0,array_buffer=0,unpack_buffer=0;
GLint max_attributes=16;
bool transform_feedback_active=false;
struct Unpack { GLint alignment=4,row_length=0,skip_rows=0,skip_pixels=0; } unpack;
Common::HostRenderQueue& Owner() {
 auto* q=owner.load(std::memory_order_acquire);
 if(!q) throw std::logic_error("GL command queue is not installed");
 return *q;
}
void Producer() {
 if(std::this_thread::get_id()!=producer) throw std::logic_error("GL recorder requires its sole producer thread");
}
bool ClientArrays() {
 for(const auto& x:vaos[current_vao].attributes) if(x.second.enabled && x.second.client) return true;
 return false;
}
void Record(std::function<void()> job,std::size_t bytes=0,RpcApi api=RpcApi::Internal) {
 auto& q=Owner();
 if(q.IsWorkerThread()) { ++stats.worker_rejected_calls;throw std::logic_error("GL recorder requires producer; backend uses captured raw entrypoints"); }
 Producer();
 if(bytes>MaxBatchBytes-sizeof(std::function<void()>)) {
  // Oversize synchronous execution keeps an already-owned payload alive, without
  // bypassing byte admission with an oversized asynchronous job.
  Flush();++stats.synchronous_calls;CountRpc(api);q.Call(std::move(job));return;
 }
 const auto footprint=bytes+sizeof(std::function<void()>);
 if(!pending.empty() && (pending.size()>=MaxBatchCommands || pending_bytes>MaxBatchBytes-footprint)) Flush();
 if(pending.empty())pending_slots=std::make_shared<PayloadLease>(MaxBatchCommands*sizeof(std::function<void()>));
 if(bytes) {
   if(!copy_reservation)throw std::logic_error("Owned payload lacks pre-copy reservation");
   copy_reservation->Shrink(bytes);copy_reservation->MarkQueued();
   auto holder=std::make_shared<BudgetedJob>();holder->lease=std::move(copy_reservation);holder->job=std::move(job);
   job=[holder]{holder->job();};
 }
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
 if (Common::FrameTiming::counters.enabled && Backlog::Enabled()) {
  const bool draw = api == RpcApi::api_glDrawArrays || api == RpcApi::api_glDrawElements ||
                    api == RpcApi::api_glDrawArraysInstanced || api == RpcApi::api_glDrawElementsInstanced ||
                    api == RpcApi::api_glDrawRangeElementsBaseVertex;
  pending_tags[pending.size()] = {Backlog::source, api, draw ? Backlog::vertex_entry : Backlog::NoEntry};
 }
#endif
 pending.emplace_back(std::move(job));pending_bytes+=footprint;
 if(pending.size()==1)pending_slots->MarkQueued();
 ++stats.recorded_commands;stats.owned_payload_bytes+=bytes;
 stats.pending_commands=pending.size();stats.pending_bytes=pending_bytes;
 if(pending.size()>=MaxBatchCommands || pending_bytes>=MaxBatchBytes) Flush();
}
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
using SyncClock = std::chrono::steady_clock;
thread_local std::array<SyncTiming, RpcApiCount> sync_timings{};
struct StampOnReturn {
 SyncClock::time_point& output;
 ~StampOnReturn() { output = SyncClock::now(); }
};
template<class F> decltype(auto) ProfileSync(Common::HostRenderQueue& queue, RpcApi api, F&& fn) {
 // The promise is fulfilled after the worker scope ends. Call returns or throws
 // before the producer's stack is released, including when a queued job is cancelled.
 auto& timing = sync_timings[static_cast<std::size_t>(api)];
 SyncClock::time_point entered{}, returned{};
 const auto start = SyncClock::now();
 const bool backlog = Backlog::Enabled();
 if (backlog) {
  wait_tag.store(TagIndex({Backlog::source, api}), std::memory_order_relaxed);
  wait_start_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count(),
                      std::memory_order_release);
 }
 struct ClearWait {
  bool enabled;
  ~ClearWait() { if (enabled) wait_start_ns.store(0, std::memory_order_release); }
 } clear{backlog};
 Flush();
 struct Account {
  SyncTiming& timing;
  const SyncClock::time_point &start, &entered, &returned;
  ~Account() {
   if (entered == SyncClock::time_point{}) return;
   using std::chrono::duration_cast;
   using std::chrono::nanoseconds;
   ++timing.count;
   timing.before_driver_ns += duration_cast<nanoseconds>(entered - start).count();
   timing.in_driver_ns += duration_cast<nanoseconds>(returned - entered).count();
   timing.after_driver_ns += duration_cast<nanoseconds>(SyncClock::now() - returned).count();
  }
 } account{timing, start, entered, returned};
 return queue.Call([&]() -> decltype(auto) {
  entered = SyncClock::now();
  StampOnReturn stamp{returned};
  return fn();
 });
}
#endif
template<class F> decltype(auto) Sync(F&& f,bool pointer=false,RpcApi api=RpcApi::Internal) {
 auto& q=Owner();
 if(q.IsWorkerThread()) { ++stats.worker_rejected_calls;throw std::logic_error("GL recorder requires producer; backend uses captured raw entrypoints"); }
 Producer();++stats.synchronous_calls;CountRpc(api);
 if(pointer) ++stats.conservative_pointer_rpcs;
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
 if (Common::FrameTiming::counters.enabled)
  return ProfileSync(q, api, std::forward<F>(f));
#endif
 Flush();
 return q.Call(std::forward<F>(f));
}
template<class F> decltype(auto) SyncApi(RpcApi api,F&& fn,bool pointer=false){return Sync(std::forward<F>(fn),pointer,api);}
void RecordApi(RpcApi api,std::function<void()> fn,std::size_t bytes=0){Record(std::move(fn),bytes,api);}
bool Small(std::size_t count,std::size_t unit,std::size_t& size) {
 if(unit && count>(MaxBatchBytes-sizeof(std::function<void()>))/unit) return false;
 size=count*unit;return true;
}
std::vector<std::uint8_t> Copy(const void* p,std::size_t bytes) {
 std::vector<std::uint8_t> value(bytes);if(bytes) std::memcpy(value.data(),p,bytes);return value;
}
bool Mul(std::size_t a,std::size_t b,std::size_t& v) {
 if(b && a>std::numeric_limits<std::size_t>::max()/b)return false;v=a*b;return true;
}
bool Add(std::size_t a,std::size_t b,std::size_t& v) {
 if(a>std::numeric_limits<std::size_t>::max()-b)return false;v=a+b;return true;
}
std::size_t PixelBytes(GLenum format,GLenum type) {
 unsigned channels=0;
 switch(format) {
  case GL_RED:case GL_RED_INTEGER:case GL_DEPTH_COMPONENT:case GL_STENCIL_INDEX:channels=1;break;
  case GL_RG:case GL_RG_INTEGER:case GL_DEPTH_STENCIL:channels=2;break;
  case GL_RGB:case GL_BGR:case GL_RGB_INTEGER:case GL_BGR_INTEGER:channels=3;break;
  case GL_RGBA:case GL_BGRA:case GL_RGBA_INTEGER:case GL_BGRA_INTEGER:channels=4;break;
  default:return 0;
 }
 switch(type) {
  case GL_BYTE:case GL_UNSIGNED_BYTE:return channels;
  case GL_SHORT:case GL_UNSIGNED_SHORT:case GL_HALF_FLOAT:return channels*2;
  case GL_INT:case GL_UNSIGNED_INT:case GL_FLOAT:return channels*4;
  case GL_DOUBLE:return channels*8;
  case GL_UNSIGNED_BYTE_3_3_2:case GL_UNSIGNED_BYTE_2_3_3_REV:return channels==3?1:0;
  case GL_UNSIGNED_SHORT_5_6_5:case GL_UNSIGNED_SHORT_5_6_5_REV:return channels==3?2:0;
  case GL_UNSIGNED_SHORT_4_4_4_4:case GL_UNSIGNED_SHORT_4_4_4_4_REV:
  case GL_UNSIGNED_SHORT_5_5_5_1:case GL_UNSIGNED_SHORT_1_5_5_5_REV:return channels==4?2:0;
  case GL_UNSIGNED_INT_8_8_8_8:case GL_UNSIGNED_INT_8_8_8_8_REV:
  case GL_UNSIGNED_INT_10_10_10_2:case GL_UNSIGNED_INT_2_10_10_10_REV:return channels==4?4:0;
  case GL_UNSIGNED_INT_10F_11F_11F_REV:case GL_UNSIGNED_INT_5_9_9_9_REV:return channels==3?4:0;
  case GL_UNSIGNED_INT_24_8:return format==GL_DEPTH_STENCIL?4:0;
  case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:return format==GL_DEPTH_STENCIL?8:0;
  default:return 0;
 }
}
bool TextureSpan(GLsizei width,GLsizei height,GLenum format,GLenum type,std::size_t& bytes) {
 if(width<0||height<0||unpack.row_length<0||unpack.skip_rows<0||unpack.skip_pixels<0)return false;
 const auto bpp=PixelBytes(format,type);if(!bpp)return false;
 if(width==0||height==0){bytes=0;return true;}
 std::size_t row=0,stride=0,skip=0,skip_pixels=0,body=0,last=0;
 const auto alignment=std::size_t(unpack.alignment);
 if(alignment!=1&&alignment!=2&&alignment!=4&&alignment!=8)return false;
 if(!Mul(unpack.row_length?std::size_t(unpack.row_length):std::size_t(width),bpp,row) ||
    !Add(row,alignment-1,stride))return false;
 stride=(stride/alignment)*alignment;
 if(!Mul(std::size_t(unpack.skip_rows),stride,skip)||!Mul(std::size_t(unpack.skip_pixels),bpp,skip_pixels)||
    !Add(skip,skip_pixels,skip)||!Mul(std::size_t(height-1),stride,body)||
    !Mul(std::size_t(width),bpp,last)||!Add(skip,body,bytes)||!Add(bytes,last,bytes))return false;
 return bytes<=MaxBatchBytes-sizeof(std::function<void()>);
}
// Raw pointers and wrappers are generated from the vendored GLAD declarations.
#include "gl_command_queue_generated.inc"
// Diagnostic-only raw adapters: invoked by the existing driver-owner calls.
std::unique_ptr<PipelineCapture::Recorder> pipeline_capture;
std::unique_ptr<HostLatency::Recorder> host_latency;
#define PIPELINE_SLOTS(X) \
 X(glCreateShader) X(glCreateProgram) X(glShaderSource) X(glCompileShader) \
 X(glGetShaderiv) X(glAttachShader) X(glDetachShader) X(glLinkProgram) \
 X(glGetProgramiv) X(glUseProgram) X(glDeleteShader) X(glDeleteProgram) \
 X(glBindVertexArray) X(glBindFramebuffer) X(glDrawArrays) X(glDrawElements) X(glDrawRangeElementsBaseVertex)
#define SAVE_SLOT(name) static decltype(raw_##name) capture_original_##name=nullptr;
PIPELINE_SLOTS(SAVE_SLOT)
#undef SAVE_SLOT
// Templates instantiate fixed stack lambdas; no std::function, heap allocation, or file append here.
template<class F> void CaptureObserved(HostLatency::CaptureKind kind,F&& fn) {
 if(!pipeline_capture)return;
 HostLatency::Token t;if(host_latency)t=host_latency->Begin(HostLatency::Api::Capture,0,std::uint32_t(kind));
 fn();const auto end=t.active?HostLatency::ReadHostTick():0;if(host_latency)host_latency->End(t,end);
}
template<class F> void DriverTimed(HostLatency::Api api,GLuint object,GLenum pname,GLenum mode,GLsizei count,F&& fn) {
 HostLatency::Token t;if(host_latency)t=host_latency->Begin(api,object,pname,mode,count);
 fn();const auto end=t.active?HostLatency::ReadHostTick():0;if(host_latency)host_latency->End(t,end);
}
static GLuint APIENTRY Capture_glCreateShader(GLenum type){const auto id=capture_original_glCreateShader(type);if(host_latency)host_latency->CreateShader(id,type);CaptureObserved(HostLatency::CaptureKind::Other,[&]{pipeline_capture->CreateShader(id,type);});return id;}
static GLuint APIENTRY Capture_glCreateProgram(){const auto id=capture_original_glCreateProgram();if(host_latency)host_latency->CreateProgram(id);CaptureObserved(HostLatency::CaptureKind::Other,[&]{pipeline_capture->CreateProgram(id);});return id;}
static void APIENTRY Capture_glShaderSource(GLuint id,GLsizei count,const GLchar* const* strings,const GLint* lengths){if(host_latency)host_latency->Source(id);CaptureObserved(HostLatency::CaptureKind::Source,[&]{pipeline_capture->Source(id,count,strings,lengths);});capture_original_glShaderSource(id,count,strings,lengths);}
static void APIENTRY Capture_glCompileShader(GLuint id){CaptureObserved(HostLatency::CaptureKind::Compile,[&]{pipeline_capture->Compile(id);});DriverTimed(HostLatency::Api::Compile,id,0,0,0,[&]{capture_original_glCompileShader(id);});}
static void APIENTRY Capture_glGetShaderiv(GLuint id,GLenum pname,GLint* value){HostLatency::Token t;if(host_latency)t=host_latency->Begin(HostLatency::Api::ShaderQuery,id,pname);capture_original_glGetShaderiv(id,pname,value);const auto end=t.active?HostLatency::ReadHostTick():0;const bool copy=value&&host_latency&&host_latency->CanCopyStatus(t);if(host_latency)host_latency->End(t,end,copy?*value:0,copy);if(pname==GL_COMPILE_STATUS&&value)CaptureObserved(HostLatency::CaptureKind::Status,[&]{pipeline_capture->ShaderStatus(id,*value);});}
static void APIENTRY Capture_glAttachShader(GLuint p,GLuint shader){capture_original_glAttachShader(p,shader);CaptureObserved(HostLatency::CaptureKind::Other,[&]{pipeline_capture->Attach(p,shader,true);});}
static void APIENTRY Capture_glDetachShader(GLuint p,GLuint shader){capture_original_glDetachShader(p,shader);CaptureObserved(HostLatency::CaptureKind::Other,[&]{pipeline_capture->Attach(p,shader,false);});}
static void APIENTRY Capture_glLinkProgram(GLuint p){if(host_latency)host_latency->Link(p);CaptureObserved(HostLatency::CaptureKind::Link,[&]{pipeline_capture->Link(p);});DriverTimed(HostLatency::Api::Link,p,0,0,0,[&]{capture_original_glLinkProgram(p);});}
static void APIENTRY Capture_glGetProgramiv(GLuint id,GLenum pname,GLint* value){HostLatency::Token t;if(host_latency)t=host_latency->Begin(HostLatency::Api::ProgramQuery,id,pname);capture_original_glGetProgramiv(id,pname,value);const auto end=t.active?HostLatency::ReadHostTick():0;const bool copy=value&&host_latency&&host_latency->CanCopyStatus(t);if(host_latency)host_latency->End(t,end,copy?*value:0,copy);if(pname==GL_LINK_STATUS&&value)CaptureObserved(HostLatency::CaptureKind::Status,[&]{pipeline_capture->ProgramStatus(id,*value);});}
static void APIENTRY Capture_glUseProgram(GLuint p){capture_original_glUseProgram(p);if(host_latency)host_latency->Use(p);CaptureObserved(HostLatency::CaptureKind::Other,[&]{pipeline_capture->Use(p);});}
static void APIENTRY Capture_glDeleteShader(GLuint id){capture_original_glDeleteShader(id);if(host_latency)host_latency->DeleteShader(id);CaptureObserved(HostLatency::CaptureKind::Other,[&]{pipeline_capture->DeleteShader(id);});}
static void APIENTRY Capture_glDeleteProgram(GLuint id){capture_original_glDeleteProgram(id);if(host_latency)host_latency->DeleteProgram(id);CaptureObserved(HostLatency::CaptureKind::Other,[&]{pipeline_capture->DeleteProgram(id);});}
static void APIENTRY Capture_glBindVertexArray(GLuint id){capture_original_glBindVertexArray(id);if(host_latency)host_latency->BindVAO(id);CaptureObserved(HostLatency::CaptureKind::Other,[&]{pipeline_capture->BindVAO(id);});}
static void APIENTRY Capture_glBindFramebuffer(GLenum target,GLuint id){capture_original_glBindFramebuffer(target,id);if(host_latency)host_latency->BindFramebuffer(target,id);CaptureObserved(HostLatency::CaptureKind::Other,[&]{pipeline_capture->BindFramebuffer(target,id);});}
static void APIENTRY Capture_glDrawArrays(GLenum mode,GLint first,GLsizei count){CaptureObserved(HostLatency::CaptureKind::DrawBefore,[&]{pipeline_capture->Draw(1,mode,first,count,0,0);});DriverTimed(HostLatency::Api::DrawArrays,0,0,mode,count,[&]{capture_original_glDrawArrays(mode,first,count);});CaptureObserved(HostLatency::CaptureKind::DrawReturned,[&]{pipeline_capture->DrawReturned();});}
static void APIENTRY Capture_glDrawElements(GLenum mode,GLsizei count,GLenum type,const void* indices){CaptureObserved(HostLatency::CaptureKind::DrawBefore,[&]{pipeline_capture->Draw(2,mode,0,count,type,reinterpret_cast<std::uintptr_t>(indices));});DriverTimed(HostLatency::Api::DrawElements,0,0,mode,count,[&]{capture_original_glDrawElements(mode,count,type,indices);});CaptureObserved(HostLatency::CaptureKind::DrawReturned,[&]{pipeline_capture->DrawReturned();});}
static void APIENTRY Capture_glDrawRangeElementsBaseVertex(GLenum mode,GLuint start,GLuint end,GLsizei count,GLenum type,const void* indices,GLint base){CaptureObserved(HostLatency::CaptureKind::DrawBefore,[&]{pipeline_capture->Draw(3,mode,0,count,type,reinterpret_cast<std::uintptr_t>(indices),start,end,base);});DriverTimed(HostLatency::Api::DrawRange,0,0,mode,count,[&]{capture_original_glDrawRangeElementsBaseVertex(mode,start,end,count,type,indices,base);});CaptureObserved(HostLatency::CaptureKind::DrawReturned,[&]{pipeline_capture->DrawReturned();});}
void InstallPipelineCapture(){
 const bool capture=PipelineCapture::Requested(),timing=HostLatency::Requested();
 if(!capture&&!timing)return;
 if(capture){const char* path=std::getenv("CARA_GL_PIPELINE_CAPTURE_DIR");try{pipeline_capture=std::make_unique<PipelineCapture::Recorder>(path?path:"");}catch(...){std::fprintf(stderr,"CARA pipeline capture allocation unavailable\n");}}
 if(timing){const char* path=std::getenv("CARA_GL_HOST_LATENCY_DIR");try{host_latency=std::make_unique<HostLatency::Recorder>(path?path:"");}catch(...){std::fprintf(stderr,"CARA host latency allocation unavailable\n");}}
 if(!pipeline_capture&&!host_latency)return;
#define INSTALL_SLOT(name) capture_original_##name=raw_##name;if(raw_##name)raw_##name=&Capture_##name;
 PIPELINE_SLOTS(INSTALL_SLOT)
#undef INSTALL_SLOT
}
void UninstallPipelineCapture(){
 if(!pipeline_capture&&!host_latency)return;
#define RESTORE_SLOT(name) raw_##name=capture_original_##name;
 PIPELINE_SLOTS(RESTORE_SLOT)
#undef RESTORE_SLOT
 pipeline_capture.reset();host_latency.reset();
}
#undef PIPELINE_SLOTS

bool TransactionBool(DriverTransactions::Kind kind,const std::function<bool()>& body) {
 Producer();if(Common::FrameTiming::counters.enabled)throw std::logic_error("GL transactions require timing counters OFF");
 // Full Apply helpers restore the cached VAO/array buffer. If a raw caller made
 // that cache differ from the recorder, keep the original producer scalar path
 // so its binding/VAO metadata sees every mutation. Blit touches neither field.
 const auto tracked=OpenGLState::GetCurState();
 if(kind!=DriverTransactions::Kind::Blit &&
    (tracked.draw.vertex_array!=current_vao||tracked.draw.vertex_buffer!=array_buffer)) {
   ++stats.transaction_state_fallbacks;DriverTransactions::ExecutionScope bypass;
   if(transaction_attribution.value.enabled)
     return transaction_attribution.Fallback(unsigned(kind),body);
   return body();
 }
 if(transaction_attribution.value.enabled) {
   return transaction_attribution.Sync(unsigned(kind),[]{Flush();},
     []{++stats.synchronous_calls;CountRpc(RpcApi::Internal);++stats.transaction_sync_calls;},
     [](auto& wrapped){return Owner().Call(wrapped);},
     [&]{DriverTransactions::ExecutionScope token;return body();});
 }
 Flush();++stats.synchronous_calls;CountRpc(RpcApi::Internal);++stats.transaction_sync_calls;
 return Owner().Call([&]{DriverTransactions::ExecutionScope token;return body();});
}
void TransactionVoid(DriverTransactions::Kind kind,const std::function<void()>& body) {
 (void)TransactionBool(kind,[&]{body();return true;});
}
void TransactionLUT(DriverTransactions::LUTBody body,std::uint32_t texture,unsigned unit,
 std::uint32_t format,std::size_t offset,const std::uint8_t* data,std::size_t bytes) {
 Producer();if(!bytes){if(transaction_attribution.value.enabled)transaction_attribution.Add(transaction_attribution.value.empty_lut);return;}
 const unsigned stride=format==GL_RG?8:16;
 if((format!=GL_RG&&format!=GL_RGBA)||!data||offset%stride||bytes%stride)
   throw std::invalid_argument("Invalid CPU LUT upload span");
 if(bytes>MaxBatchBytes-sizeof(std::function<void()>)) {
#ifdef CARA_GL_CONTEXT_DISPATCH
   Sync([&] {
     DriverTransactions::ExecutionScope executing;
     body(texture, unit, format, offset, data, bytes);
   }, true);
#else
   TransactionVoid(DriverTransactions::Kind::LUT,[&]{body(texture,unit,format,offset,data,bytes);});
#endif
   return;
 }
 OwnedCopyScope reservation;auto owned=Copy(data,bytes);const auto charge=owned.capacity();
 Record([=,owned=std::move(owned)]{DriverTransactions::ExecutionScope token;
   body(texture,unit,format,offset,owned.data(),owned.size());},charge,RpcApi::UploadLUTOwned);
 ++stats.transaction_lut_calls;
 if(transaction_attribution.value.enabled)transaction_attribution.Add(transaction_attribution.value.async_lut);
}
bool UncheckedBlitProducerGate() {
 Producer();
 if(Common::FrameTiming::counters.enabled)
  throw std::logic_error("GL transactions require timing counters OFF");
 return UncheckedBlitProducerRequested();
}
const DriverTransactions::Hooks transaction_hooks{TransactionBool,TransactionVoid,TransactionLUT};
} // namespace
void Install(Common::HostRenderQueue& queue) {
 if(owner.load())throw std::logic_error("GL command queue already installed");
 const char* diagnostics=std::getenv("CARA_NATIVE_DIAGNOSTICS");
 const char* attribution=std::getenv("CARA_GL_TRANSACTION_ATTRIBUTION");
 const bool attribution_requested=attribution&&std::strcmp(attribution,"1")==0;
 if(attribution_requested&&(!diagnostics||std::strcmp(diagnostics,"1")!=0))
   throw std::invalid_argument("Transaction attribution requires diagnostic mode");
 if(UncheckedBlitProducerRequested()&&(!diagnostics||std::strcmp(diagnostics,"1")!=0))
   throw std::invalid_argument("Unchecked blit producer recording requires CARA_NATIVE_DIAGNOSTICS=1");
 // Native owned-map upload is an explicit supported queue feature. Its actual
 // API, owner-thread, FIFO, byte-budget and null-map guards remain mandatory.
 {std::lock_guard lock(budget_mutex);if(budget_live)throw std::logic_error("Leaked payload reservation at Install");budget_high=0;}

 if(queue.IsWorkerThread())throw std::logic_error("Install must run on the producer after GLAD load");
 producer=std::this_thread::get_id();pending.clear();pending.reserve(MaxBatchCommands);pending_bytes=0;
 vaos.clear();vaos.emplace(0,VAO{});known_vaos.clear();known_buffers.clear();current_vao=array_buffer=unpack_buffer=0;unpack={};
 CaptureAndInstall();
 InstallPipelineCapture();
 try {
   ++stats.synchronous_calls;CountRpc(RpcApi::InstallMaxAttributes);
   // Seed actual bindings/unpack state once; do not assume a fresh context's
   // defaults or let a raw startup caller invalidate pointer-span accounting.
   queue.Call([]{
     if(host_latency)host_latency->BindOwnerThread();
     raw_glGetIntegerv(GL_MAX_VERTEX_ATTRIBS,&max_attributes);
     if(max_attributes<=0||max_attributes>256)throw std::runtime_error("Unsupported actual GL_MAX_VERTEX_ATTRIBS");
     GLint value=0;raw_glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&value);current_vao=GLuint(value);
     raw_glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&value);array_buffer=GLuint(value);
     raw_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&value);unpack_buffer=GLuint(value);
     raw_glGetIntegerv(GL_UNPACK_ALIGNMENT,&unpack.alignment);
     raw_glGetIntegerv(GL_UNPACK_ROW_LENGTH,&unpack.row_length);
     raw_glGetIntegerv(GL_UNPACK_SKIP_ROWS,&unpack.skip_rows);
     raw_glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&unpack.skip_pixels);
     if(current_vao) {
       if(!raw_glGetVertexAttribiv)throw std::runtime_error("Cannot inspect initial VAO attributes");
       known_vaos.insert(current_vao);auto& vao=vaos[current_vao];
       raw_glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&value);vao.element=GLuint(value);
       if(vao.element)known_buffers.insert(vao.element);
       for(GLuint i=0;i<GLuint(max_attributes);++i) {
         raw_glGetVertexAttribiv(i,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&value);vao.attributes[i].enabled=value!=0;
         raw_glGetVertexAttribiv(i,GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&value);vao.attributes[i].client=value==0;
         if(value)known_buffers.insert(GLuint(value));
       }
     }
     if(array_buffer)known_buffers.insert(array_buffer);if(unpack_buffer)known_buffers.insert(unpack_buffer);
   });
 }catch(...){UninstallPipelineCapture();RestoreEntrypoints();throw;}
 programs.clear();
 transform_feedback_active=false;owner.store(&queue,std::memory_order_release);
 DriverTransactions::unchecked_blit_commands_recorded.store(0,std::memory_order_relaxed);
 transaction_attribution.BeginInstall(attribution_requested);
 // Android keeps mutable renderer state and its profiling counters on the producer.
 // Only owned GL driver calls may run on the worker in this experiment.
#ifndef CARA_GL_CONTEXT_DISPATCH
 DriverTransactions::hooks.store(&transaction_hooks,std::memory_order_release);
 DriverTransactions::unchecked_blit_producer_gate.store(
     UncheckedBlitProducerRequested()?&UncheckedBlitProducerGate:nullptr,std::memory_order_release);
#endif
}
#ifdef CARA_GL_CONTEXT_DISPATCH
#ifdef __ANDROID__
// Publish one immutable snapshot to both the FIFO driver call and the resident
// upload cache. Both can outlive the guest input and release it independently.
bool TryQueueVertexUpload(GLsizei width, GLsizei height, GLenum format, GLenum type,
                          const void* pixels, std::size_t bytes, SharedVertexShadow& retained) {
    static const bool requested = [] {
        const char* value = std::getenv("CARA_SHARED_VERTEX_UPLOAD");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!requested || !Enabled()) return false;
    Producer();
    std::size_t span = 0;
    if (unpack_buffer || !pixels || !bytes || bytes > MaxBatchBytes - sizeof(std::function<void()>) ||
        !TextureSpan(width, height, format, type, span) || span != bytes) return false;
    OwnedCopyScope reservation;
    SharedVertexShadow snapshot;
    const auto* first = static_cast<const std::uint8_t*>(pixels);
    snapshot.assign(first, first + bytes);
    auto lease = snapshot.CaptureLease(MaxBatchBytes - sizeof(std::function<void()>));
    if (!lease) return false;
    const auto charge = lease->capacity();
    RecordApi(RpcApi::api_glTexSubImage2D, [=, data = std::move(lease)] {
        raw_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, type, data->data());
    }, charge);
    retained = std::move(snapshot);
    ++stats.texture_owned_uploads;
    return true;
}

bool MailboxFencePacketEnabled() {
    static const bool requested = [] {
        const char* value = std::getenv("CARA_MAILBOX_FENCE_PACKET");
        return value && std::strcmp(value, "1") == 0;
    }();
    return requested && Enabled();
}

void QueueConsumedPresentFence(GLsync fence, bool server_wait_only) {
    // The old owner handed this handle to the render context. Only that
    // context consumes the wait result. All subsequent frame commands follow.
    RecordApi(RpcApi::ConsumePresentFence, [fence, server_wait_only] {
        if (!server_wait_only) {
            raw_glClientWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
        }
        raw_glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
        raw_glDeleteSync(fence);
    });
}

void QueuePublishedRenderFence(std::function<void(GLsync)> publish) {
    RecordApi(RpcApi::PublishRenderFence, [publish = std::move(publish)] {
        const auto fence = raw_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!fence) {
            throw std::runtime_error("Could not create the mailbox render fence");
        }
        raw_glFlush();
        publish(fence);
    });
    Flush();
}

GLsync CreatePublishedRenderFence() {
    return SyncApi(RpcApi::PublishRenderFence, [] {
        const auto fence = raw_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        raw_glFlush();
        return fence;
    });
}

struct RemapState {
    GLint active_texture = GL_TEXTURE0;
    GLboolean dither = GL_FALSE;
    GLboolean discard = GL_FALSE;
};

std::shared_ptr<RemapState> SaveRemapStateIfEnabled() {
    static const bool requested = [] {
        const char* value = std::getenv("CARA_CHECKED_REMAP_PACKET");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!requested || !Enabled()) return {};
    auto state = std::make_shared<RemapState>();
    RecordApi(RpcApi::SaveRemapState, [state] {
        raw_glGetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture);
        state->dither = raw_glIsEnabled(GL_DITHER);
        state->discard = raw_glIsEnabled(GL_RASTERIZER_DISCARD);
    });
    return state;
}

void RestoreRemapState(std::shared_ptr<RemapState> state) {
    RecordApi(RpcApi::RestoreRemapState, [state = std::move(state)] {
        if (state->dither) raw_glEnable(GL_DITHER);
        if (state->discard) raw_glEnable(GL_RASTERIZER_DISCARD);
        raw_glActiveTexture(state->active_texture);
    });
}

bool CheckedRemapDraw(std::function<bool()> draw) {
    return SyncApi(RpcApi::CheckedRemapDraw, std::move(draw));
}
#endif
void QueueLUT(DriverTransactions::LUTBody body, GLuint texture, unsigned unit, GLenum format,
              std::size_t offset, const std::uint8_t* data, std::size_t bytes) {
    TransactionLUT(body, texture, unit, format, offset, data, bytes);
}
#endif
void Flush() {
 auto* active=owner.load();if(!active)return;auto& q=*active;if(q.IsWorkerThread())throw std::logic_error("GL Flush requires producer");Producer();if(pending.empty())return;
 struct Batch {
   std::shared_ptr<PayloadLease> slots;std::vector<std::function<void()>> jobs;
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
   std::vector<Backlog::Tag> tags;
#endif
   ~Batch(){jobs.clear();slots.reset();}
 };
 auto batch=std::make_shared<Batch>();batch->slots=std::move(pending_slots);batch->jobs=std::move(pending);
 batch->slots->Shrink(batch->jobs.size()*sizeof(std::function<void()>));
 const auto bytes=pending_bytes;pending={};pending.reserve(MaxBatchCommands);pending_bytes=0;
 stats.pending_commands=0;stats.pending_bytes=0;
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
 if (Common::FrameTiming::counters.enabled && Backlog::Enabled()) {
  batch->tags.assign(pending_tags.begin(), pending_tags.begin() + batch->jobs.size());
  q.Submit([batch] {
   for (std::size_t i = 0; i < batch->jobs.size(); ++i)
    RunProfiled(batch->tags[i], batch->jobs[i]);
  }, bytes);
  ++stats.submitted_batches;
  return;
 }
#endif
 q.Submit([batch] { for(auto& job:batch->jobs)job(); },bytes);++stats.submitted_batches;
}
void Drain() {auto* active=owner.load();if(!active)return;auto& q=*active;if(q.IsWorkerThread())throw std::logic_error("GL Drain on worker");Flush();q.Drain();}
void SubmitHost(std::function<void()> job,std::size_t bytes) {auto& q=Owner();Producer();Flush();q.Submit(std::move(job),bytes);}
void Uninstall() {
 auto* q=owner.load();if(!q)return;Producer();
 std::exception_ptr failure;
 try{Drain();}catch(...){failure=std::current_exception();try{q->Stop();}catch(...){}}
 // On failure Stop's contract guarantees worker join before any raw restoration.
 DriverTransactions::unchecked_blit_producer_gate.store(nullptr,std::memory_order_release);
 DriverTransactions::hooks.store(nullptr,std::memory_order_release);
 UninstallPipelineCapture();RestoreEntrypoints();owner.store(nullptr,std::memory_order_release);pending.clear();pending_slots.reset();pending_bytes=0;
 stats.pending_commands=0;stats.pending_bytes=0;
 if(failure)std::rethrow_exception(failure);
}
bool ForwardsGetError(PFNGLGETERRORPROC expected){return Enabled()?raw_glGetError==expected&&glGetError==Wrap_glGetError:glGetError==expected;}
bool Enabled(){return owner.load(std::memory_order_acquire)!=nullptr;}
bool UncheckedBlitProducerRequested(){static const bool enabled=[] {const char* v=std::getenv("CARA_GL_QUEUE_UNCHECKED_BLIT_PRODUCER");return v&&std::strcmp(v,"1")==0;}();return enabled;}
UncheckedBlitProducerStats GetUncheckedBlitProducerStats(){return {
 UncheckedBlitProducerRequested(),Enabled()&&UncheckedBlitProducerRequested(),
 DriverTransactions::unchecked_blit_commands_recorded.load(std::memory_order_relaxed)};}
bool NativeMapUploadRequested(){static const bool enabled=[] {const char* v=std::getenv("CARA_GL_QUEUE_NATIVE_MAP_UPLOAD");return v&&std::strcmp(v,"1")==0;}();return enabled;}
bool NativeMapUploadActive(){return Enabled()&&NativeMapUploadRequested();}
Common::HostRenderQueue* Queue(){return owner.load(std::memory_order_acquire);}
TransactionAttribution::Snapshot GetTransactionAttribution(){
 Producer();return transaction_attribution.value;
}
std::uint64_t ReadWorkerCPUForProfile(){
 if(!Enabled())return 0;Producer();Flush();
 const auto result=Owner().Call([] {
     const auto ns = TransactionAttribution::HostClock(true);
     if (!ns) throw std::runtime_error("Worker CPU profile clock unavailable");
     return ns;
 });
 // Promise completion precedes job retirement; profiles require zero live jobs.
 Owner().Drain();return result;
}
std::shared_ptr<void> ReserveStreamPayload() {Producer();if(!Enabled())throw std::logic_error("Stream reservation without active queue");return std::make_shared<PayloadLease>(OwnedCopyReserve);}
void UploadBufferOwned(GLenum target,GLintptr offset,std::vector<std::uint8_t> bytes,bool orphan,GLsizeiptr fullsize,
 bool check_error,std::shared_ptr<void> reservation,GLsizeiptr map_reserved_size) {
 if(copy_reservation)throw std::logic_error("Nested stream upload reservation");
 copy_reservation=std::static_pointer_cast<PayloadLease>(std::move(reservation));
 struct ResetCopy {~ResetCopy(){copy_reservation.reset();}}reset_copy;
 if(offset<0||fullsize<0||bytes.size()>std::size_t(std::numeric_limits<GLsizeiptr>::max()) ||
    (orphan && (offset>fullsize || bytes.size()>std::size_t(fullsize-offset))))throw std::invalid_argument("Invalid owned GL buffer upload");
 const bool native_map=NativeMapUploadActive();
 // Preserve original Map reservation separately from the shorter written payload.
 if(native_map&&(map_reserved_size<=0||offset>fullsize||map_reserved_size>fullsize-offset||
                 bytes.size()>std::size_t(map_reserved_size)))
   throw std::invalid_argument("Invalid original native map reservation");
 auto map=raw_glMapBufferRange;auto flush=raw_glFlushMappedBufferRange;auto unmap=raw_glUnmapBuffer;
 if(native_map&&(!map||!unmap||(check_error&&!flush)))throw std::logic_error("Native map entrypoints unavailable");
 const auto charge=bytes.capacity();auto data=raw_glBufferData;auto sub=raw_glBufferSubData;
 if(!data||!sub)throw std::logic_error("Owned GL buffer entrypoints unavailable");
 auto error=raw_glGetError;
 Record([=,bytes=std::move(bytes)] {
   if(native_map) {
     const GLbitfield flags=GL_MAP_WRITE_BIT|(check_error?GL_MAP_FLUSH_EXPLICIT_BIT:0)|
                           (orphan?GL_MAP_INVALIDATE_BUFFER_BIT:GL_MAP_UNSYNCHRONIZED_BIT);
     auto* destination=static_cast<std::uint8_t*>(map(target,offset,map_reserved_size,flags));
     if(!destination)throw std::runtime_error("Worker native stream map returned null");
     if(!bytes.empty())std::memcpy(destination,bytes.data(),bytes.size());
     if(check_error&&!bytes.empty()) {
       flush(target,0,GLsizeiptr(bytes.size()));
       const auto result=error();++stats.owned_unmap_error_queries;
       if(result!=GL_NO_ERROR)LOG_DEBUG(Render_OpenGL,"native owned stream flush error: {:04X}, target: {}, offset: {}, size: {}",result,target,offset,bytes.size());
     }
     unmap(target);++stats.owned_native_map_uploads;return;
   }
   if(orphan)data(target,fullsize,nullptr,GL_STREAM_DRAW);
   if(!bytes.empty())sub(target,offset,GLsizeiptr(bytes.size()),bytes.data());
   if(check_error&&!bytes.empty()) {const auto result=error();++stats.owned_unmap_error_queries;
     if(result!=GL_NO_ERROR)LOG_DEBUG(Render_OpenGL,"owned stream upload error: {:04X}, target: {}, offset: {}, size: {}",result,target,offset,bytes.size());}
 },charge,RpcApi::UploadBufferOwned);
}
#if defined(__ANDROID__) && defined(CARA_GL_CONTEXT_DISPATCH)
bool Backlog::Enabled() {
 static const bool enabled = [] {
  const char* flag = std::getenv("CARA_TRACE_GLES_BACKLOG");
  return flag && std::strcmp(flag, "1") == 0;
 }();
 return enabled;
}
std::vector<Backlog::Row> Backlog::Take() {
 Producer();
 return Owner().Call([] {
  std::vector<Row> rows;
  rows.reserve(worker_backlog.size());
  for (const auto& entry : worker_backlog) rows.push_back(entry.second);
  worker_backlog.clear();
  return rows;
 });
}
std::array<SyncTiming, RpcApiCount> TakeSyncTimingsForProfile() {
 Producer();
 return std::exchange(sync_timings, {});
}
#endif
Stats SnapshotStats(){ Stats s;{std::lock_guard lock(budget_mutex);s.owned_budget_live=budget_live;s.owned_budget_high=budget_high;}
 for(std::size_t i=0;i<RpcApiCount;++i)s.rpc_by_api[i]=rpc_counts[i].load();
 s.uniform_location_rpcs=s.rpc_by_api[std::size_t(RpcApi::api_glGetUniformLocation)];
#define COPY_STAT(n) s.n=stats.n.load()
 COPY_STAT(owned_native_map_uploads);COPY_STAT(transaction_inline_calls);COPY_STAT(transaction_sync_calls);COPY_STAT(transaction_state_fallbacks);COPY_STAT(transaction_lut_calls);COPY_STAT(owned_unmap_error_queries);COPY_STAT(owned_budget_waits);
 COPY_STAT(recorded_commands);COPY_STAT(submitted_batches);COPY_STAT(synchronous_calls);COPY_STAT(owned_payload_bytes);
 COPY_STAT(conservative_pointer_rpcs);COPY_STAT(texture_pointer_rpcs);COPY_STAT(texture_owned_uploads);COPY_STAT(texture_pbo_offsets);
 COPY_STAT(client_array_rpcs);COPY_STAT(pending_commands);COPY_STAT(pending_bytes);COPY_STAT(worker_inline_calls);COPY_STAT(worker_rejected_calls);COPY_STAT(uniform_location_cache_hits);COPY_STAT(attrib_location_cache_hits);COPY_STAT(uniform_block_index_cache_hits);
#undef COPY_STAT
 return s;}
void ResetStats(){Drain();DriverTransactions::unchecked_blit_commands_recorded.store(0,std::memory_order_relaxed);for(auto& v:rpc_counts)v=0;
#define ZERO_STAT(n) stats.n=0
 ZERO_STAT(owned_native_map_uploads);ZERO_STAT(transaction_inline_calls);ZERO_STAT(transaction_sync_calls);ZERO_STAT(transaction_state_fallbacks);ZERO_STAT(transaction_lut_calls);ZERO_STAT(owned_unmap_error_queries);ZERO_STAT(owned_budget_waits);
 ZERO_STAT(recorded_commands);ZERO_STAT(submitted_batches);ZERO_STAT(synchronous_calls);ZERO_STAT(owned_payload_bytes);
 ZERO_STAT(conservative_pointer_rpcs);ZERO_STAT(texture_pointer_rpcs);ZERO_STAT(texture_owned_uploads);ZERO_STAT(texture_pbo_offsets);
 ZERO_STAT(client_array_rpcs);ZERO_STAT(pending_commands);ZERO_STAT(pending_bytes);ZERO_STAT(worker_inline_calls);ZERO_STAT(worker_rejected_calls);ZERO_STAT(uniform_location_cache_hits);ZERO_STAT(attrib_location_cache_hits);ZERO_STAT(uniform_block_index_cache_hits);
#undef ZERO_STAT
}
} // namespace OpenGL::CommandQueue
