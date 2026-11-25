// SPDX-License-Identifier: GPL-2.0-or-later
#include "android/jni/atlas_uniform_policy.h"

#include <optional>
#include <iostream>

using Cara::AndroidAtlas::DefaultPersistentUniforms;
using Cara::AndroidAtlas::ResolvePersistentUniformsPreference;

int main() {
    static_assert(DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G57", true));
    static_assert(DefaultPersistentUniforms(true, false, "RG Slide", "", "ums9620_2h10_native", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(false, false, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, true, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G57", false));
    static_assert(!DefaultPersistentUniforms(true, false, "Other", "ums9620_2h10_native", "", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "other", "other", "ARM", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "Qualcomm", "Mali-G57", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "ARM", "Mali-G52", true));
    static_assert(!DefaultPersistentUniforms(true, false, "RG Slide", "ums9620_2h10_native", "", "", "", true));
    static_assert(!DefaultPersistentUniforms(true, false, "", "", "", "", "", true));

    static_assert(ResolvePersistentUniformsPreference(std::nullopt, false, true));
    static_assert(!ResolvePersistentUniformsPreference(std::nullopt, false, false));
    static_assert(!ResolvePersistentUniformsPreference(std::nullopt, true, true)); // malformed
    static_assert(!ResolvePersistentUniformsPreference(false, true, true)); // explicit opt-out
    static_assert(ResolvePersistentUniformsPreference(true, true, false)); // prior manual opt-in
    using Cara::AndroidAtlas::ResolveValidatedReuse;
    constexpr auto defaults = ResolveValidatedReuse(true, true, {}, false, {}, false, {}, false);
    static_assert(defaults.layout && defaults.state && defaults.packed_fetch);
    constexpr auto adreno = ResolveValidatedReuse(false, true, {}, false, {}, false, {}, false);
    static_assert(!adreno.layout && !adreno.state && !adreno.packed_fetch);
    constexpr auto no_ubo = ResolveValidatedReuse(true, false, {}, false, {}, false, {}, false);
    static_assert(!no_ubo.layout && !no_ubo.state && !no_ubo.packed_fetch);
    constexpr auto layout_off = ResolveValidatedReuse(true, true, false, true, {}, false, {}, false);
    static_assert(!layout_off.layout && !layout_off.state && !layout_off.packed_fetch);
    constexpr auto state_off = ResolveValidatedReuse(true, true, {}, false, false, true, {}, false);
    static_assert(state_off.layout && !state_off.state && !state_off.packed_fetch);
    constexpr auto fetch_off = ResolveValidatedReuse(true, true, {}, false, {}, false, false, true);
    static_assert(fetch_off.layout && fetch_off.state && !fetch_off.packed_fetch);
    constexpr auto malformed = ResolveValidatedReuse(true, true, {}, true, {}, false, {}, false);
    static_assert(!malformed.layout && !malformed.state && !malformed.packed_fetch);
    constexpr auto manual = ResolveValidatedReuse(false, false, true, true, true, true, true, true);
    static_assert(manual.layout && manual.state && manual.packed_fetch);
    std::cout << "PASS: narrow RG Slide Atlas auto-default, unknown/missing capability, worker, accuracy and explicit config guards\n";
}
