// SPDX-License-Identifier: GPL-2.0-or-later
// Authored GPU synchronization; no guest ROM or command-buffer hardware faults.
#define main CompletedPredecessorsSourceFixtureMain
#include "metal_transfer_source_reuse_test.mm"
#undef main
#include <atomic>
#include <thread>
#include <type_traits>

namespace {
void CheckConcurrentSnapshot(id<MTLDevice> device, unsigned bad) {
    auto queue=[device newCommandQueue];QueueGate gate(queue);
    auto first=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    auto second=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    auto raw=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *static_cast<u32*>(first.contents)=0;*static_cast<u32*>(second.contents)=0;
    *static_cast<u32*>(raw.contents)=0;
    MetalProducerPredecessors::Commands commands{};
    MetalProducerPredecessors::Validations validations{};
    for(unsigned i=0;i<commands.size();++i) {
        auto command=[queue commandBuffer];commands[i]=command;
        if(i+1==commands.size()) {
            auto blit=[command blitCommandEncoder];
            [blit fillBuffer:raw range:NSMakeRange(0,4) value:0x6b];
            [blit fillBuffer:(bad==1?first:second) range:NSMakeRange(0,4) value:(bad?1:0)];
            [blit endEncoding];
            validations[i]=std::make_shared<PicaMetalValidationReceipt>(command,
                std::array<id<MTLBuffer>,2>{first,second});
        }
        [command commit];
    }
    const auto receipt=validations.back();
    const auto snapshot=std::make_shared<MetalProducerPredecessors>(std::move(commands),
                                                                  std::move(validations),32);
    static_assert(!std::is_copy_constructible_v<MetalProducerPredecessors>);
    static_assert(std::is_const_v<decltype(snapshot->commands)>);
    static_assert(std::is_const_v<decltype(snapshot->validations)>);
    static_assert(std::is_const_v<decltype(snapshot->count)>);
    std::atomic<bool> exported{false};
    const auto writer=std::make_shared<MetalSurfaceWriter>(raw,snapshot->commands.back(),1,base,base+4,
        [&](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8> out) {
            exported.store(true);std::memcpy(out.data(),raw.contents,out.size());return true;
        },0,snapshot,raw);
    Require(snapshot->Status()==MetalGuestWriteStatus::Pending,"Pending snapshot incorrectly completed");
    Require(receipt->Poll()==PicaMetalCompletion::Pending,"Pending receipt incorrectly completed");
    std::atomic<unsigned> ready{0},wait_finished{0},pending_observations{0};
    std::atomic<bool> release{false},passed{true};
    std::vector<std::thread> readers;
    const auto wanted=bad?MetalGuestWriteStatus::Failed:MetalGuestWriteStatus::Complete;
    const auto receipt_wanted=bad?PicaMetalCompletion::Failed:PicaMetalCompletion::Complete;
    for(unsigned i=0;i<4;++i)readers.emplace_back([&] { @autoreleasepool {
        ++ready;
        while(!release.load(std::memory_order_acquire)) {
            const auto before=snapshot->Status();const auto checked=receipt->Poll();
            if(!release.load(std::memory_order_acquire) &&
               (before!=MetalGuestWriteStatus::Pending || checked!=PicaMetalCompletion::Pending))passed=false;
            if(before==MetalGuestWriteStatus::Pending && checked==PicaMetalCompletion::Pending)
                ++pending_observations;
            std::this_thread::yield();
        }
        // Race several initial completed inspections of the same receipt and
        // snapshot, then repeatedly exercise both terminal fast paths.
        if(receipt->Wait()!=receipt_wanted)passed=false;
        for(unsigned n=0;n<1000;++n)
            if(snapshot->Status()!=wanted || snapshot->Wait()!=!bad || receipt->Poll()!=receipt_wanted)
                passed=false;
    }});
    for(unsigned i=0;i<4;++i)readers.emplace_back([&,i] { @autoreleasepool {
        ++ready;
        if(i&1) {if(receipt->Wait()!=receipt_wanted)passed=false;}
        else if(snapshot->Wait()!=!bad)passed=false;
        ++wait_finished;
    }});
    std::array<u8,4> output{};std::atomic<bool> read_finished{false};bool read_success=false;
    readers.emplace_back([&] { @autoreleasepool {
        ++ready;read_success=writer->ExportGuestBytes(base,base+4,output);read_finished=true;
    }});
    while(ready.load()!=9)std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const bool escaped=wait_finished.load()!=0 || read_finished.load() || exported.load();
    // Publish the CPU release before signaling the GPU gate so pending-reader
    // assertions cannot race a legitimate transition to a final GPU state.
    release.store(true,std::memory_order_release);gate.Release();
    for(auto& thread:readers)thread.join();
    Require(!escaped && pending_observations.load()!=0,"CPU visibility escaped pending GPU validation");
    Require(passed.load(),"Concurrent receipt/snapshot observations disagreed");
    Require(read_success==!bad && exported.load()==!bad,"CPU read did not honor final-slot validation");
    if(!bad)Require(std::all_of(output.begin(),output.end(),[](u8 b){return b==0x6b;}),"Valid GPU bytes changed");
    Require(snapshot->Status()==wanted && snapshot->Wait()==!bad,"Final snapshot state changed");
    Require(receipt->Poll()==receipt_wanted,"Final receipt state changed");
    {
        auto wrong_commands=snapshot->commands;
        wrong_commands.back()=snapshot->commands.front();
        const auto mismatch=std::make_shared<MetalProducerPredecessors>(std::move(wrong_commands),
                                                                       snapshot->validations,32);
        Require(mismatch->Status()==MetalGuestWriteStatus::Failed && !mismatch->Wait(),
                "Mismatched final receipt authorized completion");
        const auto invalid_count=std::make_shared<MetalProducerPredecessors>(
            MetalProducerPredecessors::Commands{},MetalProducerPredecessors::Validations{},33);
        Require(invalid_count->Status()==MetalGuestWriteStatus::Failed && !invalid_count->Wait(),
                "Oversized snapshot authorized completion");
    }
    if(bad) {
        // Test terminal failure latching only. Successful GPU checks are never
        // mutated after completion; Metal command final states are immutable.
        *static_cast<u32*>(first.contents)=0;*static_cast<u32*>(second.contents)=0;
        Require(snapshot->Status()==MetalGuestWriteStatus::Failed &&
                receipt->Poll()==PicaMetalCompletion::Failed,"Terminal failure was forgotten");
    }
    std::cout<<"case bad_buffer="<<bad<<" pending_observations="<<pending_observations.load()<<'\n';
}
}
int main() { @autoreleasepool {try {
    auto device=MTLCreateSystemDefaultDevice();Require(device!=nil,"No Metal device");
    for(unsigned bad=0;bad<3;++bad)CheckConcurrentSnapshot(device,bad);
    std::cout<<"PASS cache_completed_predecessors="<<Option("CARA_METAL_CACHE_COMPLETED_PREDECESSORS")
             <<" concurrent_cases=3 readers_per_case=9 final_slot=31\n";return 0;
} catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;} } }
