// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include "common/memory_ref.h"

// Test fixtures retain the array for the entire mapped-memory test.
class TestArrayMemory final : public BackingMem {
public:
    TestArrayMemory(u8* bytes, std::size_t size) : bytes(bytes), size(size) {}
    u8* GetPtr() override { return bytes; }
    const u8* GetPtr() const override { return bytes; }
    std::size_t GetSize() const override { return size; }
private:
    u8* bytes;
    std::size_t size;
};
template <typename T, std::size_t N>
MemoryRef TestMemory(std::array<T, N>& array) {
    return MemoryRef(std::make_shared<TestArrayMemory>(reinterpret_cast<u8*>(array.data()), sizeof(array)));
}
