// Host-only diagnostic timing. Does not read or modify guest clocks or issue GL calls.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
namespace OpenGL::HostLatency {
constexpr std::uint32_t Schema=1, MaxSlow=64, MaxShaders=512, MaxPrograms=1024;
enum class Api:std::uint32_t {Compile,Link,ShaderQuery,ProgramQuery,DrawArrays,DrawElements,DrawRange,Capture,Count};
enum class CaptureKind:std::uint32_t {Other,Source,Compile,Link,Status,DrawBefore,DrawReturned};
enum Flag:std::uint32_t {UnknownObject=1,FirstObservedDraw=2,StatusPresent=4,DeletedRequested=8,LinkStatusUnobserved=16,LinkStatusFalse=32};
enum HeaderFlag:std::uint64_t {ClockUnavailable=1,SequenceStopped=2,MetadataOverflow=4,CounterSaturated=8,ObserverInvalidated=16};
struct Record {
 std::uint64_t token=0,begin_tick=0,end_tick=0,object_generation=0,object_revision=0,program_generation=0,link_serial=0,draw_ordinal=0;
 std::uint32_t api=0,pname=0,object=0,program=0,vao=0,framebuffer=0,flags=0,phase=0,mode=0;
 std::int32_t count=0,status=0;
 std::uint32_t reserved=0;
};
static_assert(sizeof(Record)==112 && std::is_trivially_copyable_v<Record>);
struct Bucket {std::uint64_t calls=0,ticks=0,max_ticks=0;std::array<std::uint64_t,64> histogram{};};
constexpr std::size_t BucketCount=std::size_t(Api::Count)*6;
struct Payload {std::array<std::uint64_t,sizeof(Record)/8> words{};};
struct Slot {std::uint64_t committed=0;Payload payload{};};
struct Journal {
 std::array<char,8> magic{};
 std::uint32_t schema=Schema,bytes=sizeof(Journal),record_bytes=sizeof(Record),slow_capacity=MaxSlow;
 std::uint64_t pid=0,owner_tid=0,owner_ready=0;
 std::array<char,65> build_identity{};
 std::uint32_t capture_requested=0;
 std::uint32_t timebase_numer=0,timebase_denom=0;
 std::uint64_t slow_threshold_ticks=0,flags=0,wrong_owner=0,reentry=0,metadata_unknown=0;
 std::uint64_t committed=0;
 std::array<Payload,2> latest{};
 std::uint64_t stats_committed=0,completed_calls=0,slow_returns=0,overwritten_slow=0;
 std::array<Bucket,BucketCount> buckets{};
 std::array<Slot,MaxSlow> slow{};
};
static_assert(sizeof(Journal)<=256*1024 && std::is_trivially_copyable_v<Journal>);
static_assert(offsetof(Journal,committed)%8==0 && offsetof(Slot,committed)%8==0);
struct Token {Record record{};bool active=false;};
bool Requested() noexcept;
std::uint64_t ReadHostTick() noexcept;
class Recorder {
public:
 explicit Recorder(const std::string& fresh_directory) noexcept;
 ~Recorder();
 Recorder(const Recorder&)=delete;Recorder& operator=(const Recorder&)=delete;
 void BindOwnerThread() noexcept;
 Token Begin(Api api,std::uint32_t object=0,std::uint32_t pname=0,std::uint32_t mode=0,std::int32_t count=0) noexcept;
 void End(Token token,std::uint64_t end_tick,std::int32_t status=0,bool status_present=false) noexcept;
 bool CanCopyStatus(const Token& token) noexcept;
 void CreateShader(std::uint32_t id,std::uint32_t type) noexcept;
 void Source(std::uint32_t id) noexcept;
 void DeleteShader(std::uint32_t id) noexcept;
 void CreateProgram(std::uint32_t id) noexcept;
 void Link(std::uint32_t id) noexcept;
 void Use(std::uint32_t id) noexcept;
 void DeleteProgram(std::uint32_t id) noexcept;
 void BindVAO(std::uint32_t id) noexcept;
 void BindFramebuffer(std::uint32_t target,std::uint32_t id) noexcept;
private:
 struct Impl;std::unique_ptr<Impl> impl;
 friend struct HostLatencyTestAccess;
};
} // namespace OpenGL::HostLatency
