// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#import <Metal/Metal.h>

namespace Cara {

// Metal can strip a declared resource after SPIR-V translation, for example
// when a linked color write mask makes its fragment result unused. Every
// active slot still has to agree with the texture ABI selected by the caller.
inline bool MatchesMaterialBinding(NSArray<id<MTLBinding>>* bindings, MTLBindingType kind,
                                   NSUInteger index, NSString* expected, bool allow_inactive)
    API_AVAILABLE(macos(13.0), ios(16.0)) {
    for (id<MTLBinding> value in bindings) {
        if (value.isUsed && value.type == kind && value.index == index)
            return [value.name isEqualToString:expected];
    }
    return allow_inactive;
}

} // namespace Cara
