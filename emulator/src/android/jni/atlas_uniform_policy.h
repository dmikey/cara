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

struct ValidatedReuse {
    bool layout;
    bool state;
    bool packed_fetch;
};

constexpr ValidatedReuse ResolveValidatedReuse(
    bool validated_hardware, bool persistent_uniforms,
    std::optional<bool> layout, bool layout_present,
    std::optional<bool> state, bool state_present,
    std::optional<bool> packed_fetch, bool packed_fetch_present) {
    const bool auto_base = validated_hardware && persistent_uniforms;
    const bool use_layout = ResolvePersistentUniformsPreference(layout, layout_present, auto_base);
    const bool use_state = ResolvePersistentUniformsPreference(state, state_present,
                                                              auto_base && use_layout);
    const bool use_fetch = ResolvePersistentUniformsPreference(packed_fetch, packed_fetch_present,
                                                              auto_base && use_layout && use_state);
    return {use_layout, use_state, use_fetch};
}

} // namespace Cara::AndroidAtlas
