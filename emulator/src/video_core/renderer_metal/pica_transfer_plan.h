#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace Cara::MetalTransfer {

struct GuestInterval {
    std::uint32_t begin{};
    std::uint32_t end{};

    constexpr std::uint32_t Size() const { return end - begin; }
    constexpr bool Contains(GuestInterval other) const {
        return begin < end && other.begin < other.end && begin <= other.begin && end >= other.end;
    }
    constexpr bool Overlaps(GuestInterval other) const {
        return begin < other.end && other.begin < end;
    }
};

struct Rows {
    GuestInterval footprint;
    std::uint32_t width_bytes;
    std::uint32_t stride_bytes;
    std::uint32_t count;

    constexpr GuestInterval Stripe(std::uint32_t index) const {
        if (index >= count) {
            return {};
        }
        const auto start = footprint.begin + index * stride_bytes;
        return {start, start + width_bytes};
    }
};

struct TextureCopyRegisters {
    std::uint32_t source_address;
    std::uint32_t destination_address;
    std::uint32_t size;
    std::uint16_t source_width_16;
    std::uint16_t source_gap_16;
    std::uint16_t destination_width_16;
    std::uint16_t destination_gap_16;
};

struct TextureCopyPlan {
    std::uint32_t payload_bytes;
    Rows source;
    Rows destination;
};

inline constexpr std::optional<GuestInterval> AlignedGuestInterval(std::uint32_t address,
                                                                   std::uint64_t bytes) {
    if (address == 0 || address % 8 != 0 || bytes == 0 ||
        bytes > std::numeric_limits<std::uint32_t>::max() - address) {
        return std::nullopt;
    }
    return GuestInterval{address, address + static_cast<std::uint32_t>(bytes)};
}

inline constexpr std::optional<Rows> PlanRows(std::uint32_t address, std::uint32_t payload,
                                             std::uint16_t width_16, std::uint16_t gap_16) {
    auto width = std::uint32_t{width_16} * 16;
    auto gap = std::uint32_t{gap_16} * 16;
    if (payload == 0 || (gap != 0 && width == 0)) {
        return std::nullopt;
    }
    if (gap == 0 || width >= payload) {
        width = payload;
        gap = 0;
    }
    if (payload % width != 0) {
        return std::nullopt;
    }
    const auto count = payload / width;
    const auto stride = width + gap;
    const auto range = AlignedGuestInterval(address, std::uint64_t{count - 1} * stride + width);
    if (!range) {
        return std::nullopt;
    }
    return Rows{*range, width, stride, count};
}

inline constexpr std::optional<TextureCopyPlan> PlanTextureCopy(TextureCopyRegisters regs) {
    const auto payload = regs.size & ~std::uint32_t{15};
    const auto source =
        PlanRows(regs.source_address, payload, regs.source_width_16, regs.source_gap_16);
    const auto destination = PlanRows(regs.destination_address, payload, regs.destination_width_16,
                                       regs.destination_gap_16);
    if (!source || !destination || source->footprint.Overlaps(destination->footprint)) {
        return std::nullopt;
    }
    return TextureCopyPlan{payload, *source, *destination};
}

// Initial display lane accepts equal-size linear RGBA8 and the optional vertical flip.
struct DisplayTransferRegisters {
    std::uint32_t source_address;
    std::uint32_t destination_address;
    std::uint16_t input_width;
    std::uint16_t input_height;
    std::uint16_t output_width;
    std::uint16_t output_height;
    std::uint32_t flags;
    bool address_override_enabled;
};

struct DisplayTransferPlan {
    TextureCopyPlan copy;
    bool flip_vertically;
};

inline constexpr std::optional<DisplayTransferPlan> PlanDisplayTransfer(
    DisplayTransferRegisters regs) {
    constexpr std::uint32_t Flip = 1u << 0;
    constexpr std::uint32_t LinearRgba8 = (1u << 1) | (1u << 5);
    if ((regs.flags & ~Flip) != LinearRgba8 || regs.address_override_enabled ||
        regs.input_width != regs.output_width || regs.input_height != regs.output_height ||
        regs.input_width == 0 || regs.input_height == 0) {
        return std::nullopt;
    }
    const std::uint32_t width = std::uint32_t{regs.input_width} * 4;
    const std::uint64_t bytes = std::uint64_t{width} * regs.input_height;
    const auto source = AlignedGuestInterval(regs.source_address, bytes);
    const auto destination = AlignedGuestInterval(regs.destination_address, bytes);
    if (!source || !destination || source->Overlaps(*destination)) {
        return std::nullopt;
    }
    const bool flip = regs.flags & Flip;
    const auto row_width = flip ? width : source->Size();
    const auto count = flip ? regs.input_height : 1u;
    return DisplayTransferPlan{{source->Size(),
                                {*source, row_width, row_width, count},
                                {*destination, row_width, row_width, count}},
                               flip};
}

// Tiled color cropped to the requested extent and scaled with the integer
// guest box filter. ScaleXY keeps the output tiled.
struct ScaledColorCropPlan {
    GuestInterval source;
    GuestInterval destination;
    std::uint32_t output_width;
    std::uint32_t output_height;
    std::uint32_t source_bpp;
    std::uint32_t destination_bpp;
    bool scale_xy_tiled;
};

inline constexpr std::optional<ScaledColorCropPlan> PlanScaledColorCrop(
    DisplayTransferRegisters regs) {
    constexpr std::uint32_t ScaleXCropRgb8 = 0x01001004u;
    constexpr std::uint32_t ScaleXCropRgb565 = 0x01002004u;
    constexpr std::uint32_t ScaleXCropPackedRgb565 = 0x01002204u;
    constexpr std::uint32_t ScaleXYTiledRgb565 = 0x02002220u;
    constexpr std::uint32_t ScaleXYTiledRgba8 = 0x02000020u;
    const auto flags = regs.flags & ~1u;
    const bool scale_xy_tiled = flags == ScaleXYTiledRgb565 || flags == ScaleXYTiledRgba8;
    if ((flags != ScaleXCropRgb8 && flags != ScaleXCropRgb565 &&
         flags != ScaleXCropPackedRgb565 && !scale_xy_tiled) ||
        regs.address_override_enabled ||
        ((regs.input_width | regs.input_height) & 7u) || (regs.output_width & 1u) ||
        (scale_xy_tiled && ((regs.output_width | regs.output_height) & 15u)) ||
        !regs.output_width || !regs.output_height ||
        regs.output_width > regs.input_width || regs.output_height > regs.input_height)
        return std::nullopt;
    constexpr std::uint64_t Limit = 16u * 1024u * 1024u;
    const std::uint32_t source_bpp =
        flags == ScaleXCropPackedRgb565 || flags == ScaleXYTiledRgb565 ? 2u : 4u;
    const auto src_bytes = std::uint64_t{regs.input_width} * regs.input_height * source_bpp;
    const auto dst_width = std::uint32_t{regs.output_width} / 2u;
    const auto dst_height = std::uint32_t{regs.output_height} / (scale_xy_tiled ? 2u : 1u);
    const std::uint32_t destination_bpp = scale_xy_tiled ? source_bpp :
                                          flags == ScaleXCropRgb8 ? 3u : 2u;
    const auto dst_bytes = std::uint64_t{dst_width} * dst_height * destination_bpp;
    if (src_bytes > Limit)
        return std::nullopt;
    const auto src = AlignedGuestInterval(regs.source_address, src_bytes);
    const auto dst = AlignedGuestInterval(regs.destination_address, dst_bytes);
    if (!src || !dst || src->Overlaps(*dst))
        return std::nullopt;
    return ScaledColorCropPlan{*src, *dst, dst_width, dst_height, source_bpp,
                               destination_bpp, scale_xy_tiled};
}

struct LinearToTiledRgb8Plan {
    GuestInterval source;
    GuestInterval destination;
};

inline constexpr std::optional<LinearToTiledRgb8Plan> PlanLinearToTiledRgb8(
    DisplayTransferRegisters regs) {
    constexpr std::uint32_t LinearRgba8ToTiledRgb8 = 0x00001002u;
    if ((regs.flags & ~1u) != LinearRgba8ToTiledRgb8 || regs.address_override_enabled ||
        !regs.input_width || !regs.input_height ||
        ((regs.input_width | regs.input_height) & 7u) ||
        regs.input_width != regs.output_width || regs.input_height != regs.output_height)
        return std::nullopt;
    constexpr std::uint64_t MaxBackingBytes = 16u * 1024u * 1024u;
    const auto pixels = std::uint64_t{regs.input_width} * regs.input_height;
    if (pixels > MaxBackingBytes / 4u)
        return std::nullopt;
    const auto source = AlignedGuestInterval(regs.source_address, pixels * 4u);
    const auto destination = AlignedGuestInterval(regs.destination_address, pixels * 3u);
    if (!source || !destination || source->Overlaps(*destination))
        return std::nullopt;
    return LinearToTiledRgb8Plan{*source, *destination};
}

struct LinearColorTransferPlan {
    GuestInterval source;
    GuestInterval destination;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t destination_bpp;
    bool source_tiled;
    bool flip;
};

inline constexpr std::optional<LinearColorTransferPlan> PlanLinearColorTransfer(
    DisplayTransferRegisters regs) {
    constexpr std::uint32_t Flip = 1u;
    constexpr std::uint32_t InputLinear = 1u << 1;
    constexpr std::uint32_t DontSwizzle = 1u << 5;
    constexpr std::uint32_t OutputRgb8 = 1u << 12;
    constexpr std::uint32_t Supported = Flip | InputLinear | DontSwizzle | OutputRgb8;
    const bool input_linear = (regs.flags & InputLinear) != 0;
    const bool dont_swizzle = (regs.flags & DontSwizzle) != 0;
    const std::uint32_t width = regs.input_width;
    const std::uint32_t height = regs.input_height;
    // RGBA8 input, and RGB8 or RGBA8 output. Both supported flag pairs
    // produce linear output. Preserve the original path for any scaling,
    // cropping, larger swizzle blocks or address shims.
    if ((regs.flags & ~Supported) || input_linear != dont_swizzle ||
        regs.address_override_enabled || width != regs.output_width ||
        height != regs.output_height || (!input_linear && ((width | height) & 7u))) {
        return std::nullopt;
    }
    const auto pixels = std::uint64_t{width} * height;
    if (!pixels || width > 16384u || height > 16384u || pixels > (16u * 1024 * 1024) / 4u)
        return std::nullopt;
    const std::uint32_t bpp = regs.flags & OutputRgb8 ? 3u : 4u;
    const auto source = AlignedGuestInterval(regs.source_address, pixels * 4);
    const auto destination = AlignedGuestInterval(regs.destination_address, pixels * bpp);
    if (!source || !destination || source->Overlaps(*destination))
        return std::nullopt;
    return LinearColorTransferPlan{*source, *destination, width, height, bpp,
                                   !input_linear, (regs.flags & Flip) != 0};
}

struct FillRegisters {
    std::uint32_t start;
    std::uint32_t end;
    std::uint32_t value;
    bool fill_24bit;
    bool fill_32bit;
};

struct FillPlan {
    GuestInterval destination;
    std::uint32_t little_endian_pattern;
    std::uint32_t pattern_bytes;
};

inline constexpr std::optional<FillPlan> PlanFill(FillRegisters regs) {
    const std::uint32_t period = regs.fill_24bit ? 3 : (regs.fill_32bit ? 4 : 2);
    if (regs.start >= regs.end || regs.end % 8 != 0 || (regs.fill_24bit && regs.fill_32bit) ||
        (regs.end - regs.start) % period != 0) {
        return std::nullopt;
    }
    const auto range = AlignedGuestInterval(regs.start, regs.end - regs.start);
    if (!range) {
        return std::nullopt;
    }
    return FillPlan{*range, regs.value, period};
}

} // namespace Cara::MetalTransfer
