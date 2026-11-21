// Licensed under GPLv2 or any later version
#pragma once

#include <memory>
#include <span>
#include "common/common_types.h"

namespace Cara {

// One committed producer and its immutable logical backing generation. The
// implementation owns the backing and completion ticket until the last dirty
// interval releases the lease.
enum class MetalGuestWriteStatus { Pending, Complete, Failed };

struct MetalResidencyStamp {
    u64 revision = 0;
    u64 generation = 0;

    bool operator==(const MetalResidencyStamp&) const = default;
};

class MetalGuestWriter {
public:
    virtual ~MetalGuestWriter() = default;
    virtual MetalGuestWriteStatus Status() const = 0;
    virtual bool Covers(PAddr begin, PAddr end) const = 0;

    // Nonzero only for a native texture initialized from a complete certified
    // logical surface. Transfer writers and generic leases make no such claim.
    virtual MetalResidencyStamp Residency() const { return {}; }

    // Wait for the owning command and any required GPU export. Address the
    // subset of the original producer literally, including partial pixels.
    // Return false on any failure. This buffer is private scratch and the
    // implementation never reads/writes guest RAM or calls back into the cache.
    virtual bool ExportGuestBytes(PAddr begin, PAddr end, std::span<u8> output) const = 0;
};

using MetalGuestWriterRef = std::shared_ptr<const MetalGuestWriter>;

} // namespace Cara
