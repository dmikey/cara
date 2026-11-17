// Diagnostic metadata only; guest pointers are never translated into PAddr here.
#pragma once
#include "cara/checked_ram_snapshot.h"
namespace Cara::GuestGPUTrace::RecordSnapshots {
namespace RAM = CaraRecordSpan::RAM;
enum class Group : std::uint32_t { Recorder=1,Entry,Payload,Subtype,Descriptor,List,Globals,RecordingList,ExecutingList };
constexpr std::uint32_t DependencyUnknown=0x100, MetadataDomainRejected=0x101;
struct GroupResult {
    Group group{};std::uint32_t reason=0;bool known=false;
    std::uint32_t requested=0,failed_index=0,first_word=0,word_count=0;
    std::uint64_t base=0;
};
struct Word {Group group{};RAM::Word word{};};
struct Packet {
    std::uint32_t r4=0,r5=0,r8=0;
    std::size_t group_count=0,word_count=0;
    std::array<GroupResult,6> groups{};
    std::array<Word,32> words{};
    std::size_t RecordCount() const {return 1+group_count+word_count;}
};
inline bool TypedPC(std::uint32_t pc) {
    return pc==0x93b550 || pc==0x93b4bc || pc==0xa22398 || pc==0xa1b434 || pc==0xa19830;
}
template<class Adapter,std::size_t N>
RAM::Snapshot<> Read(Packet& p,Adapter& a,Group group,std::uint64_t base,
                     const std::array<RAM::Request,N>& requests) {
    static_assert(N<=6);
    RAM::Snapshot<> result;
    bool domain_ok=true;
    for(std::size_t i=0;i<N;++i) {
        std::uint32_t offset=0;
        if(RAM::Offset(requests[i].field,offset) && requests[i].base<=0xfffffffcULL &&
           offset<=0xfffffffcULL-requests[i].base &&
           !a.MetadataVA(requests[i].base+offset)) {
            domain_ok=false;result.failed_index=i;break;
        }
    }
    if(domain_ok) result=RAM::Take(a,true,true,requests.data(),requests.size());
    GroupResult g;g.group=group;g.base=base;g.reason=domain_ok?static_cast<std::uint32_t>(result.reason):MetadataDomainRejected;
    g.known=result.known;g.requested=N;g.failed_index=result.failed_index;
    g.first_word=p.word_count;g.word_count=result.count;
    // Closed plans below guarantee <=4 groups and <=16 words per callback.
    p.groups[p.group_count++]=g;
    for(std::size_t i=0;i<result.count;++i)p.words[p.word_count++]={group,result.words[i]};
    return result;
}
inline void Missing(Packet& p,Group group,std::uint64_t base) {
    GroupResult g;g.group=group;g.base=base;g.reason=DependencyUnknown;
    g.first_word=p.word_count;p.groups[p.group_count++]=g;
}
template<class Adapter>
void List(Packet& p,Adapter& a,Group group,std::uint64_t base) {
    if(!base){Missing(p,group,base);return;}
    Read(p,a,group,base,std::array<RAM::Request,6>{{
        {base,RAM::Field::ListBase},{base,RAM::Field::ListExtent},
        {base,RAM::Field::ListFinalized},{base,RAM::Field::ListDescriptors},
        {base,RAM::Field::ListProduced},{base,RAM::Field::ListConsumed}}});
}
template<class Adapter>
Packet Capture(Adapter adapter,std::uint32_t pc,std::uint32_t r2,
               std::uint32_t r4,std::uint32_t r5,std::uint32_t r8) {
    Packet p;p.r4=r4;p.r5=r5;p.r8=r8;
    if(pc==0x93b550 || pc==0x93b4bc){
        if(!r4)Missing(p,Group::Recorder,r4);
        else Read(p,adapter,Group::Recorder,r4,std::array<RAM::Request,5>{{
            {r4,RAM::Field::RecorderDepth},{r4,RAM::Field::RecorderBase},
            {r4,RAM::Field::RecorderCount},{r4,RAM::Field::RecorderCommitted},
            {r4,RAM::Field::RecorderExtent}}});
        if(pc==0x93b550){
            if(!r2){Missing(p,Group::Entry,r2);Missing(p,Group::Payload,0);return p;}
            auto entry=Read(p,adapter,Group::Entry,r2,std::array<RAM::Request,2>{{
                {r2,RAM::Field::Word0},{r2,RAM::Field::Word4}}});
            if(!entry.known || !entry.words[1].value){Missing(p,Group::Payload,0);return p;}
            const auto payload=entry.words[1].value;
            auto header=Read(p,adapter,Group::Payload,payload,std::array<RAM::Request,1>{{{payload,RAM::Field::Word0}}});
            if(header.known && ((header.words[0].value>>24)&15u)==11u)
                Read(p,adapter,Group::Subtype,payload,std::array<RAM::Request,1>{{{payload,RAM::Field::Word4}}});
        }
    }else if(pc==0xa22398){
        if(!r4)Missing(p,Group::Descriptor,r4);
        else Read(p,adapter,Group::Descriptor,r4,std::array<RAM::Request,4>{{
            {r4,RAM::Field::DescriptorKind},{r4,RAM::Field::DescriptorAddress},
            {r4,RAM::Field::DescriptorBytes},{r4,RAM::Field::DescriptorFlags}}});
        List(p,adapter,Group::List,r5);
    }else if(pc==0xa1b434 || pc==0xa19830){
        // No assumption about r4/r5 recorder identity at library helper entry.
        auto globals=Read(p,adapter,Group::Globals,0xbbf6d8,std::array<RAM::Request,4>{{
            {0xbbf6d8,RAM::Field::Word0},{0xbbf6d8,RAM::Field::Word4},
            {0xc220ec,RAM::Field::Word0},{0xc220ec,RAM::Field::Word4}}});
        if(!globals.known){Missing(p,Group::RecordingList,0);Missing(p,Group::ExecutingList,0);return p;}
        List(p,adapter,Group::RecordingList,globals.words[2].value);
        List(p,adapter,Group::ExecutingList,globals.words[3].value);
    }
    return p;
}
} // namespace Cara::GuestGPUTrace::RecordSnapshots
