// SPDX-License-Identifier: GPL-2.0-or-later
// Requires the matching rebuilt engine archive after queue layout changes.
#define main SubmissionWindowSourceFixtureMain
#include "metal_transfer_source_reuse_test.mm"
#undef main
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
template<class F> bool Watchdog(QueueGate& gate,F&& operation) {
    std::mutex mutex;std::condition_variable cv;bool returned=false,released=false;
    std::thread worker([&] {std::unique_lock lock(mutex);
        if(!cv.wait_for(lock,std::chrono::milliseconds(100),[&]{return returned;})) {released=true;gate.Release();}
    });
    try {operation();} catch(...) {
        {std::lock_guard lock(mutex);returned=true;}cv.notify_one();gate.Release();worker.join();throw;
    }
    {std::lock_guard lock(mutex);returned=true;}cv.notify_one();worker.join();return released;
}

void CheckWindowFIFO(id<MTLDevice> device, unsigned capacity) {
    auto queue=[device newCommandQueue]; unsigned rejected=0;
    PicaMetalSubmissionQueue submissions(queue,[&]{++rejected;});
    Require(submissions.Limit()==capacity,"Wrong effective submission window");
    static_assert(PicaMetalSubmissionQueue::Capacity==8);
    static_assert(PicaMetalSubmissionQueue::MaxCapacity==32);
    for(unsigned round=0;round<3;++round) {
        // Advance the first slot by one so full dependency snapshots cross
        // the physical wrap, rather than testing only origin-zero windows.
        auto prime=[queue commandBuffer];[prime commit];PicaMetalDrawTicket primed;primed.command=prime;
        submissions.Track(primed);[prime waitUntilCompleted];submissions.Drain();
        QueueGate gate(queue);
        auto output=[device newBufferWithLength:capacity*4 options:MTLResourceStorageModeShared];
        std::vector<id<MTLCommandBuffer>> commands;
        for(unsigned i=0;i<capacity;++i) {
            Require(!submissions.Full(),"Queue became full before actual limit");
            auto command=[queue commandBuffer];auto blit=[command blitCommandEncoder];
            [blit fillBuffer:output range:NSMakeRange(i*4,4) value:u8(i+round+1)];
            [blit endEncoding];[command commit];
            PicaMetalDrawTicket ticket;ticket.command=command;submissions.Track(ticket);
            commands.push_back(command);
        }
        Require(submissions.Full(),"Queue did not become full at actual limit");
        const auto snapshot=submissions.Predecessors();
        Require(snapshot && snapshot->count==capacity,"Snapshot truncated live window");
        for(unsigned i=0;i<capacity;++i)
            Require(snapshot->commands[i]==commands[i],"FIFO snapshot order changed");
        Require(Watchdog(gate,[&]{submissions.Reserve();}),"Full queue did not wait oldest producer");
        Require(!submissions.Full(),"Reserve did not release capacity");
        submissions.Drain();
        Require(snapshot->Wait(),"Retired snapshot lost successful commands");
        const auto* bytes=static_cast<const u8*>(output.contents);
        for(unsigned i=0;i<capacity;++i)
            for(unsigned k=0;k<4;++k)
                Require(bytes[i*4+k]==u8(i+round+1),"GPU output/lifetime changed after FIFO retirement");
    }
    Require(rejected==0,"Valid window unexpectedly rejected writes");
}
void CheckWindowFailure(id<MTLDevice> device,unsigned capacity,unsigned bad_slot,unsigned bad_buffer) {
    auto queue=[device newCommandQueue];MetalGuestAuthority authority;unsigned rejected=0;
    PicaMetalSubmissionQueue submissions(queue,[&]{++rejected;authority.RejectSubmittedWrites();});
    auto first=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    auto second=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    auto raw=[device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *static_cast<u32*>(first.contents)=0;*static_cast<u32*>(second.contents)=0;
    QueueGate gate(queue);std::shared_ptr<const PicaMetalValidationReceipt> receipt;
    for(unsigned i=0;i<capacity;++i) {
        auto command=[queue commandBuffer];
        PicaMetalDrawTicket ticket;ticket.command=command;
        if(i==bad_slot) {
            auto blit=[command blitCommandEncoder];
            [blit fillBuffer:(bad_buffer ? second:first) range:NSMakeRange(0,4) value:1];[blit endEncoding];
            receipt=std::make_shared<PicaMetalValidationReceipt>(command,std::array<id<MTLBuffer>,2>{first,second});
            ticket.validation=receipt;
        }
        [command commit];submissions.Track(ticket);
    }
    auto snapshot=submissions.Predecessors();Require(snapshot && snapshot->count==capacity,"Failure snapshot truncated");
    Require(snapshot->validations[bad_slot]==receipt,"High-slot validation receipt missing");
    bool exported=false;auto writer=std::make_shared<MetalSurfaceWriter>(raw,snapshot->commands[capacity-1],1,base,base+4,
        [&](id<MTLCommandQueue>,id<MTLResource>,u64,PAddr,PAddr,std::span<u8> bytes) {
            exported=true;std::fill(bytes.begin(),bytes.end(),0x55);return true;
        },0,snapshot,raw);
    MetalGuestWrite write{base,base+4,writer};authority.Publish({&write,1});
    gate.Release();[snapshot->commands[capacity-1] waitUntilCompleted];
    std::array<u8,4> bytes{};
    Require(writer->Status()==MetalGuestWriteStatus::Failed,"High-slot failure lost in descendant");
    Require(!writer->ExportGuestBytes(base,base+4,bytes) && !exported,"Failed descendant reached guest CPU");
    Rejected([&]{authority.NativeSegments(base,base+4);});
    // Failure in slot zero guarantees capacity retirement discovers it before
    // the newly committed command can receive a ring slot.
    if(bad_slot==0) {
        QueueGate untracked_gate(queue);auto untracked=[queue commandBuffer];auto fill=[untracked blitCommandEncoder];
        [fill fillBuffer:raw range:NSMakeRange(0,4) value:0x7b];[fill endEncoding];[untracked commit];
        PicaMetalDrawTicket ticket;ticket.command=untracked;
        Require(Watchdog(untracked_gate,[&]{Rejected([&]{submissions.Track(ticket);});}),"Unslotted command escaped failure cleanup");
        Require(untracked.status==MTLCommandBufferStatusCompleted && !untracked.error &&
                *static_cast<const u32*>(raw.contents)==0x7b7b7b7b,"Unslotted resource released before GPU completion");
    }
    Rejected([&]{submissions.Drain();});Require(rejected!=0,"Validation failure did not reject queue writes");
    submissions.Abort();Rejected([&]{submissions.Drain();});
    *static_cast<u32*>(first.contents)=0;*static_cast<u32*>(second.contents)=0;
    Require(receipt->Poll()==PicaMetalCompletion::Failed,"Abort/host mutation lost terminal failure");
    Require(writer->Status()==MetalGuestWriteStatus::Failed,"Retained descendant lost failure after Abort");
}
}
int main(int argc,char** argv) { @autoreleasepool {try {
    Require(argc==2,"Pass expected effective window");const auto capacity=unsigned(std::stoul(argv[1]));
    Require(capacity==8 || capacity==16 || capacity==32,"Unsupported fixture capacity");
    auto device=MTLCreateSystemDefaultDevice();Require(device!=nil,"No Metal device");
    CheckWindowFIFO(device,capacity);
    CheckWindowFailure(device,capacity,0,0);
    CheckWindowFailure(device,capacity,capacity-1,0);
    CheckWindowFailure(device,capacity,capacity-1,1);
    std::cout<<"PASS submission_window="<<capacity<<" fifo_wrap_rounds=3 validation_cases=3 ticket_bytes="<<sizeof(PicaMetalDrawTicket)
             <<" predecessor_bytes="<<sizeof(MetalProducerPredecessors)<<"\n";return 0;
} catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;} } }
