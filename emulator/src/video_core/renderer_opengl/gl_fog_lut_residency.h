// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace OpenGL {

// Each entry names bytes uploaded to the current lighting/fog stream lap.
template <typename Table>
class FogLUTResidency {
public:
    int Find(const Table& table) const {
        for (const auto& entry : entries) {
            if (entry.offset >= 0 &&
                std::memcmp(entry.table.data(), table.data(), sizeof(Table)) == 0)
                return entry.offset;
        }
        return -1;
    }

    void Remember(const Table& table, int offset) {
        entries[next] = {table, offset};
        next = (next + 1) % entries.size();
    }

    void Invalidate() {
        for (auto& entry : entries)
            entry.offset = -1;
        next = 0;
    }

private:
    static_assert(std::is_trivially_copyable_v<Table>);
    static_assert(sizeof(Table) == sizeof(typename Table::value_type) * Table{}.size());

    struct Entry {
        Table table{};
        int offset = -1;
    };

    std::array<Entry, 8> entries{};
    std::size_t next = 0;
};

} // namespace OpenGL
