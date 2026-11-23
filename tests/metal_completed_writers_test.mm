// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/pica_metal_surface_writer.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

// Protocol adapter counts observations but delegates actual GPU synchronization.
// Forced errors below are authored contract failures, not injected GPU faults.
@interface CountedWriterCommand : NSObject {
    id<MTLCommandBuffer> actual;
@public
    std::atomic<unsigned> statuses, errors, waits, queues;
    std::atomic<int> forced_status;
    bool forced_error;
}
- (instancetype)initWithCommand:(id<MTLCommandBuffer>)command;
- (MTLCommandBufferStatus)status;
- (NSError*)error;
- (id<MTLCommandQueue>)commandQueue;
- (id<MTLDevice>)device;
- (void)waitUntilCompleted;
@end
@implementation CountedWriterCommand
- (instancetype)initWithCommand:(id<MTLCommandBuffer>)command {
    self=[super init];if(self){actual=command;statuses=0;errors=0;waits=0;queues=0;forced_status=-1;forced_error=false;}return self;
}
- (MTLCommandBufferStatus)status {++statuses;const auto state=forced_status.load();return state<0?actual.status:MTLCommandBufferStatus(state);}
- (NSError*)error {++errors;return forced_error?[NSError errorWithDomain:@"AuthoredWriterFailure" code:1 userInfo:nil]:actual.error;}
- (id<MTLCommandQueue>)commandQueue {++queues;return actual.commandQueue;}
- (id<MTLDevice>)device {return actual.device;}
- (void)waitUntilCompleted {++waits;[actual waitUntilCompleted];}
@end
namespace {
using namespace Cara;
constexpr PAddr base=0x20000000;
void Check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
bool Option(const char* name){const auto* v=std::getenv(name);return v&&std::string_view(v)=="1";}
id<MTLCommandBuffer> Token(CountedWriterCommand* command){return (id<MTLCommandBuffer>)command;}
struct Gate {
    id<MTLSharedEvent> event;id<MTLCommandBuffer> command;
    explicit Gate(id<MTLCommandQueue> q):event([q.device newSharedEvent]),command([q commandBuffer]){[command encodeWaitForEvent:event value:1];[command commit];}
    void Open(){event.signaledValue=1;}
    ~Gate(){Open();[command waitUntilCompleted];}
};
std::shared_ptr<MetalSurfaceWriter> Writer(id<MTLBuffer> raw,CountedWriterCommand* command,
    std::shared_ptr<const MetalProducerPredecessors> before={},
    std::shared_ptr<const PicaMetalValidationReceipt> validation={},std::atomic<bool>* exported=nullptr) {
    return std::make_shared<MetalSurfaceWriter>(raw,Token(command),9,base,base+4,
        [raw,exported](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8> out){
            if(exported)*exported=true;std::memcpy(out.data(),raw.contents,out.size());return true;
        },0,std::move(before),raw,std::nullopt,std::nullopt,std::move(validation));
}
CountedWriterCommand* Completed(id<MTLCommandQueue> queue) {
    auto c=[queue commandBuffer];[c commit];[c waitUntilCompleted];return [[CountedWriterCommand alloc] initWithCommand:c];
}
void CompletedProof(id<MTLDevice> device,bool enabled) {
    auto queue=[device newCommandQueue];auto command=Completed(queue);
    auto raw=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *static_cast<u32*>(raw.contents)=0x12345678;
    auto writer=Writer(raw,command);
    Check(writer->Status()==MetalGuestWriteStatus::Complete,"Completed command rejected");
    const auto statuses=command->statuses.load(),errors=command->errors.load(),waits=command->waits.load(),queues=command->queues.load();
    for(unsigned i=0;i<100;++i){
        Check(writer->Status()==MetalGuestWriteStatus::Complete&&writer->ValidateProducerCompletion(),"Completed proof changed");
        Check(writer->Covers(base,base+4),"Complete coverage lost");
        Check(!writer->Covers(base-1,base+4)&&!writer->Covers(base,base)&&!writer->Covers(base,base+5),"Cached success bypassed range validation");
    }
    Check((command->statuses.load()==statuses)==enabled,"Status memo observation count differs");
    Check((command->errors.load()==errors)==enabled,"Error memo observation count differs");
    Check((command->waits.load()==waits)==enabled,"Completion wait was not memoized exactly when enabled");
    Check(command->queues.load()>queues,"Covers skipped immutable queue metadata validation");
    std::array<u8,4> out{};Check(writer->ExportGuestBytes(base,base+4,out),"Completed export failed");
    Check(std::memcmp(out.data(),raw.contents,4)==0,"Completed export bytes changed");
    Check(!writer->ExportGuestBytes(base,base+4,std::span(out).first(3)),"Cached success bypassed output size validation");
    MetalGuestExport exporter=[](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8>){return true;};
    MetalSurfaceWriter no_resource(nil,Token(command),9,base,base+4,exporter);
    MetalSurfaceWriter no_generation(raw,Token(command),0,base,base+4,exporter);
    MetalSurfaceWriter no_export(raw,Token(command),9,base,base+4,{});
    for(auto* invalid:{&no_resource,&no_generation,&no_export}){
        Check(invalid->Status()==MetalGuestWriteStatus::Complete,"Status semantics changed for malformed metadata");
        Check(!invalid->Covers(base,base+4),"Cached complete accepted missing metadata");
    }
    // A successful blocking validation can establish the same proof directly.
    auto second=Completed(queue);auto validated=Writer(raw,second);
    Check(validated->ValidateProducerCompletion(),"Initial completed validation failed");
    const auto first_waits=second->waits.load();
    Check(validated->ValidateProducerCompletion(),"Repeated completed validation failed");
    Check((second->waits.load()==first_waits)==enabled,"Wait-established proof not shared");
}
void Rejections(id<MTLDevice> device) {
    auto queue=[device newCommandQueue];auto raw=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    for(bool error_property:{false,true}) {
        auto token=Completed(queue);if(error_property)token->forced_error=true;else token->forced_status=MTLCommandBufferStatusError;
        auto writer=Writer(raw,token);Check(writer->Status()==MetalGuestWriteStatus::Failed,"Own failure ignored");
        auto observations=token->statuses.load();Check(writer->Status()==MetalGuestWriteStatus::Failed&&token->statuses.load()>observations,"Failure was incorrectly memoized");
        Check(!writer->ValidateProducerCompletion(),"Own failed completion accepted");
    }
    {
        auto own=Completed(queue),failed=Completed(queue);failed->forced_status=MTLCommandBufferStatusError;
        MetalProducerPredecessors::Commands commands{};MetalProducerPredecessors::Validations validations{};commands[0]=Token(failed);
        auto before=std::make_shared<MetalProducerPredecessors>(commands,validations,1);auto writer=Writer(raw,own,before);
        Check(writer->Status()==MetalGuestWriteStatus::Failed&&!writer->ValidateProducerCompletion(),"Failed predecessor hidden by completed own command");
    }
    auto unsubmitted=[queue commandBuffer];auto token=[[CountedWriterCommand alloc] initWithCommand:unsubmitted];auto pending=Writer(raw,token);
    for(unsigned i=0;i<3;++i)Check(pending->Status()==MetalGuestWriteStatus::Pending,"Unsubmitted command became complete");
    Check(!pending->Covers(base,base+4),"Unsubmitted command covers guest bytes");
    [unsubmitted commit];[unsubmitted waitUntilCompleted];Check(pending->Status()==MetalGuestWriteStatus::Complete,"Pending state cached permanently");
    auto own=Completed(queue),other=Completed(queue);
    auto errors=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];*static_cast<u32*>(errors.contents)=0;
    auto wrong=std::make_shared<PicaMetalValidationReceipt>(Token(other),std::array<id<MTLBuffer>,2>{errors,nil});
    auto mismatch=Writer(raw,own,{},wrong);Check(mismatch->Status()==MetalGuestWriteStatus::Failed&&!mismatch->ValidateProducerCompletion(),"Mismatched own receipt accepted");
    MetalProducerPredecessors::Commands commands{};MetalProducerPredecessors::Validations receipts{};commands[0]=Token(other);receipts[0]=wrong;
    auto invalid=std::make_shared<MetalProducerPredecessors>(commands,receipts,33);
    auto bad_count=Writer(raw,own,invalid);Check(bad_count->Status()==MetalGuestWriteStatus::Failed&&!bad_count->ValidateProducerCompletion(),"Malformed predecessor count accepted");
    commands[0]=Token(own);auto bad_receipt=std::make_shared<MetalProducerPredecessors>(commands,receipts,1);
    auto pred_mismatch=Writer(raw,own,bad_receipt);Check(pred_mismatch->Status()==MetalGuestWriteStatus::Failed&&!pred_mismatch->ValidateProducerCompletion(),"Mismatched predecessor receipt accepted");
}
void GatedReceipt(id<MTLDevice> device,unsigned bad,bool predecessor) {
    auto queue=[device newCommandQueue];Gate gate(queue);
    auto raw=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    auto a=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];auto b=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *static_cast<u32*>(a.contents)=0;*static_cast<u32*>(b.contents)=0;
    auto gpu=[queue commandBuffer];auto encode=[gpu blitCommandEncoder];[encode fillBuffer:raw range:NSMakeRange(0,4) value:0x6b];
    [encode fillBuffer:(bad==1?a:b) range:NSMakeRange(0,4) value:bad?1:0];[encode endEncoding];[gpu commit];
    auto token=[[CountedWriterCommand alloc] initWithCommand:gpu];
    auto receipt=std::make_shared<PicaMetalValidationReceipt>(Token(token),std::array<id<MTLBuffer>,2>{a,b});
    std::shared_ptr<const MetalProducerPredecessors> before;
    auto owner=token;
    if(predecessor){
        MetalProducerPredecessors::Commands commands{};MetalProducerPredecessors::Validations validations{};commands[0]=Token(token);validations[0]=receipt;
        before=std::make_shared<MetalProducerPredecessors>(commands,validations,1);
        // Independent completed producer must not hide its pending dependency.
        owner=Completed([device newCommandQueue]);
    }
    std::atomic<bool> exported{false};auto writer=Writer(raw,owner,before,predecessor?nullptr:receipt,&exported);
    const auto desired=bad?MetalGuestWriteStatus::Failed:MetalGuestWriteStatus::Complete;
    for(unsigned i=0;i<10;++i)Check(writer->Status()==MetalGuestWriteStatus::Pending,"Gated producer/receipt not pending");
    std::atomic<unsigned> ready{0},done{0},observed{0};std::atomic<bool> release{false},okay{true};
    std::vector<std::thread> threads;
    for(unsigned i=0;i<4;++i)threads.emplace_back([&]{@autoreleasepool{
        ++ready;while(!release.load(std::memory_order_acquire)){if(writer->Status()==MetalGuestWriteStatus::Pending)++observed;std::this_thread::yield();}
        if(writer->ValidateProducerCompletion()!=!bad)okay=false;
        for(unsigned n=0;n<1000;++n)if(writer->Status()!=desired||writer->ValidateProducerCompletion()!=!bad)okay=false;
    }});
    for(unsigned i=0;i<4;++i)threads.emplace_back([&]{@autoreleasepool{++ready;if(writer->ValidateProducerCompletion()!=!bad)okay=false;++done;}});
    std::array<u8,4> output{};threads.emplace_back([&]{@autoreleasepool{++ready;if(writer->ExportGuestBytes(base,base+4,output)!=!bad)okay=false;++done;}});
    while(ready.load()!=9)std::this_thread::yield();std::this_thread::sleep_for(std::chrono::milliseconds(15));
    const bool escaped=done.load()!=0||exported.load();release.store(true,std::memory_order_release);gate.Open();
    for(auto& t:threads)t.join();Check(!escaped&&observed.load()!=0,"Pending completion escaped validation");
    Check(okay.load()&&writer->Status()==desired&&exported.load()==!bad,"Concurrent poll/wait/export disagreed");
    if(!bad)for(auto byte:output)Check(byte==0x6b,"Gated GPU export bytes differ");
    if(bad){const auto polls=owner->statuses.load();Check(writer->Status()==MetalGuestWriteStatus::Failed&&owner->statuses.load()>polls,"Bad receipt writer failure memoized");}
}
}
int main(){@autoreleasepool{try{
    auto device=MTLCreateSystemDefaultDevice();Check(device!=nil,"Metal device missing");const bool enabled=Option("CARA_METAL_CACHE_COMPLETED_WRITERS");
    CompletedProof(device,enabled);Rejections(device);
    for(bool predecessor:{false,true})for(unsigned bad=0;bad<3;++bad)GatedReceipt(device,bad,predecessor);
    std::cout<<"PASS completed_writer="<<enabled<<" predecessor="<<Option("CARA_METAL_CACHE_COMPLETED_PREDECESSORS")<<" own/pred/receipt/gates/failure/metadata/concurrency\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}}
