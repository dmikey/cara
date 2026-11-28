// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <memory>
namespace Cara {
class ExactMetalIndexScanMemo;
// Identifies bytes verified against one memo revision, not a guest address.
// Retained identities cannot be recycled into a different payload revision.
class MetalIndexSnapshotIdentity final {
public:
    std::size_t Bytes() const noexcept { return bytes; }
private:
    friend class ExactMetalIndexScanMemo;
    explicit MetalIndexSnapshotIdentity(std::size_t bytes_) : bytes(bytes_) {}
    const std::size_t bytes;
};
using MetalIndexSnapshotLease = std::shared_ptr<const MetalIndexSnapshotIdentity>;
// The verified read is also bound to the actual captured payload address.
// A lease from one byte-equal allocation cannot authorize another allocation.
class MetalVerifiedIndexRead final {
public:
    MetalVerifiedIndexRead() = default;
    explicit operator bool() const noexcept { return bool(identity); }
    void reset() noexcept { identity.reset(); data=nullptr; bytes=0; }
    const MetalIndexSnapshotLease& Identity() const noexcept { return identity; }
    bool MatchesPayload(const void* first, std::size_t size) const noexcept {
        return identity && data==first && bytes==size && size==identity->Bytes();
    }
private:
    friend class ExactMetalIndexScanMemo;
    MetalVerifiedIndexRead(MetalIndexSnapshotLease id,const void* first,std::size_t size)
        : identity(std::move(id)),data(first),bytes(size) {}
    MetalIndexSnapshotLease identity;
    const void* data = nullptr;
    std::size_t bytes = 0;
};
} // namespace Cara
