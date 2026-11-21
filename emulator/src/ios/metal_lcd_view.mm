// SPDX-License-Identifier: GPL-2.0-or-later
#include "ios/metal_lcd_view.h"

#include <cmath>
#include <limits>

@implementation CaraNativeMetalLcdView
+ (Class)layerClass { return CAMetalLayer.class; }

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.userInteractionEnabled = NO;
        self.isAccessibilityElement = NO;
        CAMetalLayer* layer = self.metalLayer;
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.opaque = YES;
    }
    return self;
}

- (CAMetalLayer*)metalLayer { return (CAMetalLayer*)self.layer; }
- (id<MTLDevice>)metalDevice { return self.metalLayer.device; }

- (void)setDrawablePixelWidth:(unsigned)width height:(unsigned)height {
    if (width && height) {
        const auto size = CGSizeMake(width, height);
        if (!CGSizeEqualToSize(self.metalLayer.drawableSize, size))
            self.metalLayer.drawableSize = size;
    }
}
@end

namespace Cara::iOS {

std::optional<Layout::FramebufferLayout> MakeMetalLcdLayout(
    CGRect upper, CGRect lower, CGSize view_size, CGFloat pixel_scale) {
    const auto positive_pixel_dimension = [](double value) {
        return std::isfinite(value) && value >= 1 && value <= std::numeric_limits<int>::max();
    };
    if (!positive_pixel_dimension(view_size.width * pixel_scale) ||
        !positive_pixel_dimension(view_size.height * pixel_scale) || view_size.width <= 0 ||
        view_size.height <= 0 || pixel_scale <= 0)
        return std::nullopt;

    const u32 width = std::lround(view_size.width * pixel_scale);
    const u32 height = std::lround(view_size.height * pixel_scale);
    const auto bounds = [&](CGRect r) -> std::optional<Common::Rectangle<u32>> {
        const auto x0 = r.origin.x / view_size.width * width;
        const auto x1 = CGRectGetMaxX(r) / view_size.width * width;
        const auto y0 = r.origin.y / view_size.height * height;
        const auto y1 = CGRectGetMaxY(r) / view_size.height * height;
        if (!std::isfinite(x0) || !std::isfinite(x1) || !std::isfinite(y0) ||
            !std::isfinite(y1) || x0 < -0.5 || y0 < -0.5 || x1 >= width + 0.5 ||
            y1 >= height + 0.5 || x0 >= x1 || y0 >= y1)
            return std::nullopt;
        const long left = std::lround(x0), right = std::lround(x1);
        // Reverse the same rounded bottom-left pixel edges as the GLES
        // blit. This also keeps exact half-pixel ties on the original side.
        const long top = long(height) - std::lround((1 - r.origin.y / view_size.height) * height);
        const long bottom = long(height) - std::lround((1 - CGRectGetMaxY(r) / view_size.height) * height);
        if (left < 0 || top < 0 || right <= left || bottom <= top)
            return std::nullopt;
        return Common::Rectangle<u32>{u32(left), u32(top), u32(right), u32(bottom)};
    };
    const auto top = bounds(upper);
    const auto bottom = bounds(lower);
    if (!top || !bottom)
        return std::nullopt;

    return Layout::FramebufferLayout{width, height, true, true, *top, *bottom};
}

} // namespace Cara::iOS
