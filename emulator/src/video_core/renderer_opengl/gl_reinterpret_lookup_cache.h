// Cara per-call read-only lookup reuse; SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <utility>

namespace OpenGL {
// The caller retains its original iteration order, parameter mutations and
// logging. Only a repeated pure lookup with the same key is reused.
template <typename Key, typename Value, std::size_t Capacity>
class ReinterpretLookupCache {
public:
    struct Result { Value value; bool reused; };
    template <typename Lookup>
    Result Get(bool enabled, Key key, Lookup&& lookup) {
        if (enabled) {
            for (std::size_t i = 0; i < used; ++i)
                if (entries[i].key == key) return {entries[i].value, true};
        }
        Value value = lookup();
        if (enabled && used < Capacity) entries[used++] = {key, value};
        return {std::move(value), false};
    }
private:
    struct Entry { Key key{}; Value value{}; };
    std::array<Entry, Capacity> entries{};
    std::size_t used = 0;
};
} // namespace OpenGL
