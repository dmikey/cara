// SPDX-License-Identifier: GPL-2.0-or-later
// Production native-lane GPU regression. Run once with batching unset and once
// with CARA_METAL_BATCH_TEXTURE_COPIES=1. See run_metal_texture_copy_batch_test.py.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_native_draw_lane.h"
#include "video_core/renderer_metal/pica_metal_frame_cost.hh"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Cara {
void RegisterNativeMetalTextureCopyBatch(PicaMetalNativeDrawLane&);
void UnregisterNativeMetalTextureCopyBatch(PicaMetalNativeDrawLane&) noexcept;
}
namespace {
using namespace Cara;
using namespace Cara::NativeMetalCost;
constexpr PAddr vram_base = 0x18000000, ram_base = 0x20000000;
constexpr PAddr source_a = ram_base+0x1000, source_b = ram_base+0x2000;
constexpr PAddr dest_a = vram_base+0x1000, dest_b = vram_base+0x2000;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void RequireFailure(Function&& operation, const char* message) {
    bool threw = false;
    try { operation(); } catch (const std::exception&) { threw = true; }
    Require(threw,message);
}
std::uint64_t Counted(Phase phase) { return spans[phase].calls; }
struct Fixture {
    std::vector<u8> vram = std::vector<u8>(65536,0xcd);
    std::vector<u8> ram = std::vector<u8>(65536);
    std::vector<std::pair<PAddr,PAddr>> notices;
    PicaMetalNativeDrawLane lane;
    const MetalTransfer::Pipelines& pipelines;
    Fixture(id<MTLCommandQueue> queue, const MetalTransfer::Pipelines& transfer, bool notice = true)
        : lane(queue,256*1024,notice ? PicaMetalNativeDrawLane::GuestWriteNotice{
              [this](PAddr begin,PAddr end) { notices.emplace_back(begin,end); }} :
              PicaMetalNativeDrawLane::GuestWriteNotice{}), pipelines(transfer) {
        spans = {};
        for (std::size_t i=0;i<ram.size();++i) ram[i] = (i*37+(i>>8)*11)&255;
        RegisterNativeMetalTextureCopyBatch(lane);
    }
    ~Fixture() { UnregisterNativeMetalTextureCopyBatch(lane); }
    u8* Pointer(PAddr address, std::size_t size) {
        if (address>=vram_base && address-vram_base<=vram.size() && size<=vram.size()-(address-vram_base))
            return vram.data()+address-vram_base;
        if (address>=ram_base && address-ram_base<=ram.size() && size<=ram.size()-(address-ram_base))
            return ram.data()+address-ram_base;
        return nullptr;
    }
    MetalGuestAuthority::GuestReader Reader() {
        return [this](PAddr address,std::span<u8> output) {
            const auto* source = Pointer(address,output.size());
            if (!source) return false;
            std::memcpy(output.data(),source,output.size());
            return true;
        };
    }
    std::vector<u8> Raw(PAddr address,std::size_t size) {
        const auto* source = Pointer(address,size);
        Require(source != nullptr,"Test RAM address outside fixture");
        return {source,source+size};
    }
    void Copy(PAddr source,PAddr destination,bool striped=false) {
        // Four exact 16-byte destination stripes with 32-byte untouched gaps.
        const MetalTransfer::TextureCopyRegisters regs{
            source,destination,64,0,0,std::uint16_t(striped ? 1 : 0),std::uint16_t(striped ? 2 : 0)};
        lane.CopyTexture(regs,pipelines,Reader());
    }
    std::vector<u8> Read(PAddr address,std::size_t size) {
        std::vector<u8> output(size);
        Require(lane.ReadGuest(address,output,Reader()),"Native guest read failed");
        return output;
    }
    void Flush(PAddr address,std::size_t size) {
        lane.FlushGuest(address,address+size,[this](PAddr first,std::span<const u8> bytes) {
            auto* output = Pointer(first,bytes.size());
            if (!output) return false;
            std::memcpy(output,bytes.data(),bytes.size());
            return true;
        });
    }
    void CheckCounts(std::uint64_t commands,std::uint64_t queued,std::uint64_t commits,
                     std::uint64_t saved,std::uint64_t hazards=0) {
        lane.Drain();
        Require(Counted(GpuCompleted)+Counted(GpuTimeMissing)==commands,
                "Unexpected submitted/retired GPU command count");
        Require(Counted(TextureCopyBatchQueued)==queued,"Unexpected queued copy count");
        Require(Counted(TextureCopyBatchCommits)==commits,"Unexpected copy batch commit count");
        Require(Counted(TextureCopyBatchCommandBuffersSaved)==saved,"Unexpected saved command count");
        Require(Counted(TextureCopyBatchHazards)==hazards,"Unexpected copy batch hazard count");
        Require(Counted(TextureCopyBatchAbandoned)==0,"A live copy was abandoned");
    }
};
}
int main(int argc,char** argv) { @autoreleasepool { try {
    Require(argc==2,"Pass path to production cara-pica.metallib");
    const auto* option = std::getenv("CARA_METAL_BATCH_TEXTURE_COPIES");
    const bool batch = option && std::string_view(option)=="1";
    setenv("CARA_METAL_FRAME_COST","1",1);
    setenv("CARA_METAL_OVERLAP_TRANSFERS","1",1);
    setenv("CARA_METAL_REDUCE_COPY_QUEUE_WORK","1",1);
    setenv("CARA_METAL_DRAW_STAGE_PROFILE","0",1);
    auto device = MTLCreateSystemDefaultDevice();
    Require(device && device.hasUnifiedMemory,"Requires supported unified-memory Apple GPU");
    auto queue = [device newCommandQueue];
    NSError* error = nil;
    auto url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
    auto library = [device newLibraryWithURL:url error:&error];
    if (!library) throw std::runtime_error(error.localizedDescription.UTF8String);
    auto copy_rows = [device newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"cara_pica_copy_rows"] error:&error];
    if (!copy_rows) throw std::runtime_error(error.localizedDescription.UTF8String);
    MetalTransfer::Pipelines pipelines{copy_rows,nil,nil};
    unsigned cases = 0;
    {
        Fixture f(queue,pipelines);
        const auto a = f.Raw(source_a,64), b = f.Raw(source_b,64);
        f.Copy(source_a,dest_a);
        Require(Counted(TextureCopyBatchQueued)==std::uint64_t(batch),"First eligible copy not admitted correctly");
        Require(Counted(TextureCopyBatchCommits)==0,"First copy committed prematurely as a batch");
        f.Copy(source_b,dest_b);
        f.CheckCounts(batch ? 1 : 2,batch ? 2 : 0,batch ? 1 : 0,batch ? 1 : 0);
        Require(f.Read(dest_a,64)==a && f.Read(dest_b,64)==b,"Independent copies differ from RAM source bytes");
        Require(f.Raw(dest_a,64)==std::vector<u8>(64,0xcd),"Native copy unexpectedly wrote guest RAM");
        Require(f.notices.size()==2,"Destination notices were not exact");
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        const auto source = f.Raw(source_a,64);
        f.Copy(source_a,dest_a,true); f.Copy(source_b,dest_b,true);
        f.CheckCounts(batch ? 1 : 2,batch ? 2 : 0,batch ? 1 : 0,batch ? 1 : 0);
        auto expected = std::vector<u8>(160,0xcd);
        for (unsigned row=0;row<4;++row)
            std::copy_n(source.begin()+row*16,16,expected.begin()+row*48);
        Require(f.Read(dest_a,160)==expected,"Striped copy lost payload or guest-owned gaps");
        f.Flush(dest_a,160);
        Require(f.Raw(dest_a,160)==expected,"Striped flush overwrote gap ownership");
        Require(f.notices.size()==8,"Striped copy did not mark each exact destination stripe");
        for (unsigned row=0;row<4;++row)
            Require(f.notices[row]==std::pair<PAddr,PAddr>{dest_a+row*48,dest_a+row*48+16},
                    "Write notice included a destination gap");
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        const auto expected = f.Raw(source_a,64);
        f.Copy(source_a,dest_a); f.Copy(dest_a,dest_b);
        f.CheckCounts(2,batch ? 2 : 0,batch ? 2 : 0,0,batch ? 1 : 0);
        Require(f.Read(dest_b,64)==expected,"Dependent source read stale underlying RAM");
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        auto expected = f.Raw(source_a,32);
        const auto second = f.Raw(source_b,64);
        expected.insert(expected.end(),second.begin(),second.end());
        f.Copy(source_a,dest_a); f.Copy(source_b,dest_a+32);
        f.CheckCounts(2,batch ? 2 : 0,batch ? 2 : 0,0,batch ? 1 : 0);
        Require(f.Read(dest_a,96)==expected,"Overlapping destination copy order changed");
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        const PAddr outside_vram = ram_base+0x3000;
        const auto expected = f.Raw(source_a,64);
        f.Copy(source_a,outside_vram); f.Copy(source_b,dest_b);
        f.CheckCounts(2,batch ? 1 : 0,batch ? 1 : 0,0);
        Require(f.Read(outside_vram,64)==expected,"Non-VRAM immediate fallback changed bytes");
        ++cases;
    }
    {
        Fixture f(queue,pipelines,false);
        f.Copy(source_a,dest_a); f.Copy(source_b,dest_b);
        f.CheckCounts(2,0,0,0);
        Require(f.notices.empty(),"No-notice fixture received a callback");
        Require(f.Read(dest_a,64)==f.Raw(source_a,64),"No-notice immediate fallback changed bytes");
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        const auto expected = f.Raw(source_a,64);
        f.Copy(source_a,dest_a);
        Require(f.Read(dest_a,64)==expected,"CPU read did not submit a pending first copy");
        f.CheckCounts(1,batch ? 1 : 0,batch ? 1 : 0,0);
        Require(f.Raw(dest_a,64)==std::vector<u8>(64,0xcd),"Scratch read unexpectedly materialized RAM");
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        const auto expected = f.Raw(source_a,64);
        f.Copy(source_a,dest_a); f.Flush(dest_a,64);
        Require(f.Raw(dest_a,64)==expected,"CPU flush missed pending first copy");
        f.CheckCounts(1,batch ? 1 : 0,batch ? 1 : 0,0);
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        f.Copy(source_a,dest_a);
        f.lane.InvalidateGuest(dest_a,dest_a+64);
        std::fill_n(f.Pointer(dest_a,64),64,0x7e);
        Require(f.Read(dest_a,64)==std::vector<u8>(64,0x7e),"CPU invalidation retained a stale native producer");
        f.CheckCounts(1,batch ? 1 : 0,batch ? 1 : 0,0);
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        const auto expected = f.Raw(source_a,64);
        f.Copy(source_a,dest_a); f.lane.Drain();
        Require(f.Read(dest_a,64)==expected,"Drain lost pending first copy");
        f.CheckCounts(1,batch ? 1 : 0,batch ? 1 : 0,0);
        UnregisterNativeMetalTextureCopyBatch(f.lane);
        Require(Counted(TextureCopyBatchAbandoned)==0,"Normal drained teardown abandoned writes");
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        f.Copy(source_a,dest_a); f.Copy(source_b,dest_b); f.Copy(source_a,dest_a+0x100);
        f.CheckCounts(batch ? 2 : 3,batch ? 3 : 0,batch ? 2 : 0,batch ? 1 : 0);
        Require(f.Read(dest_a+0x100,64)==f.Raw(source_a,64),"Copy after full batch reused stale state");
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        auto other_queue = [device newCommandQueue];
        PicaMetalNativeDrawLane other(other_queue,256*1024);
        if (batch) RequireFailure([&] { RegisterNativeMetalTextureCopyBatch(other); },
                                 "A second owner registered on the active lane thread");
        UnregisterNativeMetalTextureCopyBatch(other); // Must not clear f's registration.
        f.Copy(source_a,dest_a); f.Copy(source_b,dest_b);
        f.CheckCounts(batch ? 1 : 2,batch ? 2 : 0,batch ? 1 : 0,batch ? 1 : 0);
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        f.Copy(source_a,dest_a);
        // Explicit stop abandons the uncommitted command; a drained normal
        // teardown above must not. The next fixture must register cleanly.
        UnregisterNativeMetalTextureCopyBatch(f.lane);
        Require(Counted(TextureCopyBatchAbandoned)==std::uint64_t(batch),"Stop abandonment count incorrect");
        f.lane.Drain();
        ++cases;
    }
    {
        Fixture f(queue,pipelines);
        f.Copy(source_a,dest_a);
        f.lane.PublishPending(); // A real submitted producer exists before rejection.
        RequireFailure([&] { f.Copy(0x30000000,dest_b); },"Unavailable RAM source was accepted");
        RequireFailure([&] { (void)f.Read(dest_a,64); },"Memory owner resumed after rejected producer");
        RequireFailure([&] { f.lane.Drain(); },"Submission lane resumed after source failure");
        Require(f.Raw(dest_a,64)==std::vector<u8>(64,0xcd),"Failed source changed guest RAM");
        ++cases;
    }
    // The failed lane cannot Drain, but its earlier valid producer must finish
    // before this regression hands the GPU back to a timing campaign.
    auto barrier = [queue commandBuffer];
    [barrier commit];
    [barrier waitUntilCompleted];
    Require(barrier.status==MTLCommandBufferStatusCompleted && !barrier.error,"Final GPU barrier failed");
    std::cout << "PASS: texture_copy_batch=" << batch << ", " << cases
              << " production lane GPU/ownership cases; exact guest bytes and submission counters\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
} } }
