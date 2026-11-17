#pragma once
#include "core/memory.h"
#include "cara/checked_ram_snapshot.h"
namespace Cara::GuestGPUTrace::RecordSnapshots {
struct PageAdapter {
    Memory::PageTable* table=nullptr;
    u8* const* active_pages=nullptr;
    bool HasTable() const {return table!=nullptr;}
    bool MetadataVA(std::uint64_t va) const {
        // Dynamic pointers are confined to heap/linear metadata domains.
        // Only four proven fixed data words are allowed in the image domain.
        return (va>=Memory::HEAP_VADDR && va+4<=Memory::HEAP_VADDR_END) ||
            (va>=Memory::LINEAR_HEAP_VADDR && va+4<=Memory::LINEAR_HEAP_VADDR_END) ||
            (va>=Memory::NEW_LINEAR_HEAP_VADDR && va+4<=Memory::NEW_LINEAR_HEAP_VADDR_END) ||
            va==0xbbf6d8 || va==0xbbf6dc || va==0xc220ec || va==0xc220f0;
    }
    const void* TableArray() const {return table?table->GetPointerArray().data():nullptr;}
    const void* ActiveArray() const {return active_pages;}
    std::size_t EntryCount() const {return Memory::PAGE_TABLE_NUM_ENTRIES;}
    CaraRecordSpan::RAM::PageClass Class(std::size_t index) const {
        switch(table->attributes[index]){
        case Memory::PageType::Memory:return CaraRecordSpan::RAM::PageClass::Ordinary;
        case Memory::PageType::RasterizerCachedMemory:return CaraRecordSpan::RAM::PageClass::Cached;
        case Memory::PageType::Special:return CaraRecordSpan::RAM::PageClass::MMIO;
        case Memory::PageType::Unmapped:return CaraRecordSpan::RAM::PageClass::Unmapped;
        }
        return static_cast<CaraRecordSpan::RAM::PageClass>(255);
    }
    const std::uint8_t* Pointer(std::size_t index) const {return active_pages[index];}
};
} // namespace Cara::GuestGPUTrace::RecordSnapshots
