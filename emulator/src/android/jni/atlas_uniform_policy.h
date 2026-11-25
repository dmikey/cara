// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <string_view>

namespace Cara::AndroidAtlas {

// Only the physical RG Slide T820 / Mali-G57 direct Atlas Performance path
// has a matched gameplay timing bracket. The driver's real allocation/map
// and queue/fence checks still control activation in OGLStreamBuffer.
constexpr bool DefaultPersistentUniforms(bool performance, bool ordered_worker,
                                         std::string_view model, std::string_view product,
                                         std::string_view device, std::string_view vendor,
                                         std::string_view renderer, bool buffer_storage) {
    return performance && !ordered_worker && buffer_storage && model == "RG Slide" &&
           (product == "ums9620_2h10_native" || device == "ums9620_2h10_native") &&
           vendor == "ARM" && renderer == "Mali-G57";
}

// An unknown/malformed explicit value must not opt a user into a new default.
// A valid manual true/false retains the existing Debug override behavior.
constexpr bool ResolvePersistentUniformsPreference(std::optional<bool> configured,
                                                   bool present, bool automatic_default) {
    return configured.value_or(!present && automatic_default);
}

} // namespace Cara::AndroidAtlas
