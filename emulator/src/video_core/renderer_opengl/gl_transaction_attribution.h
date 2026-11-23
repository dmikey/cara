// Source-only diagnostic prototype. Host spans, never guest or GPU time.
#pragma once
#include <array>
#ifdef _WIN32
#include <chrono>
#endif
#include <cstdint>
#include <ctime>
#include <limits>
#include <ostream>
namespace OpenGL::TransactionAttribution {
using U = std::uint64_t;
enum class Metric : unsigned { Flush, Call, Body, BeforeBody, AfterBody, ProducerCPU, OffCPU, Count };
constexpr unsigned Kinds=5, Metrics=unsigned(Metric::Count);
struct Distribution { U count=0,total=0,maximum=0;std::array<U,64> histogram{}; };
struct Bucket {
 U attempts=0,returned_true=0,returned_false=0,exceptions=0,not_started=0,censored=0;
 std::array<Distribution,Metrics> spans{};
};
struct Snapshot {
 bool enabled=false,saturated=false,invalid_kind=false;
 U epoch=0,flush_failures=0,async_lut=0,empty_lut=0;
 std::array<Bucket,Kinds> sync{},fallback{};
};
using Clock = U(*)(bool thread_cpu) noexcept;
inline U HostClock(bool cpu) noexcept {
#ifdef _WIN32
 if (cpu) return 0;
 return static_cast<U>(std::chrono::duration_cast<std::chrono::nanoseconds>(
     std::chrono::steady_clock::now().time_since_epoch()).count());
#else
 timespec t{};
 if(clock_gettime(cpu?CLOCK_THREAD_CPUTIME_ID:CLOCK_MONOTONIC,&t)||t.tv_sec<0||t.tv_nsec<0||t.tv_nsec>=1000000000)return 0;
 constexpr U max=std::numeric_limits<U>::max();
 if(U(t.tv_sec)>(max-U(t.tv_nsec))/1000000000)return 0;
 return U(t.tv_sec)*1000000000+U(t.tv_nsec);
#endif
}
// Each worker writes only the local record. Existing future completion orders
// these writes before producer aggregation; no global worker diagnostics.
struct Local {U p0=0,p1=0,w0=0,w1=0,p2=0,c0=0,c2=0;bool started=false;};
struct BodyExit {
 Local& local;Clock clock;
 ~BodyExit() noexcept {local.w1=clock(false);}
};
struct Recorder {
 Snapshot value{};Clock clock=HostClock;
 void Add(U& x,U n=1) noexcept {
  if(n>std::numeric_limits<U>::max()-x){x=std::numeric_limits<U>::max();value.saturated=true;}else x+=n;
 }
 void BeginInstall(bool enabled) noexcept {
  const U old=value.epoch;const bool overflow=old==std::numeric_limits<U>::max();
  value={};value.epoch=overflow?old:old+1;value.enabled=enabled;value.saturated=overflow;
 }
 void Span(Bucket& b,Metric metric,U ns) noexcept {
  auto& d=b.spans[unsigned(metric)];Add(d.count);Add(d.total,ns);if(ns>d.maximum)d.maximum=ns;
  unsigned bin=0;for(U x=ns;x>1;x>>=1)++bin;Add(d.histogram[bin]);
 }
 Bucket* Get(unsigned kind,bool fallback) noexcept {
  if(kind>=Kinds){value.invalid_kind=true;return nullptr;}
  return fallback?&value.fallback[kind]:&value.sync[kind];
 }
 void Finish(unsigned kind,const Local& r,bool fallback,bool returned,bool result) noexcept {
  auto* b=Get(kind,fallback);if(!b)return;
  if(returned)Add(result?b->returned_true:b->returned_false);else Add(b->exceptions);
  if(!r.started)Add(b->not_started);
  const bool common=r.p0&&r.p1&&r.p2&&r.c0&&r.c2&&r.p0<=r.p1&&r.p1<=r.p2&&r.c0<=r.c2;
  if(!common){Add(b->censored);return;}
  Span(*b,Metric::Flush,r.p1-r.p0);Span(*b,Metric::Call,r.p2-r.p1);
  Span(*b,Metric::ProducerCPU,r.c2-r.c0);
  const U wall=r.p2-r.p0,cpu=r.c2-r.c0;
  // Clock resolution can make residual negative; censor instead of silently
  // describing it as a positive dependency or GPU wait.
  if(cpu<=wall)Span(*b,Metric::OffCPU,wall-cpu);else Add(b->censored);
  if(!r.started)return;
  if(!r.w0||!r.w1||r.w0<r.p1||r.w1<r.w0||r.p2<r.w1){Add(b->censored);return;}
  Span(*b,Metric::Body,r.w1-r.w0);Span(*b,Metric::BeforeBody,r.w0-r.p1);Span(*b,Metric::AfterBody,r.p2-r.w1);
 }
 template<class Flush,class Count,class Call,class Body>
 bool Sync(unsigned kind,Flush&& flush,Count&& count,Call&& call,Body&& body) {
  Local r;r.c0=clock(true);r.p0=clock(false);
  try {flush();}catch(...){Add(value.flush_failures);throw;}
  count();if(auto* b=Get(kind,false))Add(b->attempts);
  r.p1=clock(false);
  auto wrapped=[&] {r.started=true;r.w0=clock(false);BodyExit exit{r,clock};return body();};
  try {const bool result=call(wrapped);r.p2=clock(false);r.c2=clock(true);Finish(kind,r,false,true,result);return result;}
  catch(...){r.p2=clock(false);r.c2=clock(true);Finish(kind,r,false,false,false);throw;}
 }
 template<class Body>bool Fallback(unsigned kind,Body&& body) {
  Local r;r.c0=clock(true);r.p0=clock(false);r.p1=r.p0;
  if(auto* b=Get(kind,true))Add(b->attempts);
  r.started=true;r.w0=clock(false);
  try {bool result;{BodyExit exit{r,clock};result=body();}r.p2=clock(false);r.c2=clock(true);Finish(kind,r,true,true,result);return result;}
  catch(...){r.p2=clock(false);r.c2=clock(true);Finish(kind,r,true,false,false);throw;}
 }
};
// Called only at existing producer profile boundaries, never transaction hot path.
inline void WriteJSON(std::ostream& out,const Snapshot& s) {
 constexpr const char* names[Kinds]={"Blit","Remap","DepthEncode","DepthUpload","LUT"};
 constexpr const char* metrics[Metrics]={"flush_host_ns","call_host_ns","body_host_ns","before_body_host_ns","after_body_host_ns","producer_thread_cpu_ns","producer_off_cpu_residual_ns"};
 out << "{\"schema\":1,\"kind\":\"host_transaction_spans_not_gpu_time\",\"units\":\"nanoseconds\",\"enabled\":" << (s.enabled?"true":"false")
     << ",\"epoch\":" << s.epoch << ",\"saturated\":" << (s.saturated?"true":"false") << ",\"invalid_kind\":" << (s.invalid_kind?"true":"false")
     << ",\"flush_failures\":" << s.flush_failures << ",\"async_lut\":" << s.async_lut << ",\"empty_lut\":" << s.empty_lut;
 for(unsigned route=0;route<2;++route){
  out << (route?",\"producer_fallback\":{":",\"worker_sync\":{");
  for(unsigned k=0;k<Kinds;++k){
   const auto& b=route?s.fallback[k]:s.sync[k];
   out << (k?",":"") << '"' << names[k] << "\":{\"attempts\":" << b.attempts << ",\"returned_true\":" << b.returned_true << ",\"returned_false\":" << b.returned_false
       << ",\"exceptions\":" << b.exceptions << ",\"not_started\":" << b.not_started << ",\"censored\":" << b.censored << ",\"spans\":{";
   for(unsigned m=0;m<Metrics;++m){
    const auto& d=b.spans[m];out << (m?",":"") << '"' << metrics[m] << "\":{\"count\":" << d.count << ",\"total\":" << d.total << ",\"cumulative_max\":" << d.maximum << ",\"log2_histogram\":[";
    for(unsigned i=0;i<64;++i)out << (i?",":"") << d.histogram[i];
    out << "]}";
   }
   out << "}}";
  }
  out << "}";
 }
 out << "}";
}
} // namespace OpenGL::TransactionAttribution
