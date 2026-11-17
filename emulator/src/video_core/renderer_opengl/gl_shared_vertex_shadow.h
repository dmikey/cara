// SPDX-License-Identifier: GPL-2.0-or-later
// Original Cara immutable CPU vertex-input snapshot storage.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>
#include "common/common_types.h"

namespace OpenGL {

// Single-owner-thread wrapper. Leases may outlive, or be read independently of,
// the wrapper; concurrent access to the wrapper itself needs external locking.
// A lease owns CPU bytes only, never guest memory or a GPU upload resource.
class SharedVertexShadow {
public:
    using Buffer = std::vector<u8>;
    using Lease = std::shared_ptr<const Buffer>;
    static constexpr std::size_t MaxLeaseBytes = 8 * 1024 * 1024;

    SharedVertexShadow() = default;
    SharedVertexShadow(const SharedVertexShadow&) = delete;
    SharedVertexShadow& operator=(const SharedVertexShadow&) = delete;
    SharedVertexShadow(SharedVertexShadow&& other) noexcept
        : inline_buffer(std::move(other.inline_buffer)), shared_buffer(std::move(other.shared_buffer)) {
        other.inline_buffer.clear();
    }
    SharedVertexShadow& operator=(SharedVertexShadow&& other) noexcept {
        if (this != &other) {
            inline_buffer = std::move(other.inline_buffer);
            shared_buffer = std::move(other.shared_buffer);
            other.inline_buffer.clear();
        }
        return *this;
    }

    const u8* data() const noexcept { return ReadBuffer().data(); }
    std::size_t size() const noexcept { return ReadBuffer().size(); }
    bool empty() const noexcept { return ReadBuffer().empty(); }

    template <typename Iterator>
    void assign(Iterator first, Iterator last) {
        if constexpr (std::is_pointer_v<Iterator> &&
                      std::is_same_v<std::remove_cv_t<std::remove_pointer_t<Iterator>>, u8>) {
            AssignBytes(first, last);
        } else {
            // Iteration itself can throw, and may alias the current buffer.
            // Complete the new value before touching either owned representation.
            Buffer replacement(first, last);
            Replace(replacement);
        }
    }

    void clear() noexcept {
        // Releasing an owner never changes bytes retained by a lease.
        shared_buffer.reset();
        inline_buffer.clear();
    }

    Lease CaptureLease(std::size_t max_bytes = MaxLeaseBytes) noexcept {
        const Buffer& bytes = ReadBuffer();
        // Bound retained allocation as well as logical bytes. Do not shrink or
        // copy merely to qualify: callers retain their original fallback path.
        if (bytes.empty() || bytes.size() > max_bytes || bytes.size() > MaxLeaseBytes ||
            bytes.capacity() > max_bytes || bytes.capacity() > MaxLeaseBytes)
            return {};
        if (!shared_buffer) {
            try {
                // Allocate the control block before the noexcept vector move.
                // Allocation failure leaves the inline buffer, including capacity,
                // untouched. No byte copy and no shared allocation before capture.
                auto promoted = std::make_shared<Buffer>(std::move(inline_buffer));
                shared_buffer = std::move(promoted);
            } catch (const std::bad_alloc&) {
                return {};
            }
        }
        return shared_buffer;
    }

    void swap(SharedVertexShadow& other) noexcept {
        inline_buffer.swap(other.inline_buffer);
        shared_buffer.swap(other.shared_buffer);
    }
    friend void swap(SharedVertexShadow& a, SharedVertexShadow& b) noexcept { a.swap(b); }

private:
    const Buffer& ReadBuffer() const noexcept { return shared_buffer ? *shared_buffer : inline_buffer; }
    void Replace(Buffer& replacement) noexcept {
        shared_buffer.reset();
        inline_buffer.swap(replacement);
    }
    void AssignBytes(const u8* first, const u8* last) {
        if (first == last) {
            clear();
            return;
        }
        // Like vector::assign, this requires a valid iterator range.
        const std::size_t count = static_cast<std::size_t>(last - first);
        if (first == data() && count == size())
            return; // Exact self-assignment does not change published bytes.
        if (shared_buffer) {
            // Publication is permanent immutability, even with one strong owner:
            // a previously issued weak_ptr can concurrently acquire a reader.
            Buffer replacement(first, last);
            Replace(replacement);
            return;
        }
        const auto less = std::less<const u8*>{};
        const bool overlaps = !inline_buffer.empty() &&
            less(first, inline_buffer.data() + inline_buffer.size()) &&
            less(inline_buffer.data(), last);
        if (overlaps) {
            Buffer replacement(first, last);
            inline_buffer.swap(replacement);
        } else {
            // Preserve the legacy vector allocation/growth/exception policy for
            // never-published ordinary input. No shared control block exists.
            inline_buffer.assign(first, last);
        }
    }

    Buffer inline_buffer;
    Lease shared_buffer;
};
static_assert(std::is_nothrow_move_constructible_v<SharedVertexShadow::Buffer>);
static_assert(std::is_nothrow_move_assignable_v<SharedVertexShadow::Buffer>);

} // namespace OpenGL
