// Standalone diagnostic prototype; no emulator dependency, fallback or guest writes.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
namespace CaraRecordSpan::RAM {
enum class PageClass : std::uint8_t { Ordinary, Cached, MMIO, Unmapped };
enum class Reason : std::uint8_t {
    None, Disabled, Unmatched, NoTable, ArrayMismatch, InvalidPlan, TooManyWords,
    InvalidField, AddressOverflow, Unaligned, CrossPage, PageIndexOverflow,
    Cached, MMIO, Unmapped, NullOrdinaryPointer, InvalidPageClass
};
// Fixed offsets only. No caller-supplied arbitrary byte offset or bulk read API.
enum class Field : std::uint8_t {
    Word0, Word4, RecorderDepth, RecorderBase, RecorderCount, RecorderCommitted,
    RecorderExtent, ListBase, ListExtent, ListFinalized, ListDescriptors,
    ListProduced, ListConsumed, DescriptorKind, DescriptorAddress,
    DescriptorBytes, DescriptorFlags, Count
};
struct Request { std::uint64_t base=0; Field field=Field::Word0; };
struct Word { std::uint32_t address=0, value=0; Field field=Field::Word0; };
template<std::size_t Capacity=32> struct Snapshot {
    Reason reason=Reason::InvalidPlan;
    bool known=false;
    std::size_t count=0, failed_index=0;
    std::array<Word,Capacity> words{};
};
inline bool Offset(Field field,std::uint32_t& offset) {
    switch(field) {
    case Field::Word0: case Field::DescriptorKind: offset=0;return true;
    case Field::Word4: case Field::ListBase: case Field::DescriptorAddress: offset=4;return true;
    case Field::DescriptorBytes: offset=8;return true;
    case Field::ListExtent: case Field::DescriptorFlags: offset=0xc;return true;
    case Field::ListFinalized: offset=0x14;return true;
    case Field::ListDescriptors: offset=0x18;return true;
    case Field::ListProduced: offset=0x20;return true;
    case Field::ListConsumed: offset=0x24;return true;
    case Field::RecorderDepth: offset=0xc00;return true;
    case Field::RecorderBase: offset=0xc04;return true;
    case Field::RecorderCount: offset=0xc08;return true;
    case Field::RecorderCommitted: offset=0xc0c;return true;
    case Field::RecorderExtent: offset=0xc10;return true;
    default:return false;
    }
}
// Adapter contract, side-effect-free and borrowed for this synchronous call:
//   HasTable(), TableArray(), ActiveArray(), EntryCount(), Class(index), Pointer(index).
// TableArray/ActiveArray return array identities without dereferencing guest RAM;
// Class/Pointer only inspect page metadata. The helper compares array identities.
// Mapping lifetime/concurrency is the adapter owner's responsibility.
template<std::size_t Capacity=32,class Adapter>
Snapshot<Capacity> Take(Adapter& adapter,bool enabled,bool matched,
                        const Request* requests,std::size_t count) {
    static_assert(Capacity>0 && Capacity<=48,"bounded diagnostic allowlist");
    Snapshot<Capacity> result;
    if(!enabled){result.reason=Reason::Disabled;return result;}
    if(!matched){result.reason=Reason::Unmatched;return result;}
    if(count>Capacity){result.reason=Reason::TooManyWords;return result;}
    if(!count || !requests){result.reason=Reason::InvalidPlan;return result;}
    if(!adapter.HasTable()){result.reason=Reason::NoTable;return result;}
    const auto* active_array=adapter.ActiveArray();
    if(!active_array || active_array!=adapter.TableArray()){result.reason=Reason::ArrayMismatch;return result;}
    const auto entries=adapter.EntryCount();
    // Metadata preflight for the ENTIRE tuple precedes the first RAM read.
    // A failed later field therefore cannot publish a partly read tuple.
    std::array<const std::uint8_t*,Capacity> sources{};
    for(std::size_t i=0;i<count;++i){
        result.failed_index=i;const auto& request=requests[i];std::uint32_t offset=0;
        if(!Offset(request.field,offset)){result.reason=Reason::InvalidField;return result;}
        constexpr auto Last=std::uint64_t{0xfffffffc};
        if(request.base>Last || offset>Last-request.base){result.reason=Reason::AddressOverflow;return result;}
        const auto address=request.base+offset;
        if(address&3u){result.reason=Reason::Unaligned;return result;}
        const auto page_offset=address&0xfffu;
        if(page_offset>4092u){result.reason=Reason::CrossPage;return result;}
        const auto page_index=address>>12;
        if(page_index>=entries){result.reason=Reason::PageIndexOverflow;return result;}
        switch(adapter.Class(static_cast<std::size_t>(page_index))){
        case PageClass::Cached:result.reason=Reason::Cached;return result;
        case PageClass::MMIO:result.reason=Reason::MMIO;return result;
        case PageClass::Unmapped:result.reason=Reason::Unmapped;return result;
        case PageClass::Ordinary:break;
        default:result.reason=Reason::InvalidPageClass;return result;
        }
        const auto* page=adapter.Pointer(static_cast<std::size_t>(page_index));
        if(!page){result.reason=Reason::NullOrdinaryPointer;return result;}
        sources[i]=page+static_cast<std::size_t>(page_offset);
        result.words[i].address=static_cast<std::uint32_t>(address);
        result.words[i].field=request.field;
    }
    for(std::size_t i=0;i<count;++i){
        std::array<std::uint8_t,4> bytes{};
        std::memcpy(bytes.data(),sources[i],bytes.size());
        result.words[i].value=std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1])<<8) |
            (std::uint32_t(bytes[2])<<16) | (std::uint32_t(bytes[3])<<24);
    }
    result.known=true;result.reason=Reason::None;result.count=count;
    result.failed_index=count;return result;
}
} // namespace CaraRecordSpan::RAM
