// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include <optional>
#include "common/common_types.h"
#include "core/frontend/framebuffer_layout.h"

@interface CaraNativeMetalLcdView : UIView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
@property(nonatomic, readonly) id<MTLDevice> metalDevice;
// UIKit owner only. Screen layout publication happens after this resize.
- (void)setDrawablePixelWidth:(unsigned)width height:(unsigned)height;
@end

namespace Cara::iOS {

// Pure pixel-geometry contract for the same view-relative rectangles as the
// GLES drawable. It leaves the logical emulation/touch layout untouched.
std::optional<Layout::FramebufferLayout> MakeMetalLcdLayout(
    CGRect upper, CGRect lower, CGSize view_size, CGFloat pixel_scale);

} // namespace Cara::iOS
