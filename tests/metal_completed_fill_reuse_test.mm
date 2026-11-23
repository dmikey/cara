// SPDX-License-Identifier: GPL-2.0-or-later
// Production Fill regression; run with REUSE_COMPLETED_FILLS unset and =1.
// Shared events establish pending producers deterministically, without races
// against how quickly a particular Apple GPU completes a small fill.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include <algorithm>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
namespace {
using namespace Cara;
using namespace Cara::NativeMetalCost;
constexpr PAddr base=0x18000000,a=base+0x1000,b=base+0x2000,c=base+0x3000;
void Require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
template<class Function> void Fails(Function&& operation,const char* message) {
    bool rejected=false;
    try { operation(); } catch (const std::exception&) { rejected=true; }
    Require(rejected,message);
}
std::uint64_t Counted(Phase phase) { return spans[phase].calls; }
std::vector<u8> Pattern(unsigned bytes,u32 pattern,unsigned period) {
    std::vector<u8> result(bytes);
    for (unsigned i=0;i<bytes;++i) result[i]=(pattern>>((i%period)*8))&255;
    return result;
}
struct QueueGate {
    id<MTLSharedEvent> event;
    id<MTLCommandBuffer> command;
    explicit QueueGate(id<MTLCommandQueue> queue,bool commit=true)
        : event([queue.device newSharedEvent]),command([queue commandBuffer]) {
        Require(event && command,"Shared-event test gate unavailable");
        [command encodeWaitForEvent:event value:1];
        if (commit) [command commit];
    }
    void Release() { event.signaledValue=1; }
    ~QueueGate() { Release(); if (command.status>=MTLCommandBufferStatusCommitted) [command waitUntilCompleted]; }
};
// Watchdog releases the GPU only if the owner has not returned. This checks
// ordering, not a performance threshold, and prevents a regression hanging CI.
template<class Function> bool ReleaseIfBlocked(QueueGate& gate,Function&& operation) {
    std::mutex mutex;std::condition_variable changed;bool returned=false,released=false;
    std::thread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock,std::chrono::milliseconds(100),[&] { return returned; })) {
            released=true;gate.Release();
        }
    });
    try { operation(); } catch (...) {
        { std::lock_guard lock(mutex);returned=true; }changed.notify_one();
        gate.Release();watchdog.join();throw;
    }
    { std::lock_guard lock(mutex);returned=true; }changed.notify_one();watchdog.join();
    return released;
}
struct Fixture {
    id<MTLCommandQueue> queue;
    std::vector<u8> ram=std::vector<u8>(6*1024*1024,0xcd);
    std::vector<std::pair<PAddr,PAddr>> notices;
    PicaMetalNativeDrawLane lane;
    const MetalTransfer::Pipelines& pipelines;
    Fixture(id<MTLDevice> device,const MetalTransfer::Pipelines& transfer)
        : queue([device newCommandQueue]),lane(queue,256*1024,
            [this](PAddr begin,PAddr end) { notices.emplace_back(begin,end); }),pipelines(transfer) { spans={}; }
    u8* Pointer(PAddr address,std::size_t bytes) {
        if (address<base || address-base>ram.size() || bytes>ram.size()-(address-base)) return nullptr;
        return ram.data()+address-base;
    }
    MetalGuestAuthority::GuestReader Reader() {
        return [this](PAddr address,std::span<u8> output) {
            const auto* data=Pointer(address,output.size()); if (!data) return false;
            std::memcpy(output.data(),data,output.size()); return true;
        };
    }
    void Fill(PAddr destination,u32 value,unsigned period=4,unsigned bytes=48) {
        lane.Fill({destination,destination+bytes,value,period==3,period==4},pipelines);
    }
    void Completed(PAddr destination,u32 value,unsigned period=4,unsigned bytes=48) {
        Fill(destination,value,period,bytes); lane.Drain();
    }
    std::vector<u8> Read(PAddr address,unsigned bytes=48) {
        std::vector<u8> output(bytes);
        Require(lane.ReadGuest(address,output,Reader()),"Guest fill read failed"); return output;
    }
    void Flush(PAddr address,unsigned bytes=48) {
        lane.FlushGuest(address,address+bytes,[this](PAddr begin,std::span<const u8> data) {
            auto* output=Pointer(begin,data.size()); if (!output) return false;
            std::memcpy(output,data.data(),data.size()); return true;
        });
    }
    void Check(unsigned commands,unsigned hits,unsigned misses,bool enabled) {
        lane.Drain();
        Require(Counted(GpuCompleted)+Counted(GpuTimeMissing)==commands,"Unexpected GPU fill command count");
        Require(Counted(FillBackingHits)==(enabled ? hits : 0),"Unexpected completed fill hit count");
        Require(Counted(FillBackingMisses)==(enabled ? misses : 0),"Unexpected completed fill miss count");
    }
};
}
int main(int argc,char** argv) { @autoreleasepool { try {
    Require(argc==2,"Pass production cara-pica.metallib path");
    const auto* option=std::getenv("CARA_METAL_REUSE_COMPLETED_FILLS");
    const bool reuse=option && std::string_view(option)=="1";
    setenv("CARA_METAL_FRAME_COST","1",1);
    setenv("CARA_METAL_OVERLAP_TRANSFERS","1",1);
    setenv("CARA_METAL_REDUCE_COPY_QUEUE_WORK","1",1);
    const auto* lease_option=std::getenv("CARA_METAL_FILL_LEASE_NO_RESERVE");
    const bool no_reserve=lease_option && std::string_view(lease_option)=="1";
    setenv("CARA_METAL_DRAW_STAGE_PROFILE","0",1);
    auto device=MTLCreateSystemDefaultDevice();
    Require(device && device.hasUnifiedMemory,"Requires supported Apple unified-memory GPU");
    NSError* error=nil;
    auto library=[device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]]
                                      error:&error];
    if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
    auto fill=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cara_pica_fill_words"]
                                                  error:&error];
    if (!fill) throw std::runtime_error(error.localizedDescription.UTF8String);
    auto copy=[device newComputePipelineStateWithFunction:[library newFunctionWithName:@"cara_pica_copy_rows"]
                                                  error:&error];
    if (!copy) throw std::runtime_error(error.localizedDescription.UTF8String);
    MetalTransfer::Pipelines pipelines{copy,fill,nil};
    constexpr u32 value=0xa1b2c3d4;
    unsigned cases=0;
    {
        Fixture f(device,pipelines);
        f.Completed(a,value); f.Completed(b,value);
        Require(f.Read(b)==Pattern(48,value,4),"Completed backing was not republished at new destination");
        Require(std::all_of(f.Pointer(b,48),f.Pointer(b,48)+48,[](u8 x) { return x==0xcd; }),
                "Native publication unexpectedly overwrote guest RAM");
        f.Completed(c,0x10203040); f.Completed(c+0x100,value,2);
        f.Completed(c+0x200,value,3); f.Completed(c+0x300,value,4,96);
        f.Completed(c+0x400,value,3); f.Completed(c+0x500,value,2);
        Require(f.Read(c)==Pattern(48,0x10203040,4),"Fill pattern identity changed");
        Require(f.Read(c+0x100)==Pattern(48,value,2),"16-bit fill period changed");
        Require(f.Read(c+0x200)==Pattern(48,value,3),"24-bit fill period changed");
        Require(f.Read(c+0x300,96)==Pattern(96,value,4),"Fill size identity changed");
        Require(f.Read(c+0x400)==Pattern(48,value,3),"Reused 24-bit fill changed destination-relative phase");
        Require(f.Read(c+0x500)==Pattern(48,value,2),"Reused 16-bit fill changed bytes");
        f.Check(reuse ? 5 : 8,3,5,reuse);
        Require(f.notices.size()==8,"Reused fill omitted a guest-write notice");
        ++cases;
    }
    {
        Fixture f(device,pipelines);
        QueueGate gate(f.queue);
        f.Fill(a,value); f.Fill(b,value);
        Require(Counted(FillBackingHits)==0,"A pending fill producer was reused");
        Require(Counted(FillBackingMisses)==(reuse ? 2u : 0u),"Pending fills did not retain independent backing");
        gate.Release();
        Require(f.Read(a)==Pattern(48,value,4) && f.Read(b)==Pattern(48,value,4),"Pending fill bytes changed");
        f.Check(2,0,2,reuse); ++cases;
    }
    {
        Fixture f(device,pipelines);
        f.Completed(a,value);
        QueueGate preceding(f.queue,false);
        f.lane.SubmitSampledTextureBuild(preceding.command);
        f.Fill(b,value);
        // Consume the NEW destination through the actual GPU ownership resolver
        // while its current FIFO predecessor is still blocked. Reusing an old
        // address-bound writer would fail this copy or read stale RAM.
        f.lane.CopyTexture({b,c,48,0,0,0,0},pipelines,f.Reader());
        preceding.Release();
        Require(f.Read(c)==Pattern(48,value,4),"Reused fill lost current producer/address ownership");
        f.Check(reuse ? 3 : 4,1,1,reuse); ++cases;
    }
    {
        Fixture f(device,pipelines);
        f.Completed(a,value); f.Completed(b,value);
        f.lane.InvalidateGuest(b+8,b+24);
        std::fill_n(f.Pointer(b+8,16),16,0x7e);
        auto mixed=Pattern(48,value,4); std::fill_n(mixed.begin()+8,16,0x7e);
        Require(f.Read(b)==mixed,"CPU invalidation lost partial native/RAM ownership");
        f.Completed(b,value);
        Require(f.Read(b)==Pattern(48,value,4),"Repeated fill did not supersede CPU-written bytes");
        auto framed=std::vector<u8>(8,0xcd); const auto expected=Pattern(48,value,4);
        framed.insert(framed.end(),expected.begin(),expected.end()); framed.insert(framed.end(),8,0xcd);
        Require(f.Read(b-8,64)==framed,"New fill publication escaped exact destination bounds");
        f.Flush(b);
        Require(std::equal(expected.begin(),expected.end(),f.Pointer(b,48)),"Flush did not materialize reused fill bytes");
        f.Check(reuse ? 1 : 3,2,1,reuse);
        Require(f.notices.size()==3 && f.notices.back()==std::pair<PAddr,PAddr>{b,b+48},
                "Repeated guest fill omitted exact write/barrier notice");
        ++cases;
    }
    {
        Fixture f(device,pipelines);
        for (unsigned i=0;i<16;++i) f.Completed(a+i*0x100,0x01020300+i);
        f.Completed(b,0x01020300); // Hit does not change this FIFO's insertion order.
        f.Completed(b+0x100,0x01020310);
        f.Completed(b+0x200,0x01020300); // The first entry was evicted by entry 17.
        Require(f.Read(b+0x200)==Pattern(48,0x01020300,4),"Evicted fill backing was reconstructed incorrectly");
        f.Check(reuse ? 18 : 19,1,18,reuse); ++cases;
    }
    {
        Fixture f(device,pipelines);
        constexpr unsigned limit=1024*1024;
        f.Completed(base,value,4,limit); f.Completed(base+0x200000,value,4,limit);
        Require(f.Read(base+0x200000,limit)==Pattern(limit,value,4),"Exactly 1MiB fill reuse changed bytes");
        f.Completed(base,value,4,limit+8); f.Completed(base+0x200000,value,4,limit+8);
        Require(f.Read(base+0x200000,limit+8)==Pattern(limit+8,value,4),"Oversized uncached fill changed bytes");
        f.Check(reuse ? 3 : 4,1,1,reuse); ++cases;
    }
    {
        Fixture f(device,pipelines);
        f.Completed(a,value); f.Completed(b,value);
        Fails([&] { f.lane.CopyTexture({0x30000000,c,48,0,0,0,0},pipelines,f.Reader()); },
              "Missing RAM source was accepted");
        Fails([&] { (void)f.Read(b); },"Reused lease bypassed terminal producer rejection");
        Fails([&] { f.lane.Drain(); },"Failed submission lane resumed");
        ++cases;
    }
    {
        Fixture f(device,pipelines);f.Completed(a,value);
        QueueGate gate(f.queue,false);
        std::array<id<MTLCommandBuffer>,8> pending{};
        pending[0]=gate.command;
        for(unsigned i=0;i<pending.size();++i) {
            if(i) { pending[i]=[f.queue commandBuffer];[pending[i] encodeWaitForEvent:gate.event value:1]; }
            f.lane.SubmitSampledTextureBuild(pending[i]);
        }
        const auto reserve_before=Counted(QueueReserveFull),lease_before=Counted(FillLeaseFullQueue);
        const bool released=ReleaseIfBlocked(gate,[&] { f.Fill(b,value); });
        if(reuse && no_reserve) {
            Require(!released && gate.event.signaledValue==0,"Completed fill reuse blocked on a full queue");
            Require(Counted(QueueReserveFull)==reserve_before,"Reused lease reserved an extra queue slot");
            Require(Counted(FillLeaseFullQueue)==lease_before+1,"Full eight-slot hit was not exercised");
            for(auto command:pending)
                Require(command.status!=MTLCommandBufferStatusCompleted,"Pending FIFO command completed before gate release");
            std::vector<u8> output;
            Require(ReleaseIfBlocked(gate,[&] { output=f.Read(b); }),"Guest read bypassed pending FIFO predecessors");
            Require(output==Pattern(48,value,4),"Full queue lease guest read returned stale bytes");
        } else {
            Require(released,"Control fill failed to reserve the full queue");
            Require(f.Read(b)==Pattern(48,value,4),"Control fill bytes changed");
        }
        // New destination, partial CPU overwrite, then a newer fill at the same
        // address must publish fresh ownership rather than reuse an old writer.
        f.lane.InvalidateGuest(b+8,b+24);std::fill_n(f.Pointer(b+8,16),16,0x72);
        auto mixed=Pattern(48,value,4);std::fill_n(mixed.begin()+8,16,0x72);
        Require(f.Read(b)==mixed,"Full queue lease lost later CPU overwrite");
        f.Completed(c,value);f.Completed(b,value);
        Require(f.Read(c)==Pattern(48,value,4) && f.Read(b)==Pattern(48,value,4),
                "Fresh destination/generation did not supersede older CPU ownership");
        f.Check(reuse ? 9 : 12,3,1,reuse);
        Require(f.notices.size()==4,"Full queue lease omitted exact publication notices");
        ++cases;
    }
    if(reuse && no_reserve) {
        Fixture f(device,pipelines);f.Completed(a,value);
        QueueGate gate(f.queue,false);
        for(unsigned i=0;i<8;++i) {
            const auto command=i ? [f.queue commandBuffer] : gate.command;
            if(i) [command encodeWaitForEvent:gate.event value:1];
            f.lane.SubmitSampledTextureBuild(command);
        }
        Require(!ReleaseIfBlocked(gate,[&] { f.Fill(b,value); }),"Fault fixture lease blocked on full queue");
        Require(Counted(FillLeaseFullQueue)==1,"Fault fixture did not retain eight predecessors");
        Fails([&] { f.lane.SubmitSampledTextureBuild(nil); },"Invalid producer accepted on full queue");
        Fails([&] { (void)f.Read(b); },"Full queue reused lease bypassed terminal submission fault");
        Fails([&] { f.Fill(c,value); },"Faulted full queue admitted a new fill lease");
        gate.Release();
        Fails([&] { f.lane.Drain(); },"Faulted full queue resumed after predecessor completion");
        ++cases;
    }
    std::cout << "PASS: completed_fill_reuse=" << reuse << ", fill_lease_no_reserve=" << no_reserve << ", " << cases
              << " production GPU/ownership cases; exact bytes, pending/completed gates, bounds and publication\n";
    return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
} }
